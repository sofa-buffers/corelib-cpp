/*!
 * @file sofab_test_json.h
 * @brief Minimal, dependency-free JSON reader for the SofaBuffers test harness.
 *
 * This is a tiny recursive-descent parser used only by the test code to load the
 * shared conformance vectors (assets/test_vectors.json). It is intentionally
 * vendored (not a third-party dependency) so the test targets stay buildable on
 * every CI target without a network fetch. It supports exactly the JSON subset
 * the vector file uses: objects, arrays, strings (with \" \\ \/ \b \f \n \r \t
 * and \uXXXX escapes), numbers (incl. exponents and signs), and true/false/null.
 *
 * Numbers keep their raw token text and are converted on demand, so 64-bit
 * integer values (UINT64_MAX, INT64_MIN) survive without going through double.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SOFAB_TEST_JSON_H
#define SOFAB_TEST_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SOFAB_JSON_NULL,
    SOFAB_JSON_BOOL,
    SOFAB_JSON_NUMBER,
    SOFAB_JSON_STRING,
    SOFAB_JSON_ARRAY,
    SOFAB_JSON_OBJECT,
} sofab_json_type_t;

typedef struct sofab_json sofab_json_t;

/*! Parse a NUL-terminated/length-bounded JSON document. Returns the root node
 *  (free with sofab_json_free) or NULL on error (err filled if non-NULL). */
sofab_json_t *sofab_json_parse(const char *text, size_t len, char *err, size_t errlen);

/*! Release a tree returned by sofab_json_parse. */
void sofab_json_free(sofab_json_t *v);

sofab_json_type_t sofab_json_type(const sofab_json_t *v);

/*! Object member lookup by key (NULL if absent or not an object). */
const sofab_json_t *sofab_json_get(const sofab_json_t *v, const char *key);

/*! Array access. */
size_t sofab_json_array_size(const sofab_json_t *v);
const sofab_json_t *sofab_json_array_at(const sofab_json_t *v, size_t i);

/*! String accessor: unescaped, NUL-terminated bytes; *len gets the byte length
 *  (may contain embedded NULs are not produced by the vector file). */
const char *sofab_json_string(const sofab_json_t *v, size_t *len);

/*! Boolean accessor (0/1). */
int sofab_json_bool(const sofab_json_t *v);

/*! Number accessors — convert the raw token exactly to the requested type. */
uint64_t sofab_json_u64(const sofab_json_t *v);
int64_t  sofab_json_i64(const sofab_json_t *v);
double   sofab_json_double(const sofab_json_t *v);

#ifdef __cplusplus
}
#endif

#endif /* SOFAB_TEST_JSON_H */
