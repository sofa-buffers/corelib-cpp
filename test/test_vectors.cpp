/*!
 * @file test_vectors.cpp
 * @brief Validate the pure-C++20 implementation against the shared vectors.
 *
 * Loads assets/test_vectors.json (via the vendored JSON reader) and, for every
 * vector, drives the C++20 sofab::OStream / sofab::IStream through encode,
 * decode, roundtrip and chunked scenarios — the same conformance suite the C
 * library runs, but exercising the native C++ implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab/sofab.hpp"
#include "sofab_test_json.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#ifndef SOFAB_TEST_VECTORS_PATH
#error "SOFAB_TEST_VECTORS_PATH must point at assets/test_vectors.json"
#endif

namespace {

enum class K { U, S, B, F32, F64, Str, Blob, Arr, SeqB, SeqE };
enum class E { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64 };

struct Op
{
    K kind{};
    uint32_t id = 0;
    uint64_t u = 0;
    int64_t  s = 0;
    double   f = 0;
    std::string str;
    std::vector<uint8_t> blob;
    E elem{};
    std::vector<uint64_t> au;
    std::vector<int64_t>  ai;
    std::vector<double>   af;
};

struct Vector
{
    std::string name;
    std::vector<Op> ops;
    std::vector<uint8_t> bytes;
};

bool eq32(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
bool eq64(double a, double b) { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }

int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool hex2bin(const char *h, size_t n, std::vector<uint8_t> &out)
{
    if (n % 2) return false;
    out.clear();
    for (size_t i = 0; i < n; i += 2)
    {
        int hi = hexnib(h[i]), lo = hexnib(h[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

double parseFloat(const sofab_json_t *v)
{
    if (sofab_json_type(v) == SOFAB_JSON_STRING)
    {
        size_t l; const char *s = sofab_json_string(v, &l);
        if (s && std::strcmp(s, "inf") == 0)  return std::numeric_limits<double>::infinity();
        if (s && std::strcmp(s, "-inf") == 0) return -std::numeric_limits<double>::infinity();
        return 0.0;
    }
    return sofab_json_double(v);
}

bool parseElem(const char *s, E &e)
{
    if (!std::strcmp(s, "u8")) { e = E::U8; return true; }
    if (!std::strcmp(s, "u16")) { e = E::U16; return true; }
    if (!std::strcmp(s, "u32")) { e = E::U32; return true; }
    if (!std::strcmp(s, "u64")) { e = E::U64; return true; }
    if (!std::strcmp(s, "i8")) { e = E::I8; return true; }
    if (!std::strcmp(s, "i16")) { e = E::I16; return true; }
    if (!std::strcmp(s, "i32")) { e = E::I32; return true; }
    if (!std::strcmp(s, "i64")) { e = E::I64; return true; }
    if (!std::strcmp(s, "fp32")) { e = E::F32; return true; }
    if (!std::strcmp(s, "fp64")) { e = E::F64; return true; }
    return false;
}
bool elemSigned(E e) { return e == E::I8 || e == E::I16 || e == E::I32 || e == E::I64; }
bool elemFloat(E e) { return e == E::F32 || e == E::F64; }

bool loadOp(const sofab_json_t *fj, Op &op)
{
    size_t l;
    const char *ops = sofab_json_string(sofab_json_get(fj, "op"), &l);
    if (!ops) return false;
    const sofab_json_t *idn = sofab_json_get(fj, "id");
    op.id = idn ? static_cast<uint32_t>(sofab_json_u64(idn)) : 0;

    if (!std::strcmp(ops, "unsigned")) { op.kind = K::U; op.u = sofab_json_u64(sofab_json_get(fj, "value")); }
    else if (!std::strcmp(ops, "signed")) { op.kind = K::S; op.s = sofab_json_i64(sofab_json_get(fj, "value")); }
    else if (!std::strcmp(ops, "boolean")) { op.kind = K::B; op.u = sofab_json_bool(sofab_json_get(fj, "value")) ? 1 : 0; }
    else if (!std::strcmp(ops, "fp32")) { op.kind = K::F32; op.f = parseFloat(sofab_json_get(fj, "value")); }
    else if (!std::strcmp(ops, "fp64")) { op.kind = K::F64; op.f = parseFloat(sofab_json_get(fj, "value")); }
    else if (!std::strcmp(ops, "string"))
    {
        op.kind = K::Str;
        size_t sl; const char *sv = sofab_json_string(sofab_json_get(fj, "value"), &sl);
        if (!sv) return false;
        op.str.assign(sv, sl);
    }
    else if (!std::strcmp(ops, "blob"))
    {
        op.kind = K::Blob;
        size_t hl; const char *hv = sofab_json_string(sofab_json_get(fj, "value_hex"), &hl);
        if (!hv || !hex2bin(hv, hl, op.blob)) return false;
    }
    else if (!std::strcmp(ops, "array"))
    {
        op.kind = K::Arr;
        size_t el; const char *et = sofab_json_string(sofab_json_get(fj, "element_type"), &el);
        if (!et || !parseElem(et, op.elem)) return false;
        const sofab_json_t *vals = sofab_json_get(fj, "values");
        size_t cnt = sofab_json_array_size(vals);
        for (size_t k = 0; k < cnt; k++)
        {
            const sofab_json_t *e = sofab_json_array_at(vals, k);
            if (elemFloat(op.elem)) op.af.push_back(parseFloat(e));
            else if (elemSigned(op.elem)) op.ai.push_back(sofab_json_i64(e));
            else op.au.push_back(sofab_json_u64(e));
        }
    }
    else if (!std::strcmp(ops, "sequence_begin")) { op.kind = K::SeqB; }
    else if (!std::strcmp(ops, "sequence_end")) { op.kind = K::SeqE; }
    else return false;
    return true;
}

bool loadVectors(const char *path, std::vector<Vector> &out, std::string &err)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) { err = "cannot open vector file"; return false; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string text(static_cast<size_t>(n), '\0');
    size_t rd = std::fread(text.data(), 1, static_cast<size_t>(n), f);
    std::fclose(f);
    text.resize(rd);

    char perr[128];
    sofab_json_t *root = sofab_json_parse(text.data(), text.size(), perr, sizeof(perr));
    if (!root) { err = std::string("json parse: ") + perr; return false; }

    const sofab_json_t *vectors = sofab_json_get(root, "vectors");
    size_t nv = sofab_json_array_size(vectors);
    for (size_t i = 0; i < nv; i++)
    {
        const sofab_json_t *vj = sofab_json_array_at(vectors, i);
        Vector v;
        size_t nl; const char *nm = sofab_json_string(sofab_json_get(vj, "name"), &nl);
        v.name.assign(nm ? nm : "", nm ? nl : 0);
        const sofab_json_t *fields = sofab_json_get(vj, "fields");
        size_t nf = sofab_json_array_size(fields);
        for (size_t k = 0; k < nf; k++)
        {
            Op op;
            if (!loadOp(sofab_json_array_at(fields, k), op)) { err = v.name + ": bad field"; sofab_json_free(root); return false; }
            v.ops.push_back(std::move(op));
        }
        size_t hl; const char *hex = sofab_json_string(sofab_json_get(sofab_json_get(vj, "serialized"), "hex"), &hl);
        if (!hex || !hex2bin(hex, hl, v.bytes)) { err = v.name + ": bad hex"; sofab_json_free(root); return false; }
        out.push_back(std::move(v));
    }
    sofab_json_free(root);
    return true;
}

/* --- encode --- */

