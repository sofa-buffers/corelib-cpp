/*!
 * @file test_vectors.cpp
 * @brief Validate the pure-C++20 implementation against the shared vectors.
 *
 * Loads assets/test_vectors.json (via the vendored JSON reader) and, for every
 * vector, drives the C++20 sofab::OStream / sofab::IStream through encode,
 * decode, roundtrip and chunked scenarios — the same conformance suite the C
 * library runs, but exercising the native C++ implementation.
 *
 * The asserted column is `serialized`, plus `serialized_sparse` for the vectors
 * whose sparse form is pure sequence omission (§2) — the rest of the sparse
 * form needs a schema and belongs to the generator's conformance drivers. See
 * the note in loadVectors().
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab/sofab.hpp"
#include "sofab_test_json.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef SOFAB_TEST_VECTORS_PATH
#error "SOFAB_TEST_VECTORS_PATH must point at assets/test_vectors.json"
#endif

namespace {

enum class K { U, S, B, F32, F64, Str, Blob, Arr, SeqB, SeqE };
/* `element: true` on a sequence_end marks an ELEMENT-position sequence: it
 * keeps its frame even when contentless (MESSAGE_SPEC §5.1), unlike a FIELD,
 * which §2 omits. See assets/test_vectors_README.md. */
enum class E { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64 };

/* Optional library-feature capability tags (see assets/test_vectors_README.md upstream).
 * A vector's "requires" list names the features it needs; a build compiled
 * without a feature (a SOFAB_DISABLE_* flag) skips the vectors that need it, so
 * the same vector file drives every build configuration. This pure-C++20
 * implementation always supports the full wire format, so buildCaps() reports
 * everything and nothing is skipped — but the filter mirrors the shared C runner
 * so a feature-reduced build (if ever introduced) would Just Work. */
enum Cap : uint32_t
{
    CAP_FIXLEN   = 1u << 0,
    CAP_ARRAY    = 1u << 1,
    CAP_SEQUENCE = 1u << 2,
    CAP_FP64     = 1u << 3,
    CAP_INT64    = 1u << 4,
    /* The `sequence_growth` block's gate. A wrapper array's container grows as
     * elements arrive, so a statically bounded profile (corelib-c-cpp, Rust
     * no_std) never runs it. This corelib collects into std::vector, so it
     * does. */
    CAP_DYN_ARR  = 1u << 5,
};

constexpr uint32_t buildCaps()
{
    uint32_t c = 0;
#if !defined(SOFAB_DISABLE_FIXLEN_SUPPORT)
    c |= CAP_FIXLEN;
#endif
#if !defined(SOFAB_DISABLE_ARRAY_SUPPORT)
    c |= CAP_ARRAY;
#endif
#if !defined(SOFAB_DISABLE_SEQUENCE_SUPPORT)
    c |= CAP_SEQUENCE;
#endif
    /* fp64 helpers live inside the fixlen block, so they need both. */
#if !defined(SOFAB_DISABLE_FP64_SUPPORT) && !defined(SOFAB_DISABLE_FIXLEN_SUPPORT)
    c |= CAP_FP64;
#endif
#if !defined(SOFAB_DISABLE_INT64_SUPPORT)
    c |= CAP_INT64;
#endif
    c |= CAP_DYN_ARR;
    return c;
}

uint32_t capFromName(const char *s)
{
    if (!std::strcmp(s, "fixlen"))   return CAP_FIXLEN;
    if (!std::strcmp(s, "array"))    return CAP_ARRAY;
    if (!std::strcmp(s, "sequence")) return CAP_SEQUENCE;
    if (!std::strcmp(s, "fp64"))     return CAP_FP64;
    if (!std::strcmp(s, "int64"))    return CAP_INT64;
    if (!std::strcmp(s, "dynamic_arrays")) return CAP_DYN_ARR;
    return 0; /* unknown tag: ignore (forward-compatible) */
}

struct Op
{
    K kind{};
    bool elementPos = false;  /* sequence_end only: §5.1 element, keeps its frame */
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
    std::vector<uint32_t> skip;   // field ids a receiver is expected to skip (skip_ids)
    uint32_t req = 0;             // capability mask from the "requires" tags
    std::vector<uint8_t> sparse;  // serialized_sparse column (see loadVectors)
    bool hasSparse = false;
    bool contentless = false;     // op list contains a sequence with no leaf field
};

/* Does the op list contain a sequence that receives no leaf field (directly or
 * through nothing but further empty sequences)? Those are exactly the sequences
 * MESSAGE_SPEC §2 omits, and — see the loadVectors() note — exactly the vectors
 * whose sparse form the raw encoder can reproduce on its own. */
bool hasContentlessSequence(const std::vector<Op> &ops)
{
    std::vector<bool> stack;   // per open sequence: has it seen a leaf field?
    bool found = false;
    for (const Op &op : ops)
    {
        if (op.kind == K::SeqB) stack.push_back(false);
        else if (op.kind == K::SeqE)
        {
            if (stack.empty()) continue;
            const bool had = stack.back();
            stack.pop_back();
            /* Only the array's LAST element keeps its frame when contentless
             * (§2: nothing that carries the length may be elided); an INTERIOR
             * all-default element is now an id gap like a default leaf. The
             * vectors' `element` marker was narrowed to mean exactly that last
             * position, so a marked closer is never an omission -- and it IS
             * content for the wrapper enclosing it, exactly like a leaf. */
            if (!had && !op.elementPos) found = true;
            if (!stack.empty() && (had || op.elementPos)) stack.back() = true;
        }
        else if (!stack.empty()) stack.back() = true;
    }
    return found;
}

