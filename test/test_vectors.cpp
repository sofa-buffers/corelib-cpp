/*!
 * @file test_vectors.cpp
 * @brief Validate the pure-C++20 implementation against the shared vectors.
 *
 * Loads assets/test_vectors.json (via the vendored JSON reader) and, for every
 * vector, drives the C++20 sofab::OStream / sofab::IStream through encode,
 * decode, roundtrip and chunked scenarios — the same conformance suite the C
 * library runs, but exercising the native C++ implementation. A vector carrying
 * `skip_ids` additionally runs the skip scenario (CORELIB_PLAN §7.2 item 7):
 * those ids are left unread at every nesting level so the decoder auto-skips
 * them, whole and again one byte at a time.
 *
 * The asserted column is `serialized`, plus `serialized_sparse` for the vectors
 * whose sparse form is pure sequence omission (§2) — the rest of the sparse
 * form needs a schema and belongs to the generator's conformance drivers. See
 * the note in loadVectors().
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab/sofab.hpp"

/* Every decoder states the receiver's field-span budget: `sofab::Limits` has no
 * default member and `IStreamObject`/`IStreamInline` have no default
 * constructor, because CORELIB_PLAN §6.2.1 forbids the codec a limit of its own
 * ("MUST NOT supply a default for one it was not given, MUST NOT read an omitted
 * argument as *unlimited*").
 *
 * A test that is not about the field-span cap states the platform's own ceiling.
 * That is a number this CALLER chose, not a mode the library offers: the check
 * still runs on every field and simply never fires. The tests that ARE about the
 * cap state a small number of their own. */
static constexpr sofab::Limits kMaxSpan{SIZE_MAX};
#include "sofab_test_json.h"

#include <algorithm>
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
    /* The `header_limits` block's gate, and a PROFILE capability like
     * CAP_DYN_ARR rather than a wire construct: a port declares it when its
     * generated code carries §6.2.1 receiver caps DISTINCT from schema bounds.
     * This corelib's read API is split into exactly that pair — readString
     * takes the declared `maxlen`, readStringCapped the receiver's
     * `max_dyn_string_len`, and neither has a default — so it does. */
    CAP_RECV_CAP = 1u << 6,
    /* Never set by buildCaps(): the bit hdrReqMask() raises for a tag this
     * reader does not know, so an unrecognised `requires` entry in the
     * `header_limits` block SKIPS the case instead of running it. */
    CAP_UNKNOWN  = 1u << 31,
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
    c |= CAP_RECV_CAP;
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
    if (!std::strcmp(s, "receiver_caps")) return CAP_RECV_CAP;
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
    std::string group;            // the vector file's own grouping, e.g. "skip/matrix"
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
        size_t gl; const char *gp = sofab_json_string(sofab_json_get(vj, "group"), &gl);
        v.group.assign(gp ? gp : "", gp ? gl : 0);
        const sofab_json_t *fields = sofab_json_get(vj, "fields");
        size_t nf = sofab_json_array_size(fields);
        for (size_t k = 0; k < nf; k++)
        {
            Op op;
            if (!loadOp(sofab_json_array_at(fields, k), op)) { err = v.name + ": bad field"; return false; }
            v.ops.push_back(std::move(op));
        }
        /* NO FIXED BOUND ANYWHERE ON THIS PATH. The C harness upstream carried a
         * fixed MAXSKIP that silently TRUNCATED an over-long skip_ids list: the
         * ids past the cap were read instead of skipped, so the vector still
         * passed while testing less than it claimed (corelib-c-cpp#160). Every
         * list here grows (std::vector) and every id is a full uint32_t, and the
         * "loader-truncation witnesses" block in main() measures that back off
         * the loaded structures so the property cannot rot unnoticed. */
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
        if (id == field) { sofab::StringSeq c{out, -1, -1, kGrowthCap, kGrowthCap}; is.read(c); }
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

/* --- header ceilings (top-level "header_limits"; CORELIB_PLAN §6.2.1 / §6.3,
 *     landed upstream as corelib-c-cpp@3aa34353be0a) -------------------------
 *
 * Bytes that DECLARE a length or a count and then END, with not one payload
 * byte behind them:
 *
 *     02 a2 06   then EOF
 *     ^^ id 0, wire type 2 (fixlen)
 *        ^^^^^ length word (100 << 3) | 2 -> a 100-byte STRING is declared
 *
 * The ceiling is decided AT THAT WORD, before the payload is asked for, so the
 * answer is the ceiling's and it is TERMINAL. `INCOMPLETE` is not merely
 * unhelpful here: §5.2.1 defines it as the outcome more bytes CAN change and
 * §5.2.4 has a streaming caller read it as "feed me the next chunk", which is a
 * false statement about the state once a ceiling has fired.
 *
 * WHICH ceiling speaks is the subject, and the two give opposite answers on the
 * identical word — §6.2.1 forbids applying a receiver cap to a field the schema
 * already bounds, so a case states one of them and never both:
 *
 *   "schema": { "maxlen": N }    -> readString / readBlob / readArray
 *                                   -> InvalidMessage (MESSAGE_SPEC §7.1)
 *   "limits": { "max_dyn_*": N } -> readStringCapped / readBlobCapped /
 *                                   readArrayCapped -> LimitExceeded (§6.2.1)
 *
 * `header_string_schema_bounded` and `header_string_over_cap` carry IDENTICAL
 * bytes and differ only in which ceiling the case configures; that pair is what
 * keeps the two categories apart, and a port that routes both into one passes
 * every other case in the block and fails exactly it.
 *
 * Unlike `sequence_growth`, the numbers here are ABSOLUTE rather than
 * cap-relative: the declared length is baked into the varint, so the case has
 * to TELL this port which ceiling to configure for its run instead of being
 * rebuilt against the port's own. `limits`/`schema` are that instruction and
 * nothing is retained past the case — this corelib holds no ceiling of its own
 * (§6.2.1), every number below is passed into the one call that compares it.
 *
 * Every rejection in the block is paired with an IN-CAP CONTROL: the same shape
 * at a length the ceiling admits, which must still answer `incomplete`. They
 * are not filler — without them a port that rejected every short read would
 * pass all six rejection cases and be badly broken. --- */

enum class HKind { Str, Blob, Arr };
enum class HCeil { Schema, Cap };
enum class HOut { LimitExceeded, Invalid, Incomplete };

struct HeaderCase
{
    std::string name;
    uint32_t req = 0;
    std::vector<std::string> reqTags;          // the `requires` names, to say WHICH gated
    uint32_t fieldId = 0;
    long declared = -1;                        // the length/count the header claims
    HKind kind{};
    HCeil ceiling{};
    long bound = 0;                            // the ceiling's number
    /* `header_limits_nested` only: the chain of SEQUENCE field ids the target
     * field is nested in, outermost first. Empty in the flat block, where the
     * field arrives in the top-level scope. */
    std::vector<uint32_t> frames;
    std::vector<uint8_t> bytes;                // `serialized`
    std::vector<std::vector<uint8_t>> chunks;  // `chunks`, or empty: feed whole
    HOut want{};
    bool terminal = false;
};

