/*
 * Unit tests for the minimal JSON reader (src/json.c).
 *
 * Portable C11 with no Win32 dependency, like the reader itself: it builds with MSVC as part of
 * build.cmd and equally with any hosted C compiler, e.g. from src/:
 *   gcc -std=c11 -I. -fsanitize=address,undefined tests/test_json.c json.c
 */
#if defined(_MSC_VER) && defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT_TRUE(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("[FAIL] %s:%d: %s\n", __func__, __LINE__, (msg)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

static JsonDoc* parse_str(const char* s) {
    return json_parse(s, strlen(s));
}

/* strcmp that treats a NULL actual value as a mismatch instead of crashing */
static int str_eq(const char* actual, const char* expected) {
    return actual != NULL && strcmp(actual, expected) == 0;
}

/* Inputs a strict RFC 8259 parser must refuse */
static void test_rejects_malformed(void) {
    static const char* const bad[] = {
        "", " ", "{", "}", "[", "]", "[1,]", "{\"a\":1,}", "{,}", "[,1]", "{\"a\" 1}", "{a:1}",
        "{'a':1}", "[01]", "[-]", "[1.]", "[.5]", "[1e]", "[1e+]", "[+1]", "[0x10]", "[NaN]",
        "[Infinity]", "[tru]", "[nul]", "[True]", "\"abc", "\"a\\x\"", "\"\\u12\"", "\"\\u12G4\"",
        "\"tab\there\"", "\"\\u0000\"", "1 2", "{} {}", "[1] x", "// c\n1", "/* c */1",
        "\"\\uD800\\u12\"", "{\"a\":}", "[\"a\" \"b\"]", "\"\\\"",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        JsonDoc* d = parse_str(bad[i]);
        if (d) printf("       accepted: %s\n", bad[i]);
        json_free(d);
        EXPECT_TRUE(d == NULL, "Malformed input rejected");
    }
    /* A raw control character inside a string (here a literal newline) */
    JsonDoc* d = json_parse("\"a\nb\"", 5);
    EXPECT_TRUE(d == NULL, "Unescaped control character rejected");
    /* NULL text */
    EXPECT_TRUE(json_parse(NULL, 0) == NULL, "NULL input rejected");
}

/* Scalars, whitespace and the length argument (input need not be NUL-terminated) */
static void test_scalars(void) {
    JsonDoc* d = parse_str(" \t\r\n null \n");
    EXPECT_TRUE(d && json_root(d)->type == JSON_NULL, "null with surrounding whitespace");
    json_free(d);

    d = parse_str("true");
    EXPECT_TRUE(d && json_root(d)->type == JSON_TRUE, "true");
    json_free(d);
    d = parse_str("false");
    EXPECT_TRUE(d && json_root(d)->type == JSON_FALSE, "false");
    json_free(d);

    d = json_parse("[1]garbage", 3);
    EXPECT_TRUE(d && json_root(d)->type == JSON_ARRAY, "Only `len` bytes are parsed");
    json_free(d);

    static const char* const numbers[] = {"0", "-0", "12", "-12", "1.5", "1e10", "1E-2", "-0.0e+0"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
        d = parse_str(numbers[i]);
        EXPECT_TRUE(d && json_root(d)->type == JSON_NUMBER, "Valid number accepted");
        EXPECT_TRUE(str_eq(json_root(d)->text, numbers[i]), "Number lexeme preserved");
        json_free(d);
    }
}

/* json_u64: exact 64-bit integers, refusing anything that is not a non-negative integer */
static void test_u64(void) {
    unsigned long long v = 7;
    JsonDoc* d = parse_str("18446744073709551615");
    EXPECT_TRUE(d && json_u64(json_root(d), &v) && v == 18446744073709551615ULL, "UINT64_MAX exact");
    json_free(d);

    d = parse_str("9007199254740993"); /* 2^53 + 1: not representable as a double */
    EXPECT_TRUE(d && json_u64(json_root(d), &v) && v == 9007199254740993ULL, "Beyond 2^53 exact");
    json_free(d);

    static const char* const refused[] = {"18446744073709551616", "-1", "1.0", "1e3", "\"5\"", "null"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        v = 7;
        d = parse_str(refused[i]);
        EXPECT_TRUE(d != NULL, "Refused-for-u64 input is still valid JSON");
        EXPECT_TRUE(!json_u64(json_root(d), &v) && v == 7, "Not a u64: refused, output untouched");
        json_free(d);
    }
    EXPECT_TRUE(!json_u64(NULL, &v), "NULL value refused");
}