/* A name-only Vector, for checks that are about the vector SET rather than one
 * vector's bytes (the run() reporter takes a Vector for its label). */
Vector named(const char *nm) { Vector v; v.name = nm; return v; }

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
    else if (!std::strcmp(ops, "sequence_end")) {
        op.kind = K::SeqE;
        const sofab_json_t *ep = sofab_json_get(fj, "element");
        op.elementPos = ep && sofab_json_bool(ep);
    }
    else return false;
    return true;
}

/* --- the shared vector file -------------------------------------------------
 *
 * Read and JSON-parsed ONCE per run: the file holds several top-level groups
 * ("vectors", "invalid_utf8", …) and each group walker below takes the parsed
 * root, never a path.
 *
 * The envelope is owned upstream (corelib-c-cpp generates it) and has already
 * grown a key once, so every walker DEMANDS its own top-level array through
 * group(): a renamed or dropped key is a loud failure instead of an empty list,
 * which would otherwise read as "nothing to test". */
struct VectorFile
{
    std::string text;
    sofab_json_t *root = nullptr;

    /*! How often the file has been read this run — the run-once guard in main()
     *  asserts exactly one (corelib-cpp#100). */
    static int reads;

    VectorFile() = default;
    VectorFile(const VectorFile &) = delete;
    VectorFile &operator=(const VectorFile &) = delete;
    ~VectorFile() { if (root) sofab_json_free(root); }
};
int VectorFile::reads = 0;

bool loadVectorFile(const char *path, VectorFile &vf, std::string &err)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) { err = "cannot open vector file"; return false; }
    ++VectorFile::reads;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    vf.text.assign(static_cast<size_t>(n), '\0');
    size_t rd = std::fread(vf.text.data(), 1, static_cast<size_t>(n), f);
    std::fclose(f);
    vf.text.resize(rd);

    char perr[128];
    vf.root = sofab_json_parse(vf.text.data(), vf.text.size(), perr, sizeof(perr));
    if (!vf.root) { err = std::string("json parse: ") + perr; return false; }
    return true;
}

/* The top-level array a group walker consumes. Absent, not an array or empty is
 * an ERROR — see the VectorFile note on envelope drift. */
const sofab_json_t *group(const sofab_json_t *root, const char *key, std::string &err)
{
    const sofab_json_t *g = root ? sofab_json_get(root, key) : nullptr;
    if (!g || sofab_json_type(g) != SOFAB_JSON_ARRAY || sofab_json_array_size(g) == 0)
    {
        err = std::string("vector file has no non-empty \"") + key + "\" array";
        return nullptr;
    }
    return g;
}

/* The capability mask from a vector's "requires" tags (both groups carry it). */
uint32_t reqMask(const sofab_json_t *vj)
{
    uint32_t mask = 0;
    const sofab_json_t *req = sofab_json_get(vj, "requires");
    size_t nr = sofab_json_array_size(req);
    for (size_t k = 0; k < nr; k++)
    {
        size_t tl; const char *tn = sofab_json_string(sofab_json_array_at(req, k), &tl);
        if (tn) mask |= capFromName(tn);
    }
    return mask;
}

bool loadVectors(const sofab_json_t *root, std::vector<Vector> &out, std::string &err)
{
    const sofab_json_t *vectors = group(root, "vectors", err);
    if (!vectors) return false;
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
            if (!loadOp(sofab_json_array_at(fields, k), op)) { err = v.name + ": bad field"; return false; }
            v.ops.push_back(std::move(op));
        }
        const sofab_json_t *skip = sofab_json_get(vj, "skip_ids");
        size_t nsk = sofab_json_array_size(skip);
        for (size_t k = 0; k < nsk; k++)
            v.skip.push_back(static_cast<uint32_t>(sofab_json_u64(sofab_json_array_at(skip, k))));
        v.req = reqMask(vj);
        /* WHICH COLUMNS THIS REPO ASSERTS.
         *
         * `serialized` -- the primitive-layer ground truth: the exact bytes this
         * vector's op list produces when replayed through the raw encoder,
         * closing every sequence with the frame-keeping closer.
         *
         * `serialized_sparse` -- the same message with every all-default
         * sequence FIELD omitted (MESSAGE_SPEC §2). Reproducing it IN GENERAL
         * needs a message layer -- a schema, per-field defaults, and the static
         * choice of closer per position -- which a corelib does not have; for
         * most vectors the two columns differ because a default-valued SCALAR
         * was dropped, and only generated code knows that a value is the
         * declared default.
         *
         * But not for all of them. Where a vector's sparse form differs from its
         * dense one ONLY by sequence omission -- the op list contains a sequence
         * that gets no field -- the raw encoder reproduces it with no schema at
         * all: replay the same ops closing with the DROPPING closer. That is
         * this library's §2 primitive measured against the shared vectors, so
         * the "dropping-closer" scenario below asserts exactly that, and asserts
         * for every other vector that the dropping closer changes nothing (it
         * must still give `serialized`). hasContentlessSequence() decides which
         * of the two, from the op list alone.
         *
         * The rest of §2 -- which closer generated code picks per field -- stays
         * with the GENERATOR's conformance drivers (sofabgen,
         * tests/conformance/<lang>), which own the schema. The hold-back trio
         * itself (sequenceBeginLazy / sequenceEnd / sequenceEndKeep) is covered
         * directly in test_roundtrip.cpp's lazySequenceFraming() and
         * deepHoldBack(). */
        size_t hl; const char *hex = sofab_json_string(sofab_json_get(sofab_json_get(vj, "serialized"), "hex"), &hl);
        if (!hex || !hex2bin(hex, hl, v.bytes)) { err = v.name + ": bad hex"; return false; }
        if (const sofab_json_t *sp = sofab_json_get(vj, "serialized_sparse"))
        {
            size_t sl2; const char *shex = sofab_json_string(sofab_json_get(sp, "hex"), &sl2);
            if (!shex || !hex2bin(shex, sl2, v.sparse)) { err = v.name + ": bad sparse hex"; return false; }
            v.hasSparse = true;
        }
        v.contentless = hasContentlessSequence(v.ops);
        out.push_back(std::move(v));
    }
    return true;
}