/* A base-128 varint (§4.1); false when the bytes run out or it does not end. */
bool hdrVarint(const std::vector<uint8_t> &b, size_t &at, uint64_t &out)
{
    out = 0;
    unsigned shift = 0;
    while (at < b.size())
    {
        const uint8_t byte = b[at++];
        out |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

/* What the case's own bytes say: the field id, which read the destination needs
 * and the length/count the header claims. Read off the wire rather than taken
 * from the JSON on purpose — `field_id` and `declared` are then CHECKED against
 * them in loadHeaderCases(), and a case whose bytes this reader misreads fails
 * to load instead of dispatching the wrong read. That distinction matters here:
 * a wrong read leaves the field for the decoder to skip (§7.3), and a skipped
 * field is never capped (§6.2.1) — so the whole block would report a green
 * `incomplete` while testing nothing. */
bool hdrDecode(const std::vector<uint8_t> &b, const std::vector<uint32_t> &frames,
               uint32_t &id, HKind &kind, uint64_t &declared)
{
    size_t at = 0;
    uint64_t head = 0;
    /* The nested block's bytes open one sequence per entry of `frames` before
     * the target field's own header. Walking them here is what CHECKS the chain
     * the runner is about to build against the chain the bytes actually carry:
     * a receiver wired one level too shallow would never reach the ceiling, and
     * the case would report a green `incomplete` while testing nothing. */
    for (uint32_t want : frames)
    {
        if (!hdrVarint(b, at, head)) return false;
        if ((head & 7) != 6) return false;                     /* not a sequence open */
        if (static_cast<uint32_t>(head >> 3) != want) return false;
    }
    if (!hdrVarint(b, at, head)) return false;
    id = static_cast<uint32_t>(head >> 3);
    switch (head & 7)
    {
        case 2:                       /* fixlen: the length word carries the subtype */
        {
            uint64_t word = 0;
            if (!hdrVarint(b, at, word)) return false;
            declared = word >> 3;
            if ((word & 7) == 2)      kind = HKind::Str;
            else if ((word & 7) == 3) kind = HKind::Blob;
            else return false;        /* fp32/fp64 declare no length */
            return true;
        }
        case 3: case 4: case 5:       /* count-prefixed array */
            if (!hdrVarint(b, at, declared)) return false;
            kind = HKind::Arr;
            return true;
        default:
            return false;             /* nothing else carries a header ceiling */
    }
}

/* The `header_limits` block's own gate. `requires` means SKIP there, for EVERY
 * tag — not the reduced-build rejection a *vector* gets, where an unsatisfied
 * wire-construct tag turns the vector into a negative case. These cases already
 * assert a rejection WITH A SPECIFIC CATEGORY, so a build that cannot represent
 * the construct would reject it for an unrelated reason and appear to pass
 * while testing nothing. For the same reason an UNKNOWN tag skips rather than
 * being ignored the way reqMask() ignores it for a vector: a tag this reader
 * cannot evaluate is a tag it cannot claim to satisfy. */
uint32_t hdrReqMask(const sofab_json_t *cj, std::vector<std::string> *tags = nullptr)
{
    uint32_t mask = 0;
    const sofab_json_t *req = sofab_json_get(cj, "requires");
    for (size_t k = 0, nr = sofab_json_array_size(req); k < nr; k++)
    {
        size_t tl; const char *tn = sofab_json_string(sofab_json_array_at(req, k), &tl);
        const uint32_t bit = tn ? capFromName(tn) : 0;
        mask |= bit ? bit : CAP_UNKNOWN;
        if (tags) tags->emplace_back(tn ? tn : "", tn ? tl : 0);
    }
    return mask;
}

/* Which of a gated case's own tags this build does not satisfy — so the summary
 * can NAME the reason rather than report an unexplained skip. */
std::string hdrGatedBy(const HeaderCase &c, uint32_t caps)
{
    std::string why;
    for (const std::string &t : c.reqTags)
    {
        const uint32_t bit = capFromName(t.c_str());
        if (((bit ? bit : CAP_UNKNOWN) & ~caps) == 0) continue;
        if (!why.empty()) why += ", ";
        why += t;
    }
    return why.empty() ? std::string("(unknown)") : why;
}

bool loadHeaderCases(const sofab_json_t *root, const char *key, bool wantFrames,
                     std::vector<HeaderCase> &out, std::string &err)
{
    const sofab_json_t *arr = group(root, key, err);
    if (!arr) return false;
    size_t n = sofab_json_array_size(arr);
    for (size_t i = 0; i < n; i++)
    {
        const sofab_json_t *cj = sofab_json_array_at(arr, i);
        HeaderCase c;
        size_t nl; const char *nm = sofab_json_string(sofab_json_get(cj, "name"), &nl);
        c.name.assign(nm ? nm : "", nm ? nl : 0);
        c.req = hdrReqMask(cj, &c.reqTags);
        c.fieldId = static_cast<uint32_t>(sofab_json_u64(sofab_json_get(cj, "field_id")));

        /* `frames` is the ONE key the nested block adds, and the whole of what
         * it adds. A flat case carrying one would bind its ceiling at the top
         * level while its field arrived a frame down, so the key is refused
         * there rather than ignored. */
        const sofab_json_t *fr = sofab_json_get(cj, "frames");
        if (wantFrames)
        {
            if (!fr || sofab_json_type(fr) != SOFAB_JSON_ARRAY || sofab_json_array_size(fr) == 0)
            { err = c.name + ": a nested case needs a non-empty \"frames\" chain"; return false; }
            for (size_t k = 0, nf = sofab_json_array_size(fr); k < nf; k++)
                c.frames.push_back(
                    static_cast<uint32_t>(sofab_json_u64(sofab_json_array_at(fr, k))));
        }
        else if (fr)
        { err = c.name + ": \"frames\" outside the nested block"; return false; }
        const sofab_json_t *dec = sofab_json_get(cj, "declared");
        c.declared = dec ? static_cast<long>(sofab_json_i64(dec)) : -1;

        size_t sl; const char *sh = sofab_json_string(sofab_json_get(cj, "serialized"), &sl);
        if (!sh || !hex2bin(sh, sl, c.bytes)) { err = c.name + ": bad serialized hex"; return false; }
        if (c.bytes.empty()) { err = c.name + ": empty serialized"; return false; }

        const sofab_json_t *ch = sofab_json_get(cj, "chunks");
        std::vector<uint8_t> joined;
        for (size_t k = 0, nk = sofab_json_array_size(ch); k < nk; k++)
        {
            size_t hl; const char *hx = sofab_json_string(sofab_json_array_at(ch, k), &hl);
            std::vector<uint8_t> part;
            if (!hx || !hex2bin(hx, hl, part)) { err = c.name + ": bad chunk hex"; return false; }
            joined.insert(joined.end(), part.begin(), part.end());
            c.chunks.push_back(std::move(part));
        }
        /* The chunked cases are about WHERE the feed boundaries fall, not about
         * different bytes: a `chunks` list that does not reassemble into
         * `serialized` would silently test some other message. */
        if (!c.chunks.empty() && joined != c.bytes)
        { err = c.name + ": chunks do not reassemble into serialized"; return false; }

        /* §6.2.1: "a receiver limit MUST NOT be applied to a field the schema
         * already bounds", so a case states exactly one of the two ceilings. */
        const sofab_json_t *lim = sofab_json_get(cj, "limits");
        const sofab_json_t *sch = sofab_json_get(cj, "schema");
        if (lim && sch) { err = c.name + ": states both a receiver cap and a schema bound"; return false; }
        HKind capKind = HKind::Str;
        bool capKindKnown = false;
        if (lim)
        {
            c.ceiling = HCeil::Cap;
            static const struct { const char *key; HKind kind; } kCaps[] = {
                {"max_dyn_string_len",  HKind::Str },
                {"max_dyn_blob_len",    HKind::Blob},
                {"max_dyn_array_count", HKind::Arr },
            };
            for (const auto &ck : kCaps)
                if (const sofab_json_t *v = sofab_json_get(lim, ck.key))
                {
                    if (capKindKnown) { err = c.name + ": more than one receiver cap"; return false; }
                    capKindKnown = true;
                    capKind = ck.kind;
                    c.bound = static_cast<long>(sofab_json_i64(v));
                }
            if (!capKindKnown) { err = c.name + ": \"limits\" names no cap this reader knows"; return false; }
        }
        else if (sch)
        {
            c.ceiling = HCeil::Schema;
            const sofab_json_t *ml = sofab_json_get(sch, "maxlen");
            const sofab_json_t *ct = sofab_json_get(sch, "count");
            if (ml && ct) { err = c.name + ": \"schema\" states both maxlen and count"; return false; }
            if (ml)      c.bound = static_cast<long>(sofab_json_i64(ml));
            else if (ct) c.bound = static_cast<long>(sofab_json_i64(ct));
            else { err = c.name + ": \"schema\" names no bound this reader knows"; return false; }
        }
        else { err = c.name + ": states neither \"limits\" nor \"schema\""; return false; }
        if (c.bound < 0) { err = c.name + ": the ceiling is negative"; return false; }

        uint32_t wireId = 0;
        uint64_t wireDeclared = 0;
        HKind wireKind{};
        if (!hdrDecode(c.bytes, c.frames, wireId, wireKind, wireDeclared))
        { err = c.name + ": serialized is not the frame chain plus a length/count "
                         "header this reader knows"; return false; }
        if (wireId != c.fieldId)
        { err = c.name + ": the bytes declare id " + std::to_string(wireId) +
                ", the case says field_id " + std::to_string(c.fieldId); return false; }
        if (c.declared >= 0 && wireDeclared != static_cast<uint64_t>(c.declared))
        { err = c.name + ": the header claims " + std::to_string(wireDeclared) +
                ", the case says declared " + std::to_string(c.declared); return false; }
        if (capKindKnown && wireKind != capKind)
        { err = c.name + ": the receiver cap names a different kind than the bytes declare"; return false; }
        c.kind = wireKind;

        const sofab_json_t *ex = sofab_json_get(cj, "expect");
        if (!ex) { err = c.name + ": no expect"; return false; }
        size_t ol; const char *oc = sofab_json_string(sofab_json_get(ex, "outcome"), &ol);
        if (!oc) { err = c.name + ": no expect.outcome"; return false; }
        const std::string outcome(oc, ol);
        if (outcome == "limit_exceeded") c.want = HOut::LimitExceeded;
        else if (outcome == "invalid")   c.want = HOut::Invalid;
        else if (outcome == "incomplete") c.want = HOut::Incomplete;
        else { err = c.name + ": unknown outcome " + outcome; return false; }
        if (const sofab_json_t *t = sofab_json_get(ex, "terminal")) c.terminal = sofab_json_bool(t) != 0;
        /* §5.2.1/§5.2.4 again, from the block's side: `incomplete` IS the state
         * more bytes lift, so it is never the terminal one. */
        if (c.terminal && c.want == HOut::Incomplete)
        { err = c.name + ": an incomplete outcome cannot be terminal"; return false; }
        out.push_back(std::move(c));
    }
    return true;
}

/* --- boolean tolerance (top-level "boolean_tolerant", CORELIB_PLAN §4.4) ----
 *
 * "Canonical on encode, tolerant on decode": an encoder MUST write `true` as
 * `1`, a decoder MUST read EVERY non-zero value as `true`. A boolean carries no
 * width bound at all — unlike an `enum` or a `bitfield` (MESSAGE_SPEC §1) —
 * so `256` and `2^64-1` at a boolean position are `true`, not INVALID and not a
 * truncation to `false`.
 *
 * Hand-authored, and it has to be: these bytes are produced by nobody's
 * conforming encoder, so the positive `vectors` block cannot reach this half of
 * §4.4. Each case is decode-then-RE-ENCODE, because the three defects the block
 * exists for land in three different assertions and no two of them overlap:
 *
 *   - answering INVALID for `256`          -> the outcome assertion
 *   - masking `256` to the destination
 *     width before the zero-test (`false`) -> the stored-byte assertion; the
 *                                             outcome is `complete` and looks
 *                                             perfect
 *   - storing the raw `2` unnormalised     -> the re-encode assertion; both the
 *                                             outcome and any "is it true?"
 *                                             check pass, since `2` is true
 *                                             under every truthiness test
 *
 * A runner asserting only the outcome certifies a decoder that violates §4.4 in
 * two of the three ways. */

/* The longest `expect.values` the block carries is 5; the slack is so an
 * upstream case with more elements fails to LOAD rather than being silently
 * truncated to what fits (the corelib-c-cpp#160 failure mode). */
constexpr size_t kBoolMaxElems = 16;

struct BoolCase
{
    std::string name;
    uint32_t req = 0;
    uint32_t fieldId = 0;
    std::vector<uint8_t> bytes;   // `serialized_hex`
    std::vector<uint8_t> want;    // `expect.values`, one byte per element: 0 or 1
    std::vector<uint8_t> reenc;   // `expect.reencoded_hex`
};

bool loadBoolCases(const sofab_json_t *root, std::vector<BoolCase> &out, std::string &err)
{
    const sofab_json_t *arr = group(root, "boolean_tolerant", err);
    if (!arr) return false;
    for (size_t i = 0, n = sofab_json_array_size(arr); i < n; i++)
    {
        const sofab_json_t *cj = sofab_json_array_at(arr, i);
        BoolCase c;
        size_t nl; const char *nm = sofab_json_string(sofab_json_get(cj, "name"), &nl);
        c.name.assign(nm ? nm : "", nm ? nl : 0);
        if (c.name.empty()) { err = "boolean_tolerant case with no name"; return false; }
        /* reqMask(), not hdrReqMask(): an unsatisfied tag REJECTS here (see the
         * gate in main()), and an UNRECOGNISED tag is ignored, the way the
         * corpus's reference runner ignores it — a tag this reader does not
         * know contributes nothing to the needed set, so the case runs
         * positively and every port agrees on the same behaviour when upstream
         * adds a tag. */
        c.req = reqMask(cj);
        /* Read from the case, never hardcoded: every case currently carries id
         * 0, so a hardcoded 0 is green today and writes the RE-ENCODE at the
         * wrong id the moment upstream adds a case with another. */
        const sofab_json_t *idj = sofab_json_get(cj, "id");
        if (!idj) { err = c.name + ": no id"; return false; }
        c.fieldId = static_cast<uint32_t>(sofab_json_u64(idj));

        size_t sl; const char *sh = sofab_json_string(sofab_json_get(cj, "serialized_hex"), &sl);
        if (!sh || !hex2bin(sh, sl, c.bytes)) { err = c.name + ": bad serialized_hex"; return false; }
        if (c.bytes.empty()) { err = c.name + ": empty serialized_hex"; return false; }

        const sofab_json_t *ex = sofab_json_get(cj, "expect");
        if (!ex) { err = c.name + ": no expect"; return false; }
        /* Read rather than assumed: "this block is always complete" is true
         * today, and a future case carrying anything else must fail loudly
         * instead of being silently mis-run as a positive one. */
        size_t ol; const char *oc = sofab_json_string(sofab_json_get(ex, "outcome"), &ol);
        if (!oc) { err = c.name + ": no expect.outcome"; return false; }
        if (std::string(oc, ol) != "complete")
        { err = c.name + ": unsupported expect.outcome " + std::string(oc, ol); return false; }

        const sofab_json_t *vals = sofab_json_get(ex, "values");
        if (!vals || sofab_json_type(vals) != SOFAB_JSON_ARRAY)
        { err = c.name + ": no expect.values array"; return false; }
        const size_t nv = sofab_json_array_size(vals);
        if (nv == 0) { err = c.name + ": expect.values is empty"; return false; }
        if (nv > kBoolMaxElems)
        { err = c.name + ": " + std::to_string(nv) + " elements exceeds this runner's destination"; return false; }
        for (size_t k = 0; k < nv; k++)
        {
            const sofab_json_t *bv = sofab_json_array_at(vals, k);
            /* A JSON boolean, by construction — so the comparison downstream is
             * against a real `true`/`false` and never against a coerced number. */
            if (sofab_json_type(bv) != SOFAB_JSON_BOOL)
            { err = c.name + ": expect.values element " + std::to_string(k) + " is not a JSON boolean"; return false; }
            c.want.push_back(sofab_json_bool(bv) ? 1u : 0u);
        }

        size_t rl; const char *rh = sofab_json_string(sofab_json_get(ex, "reencoded_hex"), &rl);
        if (!rh || !hex2bin(rh, rl, c.reenc)) { err = c.name + ": bad expect.reencoded_hex"; return false; }
        if (c.reenc.empty()) { err = c.name + ": empty expect.reencoded_hex"; return false; }
        out.push_back(std::move(c));
    }
    return true;
}

/* The destination for one boolean case.
 *
 * `dst` is a real `bool` array — the port's natural boolean destination, which
 * is the storage under test — and this runner NEVER reads it as `bool`. In C++
 * a `bool` object may only hold the representations of `false` and `true`; an
 * object holding `2` has no value, so comparing it against `true` cannot tell
 * you whether the decoder normalised, the comparison itself being meaningless.
 * The bytes are inspected through a `memcpy` into `unsigned char` instead, which
 * is what turns "the object ends up holding a representation it is ALLOWED to
 * have" into an assertion. */
struct BoolMsg : sofab::IStreamMessage
{
    uint32_t fieldId = 0;
    size_t n = 1;                    /* elements expected: 1 == scalar */
    bool dst[kBoolMaxElems];         /* poisoned by the caller before the feed */
    int others = 0;                  /* fields delivered at some other id */
    int delivered = 0;               /* deliveries of the case's own field */
    size_t announced = 0;            /* the array header's element count */

    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (id != fieldId) { ++others; return; }
        ++delivered;
        /* The BOOLEAN read surface, chosen by the case's element count — not the
         * unsigned reader, which would test nothing: `2` is a perfectly ordinary
         * unsigned value and normalisation is exactly what the boolean surface
         * adds. Nothing is asserted in here (an assertion thrown from inside a
         * deliver callback can be swallowed or leave the stream in a state that
         * masks the failure); what the callback saw is recorded and checked
         * after feed() returns. */
        if (n == 1)
        {
            (void)is.read(dst[0]);
            announced = 1;
        }
        else
        {
            announced = is.announcedCount();
            std::span<bool> sp{dst, n};
            (void)is.read(sp);
        }
    }
};

/* The destination for one header case: the case's own field, read under the
 * ceiling the case configures, plus a count of every OTHER field delivered —
 * which is what turns "a further feed re-raises rather than CONSUMING" into an
 * assertion instead of a re-check of the same code. */
struct HeaderMsg : sofab::IStreamMessage
{
    const HeaderCase *hc = nullptr;
    /* >= 0: read under THIS ceiling instead of the case's. The negative control
     * (see the nested block below) is the only caller that sets it, and it
     * lifts the same KIND of ceiling the case states — a schema case keeps
     * going through readString, a cap case through readStringCapped — because
     * lifting the other one would leave the rejection in place and prove
     * nothing. */
    long lifted = -1;
    int others = 0;
    std::string text;
    std::vector<uint8_t> blob;
    std::vector<uint64_t> nums;

    /* Nothing was materialised for the field: §6.2.1 is "rejected, never
     * clamped", so a ceiling that fired must leave the destination untouched. */
    [[nodiscard]] bool untouched() const { return text.empty() && blob.empty() && nums.empty(); }

    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t, size_t) noexcept override
    {
        if (!hc || id != hc->fieldId) { ++others; return; }
        const bool capped = hc->ceiling == HCeil::Cap;
        const long ceiling = lifted >= 0 ? lifted : hc->bound;
        switch (hc->kind)
        {
            case HKind::Str:
                if (capped) (void)sofab::readStringCapped(is, text, ceiling);
                else        (void)sofab::readString(is, text, ceiling);
                break;
            case HKind::Blob:
                if (capped) (void)sofab::readBlobCapped(is, blob, ceiling);
                else        (void)sofab::readBlob(is, blob, ceiling);
                break;
            case HKind::Arr:
                if (capped) (void)sofab::readArrayCapped(is, nums, ceiling);
                else        (void)sofab::readArray(is, nums, ceiling);
                break;
        }
    }
};

