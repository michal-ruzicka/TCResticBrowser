#pragma once
#include <stddef.h>

/*
 * Minimal strict JSON reader (RFC 8259) for the output of `restic ... --json`.
 *
 * Read-only by design: it parses a document into an immutable tree and offers lookups, nothing
 * else (no serialisation, no mutation). Everything a parse allocates lives in one arena owned by
 * the JsonDoc, so a single json_free() releases it and no partially built tree can leak.
 *
 * Deliberate restrictions, all of which make a malformed or hostile payload fail instead of being
 * guessed at:
 * - exactly one value per input, surrounded by optional whitespace only;
 * - no comments, trailing commas, single quotes, leading zeros or bare control characters;
 * - nesting deeper than JSON_MAX_DEPTH is rejected (bounded recursion);
 * - a string containing \u0000 is rejected, so every decoded string is a plain C string;
 * - a lone UTF-16 surrogate escape decodes to U+FFFD rather than failing the whole document.
 *
 * Portable C11 with no Win32 dependency, so it can be tested and fuzzed on any platform.
 */

#define JSON_MAX_DEPTH 64

typedef enum {
    JSON_NULL = 0,
    JSON_FALSE,
    JSON_TRUE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
struct JsonValue {
    JsonType type;
    const char* key;        /* Member name (decoded UTF-8) when inside an object, else NULL */
    const char* text;       /* JSON_STRING: decoded UTF-8; JSON_NUMBER: the number's lexeme */
    const JsonValue* child; /* JSON_ARRAY / JSON_OBJECT: first element or member, else NULL */
    const JsonValue* next;  /* Next sibling in the enclosing array or object */
};

typedef struct JsonDoc JsonDoc;

/* Parses `len` bytes of `text` (need not be NUL-terminated). Returns NULL on any error. */
JsonDoc* json_parse(const char* text, size_t len);

/* Releases a document and every value obtained from it. NULL is accepted. */
void json_free(JsonDoc* doc);

/* The document's top-level value (never NULL for a successfully parsed document). */
const JsonValue* json_root(const JsonDoc* doc);

/* First member of `obj` named exactly `key`; NULL if absent or if `obj` is not an object. */
const JsonValue* json_object_get(const JsonValue* obj, const char* key);

/* Decoded string of a JSON_STRING value; NULL for any other type (including NULL itself). */
const char* json_string(const JsonValue* v);

/*
 * Stores a JSON_NUMBER that is a non-negative integer (no sign, fraction or exponent) exactly in
 * *out. Returns 0 for any other value or on overflow, leaving *out untouched.
 */
int json_u64(const JsonValue* v, unsigned long long* out);