/* --- negative UTF-8 vectors (top-level "invalid_utf8" array; tracks
 *     corelib-c-cpp#97). Each carries the raw `string_hex` payload (encode must
 *     reject) and a whole `serialized_hex` wire message (decode must reject).
 *     Exercised only under a strict (SOFAB_STRICT_UTF8) build. --- */

struct NegVec
{
    std::string name;
    std::vector<uint8_t> payload;    // string_hex: raw string bytes for the encode-reject check
    std::vector<uint8_t> serialized; // serialized_hex: whole wire message for the decode-reject check
    uint32_t id = 0;
    uint32_t req = 0;
};

bool loadNegVectors(const sofab_json_t *root, std::vector<NegVec> &out, std::string &err)
{
    const sofab_json_t *arr = group(root, "invalid_utf8", err);
    if (!arr) return false;
    size_t nv = sofab_json_array_size(arr);
    for (size_t i = 0; i < nv; i++)
    {
        const sofab_json_t *vj = sofab_json_array_at(arr, i);
        NegVec v;
        size_t nl; const char *nm = sofab_json_string(sofab_json_get(vj, "name"), &nl);
        v.name.assign(nm ? nm : "", nm ? nl : 0);
        const sofab_json_t *idn = sofab_json_get(vj, "id");
        v.id = idn ? static_cast<uint32_t>(sofab_json_u64(idn)) : 0;
        v.req = reqMask(vj);
        size_t pl; const char *ph = sofab_json_string(sofab_json_get(vj, "string_hex"), &pl);
        size_t sl; const char *sh = sofab_json_string(sofab_json_get(vj, "serialized_hex"), &sl);
        if (!ph || !hex2bin(ph, pl, v.payload) || !sh || !hex2bin(sh, sl, v.serialized))
        { err = v.name + ": bad hex"; return false; }
        out.push_back(std::move(v));
    }
    return true;
}

/* --- sequence-array growth (top-level "sequence_growth"; CORELIB_PLAN §7.2
 *     item 8, landed upstream as corelib-c-cpp@bf29d26) -----------------------
 *
 * A wrapper array carries no count header: its length is *highest present id +
 * 1* (MESSAGE_SPEC §5.1), so the container grows as elements arrive and the
 * element INDEX is what a receiver cap binds (§6.2.1). "Nothing else in this
 * list reaches it: two ports that grow differently emit identical bytes and
 * reach identical outcomes, so §7.1's vectors are structurally blind to it."
 *
 * A case is keyed by a DELIVERY SEQUENCE OF ELEMENT IDS, not by bytes; the
 * indices are CAP-RELATIVE, so this runner picks the port's own cap, builds the
 * message from `deliver` and asserts `expect`. --- */

/*! The receiver cap this port configures for the growth block. The block's own
 *  note requires at least 4; anything larger only makes the built messages
 *  longer, so 4 it is. */
constexpr long kGrowthCap = 4;

struct GrowthDeliver
{
    long id = 0;          // absolute element index, already resolved against the cap
    std::string sval;     // element_type "string"
    uint64_t uval = 0;    // element_type "struct": the element's field-0 unsigned
};

struct GrowthCase
{
    std::string name;
    uint32_t req = 0;
    uint32_t fieldId = 0;
    bool structElems = false;              // element_type: "struct" vs "string"
    std::vector<GrowthDeliver> deliver;
    // expectations
    bool wantComplete = true;              // outcome: complete | limit_exceeded
    bool terminal = false;
    long wantLength = -1;                  // length / length_from_cap, or -1
    long maxLength = -1;                   // max_length, or -1
    std::vector<long> defaultIds;
};

/* `id` is absolute, `id_from_cap` is an offset onto the port's own cap. */
bool growthIndex(const sofab_json_t *o, const char *abs, const char *rel, long &out)
{
    if (const sofab_json_t *a = sofab_json_get(o, abs))
    { out = static_cast<long>(sofab_json_i64(a)); return true; }
    if (const sofab_json_t *r = sofab_json_get(o, rel))
    { out = kGrowthCap + static_cast<long>(sofab_json_i64(r)); return true; }
    return false;
}