/* --- the same ceiling one or two frames deeper ("header_limits_nested") ------
 *
 * Every case of the flat block puts its field at the top level, which leaves
 * one axis untested: the IDENTICAL over-ceiling header delivered INSIDE an open
 * sequence. This block is that axis and only that axis — same keys, same
 * outcome vocabulary, same terminality rule, same pairing of each rejection
 * with an in-cap control — plus `frames`, the chain of sequence field ids the
 * target field is nested in.
 *
 *     3e 1e 02 a2 06   then EOF
 *     ^^ id 7, wire type 6: open a sequence          frames[0]
 *        ^^ id 3, wire type 6: open another          frames[1]
 *           ^^^^^^^^ the flat block's own bytes, now two frames down
 *
 * Depth is its own axis because a port can carry the SCHEMA bound into a
 * sequence and leave the RECEIVER CAP bound at the top level, where these bytes
 * never reach it — and then answer `incomplete` for a reason that looks
 * entirely plausible, because a frame really is open.
 *
 * That plausibility is why the negative control below is load-bearing here and
 * not merely good practice: these cases end at end-of-input with one or two
 * frames unclosed, so `incomplete` has a SECOND, independent justification. A
 * port that rejected for some unrelated reason — a depth guard, a refusal of
 * unclosed frames — would pass the forward pass without ever consulting the
 * ceiling under test. Only lifting the ceiling and watching the answer change
 * tells the two apart. --- */