template <typename Vec, typename Src>
Vec castVec(const Src &src) { return Vec(src.begin(), src.end()); }

sofab::Error replay(sofab::OStreamImpl &os, const Op &op)
{
    switch (op.kind)
    {
        case K::U:    return os.write(op.id, op.u).code();
        case K::S:    return os.write(op.id, op.s).code();
        case K::B:    return os.write(op.id, static_cast<bool>(op.u != 0)).code();
        case K::F32:  return os.write(op.id, static_cast<float>(op.f)).code();
        case K::F64:  return os.write(op.id, op.f).code();
        case K::Str:  return os.write(op.id, std::string_view{op.str}).code();
        case K::Blob: return os.write(op.id, op.blob.data(), static_cast<int32_t>(op.blob.size())).code();
        case K::SeqB: return os.sequenceBegin(op.id).code();
        case K::SeqE: return os.sequenceEnd().code();
        case K::Arr:
            switch (op.elem)
            {
                case E::U8:  { auto v = castVec<std::vector<uint8_t>>(op.au);  return os.write(op.id, v).code(); }
                case E::U16: { auto v = castVec<std::vector<uint16_t>>(op.au); return os.write(op.id, v).code(); }
                case E::U32: { auto v = castVec<std::vector<uint32_t>>(op.au); return os.write(op.id, v).code(); }
                case E::U64: { auto v = castVec<std::vector<uint64_t>>(op.au); return os.write(op.id, v).code(); }
                case E::I8:  { auto v = castVec<std::vector<int8_t>>(op.ai);   return os.write(op.id, v).code(); }
                case E::I16: { auto v = castVec<std::vector<int16_t>>(op.ai);  return os.write(op.id, v).code(); }
                case E::I32: { auto v = castVec<std::vector<int32_t>>(op.ai);  return os.write(op.id, v).code(); }
                case E::I64: { auto v = castVec<std::vector<int64_t>>(op.ai);  return os.write(op.id, v).code(); }
                case E::F32: { std::vector<float> v; for (double d : op.af) v.push_back(static_cast<float>(d)); return os.write(op.id, v).code(); }
                case E::F64: { auto v = castVec<std::vector<double>>(op.af);   return os.write(op.id, v).code(); }
            }
    }
    return sofab::Error::UsageError;
}