/* String escapes, UTF-8 pass-through and surrogate handling */
static void test_strings(void) {
    JsonDoc* d = parse_str("\"q\\\" b\\\\ s\\/ \\b\\f\\n\\r\\t\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)), "q\" b\\ s/ \b\f\n\r\t"),
                "Simple escapes decoded");
    json_free(d);

    d = parse_str("\"\\u0041\\u00e9\\u65e5\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)), "A\xC3\xA9\xE6\x97\xA5"),
                "\\u escapes encoded as UTF-8 (1, 2 and 3 bytes)");
    json_free(d);

    d = parse_str("\"\\uD83D\\uDE00\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)), "\xF0\x9F\x98\x80"),
                "Surrogate pair combined into one 4-byte character");
    json_free(d);

    d = parse_str("\"a\\uD800b\\uDC00c\\uD800\\u0041\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)),
                            "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c\xEF\xBF\xBD" "A"),
                "Lone surrogates become U+FFFD; the escape after one is decoded on its own");
    json_free(d);

    d = parse_str("\"raw \xC5\xBE\xC3\xA1\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)), "raw \xC5\xBE\xC3\xA1"),
                "Raw UTF-8 bytes copied unchanged");
    json_free(d);

    d = parse_str("\"\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)), ""), "Empty string");
    json_free(d);

    d = parse_str("1");
    EXPECT_TRUE(d && json_string(json_root(d)) == NULL, "json_string on a number is NULL");
    json_free(d);
    EXPECT_TRUE(json_string(NULL) == NULL, "json_string(NULL) is NULL");
}

/* Objects and arrays: member lookup, order, duplicates and nested values skipped by lookups */
static void test_containers(void) {
    JsonDoc* d = parse_str("{\"a\":1,\"b\":{\"a\":2},\"c\":[3,{\"a\":4}],\"a\":5,\"\":6}");
    EXPECT_TRUE(d != NULL, "Nested document parsed");
    const JsonValue* root = json_root(d);
    unsigned long long v = 0;
    EXPECT_TRUE(json_u64(json_object_get(root, "a"), &v) && v == 1, "First duplicate key wins");
    EXPECT_TRUE(json_object_get(root, "A") == NULL, "Lookup is case-sensitive");
    EXPECT_TRUE(json_object_get(root, "missing") == NULL, "Missing key is NULL");
    EXPECT_TRUE(json_u64(json_object_get(root, ""), &v) && v == 6, "Empty key is a valid key");
    const JsonValue* b = json_object_get(root, "b");
    EXPECT_TRUE(b && b->type == JSON_OBJECT && json_u64(json_object_get(b, "a"), &v) && v == 2,
                "Nested object lookup");
    const JsonValue* c = json_object_get(root, "c");
    EXPECT_TRUE(c && c->type == JSON_ARRAY, "Array member");
    EXPECT_TRUE(c->child && json_u64(c->child, &v) && v == 3 && c->child->key == NULL,
                "First array element, no key");
    EXPECT_TRUE(c->child->next && c->child->next->type == JSON_OBJECT && !c->child->next->next,
                "Second and last array element");
    EXPECT_TRUE(json_object_get(c, "a") == NULL, "json_object_get on an array is NULL");
    json_free(d);

    d = parse_str("[ ]");
    EXPECT_TRUE(d && json_root(d)->type == JSON_ARRAY && json_root(d)->child == NULL, "Empty array");
    json_free(d);
    d = parse_str("{ }");
    EXPECT_TRUE(d && json_root(d)->type == JSON_OBJECT && json_root(d)->child == NULL, "Empty object");
    json_free(d);
}

/* Nesting is bounded so hostile input cannot exhaust the stack */
static void test_depth_limit(void) {
    char buf[2 * (JSON_MAX_DEPTH + 1) + 1];
    for (int depth = JSON_MAX_DEPTH; depth <= JSON_MAX_DEPTH + 1; ++depth) {
        size_t n = 0;
        for (int i = 0; i < depth; ++i) buf[n++] = '[';
        for (int i = 0; i < depth; ++i) buf[n++] = ']';
        JsonDoc* d = json_parse(buf, n);
        int ok = d != NULL;
        json_free(d);
        if (depth == JSON_MAX_DEPTH) {
            EXPECT_TRUE(ok, "Nesting at JSON_MAX_DEPTH accepted");
        } else {
            EXPECT_TRUE(!ok, "Nesting beyond JSON_MAX_DEPTH rejected");
        }
    }
}

/* Holds `len` bytes of input that may contain NUL bytes */
typedef struct {
    const char* text;
    size_t len;
} RawInput;

#define RAW(lit) {(lit), sizeof(lit) - 1}