/* One link of the frame chain. At depth d it binds the sequence the case names
 * at frames[d] and descends; at the innermost depth it delegates to the FLAT
 * block's leaf, unchanged. Sharing that leaf is the point: the two blocks are
 * required to differ in where the field arrives and in nothing else, and a
 * nested leaf of its own could pass by a mechanism the flat one never uses.
 *
 * A resumed delivery (§6.6.2, a chunk boundary inside the sequence) needs no
 * special case: the decoder replays the open levels' field ids, so dispatching
 * on the id re-enters exactly the same link. */
struct NestedFrame : sofab::IStreamMessage
{
    const std::vector<uint32_t> *frames = nullptr;
    size_t depth = 0;
    NestedFrame *child = nullptr;
    HeaderMsg *leaf = nullptr;

    void deserialize(sofab::IStreamImpl &is, sofab::id id, size_t size, size_t count) noexcept override
    {
        if (frames && depth < frames->size())
        {
            if (id == static_cast<sofab::id>((*frames)[depth]) && child) is.read(*child);
            return;                       /* anything else at this depth: let it skip */
        }
        if (leaf) leaf->deserialize(is, id, size, count);
    }
};

/* The whole receiver for one nested case: the stream, the chain built to the
 * case's exact depth (outermost first), and the shared leaf at the bottom. */
struct NestedRig
{
    sofab::IStreamObject<NestedFrame> in{kMaxSpan};
    HeaderMsg leaf;
    std::vector<NestedFrame> deeper;   // depths 1..n; depth 0 is the stream's own handler
    std::vector<uint32_t> frames;

    NestedRig(const HeaderCase &c, long lifted)
        : deeper(c.frames.size()), frames(c.frames)
    {
        leaf.hc = &c;
        leaf.lifted = lifted;
        (*in).frames = &frames;
        (*in).depth = 0;
        (*in).leaf = &leaf;
        (*in).child = deeper.empty() ? nullptr : &deeper[0];
        for (size_t d = 0; d < deeper.size(); d++)
        {
            deeper[d].frames = &frames;
            deeper[d].depth = d + 1;
            deeper[d].leaf = &leaf;
            deeper[d].child = (d + 1 < deeper.size()) ? &deeper[d + 1] : nullptr;
        }
    }

    sofab::Error feed(const std::vector<uint8_t> &b)
    {
        return in.feed(b.data(), b.size()).code();
    }
};

sofab::Error hdrWant(HOut o)
{
    return o == HOut::LimitExceeded ? sofab::Error::LimitExceeded
         : o == HOut::Invalid       ? sofab::Error::InvalidMessage
                                    : sofab::Error::Incomplete;
}

const char *hdrWantName(HOut o)
{
    return o == HOut::LimitExceeded ? "limit_exceeded"
         : o == HOut::Invalid       ? "invalid"
                                    : "incomplete";
}

/* --- envelope-drift guard (corelib-cpp#100) ---------------------------------
 *
 * Every group walker now shares ONE parse, and each demands its own top-level
 * key. Feed them a doctored envelope in memory and report, per group, the
 * walker's VERDICT and how many vectors it produced — kept apart on purpose: a
 * key the walker no longer recognises must be REJECTED, not silently walked as
 * an empty (== "nothing to test") list, and those two look the same if only the
 * count is inspected. */
struct EnvelopeWalk
{
    bool parsed = false;
    bool vectorsOk = false, negOk = false, growthOk = false, headerOk = false;
    bool nestedOk = false, boolOk = false;
    size_t nVectors = 0, nNeg = 0, nGrowth = 0, nHeader = 0, nNested = 0, nBool = 0;
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
    std::vector<HeaderCase> hs;
    std::vector<HeaderCase> hns;
    std::vector<BoolCase> bs;
    std::string e;
    w.vectorsOk = loadVectors(root, vs, e);
    w.negOk = loadNegVectors(root, ns, e);
    w.growthOk = loadGrowthCases(root, gs, e);
    w.headerOk = loadHeaderCases(root, "header_limits", false, hs, e);
    w.nestedOk = loadHeaderCases(root, "header_limits_nested", true, hns, e);
    w.boolOk = loadBoolCases(root, bs, e);
    w.nVectors = vs.size();
    w.nNeg = ns.size();
    w.nGrowth = gs.size();
    w.nHeader = hs.size();
    w.nNested = hns.size();
    w.nBool = bs.size();
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
        sofab::read(is, s);
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
        /* Re-entering a sequence the previous chunk left open (CORELIB_PLAN §6.6.2:
         * the decoder resumes rather than re-parses, and rebuilds the handler chain
         * by replaying the open levels' field ids). Nothing here is new — the op
         * cursor is already inside the sequence — so descend without touching it.
         * A generated `deserialize`, which only dispatches on the id, needs none of
         * this; this verifier keeps a position of its own. */
        if (is.resumed())
        {
            GenericMsg child;
            child.cur = cur;
            is.read(child);
            return;
        }
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
            /* The payload destinations are `static` on purpose: a field split
             * across chunks is delivered once per chunk that carries part of it and
             * written into the caller's destination as the pieces arrive
             * (CORELIB_PLAN §6.6.2), so a destination that dies with the delivery
             * would lose the earlier halves. Only one field is ever in progress, so
             * one scratch each is enough. A delivery that did not complete the field
             * is discarded wholesale by `rewind` above. */
            case K::Str: { static std::string x; sofab::read(is, x); if (x != op.str) bad("string"); break; }
            case K::Blob:
            {
                static std::vector<uint8_t> buf;
                if (is.progress() == 0) buf.assign(op.blob.size() + 1, 0);
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
                auto cmpU = [&](auto &vec) {
                    if (is.progress() == 0) vec.assign(op.au.size(), 0);
                    is.read(vec);
                    for (size_t k = 0; k < vec.size(); k++)
                        if (static_cast<uint64_t>(vec[k]) != op.au[k]) { bad("arr-u"); break; }
                };
                auto cmpI = [&](auto &vec) {
                    if (is.progress() == 0) vec.assign(op.ai.size(), 0);
                    is.read(vec);
                    for (size_t k = 0; k < vec.size(); k++)
                        if (static_cast<int64_t>(vec[k]) != op.ai[k]) { bad("arr-i"); break; }
                };
                /* Stable across deliveries, like the payload scratch above. */
                static std::vector<uint8_t>  a8;   static std::vector<uint16_t> a16;
                static std::vector<uint32_t> a32;  static std::vector<uint64_t> a64;
                static std::vector<int8_t>   s8;   static std::vector<int16_t>  s16;
                static std::vector<int32_t>  s32;  static std::vector<int64_t>  s64;
                static std::vector<float>    af32; static std::vector<double>   af64;
                switch (op.elem)
                {
                    case E::U8:  cmpU(a8);  break;
                    case E::U16: cmpU(a16); break;
                    case E::U32: cmpU(a32); break;
                    case E::U64: cmpU(a64); break;
                    case E::I8:  cmpI(s8);  break;
                    case E::I16: cmpI(s16); break;
                    case E::I32: cmpI(s32); break;
                    case E::I64: cmpI(s64); break;
                    case E::F32:
                        if (is.progress() == 0) af32.assign(op.af.size(), 0.0f);
                        is.read(af32);
                        for (size_t k = 0; k < af32.size(); k++)
                            if (!eq32(af32[k], static_cast<float>(op.af[k]))) { bad("arr-f32"); break; }
                        break;
                    case E::F64:
                        if (is.progress() == 0) af64.assign(op.af.size(), 0.0);
                        is.read(af64);
                        for (size_t k = 0; k < af64.size(); k++)
                            if (!eq64(af64[k], op.af[k])) { bad("arr-f64"); break; }
                        break;
                }
                break;
            }
            case K::SeqB:
            {
                /* A sequence's delivery is never "undone": its children were
                 * delivered and stay delivered, and only a child that did not
                 * finish rewinds (its own Rewind above). Leaving this one armed
                 * would put the op cursor back on the SequenceStart, where the
                 * resumed child would then be compared against the wrong op. */
                rewind.armed = false;
                GenericMsg child; child.cur = cur; is.read(child); break;
            }
            case K::SeqE: break;
        }
    }
};