bool loadGrowthCases(const sofab_json_t *root, std::vector<GrowthCase> &out, std::string &err)
{
    const sofab_json_t *arr = group(root, "sequence_growth", err);
    if (!arr) return false;
    size_t n = sofab_json_array_size(arr);
    for (size_t i = 0; i < n; i++)
    {
        const sofab_json_t *cj = sofab_json_array_at(arr, i);
        GrowthCase c;
        size_t nl; const char *nm = sofab_json_string(sofab_json_get(cj, "name"), &nl);
        c.name.assign(nm ? nm : "", nm ? nl : 0);
        c.req = reqMask(cj);
        c.fieldId = static_cast<uint32_t>(sofab_json_u64(sofab_json_get(cj, "field_id")));
        size_t tl; const char *et = sofab_json_string(sofab_json_get(cj, "element_type"), &tl);
        if (!et) { err = c.name + ": no element_type"; return false; }
        const std::string kind(et, tl);
        if (kind == "struct") c.structElems = true;
        else if (kind != "string") { err = c.name + ": unknown element_type " + kind; return false; }

        const sofab_json_t *dl = sofab_json_get(cj, "deliver");
        size_t nd = sofab_json_array_size(dl);
        for (size_t k = 0; k < nd; k++)
        {
            const sofab_json_t *dj = sofab_json_array_at(dl, k);
            GrowthDeliver d;
            if (!growthIndex(dj, "id", "id_from_cap", d.id))
            { err = c.name + ": deliver entry has neither id nor id_from_cap"; return false; }
            const sofab_json_t *val = sofab_json_get(dj, "value");
            if (c.structElems) d.uval = sofab_json_u64(val);
            else
            {
                size_t vl; const char *vs = sofab_json_string(val, &vl);
                if (!vs) { err = c.name + ": string element without a string value"; return false; }
                d.sval.assign(vs, vl);
            }
            c.deliver.push_back(std::move(d));
        }

        const sofab_json_t *ex = sofab_json_get(cj, "expect");
        if (!ex) { err = c.name + ": no expect"; return false; }
        size_t ol; const char *oc = sofab_json_string(sofab_json_get(ex, "outcome"), &ol);
        if (!oc) { err = c.name + ": no expect.outcome"; return false; }
        const std::string outcome(oc, ol);
        if (outcome == "complete") c.wantComplete = true;
        else if (outcome == "limit_exceeded") c.wantComplete = false;
        else { err = c.name + ": unknown outcome " + outcome; return false; }
        if (const sofab_json_t *t = sofab_json_get(ex, "terminal")) c.terminal = sofab_json_bool(t) != 0;
        (void)growthIndex(ex, "length", "length_from_cap", c.wantLength);
        if (const sofab_json_t *ml = sofab_json_get(ex, "max_length"))
            c.maxLength = static_cast<long>(sofab_json_i64(ml));
        const sofab_json_t *dids = sofab_json_get(ex, "default_ids");
        for (size_t k = 0, nk = sofab_json_array_size(dids); k < nk; k++)
            c.defaultIds.push_back(static_cast<long>(sofab_json_i64(sofab_json_array_at(dids, k))));
        out.push_back(std::move(c));
    }
    return true;
}

/* The `struct` element the block's struct cases describe: one unsigned at id 0. */
struct GrowthRow : sofab::Message
{
    uint64_t a = 0;
    sofab::OStreamImpl::Result serialize(sofab::OStreamImpl &os) const noexcept override
    {
        return os.write(0, a);
    }
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == 0) is.read(a);
    }
};

/* Destinations, each with the port's cap wired into its collector. */
struct GrowthStringMsg : sofab::IStreamMessage
{
    uint32_t field = 0;
    std::vector<std::string> out;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == field) { sofab::StringSeq c{out, -1, -1, kGrowthCap}; is.read(c); }
    }
};

struct GrowthStructMsg : sofab::IStreamMessage
{
    uint32_t field = 0;
    std::vector<GrowthRow> out;
    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id == field)
        {
            sofab::MessageSeq<std::vector<GrowthRow>> c;
            c.out = &out;
            c.dynCap = kGrowthCap;
            is.read(c);
        }
    }
};

/* Build the case's wire: the wrapper sequence at `field_id`, one element per
 * `deliver` entry at its own index. The frame is always emitted (the array is
 * present even when empty), and a struct element keeps its own frame. */
std::vector<uint8_t> growthWire(const GrowthCase &c)
{
    std::vector<uint8_t> out;
    sofab::OStream os([&out](std::span<const uint8_t> chunk) {
                          out.insert(out.end(), chunk.begin(), chunk.end());
                      },
                      std::make_shared<uint8_t[]>(4096), 4096);
    os.sequenceBeginLazy(c.fieldId);
    for (const GrowthDeliver &d : c.deliver)
    {
        const auto eid = static_cast<sofab::id>(d.id);
        if (c.structElems)
        {
            os.sequenceBeginLazy(eid);
            os.write(0, d.uval);
            os.sequenceEndKeep();
        }
        else
        {
            os.write(eid, std::string_view{d.sval});
        }
    }
    os.sequenceEndKeep();
    os.flush();
    return out;
}

/* --- envelope-drift guard (corelib-cpp#100) ---------------------------------
 *
 * Both group walkers now share ONE parse, and each demands its own top-level
 * key. Feed them a doctored envelope in memory and report, per group, the
 * walker's VERDICT and how many vectors it produced — kept apart on purpose: a
 * key the walker no longer recognises must be REJECTED, not silently walked as
 * an empty (== "nothing to test") list, and those two look the same if only the
 * count is inspected. */