bool encode(const Vector &v, size_t tiny, std::string &err)
{
    std::vector<uint8_t> out;
    if (tiny == 0)
    {
        sofab::OStream os(4096);
        for (const Op &op : v.ops)
            if (replay(os, op) != sofab::Error::None) { err = "encode error"; return false; }
        out.assign(os.data(), os.data() + os.bytesUsed());
    }
    else
    {
        std::vector<uint8_t> acc;
        auto buf = std::make_shared<uint8_t[]>(tiny);
        sofab::OStream os([&acc](std::span<const uint8_t> chunk) {
            acc.insert(acc.end(), chunk.begin(), chunk.end());
        }, buf, tiny);
        for (const Op &op : v.ops)
            if (replay(os, op) != sofab::Error::None) { err = "encode error"; return false; }
        os.flush();
        out = std::move(acc);
    }
    if (out.size() != v.bytes.size() || std::memcmp(out.data(), v.bytes.data(), out.size()) != 0)
    { err = "bytes differ from serialized.hex"; return false; }
    return true;
}

/* --- decode (generic cursor over the op list) --- */

struct Cursor
{
    const std::vector<Op> *ops = nullptr;
    size_t i = 0;
    bool fail = false;
    std::string err;
};

struct GenericMsg : sofab::IStreamMessage
{
    Cursor *cur = nullptr;