bool decode(const Vector &v, bool oneByte, std::string &err, const std::vector<uint32_t> *skip = nullptr)
{
    Cursor cur; cur.ops = &v.ops; cur.skip = skip;
    sofab::IStreamObject<GenericMsg> in{kMaxSpan};
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

    /* How much of the skip scenario (CORELIB_PLAN §7.2 item 7) actually ran.
     * A vector carrying `skip_ids` either runs it — twice, whole and one byte at
     * a time — or is gated out by `requires`; counting both halves is what lets
     * the summary state which of the two happened to each of them. Without the
     * accounting a matrix that quietly stopped running (a `requires` tag this
     * port stopped reporting, an upstream rename) would read as a green run of
     * nothing, exactly the failure the envelope guards below exist for. */
    int skipVectors = 0, skipRan = 0, skipGated = 0;
    int skipMatrixVectors = 0, skipAxisVectors = 0;
    for (const Vector &v : vectors)
    {
        if (!v.skip.empty()) ++skipVectors;
        if (v.group == "skip/matrix") ++skipMatrixVectors;
        else if (v.group == "skip") ++skipAxisVectors;
    }

    const size_t tinies[] = {1, 3, 7};
    for (const Vector &v : vectors)
    {
        /* skip vectors needing a feature this build was compiled without */
        if (v.req & ~caps) { ++skipped; if (!v.skip.empty()) ++skipGated; continue; }

        std::string d;
        run(encode(v, 0, d), v, "encode", d);
        for (size_t t : tinies) { std::string e; run(encode(v, t, e), v, "chunked-encode", e); }
        std::string d2; run(decode(v, false, d2), v, "decode", d2);
        std::string d3; run(decode(v, true, d3), v, "chunked-decode", d3);
        if (!v.skip.empty())
        {
            ++skipRan;
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

    /* --- the skip scenario is not vacuous (CORELIB_PLAN §7.2 item 7) ---------
     *
     * The skip cases are the one scenario whose SIZE is data: a vector runs it
     * only because it carries `skip_ids`, so a file, a loader or a `requires`
     * gate that stops producing them makes the scenario disappear without a
     * single check turning red. The counts below are what upstream's
     * regenerated file (corelib-c-cpp@f2b3d72) carries — a full cross product of
     * the ten skippable constructs plus the axes beside it — and they are lower
     * bounds, so upstream adding cases stays green while losing them does not.
     *
     * Every vector with `skip_ids` must be ACCOUNTED for: either it ran the
     * scenario (dense and one-byte-chunked) or its `requires` named a capability
     * this build lacks. Only the two together sum to the total. */
    run(skipRan + skipGated == skipVectors, named("(all)"), "skip-vectors-accounted",
        "of " + std::to_string(skipVectors) + " vectors carrying skip_ids, " +
            std::to_string(skipRan) + " ran and " + std::to_string(skipGated) +
            " were gated out by requires");
    run(skipVectors >= 58, named("(all)"), "skip-vectors-present",
        "expected at least 58 vectors carrying skip_ids, saw " + std::to_string(skipVectors));
    run(skipMatrixVectors >= 36, named("(all)"), "skip-matrix-present",
        "expected at least 36 \"skip/matrix\" vectors (the (read, skipped) cross "
        "product), saw " + std::to_string(skipMatrixVectors));
    run(skipAxisVectors >= 16, named("(all)"), "skip-axes-present",
        "expected at least 16 \"skip\" vectors (empty/long payloads, fp64 element "
        "length, wide ids, message edges), saw " + std::to_string(skipAxisVectors));

    /* --- loader-truncation witnesses ----------------------------------------
     *
     * "No fixed-size cap silently truncates" is a property of code that is not
     * there, and those rot without a sound: upstream's harness held a fixed
     * MAXSKIP that dropped the ids past it, and the vectors kept passing while
     * skipping fewer fields than they named (corelib-c-cpp#160). Nothing on this
     * loader's path is fixed-size, so the check has to come from the other end —
     * measure the largest thing that survived the load and compare it against
     * the sizes the current file actually needs. A cap creeping into loadOp() or
     * loadVectors() lowers one of these and fails loudly, which is the behaviour
     * §7.1 asks of a statically bounded profile too: refuse, never test less. */
    {
        size_t maxSkipIds = 0, maxArrayElems = 0, maxPayload = 0;
        uint32_t maxFieldId = 0, maxSkipId = 0;
        int skippedFp64Arrays = 0;
        for (const Vector &v : vectors)
        {
            maxSkipIds = std::max(maxSkipIds, v.skip.size());
            for (uint32_t id : v.skip) maxSkipId = std::max(maxSkipId, id);
            for (const Op &op : v.ops)
            {
                maxFieldId = std::max(maxFieldId, op.id);
                if (op.kind == K::Str)  maxPayload = std::max(maxPayload, op.str.size());
                if (op.kind == K::Blob) maxPayload = std::max(maxPayload, op.blob.size());
                if (op.kind == K::Arr)
                {
                    maxArrayElems = std::max({maxArrayElems, op.au.size(), op.ai.size(), op.af.size()});
                    if (op.elem == E::F64 &&
                        std::find(v.skip.begin(), v.skip.end(), op.id) != v.skip.end())
                        ++skippedFp64Arrays;
                }
            }
        }
        const struct { const char *label; bool ok; std::string detail; } witnesses[] = {
            {"loader-keeps-long-skip-lists", maxSkipIds >= 9,
             "longest skip_ids list loaded: " + std::to_string(maxSkipIds) + ", expected >= 9"},
            {"loader-keeps-wide-skip-ids", maxSkipId >= 100000,
             "largest skipped id loaded: " + std::to_string(maxSkipId) + ", expected >= 100000"},
            {"loader-keeps-wide-field-ids", maxFieldId >= 100001,
             "largest field id loaded: " + std::to_string(maxFieldId) + ", expected >= 100001"},
            {"loader-keeps-long-arrays", maxArrayElems >= 130,
             "longest array loaded: " + std::to_string(maxArrayElems) + " elements, expected >= 130"},
            {"loader-keeps-long-payloads", maxPayload >= 130,
             "longest string/blob payload loaded: " + std::to_string(maxPayload) +
                 " bytes, expected >= 130"},
            {"loader-keeps-skipped-fp64-arrays", skippedFp64Arrays >= 1,
             "no skipped fp64 array survived the load; the element length read from "
             "the fixlen_word (8, not 4) would go untested"},
        };
        for (const auto &w : witnesses) run(w.ok, named("(all)"), w.label, w.detail);
    }

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
            sofab::IStreamObject<NegReadMsg> in{kMaxSpan};
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
            sofab::IStreamObject<NegReadMsg> in{kMaxSpan};
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
            sofab::IStreamObject<GrowthStructMsg> in{kMaxSpan};
            (*in).field = c.fieldId;
            const auto &rows = (*in).out;
            const sofab::Error code = in.feed(wire.data(), wire.size()).code();
            const sofab::Error after = in.feed(goodTail, sizeof goodTail).code();
            check(code, rows.size(), [&rows](size_t i) { return rows[i].a == 0; }, after);
        }
        else
        {
            sofab::IStreamObject<GrowthStringMsg> in{kMaxSpan};
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

    /* --- header ceilings (top-level "header_limits", §6.2.1/§6.3). Each case
     *     is a fixed byte string that DECLARES a length or count and then ends,
     *     fed under the ceiling the case itself names. --- */
    std::vector<HeaderCase> headers;
    if (!loadHeaderCases(vf.root, "header_limits", false, headers, err))
    {
        std::printf("header_limits load failed: %s\n", err.c_str());
        return 2;
    }
    int headerRun = 0, headerSkipped = 0, headerRejects = 0;
    for (const HeaderCase &c : headers)
    {
        if (c.req & ~caps) { ++headerSkipped; continue; }   /* SKIP, for every tag */
        ++headerRun;
        const auto label = named(c.name.c_str());
        const sofab::Error want = hdrWant(c.want);
        const char *wantName = hdrWantName(c.want);

        sofab::IStreamObject<HeaderMsg> in{kMaxSpan};
        (*in).hc = &c;
        /* `chunks`, where the case carries one, divides the length varint
         * itself: the verdict is a property of the BYTES, not of where the feed
         * boundaries fell (§7.2 item 4), so it is read off the last feed. */
        sofab::Error code = sofab::Error::None;
        if (c.chunks.empty())
            code = in.feed(c.bytes.data(), c.bytes.size()).code();
        else
            for (const std::vector<uint8_t> &part : c.chunks)
                code = in.feed(part.data(), part.size()).code();
        run(code == want, label, "header-outcome",
            std::string("expected ") + wantName + ", got code " +
                std::to_string(static_cast<int>(code)));

        if (c.want == HOut::Incomplete) continue;
        ++headerRejects;
        /* ARCHITECTURE §9.5: "a claimed oversize fails fast even if the payload
         * never arrives" — the ceiling answered at the word, before the
         * destination was sized, so nothing was materialised for it. */
        run((*in).untouched(), label,
            "header-nothing-materialised",
            "the rejected header still left a value in the destination");
        if (c.terminal)
        {
            static const uint8_t goodTail[] = {0x48, 0x2a};   /* id 9, unsigned = 42 */
            const int othersBefore = (*in).others;
            const sofab::Error after = in.feed(goodTail, sizeof goodTail).code();
            run(after == want, label, "header-terminal",
                "the rejection did not survive the next feed: code " +
                    std::to_string(static_cast<int>(after)));
            /* "re-raises rather than CONSUMING": the perfectly good field behind
             * the rejection is not delivered either (§6.3 terminal). */
            run((*in).others == othersBefore, label, "header-terminal-consumes-nothing",
                "a field was delivered after the terminal rejection");
        }
    }
    /* Same guard as the growth block's, plus the one the block's own README
     * insists on: every rejection is paired with an IN-CAP CONTROL that must
     * still answer `incomplete`, so a run of nothing but rejections would mean
     * the controls stopped loading and the block proves nothing. */
    run(headerRun == static_cast<int>(headers.size()) && headerRun >= 10, named("(all)"),
        "header-limits-ran", "ran " + std::to_string(headerRun) + " of " +
                                 std::to_string(headers.size()) + " header_limits cases");
    run(headerRejects > 0 && headerRun - headerRejects > 0, named("(all)"),
        "header-limits-controls", "ran " + std::to_string(headerRejects) +
            " rejection cases and " + std::to_string(headerRun - headerRejects) +
            " in-cap controls; the block needs both");

    /* --- header ceilings one and two frames deeper (top-level
     *     "header_limits_nested", §6.2.1/§6.3). The same bytes the flat block
     *     feeds, delivered INSIDE the sequence chain the case names, against a
     *     receiver built to exactly that depth. See the NestedFrame note. --- */
    std::vector<HeaderCase> nested;
    if (!loadHeaderCases(vf.root, "header_limits_nested", true, nested, err))
    {
        std::printf("header_limits_nested load failed: %s\n", err.c_str());
        return 2;
    }
    int nestedRan = 0, nestedGated = 0, nestedRejects = 0, nestedDeep = 0;
    std::vector<std::string> nestedGatedBy;
    for (const HeaderCase &c : nested)
    {
        if (c.req & ~caps)                              /* SKIP, for every tag */
        {
            ++nestedGated;
            nestedGatedBy.push_back(c.name + " (" + hdrGatedBy(c, caps) + ")");
            continue;
        }
        ++nestedRan;
        if (c.frames.size() >= 2) ++nestedDeep;
        const auto label = named(c.name.c_str());
        const sofab::Error want = hdrWant(c.want);
        const char *wantName = hdrWantName(c.want);

        NestedRig rig{c, /*lifted=*/-1};
        sofab::Error code = sofab::Error::None;
        if (c.chunks.empty())
            code = rig.feed(c.bytes);
        else
        {
            /* No case in this block carries `chunks` today; the two blocks share
             * a key set, so it is honoured anyway — and every feed BEFORE the
             * last must answer `incomplete`, since an earlier verdict would mean
             * the decoder answered on bytes it had not seen. */
            for (size_t k = 0; k < c.chunks.size(); k++)
            {
                code = rig.feed(c.chunks[k]);
                if (k + 1 < c.chunks.size())
                    run(code == sofab::Error::Incomplete, label, "nested-chunk-incomplete",
                        "chunk " + std::to_string(k) + " answered before the last feed: code " +
                            std::to_string(static_cast<int>(code)));
            }
        }
        run(code == want, label, "nested-outcome",
            std::string("expected ") + wantName + ", got code " +
                std::to_string(static_cast<int>(code)));

        if (c.want == HOut::Incomplete) continue;       /* more bytes may still lift it */
        ++nestedRejects;
        run(rig.leaf.untouched(), label, "nested-nothing-materialised",
            "the rejected header still left a value in the destination");
        if (c.terminal)
        {
            /* The payload the header promised: bytes that WOULD complete the
             * field if anything could. Asking the stream what its last error was
             * would not distinguish a decoder that consumes them and moves on. */
            const std::vector<uint8_t> more(8, 0x61);
            const int othersBefore = rig.leaf.others;
            const sofab::Error after = rig.in.feed(more.data(), more.size()).code();
            run(after == want, label, "nested-terminal",
                "the rejection did not survive the next feed: code " +
                    std::to_string(static_cast<int>(after)));
            run(rig.leaf.others == othersBefore, label, "nested-terminal-consumes-nothing",
                "a field was delivered after the terminal rejection");
            /* Checked AFTER the second feed, so a LATE materialisation is caught
             * too — clamping that also reports the error passes every other
             * assertion here. */
            run(rig.leaf.untouched(), label, "nested-terminal-still-empty",
                "a value appeared in the destination after the terminal rejection");
        }
    }

    /* THE NEGATIVE CONTROL, and the reason this block can claim to test
     * anything. A second, independent pass with the ceiling LIFTED: the
     * rejection must go away. If it does not, the bytes were refused by
     * something other than the ceiling — the open frames at end-of-input are an
     * entirely sufficient reason to answer `incomplete`, and a depth guard or a
     * strict-mode path would produce a rejection that looks identical. Assert
     * only that the answer CHANGED, not what it changed to: what is being
     * proved is that the ceiling caused it. */
    {
        constexpr long kLifted = 1 << 16;   /* far above every `declared` here */
        int controlled = 0;
        for (const HeaderCase &c : nested)
        {
            if (c.req & ~caps) continue;
            if (c.want == HOut::Incomplete) continue;   /* only a rejection can be lifted */
            const auto label = named(c.name.c_str());
            /* Lift the same KIND the case states: NestedRig routes `lifted`
             * through the same capped/bounded read the forward pass used, so a
             * schema case gets a lifted schema bound and a cap case a lifted
             * receiver cap. Lifting the other one would change nothing. */
            NestedRig rig{c, kLifted};
            sofab::Error code = sofab::Error::None;
            if (c.chunks.empty()) code = rig.feed(c.bytes);
            else for (const std::vector<uint8_t> &part : c.chunks) code = rig.feed(part);
            ++controlled;
            run(code != hdrWant(c.want), label, "nested-control-ceiling-caused-it",
                std::string("with the ceiling lifted to ") + std::to_string(kLifted) +
                    " the answer is still " + hdrWantName(c.want) +
                    ", so the rejection did not come from the ceiling");
        }
        /* A control loop that examined nothing is green and proves nothing, so
         * the count is asserted — against the number the FORWARD pass counted,
         * which is computed independently of this loop, and against the absolute
         * four when nothing was gated. Every rejection in this block is
         * reachable with a lifted ceiling: unlike the flat block's 1 GiB
         * amplification case, which is exactly the allocation §6.2.1 exists to
         * prevent, none of these declares a size too large to admit, so the
         * control has no exemption and covers all of them. */
        run(controlled == nestedRejects && controlled > 0, named("(all)"),
            "nested-control-count", "the control checked " + std::to_string(controlled) +
                " cases, the forward pass saw " + std::to_string(nestedRejects) +
                " rejections");
        run(nestedGated > 0 || controlled == 4, named("(all)"), "nested-control-count-full",
            "nothing was gated, so the control must cover all four rejections; it checked " +
                std::to_string(controlled));
    }

    run(nestedRan + nestedGated == static_cast<int>(nested.size()), named("(all)"),
        "nested-ran-plus-gated", "ran " + std::to_string(nestedRan) + " + gated " +
            std::to_string(nestedGated) + " != " + std::to_string(nested.size()) + " cases");
    /* This build compiles nothing out and carries §6.2.1 receiver caps, so it
     * runs the block whole; a port that legitimately gates must stay
     * distinguishable in the output from one whose capability probe broke. */
    run(nestedGated == 0 && nestedRan == static_cast<int>(nested.size()), named("(all)"),
        "nested-ran", "ran " + std::to_string(nestedRan) + " of " +
            std::to_string(nested.size()) + " header_limits_nested cases");
    run(nestedRejects > 0 && nestedRan - nestedRejects > 0, named("(all)"),
        "nested-controls", "ran " + std::to_string(nestedRejects) +
            " rejection cases and " + std::to_string(nestedRan - nestedRejects) +
            " in-cap controls; the block needs both");
    /* Depth 2 exists because one level may be special-cased — a chain builder
     * off by one descends once and then treats the INNER sequence header as the
     * target field. A run that only ever saw frames of length 1 would not catch
     * that, so its presence is asserted rather than assumed. */
    run(nestedDeep > 0, named("(all)"), "nested-depth2-ran",
        "no case with two frames ran; depth 2 is a separate axis from depth 1");

    /* --- boolean tolerance (top-level "boolean_tolerant", CORELIB_PLAN §4.4).
     *     Bytes nobody's conforming encoder emits, decoded through the BOOLEAN
     *     read surface and then written back out through the boolean WRITE
     *     surface: "tolerant on decode" and "canonical on encode" are one rule
     *     and neither half is observable without the other. --- */
    std::vector<BoolCase> bools;
    if (!loadBoolCases(vf.root, bools, err))
    {
        std::printf("boolean_tolerant load failed: %s\n", err.c_str());
        return 2;
    }
    int boolDecoded = 0, boolRejected = 0, boolChecks = 0;
    for (const BoolCase &c : bools)
    {
        const auto label = named(c.name.c_str());
        const size_t n = c.want.size();

        /* §4.4 lifts the width bound the TYPE carries, never the one a BUILD
         * has: under a narrowed accumulator a boolean carrying 2^64-1 overflows
         * before any boolean rule can apply, and CORELIB_PLAN §6.2 makes that
         * INVALID (§5.2.2). So an unsatisfied tag REJECTS here — it does not
         * skip, the way it does for the `header_limits` block. Skipping would
         * assert nothing at all, leaving the truncation this block exists to
         * catch untested in exactly the build most likely to have it.
         *
         * This pure-C++20 build sets every capability bit (buildCaps()), so the
         * branch below is unreachable today and `rejected` is always 0. It is
         * here so a feature-reduced profile — should one ever be introduced —
         * needs no new code, and so the gate can never be mistaken for the
         * unconditional skip of the tagged cases that would silently drop the
         * array and 64-bit halves of the rule. */
        if (c.req & ~caps)
        {
            ++boolRejected;
            /* A handler binding nothing: what is asserted is the VERDICT, not
             * which fields arrived before the offending one. */
            sofab::IStreamObject<BoolMsg> in{kMaxSpan};
            (*in).fieldId = c.fieldId + 1;   /* never the case's own id */
            const auto r = in.feed(c.bytes.data(), c.bytes.size());
            static const uint8_t goodTail[] = {0x00};
            const sofab::Error after = in.feed(goodTail, sizeof goodTail).code();
            ++boolChecks;
            run(r.status() == sofab::DecodeStatus::Invalid &&
                    r.code() == sofab::Error::InvalidMessage && after == sofab::Error::InvalidMessage,
                label, "boolean-gated-invalid",
                "a `requires` tag this build does not satisfy must make the message INVALID "
                "and stay INVALID; got status " + std::to_string(static_cast<int>(r.status())) +
                    ", code " + std::to_string(static_cast<int>(r.code())) +
                    ", after one more byte " + std::to_string(static_cast<int>(after)));
            continue;
        }

        ++boolDecoded;
        /* Whole, and again one byte at a time. The byte-at-a-time run is what
         * puts the ten-byte varints of `boolean_tolerant_u64_max` and
         * `boolean_tolerant_array_u64_max` across feed boundaries, so a value
         * accumulator that loses its carry there cannot pass by arriving in one
         * piece; §7.2 item 4 makes the two results identical. */
        for (bool oneByte : {false, true})
        {
            const char *how = oneByte ? "-chunked" : "";
            /* A FRESH decoder and a fresh, poisoned destination per run: a
             * terminal verdict or leftover parser state from the previous case
             * must not reach this one, and a retained destination would mask a
             * decoder that never wrote. */
            sofab::IStreamObject<BoolMsg> in{kMaxSpan};
            BoolMsg &m = *in;
            m.fieldId = c.fieldId;
            m.n = n;
            /* §8.4: neither the expected result nor a valid `bool`
             * representation. Without it a decoder that never writes the
             * destination at all passes `boolean_tolerant_zero` against zeroed
             * storage and the case proves nothing. */
            std::memset(m.dst, 0xAA, sizeof m.dst);

            sofab::DecodeStatus status = sofab::DecodeStatus::Complete;
            sofab::Error code = sofab::Error::None;
            if (oneByte)
                for (uint8_t b : c.bytes)
                {
                    const auto rr = in.feed(&b, 1);
                    status = rr.status();
                    code = rr.code();
                }
            else
            {
                const auto rr = in.feed(c.bytes.data(), c.bytes.size());
                status = rr.status();
                code = rr.code();
            }
            ++boolChecks;
            run(status == sofab::DecodeStatus::Complete && code == sofab::Error::None &&
                    m.delivered >= 1 && m.others == 0,
                label, (std::string("boolean-decode-complete") + how).c_str(),
                "expected complete with the case's own field delivered; got status " +
                    std::to_string(static_cast<int>(status)) + ", code " +
                    std::to_string(static_cast<int>(code)) + ", " +
                    std::to_string(m.delivered) + " deliveries and " + std::to_string(m.others) +
                    " other fields");
            /* §13: where the decode surface exposes the wire element count, a
             * decoder delivering fewer elements than the header declares is
             * caught cheaply. */
            if (n > 1)
            {
                ++boolChecks;
                run(m.announced == n, label, (std::string("boolean-array-count") + how).c_str(),
                    "the array header announces " + std::to_string(m.announced) +
                        " elements, expect.values has " + std::to_string(n));
            }

            /* The stored REPRESENTATION, byte by byte, never the `bool` lvalue:
             * an object holding `2` has no value in C++, so `== true` on it is
             * already meaningless, and a truthiness comparison would map exactly
             * the corruption under test onto a pass. */
            unsigned char got[kBoolMaxElems];
            std::memcpy(got, m.dst, sizeof got);
            std::string sawBytes, wantBytes;
            bool bytesOk = true;
            for (size_t k = 0; k < n; k++)
            {
                char b[8];
                std::snprintf(b, sizeof b, "%02x", got[k]);    sawBytes  += b;
                std::snprintf(b, sizeof b, "%02x", c.want[k]); wantBytes += b;
                if (got[k] != c.want[k]) bytesOk = false;
            }
            run(bytesOk, label, (std::string("boolean-normalised") + how).c_str(),
                "the destination holds " + sawBytes + ", expected " + wantBytes +
                    " (0x00 false / 0x01 true; 0xaa is the poison, i.e. never written; "
                    "any other byte is an unnormalised raw value)");

            ++boolChecks;
            if (!bytesOk)
            {
                /* Reading those `bool` objects to re-encode them would be
                 * undefined behaviour where the byte is neither 0 nor 1, and
                 * deriving the values from the raw bytes instead would launder
                 * the very defect just reported into a canonical `1`. So the
                 * re-encode is not attempted — and it is still counted and still
                 * reported as failed, because the case did not prove what it
                 * exists to prove. */
                run(false, label, (std::string("boolean-reencode-canonical") + how).c_str(),
                    "not attempted: the decode destination does not hold the normalised "
                    "values (see boolean-normalised" + std::string(how) + " above)");
                continue;
            }

            /* §11.3: what the DECODER produced, never `expect.values` from the
             * JSON — feeding the expectation back in makes `reencoded_hex` match
             * trivially and leaves the decode half unverified. The byte check
             * above is what makes reading `m.dst` as `bool` legal here. */
            sofab::OStream os(std::make_shared<uint8_t[]>(64), 64);
            const sofab::Error wcode =
                (n == 1 ? os.write(c.fieldId, m.dst[0])
                        : os.write(c.fieldId, std::span<const bool>{m.dst, n})).code();
            os.flush();   /* nothing buffered survives the comparison */
            const std::span<const uint8_t> out{os.data(), os.bytesUsed()};
            /* Against `reencoded_hex`, and byte for byte: `0002` and `0001` are
             * the same LENGTH, so a length comparison passes every case whose
             * whole point is that the re-encode differs from the bytes fed in. */
            const bool reencOk = wcode == sofab::Error::None && os.ok() &&
                                 out.size() == c.reenc.size() &&
                                 std::memcmp(out.data(), c.reenc.data(), c.reenc.size()) == 0;
            std::string sawHex, wantHex;
            for (uint8_t b : out)     { char t[8]; std::snprintf(t, sizeof t, "%02x", b); sawHex  += t; }
            for (uint8_t b : c.reenc) { char t[8]; std::snprintf(t, sizeof t, "%02x", b); wantHex += t; }
            run(reencOk, label, (std::string("boolean-reencode-canonical") + how).c_str(),
                "re-encoded " + sawHex + ", expected " + wantHex + " (encoder code " +
                    std::to_string(static_cast<int>(wcode)) + ", ok=" +
                    std::to_string(static_cast<int>(os.ok())) + ")");
        }
    }
    /* §12: `found` must equal `decoded + rejected`, and a run that found zero
     * cases FAILS. Every way this block can go green while testing less — a
     * stale vector file, a gate that skipped the tagged cases, a filter that
     * kept only the scalars — shows up as a count, and as nothing else. A floor,
     * never an equality: the block grows upstream. */
    run(boolDecoded + boolRejected == static_cast<int>(bools.size()) && !bools.empty(),
        named("(all)"), "boolean-tolerant-accounted",
        "of " + std::to_string(bools.size()) + " boolean_tolerant cases, " +
            std::to_string(boolDecoded) + " decoded and " + std::to_string(boolRejected) +
            " were rejected on an unsatisfied requires tag");
    run(static_cast<int>(bools.size()) >= 8, named("(all)"), "boolean-tolerant-present",
        "expected at least 8 boolean_tolerant cases, saw " + std::to_string(bools.size()));
    /* The scalar half is easy to run and the array half is easy to postpone;
     * a runner that kept only `len(values) == 1` would lose the element-level
     * half of §4.4 entirely and report nothing about it. */
    {
        int scalars = 0, arrays = 0;
        for (const BoolCase &c : bools) (c.want.size() == 1 ? scalars : arrays)++;
        run(scalars >= 5 && arrays >= 2, named("(all)"), "boolean-tolerant-both-shapes",
            "expected at least 5 scalar and 2 array cases, saw " + std::to_string(scalars) +
                " and " + std::to_string(arrays));
    }

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
        static const char kHdr[] = "{\"name\":\"h\",\"field_id\":0,\"declared\":100,"
                                   "\"limits\":{\"max_dyn_string_len\":16},"
                                   "\"serialized\":\"02a206\","
                                   "\"expect\":{\"outcome\":\"limit_exceeded\",\"terminal\":true}}";
        /* The same case a frame deeper: the nested walker owns its own key and
         * demands the `frames` chain, so a file that dropped the key -- or an
         * upstream rename -- is a loud failure rather than a silent run of
         * nothing. */
        static const char kNst[] = "{\"name\":\"hn\",\"field_id\":0,\"declared\":100,"
                                   "\"frames\":[7],"
                                   "\"limits\":{\"max_dyn_string_len\":16},"
                                   "\"serialized\":\"3e02a206\","
                                   "\"expect\":{\"outcome\":\"limit_exceeded\",\"terminal\":true}}";
        static const char kBool[] = "{\"name\":\"b\",\"requires\":[],\"id\":0,"
                                    "\"serialized_hex\":\"0002\","
                                    "\"expect\":{\"outcome\":\"complete\",\"values\":[true],"
                                    "\"reencoded_hex\":\"0001\"}}";
        const std::string both  = std::string("{\"vectors\":[") + kVec +
                                  "],\"invalid_utf8\":[" + kNeg +
                                  "],\"sequence_growth\":[" + kGro +
                                  "],\"header_limits\":[" + kHdr +
                                  "],\"header_limits_nested\":[" + kNst +
                                  "],\"boolean_tolerant\":[" + kBool + "]}";
        const std::string onlyV = std::string("{\"vectors\":[") + kVec + "]}";
        const std::string onlyN = std::string("{\"invalid_utf8\":[") + kNeg + "]}";
        const std::string onlyG = std::string("{\"sequence_growth\":[") + kGro + "]}";
        const std::string onlyH = std::string("{\"header_limits\":[") + kHdr + "]}";
        const std::string onlyX = std::string("{\"header_limits_nested\":[") + kNst + "]}";
        const std::string onlyB = std::string("{\"boolean_tolerant\":[") + kBool + "]}";
        /* `frames` belongs to the nested block alone: in the flat one it would
         * bind the ceiling at the top level while the field arrived a frame
         * down, so the flat walker refuses it instead of ignoring it. */
        const std::string crossed = std::string("{\"header_limits\":[") + kNst + "]}";
        const std::string flatDeep = std::string("{\"header_limits_nested\":[") + kHdr + "]}";
        const std::string empty = "{\"vectors\":[],\"invalid_utf8\":[],\"sequence_growth\":[],"
                                  "\"header_limits\":[],\"header_limits_nested\":[],"
                                  "\"boolean_tolerant\":[]}";
        const std::string drift = std::string("{\"vectors_v2\":[") + kVec +
                                  "],\"invalid_utf8_v2\":[" + kNeg +
                                  "],\"sequence_growth_v2\":[" + kGro +
                                  "],\"header_limits_v2\":[" + kHdr +
                                  "],\"header_limits_nested_v2\":[" + kNst +
                                  "],\"boolean_tolerant_v2\":[" + kBool + "]}";

        const struct { const char *label; const std::string &json;
                       bool wantV, wantN, wantG, wantH, wantX, wantB; } cases[] = {
            {"envelope-all-groups",           both,  true,  true,  true,  true,  true,  true },
            {"envelope-no-invalid_utf8",      onlyV, true,  false, false, false, false, false},
            {"envelope-no-vectors",           onlyN, false, true,  false, false, false, false},
            {"envelope-only-sequence_growth", onlyG, false, false, true,  false, false, false},
            {"envelope-only-header_limits",   onlyH, false, false, false, true,  false, false},
            {"envelope-only-header_limits_nested", onlyX, false, false, false, false, true,  false},
            {"envelope-only-boolean_tolerant", onlyB, false, false, false, false, false, true },
            {"envelope-frames-in-flat-block", crossed, false, false, false, false, false, false},
            {"envelope-nested-without-frames", flatDeep, false, false, false, false, false, false},
            {"envelope-empty-groups",         empty, false, false, false, false, false, false},
            {"envelope-renamed-keys",         drift, false, false, false, false, false, false},
        };
        for (const auto &c : cases)
        {
            const EnvelopeWalk w = walkEnvelope(c.json.c_str());
            /* Accepted => the group yielded vectors; rejected => none, and the
             * walker SAID so rather than returning an empty list. */
            const bool ok = w.parsed &&
                            w.vectorsOk == c.wantV && (w.nVectors > 0) == c.wantV &&
                            w.negOk == c.wantN && (w.nNeg > 0) == c.wantN &&
                            w.growthOk == c.wantG && (w.nGrowth > 0) == c.wantG &&
                            w.headerOk == c.wantH && (w.nHeader > 0) == c.wantH &&
                            w.nestedOk == c.wantX && (w.nNested > 0) == c.wantX &&
                            w.boolOk == c.wantB && (w.nBool > 0) == c.wantB;
            run(ok, named("(all)"), c.label,
                !w.parsed ? std::string("probe envelope did not parse")
                          : "vectors ok=" + std::to_string(static_cast<int>(w.vectorsOk)) +
                                " n=" + std::to_string(w.nVectors) +
                                ", invalid_utf8 ok=" + std::to_string(static_cast<int>(w.negOk)) +
                                " n=" + std::to_string(w.nNeg) +
                                ", sequence_growth ok=" + std::to_string(static_cast<int>(w.growthOk)) +
                                " n=" + std::to_string(w.nGrowth) +
                                ", header_limits ok=" + std::to_string(static_cast<int>(w.headerOk)) +
                                " n=" + std::to_string(w.nHeader) +
                                ", header_limits_nested ok=" + std::to_string(static_cast<int>(w.nestedOk)) +
                                " n=" + std::to_string(w.nNested) +
                                ", boolean_tolerant ok=" + std::to_string(static_cast<int>(w.boolOk)) +
                                " n=" + std::to_string(w.nBool) + "; expected ok " +
                                std::to_string(static_cast<int>(c.wantV)) + "/" +
                                std::to_string(static_cast<int>(c.wantN)) + "/" +
                                std::to_string(static_cast<int>(c.wantG)) + "/" +
                                std::to_string(static_cast<int>(c.wantH)) + "/" +
                                std::to_string(static_cast<int>(c.wantX)) + "/" +
                                std::to_string(static_cast<int>(c.wantB)));
        }
    }

    std::printf("%zu vectors, %d run, %d skipped, %d checks, %d failures\n",
                vectors.size(), static_cast<int>(vectors.size()) - skipped, skipped, checks, failures);
    std::printf("%d vectors carry skip_ids (%d skip/matrix, %d skip), %d ran the skip "
                "scenario whole and byte-at-a-time, %d gated out by requires\n",
                skipVectors, skipMatrixVectors, skipAxisVectors, skipRan, skipGated);
    std::printf("%zu invalid_utf8 vectors, %d run, %d skipped\n", negs.size(), negRun, negSkipped);
    std::printf("%zu sequence_growth cases, %d run, %d skipped (cap %ld)\n",
                growth.size(), growthRun, growthSkipped, kGrowthCap);
    std::printf("%zu header_limits cases, %d run (%d rejections, %d in-cap controls), "
                "%d skipped\n", headers.size(), headerRun, headerRejects,
                headerRun - headerRejects, headerSkipped);
    /* ran and gated, both stated: a capability probe that broke, or a tag this
     * reader stopped recognising, turns the block into a no-op that reports
     * green, and the two numbers summing to the total is the cheap guard. */
    std::printf("%zu header_limits_nested cases, %d run (%d rejections, %d in-cap controls, "
                "%d at depth 2), %d gated\n", nested.size(), nestedRan, nestedRejects,
                nestedRan - nestedRejects, nestedDeep, nestedGated);
    for (const std::string &g : nestedGatedBy)
        std::printf("  gated: %s\n", g.c_str());
    std::printf("%zu boolean_tolerant cases found, %d decoded, %d rejected "
                "(unsatisfied requires), %d checks\n",
                bools.size(), boolDecoded, boolRejected, boolChecks);
    if (failures) std::printf("first failure: %s\n", first.c_str());
    if (const char *v = std::getenv("SOFAB_LIST_FAILURES"); v && *v)
        for (const auto &f : allFailures) std::printf("  FAIL %s\n", f.c_str());
    return failures ? 1 : 0;
}
