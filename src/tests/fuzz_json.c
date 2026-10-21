/*
 * Coverage-guided fuzz target for the JSON reader (libFuzzer entry point).
 *
 * Not part of build.cmd: it needs a compiler with libFuzzer, e.g. from src/ with clang:
 *   clang -std=c11 -g -O1 -I. -fsanitize=fuzzer,address,undefined tests/fuzz_json.c json.c -o fuzz_json
 *   ./fuzz_json -max_len=4096 -max_total_time=3600 corpus/
 * (MSVC can build it as well with /fsanitize=fuzzer /fsanitize=address.)
 *
 * Every input is parsed; for a document that parses, the whole tree is walked and the invariants
 * the plugin relies on are checked, so a violation aborts like a sanitizer finding would.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

#define CHECK(cond) do { if (!(cond)) abort(); } while (0)

static void walk(const JsonValue* v, int depth, int in_object) {
    CHECK(depth <= JSON_MAX_DEPTH);
    CHECK(in_object ? v->key != NULL : v->key == NULL);
    switch (v->type) {
    case JSON_STRING:
        CHECK(v->text != NULL && json_string(v) == v->text);
        break;
    case JSON_NUMBER: {
        CHECK(v->text != NULL && v->text[0] != '\0' && json_string(v) == NULL);
        unsigned long long n = 0;
        if (json_u64(v, &n)) CHECK(v->text[0] >= '0' && v->text[0] <= '9');
        break;
    }
    case JSON_ARRAY:
    case JSON_OBJECT:
        for (const JsonValue* c = v->child; c; c = c->next) {
            walk(c, depth + 1, v->type == JSON_OBJECT);
            /* Lookup by key finds a member with that key (the first one wins on duplicates) */
            if (v->type == JSON_OBJECT) {
                const JsonValue* found = json_object_get(v, c->key);
                CHECK(found != NULL && strcmp(found->key, c->key) == 0);
            }
        }
        break;
    case JSON_NULL:
    case JSON_TRUE:
    case JSON_FALSE:
        CHECK(v->child == NULL && json_string(v) == NULL);
        break;
    default:
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    JsonDoc* doc = json_parse((const char*)data, size);
    if (doc) {
        const JsonValue* root = json_root(doc);
        CHECK(root != NULL);
        walk(root, 0, 0);
    }
    json_free(doc);
    return 0;
}