    void deserialize(sofab::IStreamImpl &is, sofab::id, size_t, size_t) noexcept override
    {
        const auto &ops = *cur->ops;
        while (cur->i < ops.size() && ops[cur->i].kind == K::SeqE) cur->i++;
        if (cur->i >= ops.size()) { cur->fail = true; cur->err = "extra field"; return; }
        const Op &op = ops[cur->i++];

        auto bad = [&](const char *m) { if (!cur->fail) { cur->fail = true; cur->err = m; } };

        switch (op.kind)
        {
            case K::U:   { uint64_t x = 0; is.read(x); if (x != op.u) bad("u"); break; }
            case K::S:   { int64_t x = 0;  is.read(x); if (x != op.s) bad("s"); break; }
            case K::B:   { bool x = false; is.read(x); if (x != (op.u != 0)) bad("bool"); break; }
            case K::F32: { float x = 0;    is.read(x); if (!eq32(x, static_cast<float>(op.f))) bad("fp32"); break; }
            case K::F64: { double x = 0;   is.read(x); if (!eq64(x, op.f)) bad("fp64"); break; }
            case K::Str: { std::string x;  is.read(x); if (x != op.str) bad("string"); break; }
            case K::Blob:
            {
                std::vector<uint8_t> buf(op.blob.size() + 1);
                size_t n = is.read(buf.data(), buf.size());
                if (n != op.blob.size() || std::memcmp(buf.data(), op.blob.data(), op.blob.size()) != 0) bad("blob");
                break;
            }
            case K::Arr:
            {
                auto cmpU = [&](auto vec) { is.read(vec); for (size_t k = 0; k < vec.size(); k++) if (static_cast<uint64_t>(vec[k]) != op.au[k]) { bad("arr-u"); break; } };
                auto cmpI = [&](auto vec) { is.read(vec); for (size_t k = 0; k < vec.size(); k++) if (static_cast<int64_t>(vec[k]) != op.ai[k]) { bad("arr-i"); break; } };
                switch (op.elem)
                {
                    case E::U8:  cmpU(std::vector<uint8_t>(op.au.size()));  break;
                    case E::U16: cmpU(std::vector<uint16_t>(op.au.size())); break;
                    case E::U32: cmpU(std::vector<uint32_t>(op.au.size())); break;
                    case E::U64: cmpU(std::vector<uint64_t>(op.au.size())); break;
                    case E::I8:  cmpI(std::vector<int8_t>(op.ai.size()));   break;
                    case E::I16: cmpI(std::vector<int16_t>(op.ai.size()));  break;
                    case E::I32: cmpI(std::vector<int32_t>(op.ai.size()));  break;
                    case E::I64: cmpI(std::vector<int64_t>(op.ai.size()));  break;
                    case E::F32: { std::vector<float> v(op.af.size());  is.read(v); for (size_t k = 0; k < v.size(); k++) if (!eq32(v[k], static_cast<float>(op.af[k]))) { bad("arr-f32"); break; } break; }
                    case E::F64: { std::vector<double> v(op.af.size()); is.read(v); for (size_t k = 0; k < v.size(); k++) if (!eq64(v[k], op.af[k])) { bad("arr-f64"); break; } break; }
                }
                break;
            }
            case K::SeqB: { GenericMsg child; child.cur = cur; is.read(child); break; }
            case K::SeqE: break;
        }
    }
};

bool decode(const Vector &v, bool oneByte, std::string &err)
{
    Cursor cur; cur.ops = &v.ops;
    sofab::IStreamObject<GenericMsg> in;
    (*in).cur = &cur;

    if (oneByte)
        for (uint8_t b : v.bytes) in.feed(&b, 1);
    else
        in.feed(v.bytes.data(), v.bytes.size());

    while (cur.i < v.ops.size() && v.ops[cur.i].kind == K::SeqE) cur.i++;
    if (cur.fail) { err = "decode: " + cur.err; return false; }
    if (cur.i != v.ops.size()) { err = "decode consumed " + std::to_string(cur.i) + "/" + std::to_string(v.ops.size()); return false; }
    return true;
}

bool roundtrip(const Vector &v, std::string &err)
{
    sofab::OStream os(4096);
    for (const Op &op : v.ops)
        if (replay(os, op) != sofab::Error::None) { err = "rt encode"; return false; }
    Vector tmp = v;
    tmp.bytes.assign(os.data(), os.data() + os.bytesUsed());
    return decode(tmp, false, err);
}

} // namespace

int main()
{
    std::vector<Vector> vectors;
    std::string err;
    if (!loadVectors(SOFAB_TEST_VECTORS_PATH, vectors, err))
    {
        std::printf("load failed: %s\n", err.c_str());
        return 2;
    }

    int checks = 0, failures = 0;
    std::string first;
    auto run = [&](bool ok, const Vector &v, const char *scenario, const std::string &detail) {
        ++checks;
        if (!ok) { ++failures; if (first.empty()) first = v.name + "/" + scenario + ": " + detail; }
    };

    const size_t tinies[] = {1, 3, 7};
    for (const Vector &v : vectors)
    {
        std::string d;
        run(encode(v, 0, d), v, "encode", d);
        for (size_t t : tinies) { std::string e; run(encode(v, t, e), v, "chunked-encode", e); }
        std::string d2; run(decode(v, false, d2), v, "decode", d2);
        std::string d3; run(decode(v, true, d3), v, "chunked-decode", d3);
        std::string d4; run(roundtrip(v, d4), v, "roundtrip", d4);
    }

    std::printf("%zu vectors, %d checks, %d failures\n", vectors.size(), checks, failures);
    if (failures) std::printf("first failure: %s\n", first.c_str());
    return failures ? 1 : 0;
}