struct EnvelopeWalk
{
    bool parsed = false;
    bool vectorsOk = false, negOk = false, growthOk = false;
    size_t nVectors = 0, nNeg = 0, nGrowth = 0;
};

EnvelopeWalk walkEnvelope(const char *json)
{
    EnvelopeWalk w;
    char perr[128];
    sofab_json_t *root = sofab_json_parse(json, std::strlen(json), perr, sizeof(perr));
    if (!root) return w;
    w.parsed = true;
    std::vector<Vector> vs;
    std::vector<NegVec> ns;
    std::vector<GrowthCase> gs;
    std::string e;
    w.vectorsOk = loadVectors(root, vs, e);
    w.negOk = loadNegVectors(root, ns, e);
    w.growthOk = loadGrowthCases(root, gs, e);
    w.nVectors = vs.size();
    w.nNeg = ns.size();
    w.nGrowth = gs.size();
    sofab_json_free(root);
    return w;
}

/* Reads the delivered field into a std::string, forcing UTF-8 validation on the
 * materialised-read path (never on skip). */
struct NegReadMsg : sofab::IStreamMessage
{
    void deserialize(sofab::IStreamImpl &is, sofab::id, size_t, size_t) noexcept override
    {
        std::string s;
        is.read(s);
    }
};

/* --- encode --- */

template <typename Vec, typename Src>
Vec castVec(const Src &src) { return Vec(src.begin(), src.end()); }

sofab::Error replay(sofab::OStreamImpl &os, const Op &op, bool keepFrames = true)
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
        case K::SeqB: return os.sequenceBeginLazy(op.id).code();
        /* `serialized` is the primitive-layer ground truth and always carries the
         * frame, so close with the keeping form: identical bytes once the
         * sequence has content, and the empty-sequence vectors keep their pair.
         * The dropping form (keepFrames == false) is what the `serialized_sparse`
         * check replays with. */
        /* keepFrames is the dense pass (every frame survives). In the dropping
         * pass a FIELD closes with the dropping end, but an ELEMENT still keeps
         * its frame -- that is the §5.1 half the sparse column encodes. */
        case K::SeqE: return (keepFrames || op.elementPos)
                             ? os.sequenceEndKeep().code() : os.sequenceEnd().code();
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
    return sofab::Error::InvalidArgument;
}