/*
 * Edge cases from JSONTestSuite (https://github.com/nst/JSONTestSuite, MIT License, by Nicolas
 * Seriot), the reference conformance corpus for JSON parsers, as of commit 1ef36fa. The whole
 * suite was run against this reader once: every n_ case is rejected and every y_ case accepted
 * except the two containing \u0000, which this reader refuses by design. The cases kept here are
 * the ones not already covered above; the file each comes from is noted next to it.
 */
static void test_jsontestsuite_cases(void) {
    static const RawInput must_reject[] = {
        RAW("[\"x\"]]"),                    /* n_array_extra_close */
        RAW("[\"\"],"),                     /* n_array_comma_after_close */
        RAW("[1,,2]"),                      /* n_array_double_comma */
        RAW("[-012]"),                      /* n_number_neg_int_starting_with_zero */
        RAW("[-01]"),                       /* n_number_-01 */
        RAW("[-Infinity]"),                 /* n_number_minus_infinity */
        RAW("[2.e3]"),                      /* n_number_2.e3 */
        RAW("[0.e1]"),                      /* n_number_0.e1 */
        RAW("[-.123]"),                     /* n_number_neg_real_without_int_part */
        RAW("[-2.]"),                       /* n_number_-2. */
        RAW("[0E+]"),                       /* n_number_0_capital_E+ */
        RAW("[1ea]"),                       /* n_number_real_garbage_after_e */
        RAW("[1 000.0]"),                   /* n_number_1_000 */
        RAW("[\xef\xbc\x91]"),              /* n_number_U+FF11_fullwidth_digit_one */
        RAW("[1e\xe5]"),                    /* n_number_real_with_invalid_utf8_after_e */
        RAW("{1:1}"),                       /* n_object_non_string_key */
        RAW("{\"x\"::\"b\"}"),              /* n_object_double_colon */
        RAW("{\"a\":\"b\"}/**/"),           /* n_object_trailing_comment */
        RAW("{\"a\": true} \"x\""),         /* n_structure_object_with_trailing_garbage */
        RAW("[\"\\a\"]"),                   /* n_string_invalid_backslash_esc */
        RAW("[\"\\u00A\"]"),                /* n_string_incomplete_escaped_character */
        RAW("[\"\\uD800\\u1\"]"),           /* n_string_1_surrogate_then_escape_u1 */
        RAW("['single quote']"),            /* n_string_single_quote */
        RAW("[\"\\"),                       /* n_string_start_escape_unclosed */
        RAW("[\0]"),                        /* n_structure_null-byte-outside-string */
        RAW("[\f]"),                        /* n_structure_whitespace_formfeed */
        RAW("[\xe2\x81\xa0]"),              /* n_structure_U+2060_word_joined */
        RAW("\xef\xbb\xbf{}"),              /* i_structure_UTF-8_BOM_empty_object (rejected) */
    };
    for (size_t i = 0; i < sizeof(must_reject) / sizeof(must_reject[0]); ++i) {
        JsonDoc* d = json_parse(must_reject[i].text, must_reject[i].len);
        if (d) printf("       accepted case #%u\n", (unsigned)i);
        json_free(d);
        EXPECT_TRUE(d == NULL, "JSONTestSuite n_ case rejected");
    }

    static const RawInput must_accept[] = {
        RAW("[0e1]"), RAW("[0E+1]"), RAW("[-0]"), RAW("[1E22]"),    /* y_number_* */
        RAW("[123e-10000000]"),             /* y_number_very_big_negative_exponent... */
        RAW("[1.5e+9999]"),                 /* i_number_huge_exp (lexeme kept, never converted) */
        RAW(" [] "),                        /* y_structure_whitespace_array */
        RAW("[\"a\"]\n"),                   /* y_structure_trailing_newline */
        RAW("{\"a\":\"b\",\"a\":\"c\"}"),   /* y_object_duplicated_key */
        RAW("{\"\":0}"),                    /* y_object_empty_key */
        RAW("[\"a\x7f\"]"),                 /* y_string_with_del_character */
        RAW("[\"\xef\xbf\xbf\"]"),          /* y_string_nonCharacterInUTF-8_U+FFFF */
        RAW("[\"\\uFFFE\"]"),               /* y_string_unicode_U+FFFE_nonchar */
        RAW("-0.1"), RAW("\"asd\""), RAW("null"),                   /* y_structure_lonely_* */
    };
    for (size_t i = 0; i < sizeof(must_accept) / sizeof(must_accept[0]); ++i) {
        JsonDoc* d = json_parse(must_accept[i].text, must_accept[i].len);
        if (!d) printf("       rejected case #%u\n", (unsigned)i);
        json_free(d);
        EXPECT_TRUE(d != NULL, "JSONTestSuite y_ case accepted");
    }

    /* y_string_allowed_escapes: every escape the grammar allows, decoded */
    JsonDoc* d = parse_str("[\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]");
    const JsonValue* root = json_root(d);
    EXPECT_TRUE(root && root->child && str_eq(json_string(root->child), "\"\\/\b\f\n\r\t"),
                "All simple escapes decoded");
    json_free(d);

    /* y_string_surrogates_U+1D11E with lowercase hex: U+1D11E as 4-byte UTF-8 */
    d = parse_str("\"\\uD834\\uDd1e\"");
    EXPECT_TRUE(d && str_eq(json_string(json_root(d)), "\xf0\x9d\x84\x9e"),
                "Surrogate pair with mixed-case hex decoded");
    json_free(d);

    /* n_structure_100000_opening_arrays: refused at the depth limit, no stack exhaustion */
    enum { DEEP = 100000 };
    char* deep = (char*)malloc(DEEP);
    EXPECT_TRUE(deep != NULL, "Allocation for the deep-nesting case");
    memset(deep, '[', DEEP);
    d = json_parse(deep, DEEP);
    free(deep);
    EXPECT_TRUE(d == NULL, "100000 opening brackets rejected");
}

/* Large documents span several arena blocks */
static void test_large_document(void) {
    enum { COUNT = 20000 };
    static char buf[COUNT * 32];
    size_t n = 0;
    buf[n++] = '[';
    for (int i = 0; i < COUNT; ++i) {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s{\"n\":%d,\"s\":\"x%d\"}",
                              i ? "," : "", i, i);
    }
    buf[n++] = ']';
    JsonDoc* d = json_parse(buf, n);
    EXPECT_TRUE(d != NULL, "Large array parsed");
    int count = 0;
    int in_order = 1;
    for (const JsonValue* e = json_root(d)->child; e; e = e->next, ++count) {
        unsigned long long v = 0;
        char expect[16];
        snprintf(expect, sizeof(expect), "x%d", count);
        if (!json_u64(json_object_get(e, "n"), &v) || v != (unsigned long long)count ||
            !str_eq(json_string(json_object_get(e, "s")), expect)) {
            in_order = 0;
        }
    }
    json_free(d);
    EXPECT_TRUE(count == COUNT && in_order, "Every element present, in order, with its values");
}

/* Real restic output shapes: a `snapshots --json` array and an `ls --json` node line */
static void test_restic_shapes(void) {
    const char* snaps =
        "[{\"time\":\"2026-10-18T09:30:00.123456789+02:00\",\"tree\":\"ab\",\"paths\":[\"/home\"],"
        "\"hostname\":\"h\",\"username\":\"u\",\"uid\":1000,\"gid\":1000,\"tags\":null,"
        "\"program_version\":\"restic 0.18.1\",\"summary\":{\"files_new\":3,\"data_added\":1.5e3},"
        "\"id\":\"a1b2c3d4e5f6\",\"short_id\":\"a1b2c3d4\"}]\n";
    JsonDoc* d = parse_str(snaps);
    EXPECT_TRUE(d && json_root(d)->type == JSON_ARRAY, "snapshots array (trailing newline)");
    const JsonValue* s = json_root(d)->child;
    EXPECT_TRUE(s && str_eq(json_string(json_object_get(s, "short_id")), "a1b2c3d4"),
                "short_id found after nested members");
    json_free(d);

    const char* node =
        "{\"name\":\"a \\\"b\\\".txt\",\"type\":\"file\",\"path\":\"/home/a \\\"b\\\".txt\","
        "\"uid\":0,\"gid\":0,\"size\":5000000000,\"mode\":420,\"permissions\":\"-rw-r--r--\","
        "\"mtime\":\"2026-01-02T03:04:05Z\",\"struct_type\":\"node\",\"message_type\":\"node\"}";
    d = parse_str(node);
    unsigned long long size = 0;
    EXPECT_TRUE(d && str_eq(json_string(json_object_get(json_root(d), "name")), "a \"b\".txt"),
                "ls node name with escaped quotes");
    EXPECT_TRUE(json_u64(json_object_get(json_root(d), "size"), &size) && size == 5000000000ULL,
                "ls node size above 4 GiB");
    json_free(d);
}

int main(void) {
#if defined(_MSC_VER) && defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    printf("Running minimal JSON reader unit tests...\n");

    test_rejects_malformed();
    test_scalars();
    test_u64();
    test_strings();
    test_containers();
    test_depth_limit();
    test_jsontestsuite_cases();
    test_large_document();
    test_restic_shapes();

    printf("[JSON] %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