bool encode(const Vector &v, size_t tiny, std::string &err)
{
    std::vector<uint8_t> out;
    if (tiny == 0)
    {
        sofab::OStream os(std::make_shared<uint8_t[]>(4096), 4096);
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

/* Replay the op list closing every sequence with the DROPPING closer
 * (sequenceEnd). Expected bytes: `serialized_sparse` when the vector has a
 * contentless sequence -- that column then differs from `serialized` by exactly
 * the frames §2 omits -- and `serialized` otherwise, since dropping a closer
 * that has content changes nothing. */
bool encodeDropping(const Vector &v, std::string &err)
{
    const std::vector<uint8_t> &want = (v.contentless && v.hasSparse) ? v.sparse : v.bytes;
    if (v.contentless && !v.hasSparse)
    { err = "vector has an empty sequence but no serialized_sparse column"; return false; }

    sofab::OStream os(std::make_shared<uint8_t[]>(4096), 4096);
    for (const Op &op : v.ops)
        if (replay(os, op, /*keepFrames=*/false) != sofab::Error::None) { err = "encode error"; return false; }
    if (os.bytesUsed() != want.size() ||
        (!want.empty() && std::memcmp(os.data(), want.data(), want.size()) != 0))
    {
        err = v.contentless ? "bytes differ from serialized_sparse.hex"
                            : "dropping closer changed a framed sequence";
        return false;
    }
    return true;
}

/* --- decode (generic cursor over the op list) --- */

struct Cursor
{
    const std::vector<Op> *ops = nullptr;
    const std::vector<uint32_t> *skip = nullptr;   // null => skip nothing (plain decode)
    size_t i = 0;
    bool fail = false;
    std::string err;

    bool skipId(uint32_t id) const
    {
        if (!skip) return false;
        for (uint32_t s : *skip) if (s == id) return true;
        return false;
    }
};

struct GenericMsg : sofab::IStreamMessage
{
    Cursor *cur = nullptr;

    void deserialize(sofab::IStreamImpl &is, sofab::id, size_t, size_t) noexcept override
    {
        const auto &ops = *cur->ops;
        while (cur->i < ops.size() && ops[cur->i].kind == K::SeqE) cur->i++;
        if (cur->i >= ops.size()) { cur->fail = true; cur->err = "extra field"; return; }
        /* Header-first delivery: a field whose payload has not arrived yet is
         * offered now (so its bounds can be judged) and delivered AGAIN once it is
         * complete. Only advance the op cursor when a read actually took the
         * field; otherwise rewind and expect the same field next time. */
        const size_t opStart = cur->i;
        const Op &op = ops[cur->i++];
        struct Rewind {
            Cursor *c; size_t at; const sofab::IStreamImpl *is;
            bool failWas; std::string errWas;
            bool armed = true;   ///< cleared for a field skipped ON PURPOSE
            ~Rewind()
            {
                if (!armed || is->consumed()) return;
                /* The field was not taken: this delivery never happened as far as
                 * verification goes. Rewind the op cursor and drop anything the
                 * half-read values compared against -- the real check runs when the
                 * field is delivered complete. */
                c->i = at;
                c->fail = failWas;
                c->err = errWas;
            }
        } rewind{cur, opStart, &is, cur->fail, cur->err};

        /* skip-ids: leave the field unread so the decoder auto-skips its payload.
         * For a sequence, don't descend — the decoder skips the whole sub-tree —
         * and advance the op cursor past the matching SequenceEnd (any nesting). */
        if (cur->skipId(op.id))
        {
            /* Deliberately unread: the corelib will not offer it again (it records
             * the decline), so this delivery does count. */
            rewind.armed = false;
            if (op.kind == K::SeqB)
            {
                int depth = 1;
                while (depth > 0 && cur->i < ops.size())
                {
                    K k = ops[cur->i++].kind;
                    if (k == K::SeqB) ++depth;
                    else if (k == K::SeqE) --depth;
                }
            }
            return;
        }

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
                /* An empty blob's data() is null, and memcmp forbids that even
                 * for a zero length, so the compare is skipped in that case. */
                if (n != op.blob.size() ||
                    (!op.blob.empty() &&
                     std::memcmp(buf.data(), op.blob.data(), op.blob.size()) != 0)) bad("blob");
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

bool decode(const Vector &v, bool oneByte, std::string &err, const std::vector<uint32_t> *skip = nullptr)
{
    Cursor cur; cur.ops = &v.ops; cur.skip = skip;
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
    sofab::OStream os(std::make_shared<uint8_t[]>(4096), 4096);
    for (const Op &op : v.ops)
        if (replay(os, op) != sofab::Error::None) { err = "rt encode"; return false; }
    Vector tmp = v;
    tmp.bytes.assign(os.data(), os.data() + os.bytesUsed());
    return decode(tmp, false, err);
}

} // namespace

int main()
{
    /* One read, one parse: the file carries every group this run walks. */
    VectorFile vf;
    std::string err;
    if (!loadVectorFile(SOFAB_TEST_VECTORS_PATH, vf, err))
    {
        std::printf("load failed: %s\n", err.c_str());
        return 2;
    }

    std::vector<Vector> vectors;
    if (!loadVectors(vf.root, vectors, err))
    {
        std::printf("load failed: %s\n", err.c_str());
        return 2;
    }

    int checks = 0, failures = 0;
    std::string first;
    std::vector<std::string> allFailures;
    auto run = [&](bool ok, const Vector &v, const char *scenario, const std::string &detail) {
        ++checks;
        if (!ok) { ++failures; allFailures.push_back(v.name + "/" + scenario + ": " + detail); if (first.empty()) first = allFailures.back(); }
    };

    const uint32_t caps = buildCaps();
    int skipped = 0;
    int sparseByOmission = 0;   // vectors whose sparse form is pure sequence omission

    const size_t tinies[] = {1, 3, 7};
    for (const Vector &v : vectors)
    {
        /* skip vectors needing a feature this build was compiled without */
        if (v.req & ~caps) { ++skipped; continue; }

        std::string d;
        run(encode(v, 0, d), v, "encode", d);
        for (size_t t : tinies) { std::string e; run(encode(v, t, e), v, "chunked-encode", e); }
        std::string d2; run(decode(v, false, d2), v, "decode", d2);
        std::string d3; run(decode(v, true, d3), v, "chunked-decode", d3);
        if (!v.skip.empty())
        {
            std::string s1; run(decode(v, false, s1, &v.skip), v, "skip-ids", s1);
            std::string s2; run(decode(v, true,  s2, &v.skip), v, "skip-ids-chunked", s2);
        }
        std::string d4; run(roundtrip(v, d4), v, "roundtrip", d4);
        std::string d5; run(encodeDropping(v, d5), v, "dropping-closer", d5);
        if (v.contentless) ++sparseByOmission;
    }

    /* The dropping-closer scenario only asserts `serialized_sparse` for vectors
     * that HAVE a contentless sequence; if upstream ever renames or reshapes
     * them the check would quietly degrade into "dense == dense". Require the
     * three by name, and require the count to match. */
    for (const char *nm : {"empty_sequence", "nested_empty_sequences", "empty_sequence_between_fields"})
    {
        bool seen = false;
        for (const Vector &v : vectors) if (v.name == nm && v.contentless && v.hasSparse) seen = true;
        run(seen, named(nm), "sparse-by-omission-present",
            "vector missing, or no longer carries an empty sequence + serialized_sparse");
    }
    /* 4, not 3, since count-is-capacity: array_struct_all_default_elements gained a
     * droppable interior empty frame when interior sequence elements stopped being
     * framed unconditionally. */
    run(sparseByOmission == 4, named("(all)"), "sparse-by-omission-count",
        "expected 4 vectors whose sparse form is pure sequence omission, saw " + std::to_string(sparseByOmission));

    /* Negative UTF-8 group (top-level "invalid_utf8"). Under a strict build each
     * serialized_hex must decode to INVALID and each string_hex must be refused
     * on encode with InvalidArgument (spec §6.4). A non-strict build skips them:
     * with the check compiled out those bytes are accepted verbatim. */
    std::vector<NegVec> negs;
    if (!loadNegVectors(vf.root, negs, err))
    {
        std::printf("neg load failed: %s\n", err.c_str());
        return 2;
    }
    int negRun = 0, negSkipped = 0;
    for (const NegVec &nv : negs)
    {
        if (nv.req & ~caps) { ++negSkipped; continue; }
#if SOFAB_STRICT_UTF8
        ++negRun;
        /* §5.2 makes INVALID terminal and §7.2 item 4 makes the chunked result
         * identical to the one-shot one. So each negative vector is decoded twice —
         * whole, and one byte at a time, which puts the offending payload on the
         * other feed() path — and both runs then get a well-formed field appended:
         * a rejected stream must not recover, whichever path condemned it
         * (corelib-cpp#79). */
        static const uint8_t goodTail[] = {0x08, 0x2a}; /* id 1, unsigned = 42 */
        {
            sofab::IStreamObject<NegReadMsg> in;
            auto r = in.feed(nv.serialized.data(), nv.serialized.size());
            run(r.code() == sofab::Error::InvalidMessage && r.status() == sofab::DecodeStatus::Invalid,
                named(nv.name.c_str()), "utf8-decode-invalid",
                "expected INVALID, got code " + std::to_string(static_cast<int>(r.code())));
            auto r2 = in.feed(goodTail, sizeof goodTail);
            run(r2.code() == sofab::Error::InvalidMessage,
                named(nv.name.c_str()), "utf8-decode-invalid-terminal",
                "an INVALID stream recovered when valid bytes followed, got code " +
                    std::to_string(static_cast<int>(r2.code())));
        }
        {
            sofab::IStreamObject<NegReadMsg> in;
            sofab::Error last = sofab::Error::None;
            for (uint8_t b : nv.serialized) last = in.feed(&b, 1).code();
            run(last == sofab::Error::InvalidMessage,
                named(nv.name.c_str()), "utf8-decode-invalid-chunked",
                "expected INVALID byte-at-a-time, got code " + std::to_string(static_cast<int>(last)));
            auto r2 = in.feed(goodTail, sizeof goodTail);
            run(r2.code() == sofab::Error::InvalidMessage,
                named(nv.name.c_str()), "utf8-decode-invalid-chunked-terminal",
                "an INVALID chunked stream recovered when valid bytes followed, got code " +
                    std::to_string(static_cast<int>(r2.code())));
        }
        /* encode: writing the raw payload as a string field must be refused. */
        {
            sofab::OStream os(std::make_shared<uint8_t[]>(256), 256);
            std::string_view sv(reinterpret_cast<const char *>(nv.payload.data()), nv.payload.size());
            auto w = os.write(nv.id, sv);
            run(w.code() == sofab::Error::InvalidArgument,
                named(nv.name.c_str()), "utf8-encode-invalid-argument",
                "expected InvalidArgument, got code " + std::to_string(static_cast<int>(w.code())));
        }
#else
        ++negSkipped;
#endif
    }

    /* --- sequence-array growth (top-level "sequence_growth", §7.2 item 8). Each
     *     case is a delivery sequence of element ids against this port's own
     *     configured cap; the assertions are the container LENGTH and the
     *     OUTCOME, with no allocator instrumentation -- "which is what makes
     *     these cases portable". --- */
    std::vector<GrowthCase> growth;
    if (!loadGrowthCases(vf.root, growth, err))
    {
        std::printf("sequence_growth load failed: %s\n", err.c_str());
        return 2;
    }
    int growthRun = 0, growthSkipped = 0;
    for (const GrowthCase &c : growth)
    {
        if (c.req & ~caps) { ++growthSkipped; continue; }
        ++growthRun;
        const auto wire = growthWire(c);
        static const uint8_t goodTail[] = {0x48, 0x2a}; /* id 9, unsigned = 42 */
        const auto label = named(c.name.c_str());

        auto check = [&](sofab::Error code, size_t len,
                         const std::function<bool(size_t)> &isDefault, sofab::Error after) {
            const sofab::Error want = c.wantComplete ? sofab::Error::None : sofab::Error::LimitExceeded;
            run(code == want, label, "growth-outcome",
                "expected " + std::string(c.wantComplete ? "complete" : "limit_exceeded") +
                    ", got code " + std::to_string(static_cast<int>(code)));
            if (c.wantLength >= 0)
                run(len == static_cast<size_t>(c.wantLength), label, "growth-length",
                    "expected length " + std::to_string(c.wantLength) + ", got " + std::to_string(len));
            if (c.maxLength >= 0)
                run(len <= static_cast<size_t>(c.maxLength), label, "growth-max-length",
                    "expected length <= " + std::to_string(c.maxLength) + ", got " + std::to_string(len));
            for (long gid : c.defaultIds)
                run(gid >= 0 && static_cast<size_t>(gid) < len && isDefault(static_cast<size_t>(gid)),
                    label, "growth-gap-default",
                    "id " + std::to_string(gid) + " should hold the element default");
            if (c.terminal)
                run(after == want, label, "growth-terminal",
                    "the rejection did not survive the next feed: code " +
                        std::to_string(static_cast<int>(after)));
        };

        if (c.structElems)
        {
            sofab::IStreamObject<GrowthStructMsg> in;
            (*in).field = c.fieldId;
            const auto &rows = (*in).out;
            const sofab::Error code = in.feed(wire.data(), wire.size()).code();
            const sofab::Error after = in.feed(goodTail, sizeof goodTail).code();
            check(code, rows.size(), [&rows](size_t i) { return rows[i].a == 0; }, after);
        }
        else
        {
            sofab::IStreamObject<GrowthStringMsg> in;
            (*in).field = c.fieldId;
            const auto &tags = (*in).out;
            const sofab::Error code = in.feed(wire.data(), wire.size()).code();
            const sofab::Error after = in.feed(goodTail, sizeof goodTail).code();
            check(code, tags.size(), [&tags](size_t i) { return tags[i].empty(); }, after);
        }
    }
    /* The block exists upstream and this build runs it: a `requires` gate that
     * silently excluded every case would read as a green run of nothing. */
    run(growthRun == static_cast<int>(growth.size()) && growthRun >= 8, named("(all)"),
        "sequence-growth-ran", "ran " + std::to_string(growthRun) + " of " +
                                   std::to_string(growth.size()) + " growth cases");

    /* --- envelope guards (corelib-cpp#100) ---
     *
     * Every group above came out of ONE read and ONE parse of the vector file.
     * Two things have to stay true for that to be safe:
     *   1. the file really is read once — a second loader creeping back in is a
     *      silent duplicate of the loading prologue, which then has to be kept in
     *      step by hand;
     *   2. each walker owns its top-level key and fails loudly when it is absent,
     *      renamed or empty. The envelope is generated upstream and has grown a
     *      key before ("invalid_utf8"); a walker that quietly yields zero vectors
     *      reports as "nothing to test" and the suite still passes. */
    run(VectorFile::reads == 1, named("(all)"), "vector-file-read-once",
        "the vector file was read " + std::to_string(VectorFile::reads) +
            " times, expected exactly 1");

    {
        /* minimal, structurally valid members of either group */
        static const char kVec[] = "{\"name\":\"v\",\"fields\":[],\"serialized\":{\"hex\":\"\"}}";
        static const char kNeg[] = "{\"name\":\"n\",\"id\":0,\"string_hex\":\"ff\","
                                   "\"serialized_hex\":\"0201ff\"}";
        static const char kGro[] = "{\"name\":\"g\",\"field_id\":0,\"element_type\":\"string\","
                                   "\"deliver\":[],\"expect\":{\"outcome\":\"complete\","
                                   "\"length\":0}}";
        const std::string both  = std::string("{\"vectors\":[") + kVec +
                                  "],\"invalid_utf8\":[" + kNeg +
                                  "],\"sequence_growth\":[" + kGro + "]}";
        const std::string onlyV = std::string("{\"vectors\":[") + kVec + "]}";
        const std::string onlyN = std::string("{\"invalid_utf8\":[") + kNeg + "]}";
        const std::string onlyG = std::string("{\"sequence_growth\":[") + kGro + "]}";
        const std::string empty = "{\"vectors\":[],\"invalid_utf8\":[],\"sequence_growth\":[]}";
        const std::string drift = std::string("{\"vectors_v2\":[") + kVec +
                                  "],\"invalid_utf8_v2\":[" + kNeg +
                                  "],\"sequence_growth_v2\":[" + kGro + "]}";

        const struct { const char *label; const std::string &json; bool wantV, wantN, wantG; } cases[] = {
            {"envelope-all-groups",         both,  true,  true,  true },
            {"envelope-no-invalid_utf8",    onlyV, true,  false, false},
            {"envelope-no-vectors",         onlyN, false, true,  false},
            {"envelope-only-sequence_growth", onlyG, false, false, true },
            {"envelope-empty-groups",       empty, false, false, false},
            {"envelope-renamed-keys",       drift, false, false, false},
        };
        for (const auto &c : cases)
        {
            const EnvelopeWalk w = walkEnvelope(c.json.c_str());
            /* Accepted => the group yielded vectors; rejected => none, and the
             * walker SAID so rather than returning an empty list. */
            const bool ok = w.parsed &&
                            w.vectorsOk == c.wantV && (w.nVectors > 0) == c.wantV &&
                            w.negOk == c.wantN && (w.nNeg > 0) == c.wantN &&
                            w.growthOk == c.wantG && (w.nGrowth > 0) == c.wantG;
            run(ok, named("(all)"), c.label,
                !w.parsed ? std::string("probe envelope did not parse")
                          : "vectors ok=" + std::to_string(static_cast<int>(w.vectorsOk)) +
                                " n=" + std::to_string(w.nVectors) +
                                ", invalid_utf8 ok=" + std::to_string(static_cast<int>(w.negOk)) +
                                " n=" + std::to_string(w.nNeg) +
                                ", sequence_growth ok=" + std::to_string(static_cast<int>(w.growthOk)) +
                                " n=" + std::to_string(w.nGrowth) + "; expected ok " +
                                std::to_string(static_cast<int>(c.wantV)) + "/" +
                                std::to_string(static_cast<int>(c.wantN)) + "/" +
                                std::to_string(static_cast<int>(c.wantG)));
        }
    }

    std::printf("%zu vectors, %d run, %d skipped, %d checks, %d failures\n",
                vectors.size(), static_cast<int>(vectors.size()) - skipped, skipped, checks, failures);
    std::printf("%zu invalid_utf8 vectors, %d run, %d skipped\n", negs.size(), negRun, negSkipped);
    std::printf("%zu sequence_growth cases, %d run, %d skipped (cap %ld)\n",
                growth.size(), growthRun, growthSkipped, kGrowthCap);
    if (failures) std::printf("first failure: %s\n", first.c_str());
    if (const char *v = std::getenv("SOFAB_LIST_FAILURES"); v && *v)
        for (const auto &f : allFailures) std::printf("  FAIL %s\n", f.c_str());
    return failures ? 1 : 0;
}
