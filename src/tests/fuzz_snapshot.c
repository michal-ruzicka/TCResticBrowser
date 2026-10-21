/*
 * Coverage-guided fuzz target for the parsers of restic's output that build the browsing structures
 * (libFuzzer entry point): the restic mount tree from `restic snapshots --json` (snapshot_tree.c)
 * and the whole-snapshot index from `restic ls --json` NDJSON (snapshot_index.c).
 *
 * Not part of build.cmd: it needs a compiler with libFuzzer, e.g. from src/ with clang:
 *   clang -std=c11 -g -O1 -I. -fsanitize=fuzzer,address,undefined tests/fuzz_snapshot.c \
 *       snapshot_tree.c snapshot_index.c timestamp.c json.c -o fuzz_snapshot
 *   ./fuzz_snapshot -max_len=65536 -max_total_time=3600 corpus/
 * (MSVC can build it as well with /fsanitize=fuzzer /fsanitize=address.)
 *
 * The first byte chooses what the rest is fed to: its low bit the parser, the next bits a time
 * format or a memory limit. Everything that parses is walked and the invariants the plugin relies
 * on are checked - every name is one Windows-safe path component, folders are sorted, unique and
 * found again by name, snapshot IDs are hexadecimal, snapshots.csv reads back as one record of five
 * fields per snapshot - so a violation aborts like a sanitizer finding.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "snapshot_index.h"
#include "snapshot_tree.h"

#define CHECK(cond) do { if (!(cond)) abort(); } while (0)

static const char* const FORMATS[] = {
    NULL, "YYYY-MM-DDThh.mm.ss", "YYMMDD zzzz", "DD.MM.YYYY hh-mm", "YYYY", "x/y", "",
};

/* A name the plugin may hand to Total Commander as one path component */
static void check_name(const char* name) {
    size_t len = strlen(name);
    CHECK(len > 0);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        CHECK(c >= 0x20 && c != 0x7F && !strchr("\\/:*?\"<>|", c));
    }
    CHECK(name[len - 1] != '.' && name[len - 1] != ' ');
}

static void walk_tree(const SnapshotNode* dir, int depth) {
    CHECK(depth <= 3); /* root / hosts|tags / <host|tag> / <snapshot> */
    for (size_t i = 0; i < dir->child_count; ++i) {
        const SnapshotNode* c = dir->children[i];
        check_name(c->name);
        if (i > 0) CHECK(strcmp(dir->children[i - 1]->name, c->name) < 0);
        CHECK(snapshot_tree_child(dir, c->name) == c);
        if (c->snapshot_id) {
            size_t n = strlen(c->snapshot_id);
            CHECK(n >= SNAPSHOT_TREE_SHORT_ID_CHARS && n <= SNAPSHOT_TREE_ID_CHARS);
            CHECK(strspn(c->snapshot_id, "0123456789abcdef") == n && c->child_count == 0);
            CHECK(dir->time >= c->time);
        } else {
            walk_tree(c, depth + 1);
        }
    }
}

/* Reads snapshots.csv back by RFC 4180: BOM, then `records` CRLF-terminated records of five fields */
static void check_csv(const char* csv, size_t len, char sep, size_t records) {
    CHECK(len >= 3 && memcmp(csv, "\xEF\xBB\xBF", 3) == 0 && strlen(csv) == len);
    size_t i = 3, fields = 1, seen = 0;
    while (i < len) {
        if (csv[i] == '"') { /* A quoted field: up to the quote that is not doubled */
            for (++i;; ++i) {
                CHECK(i < len);
                if (csv[i] == '"' && (i + 1 >= len || csv[i + 1] != '"')) break;
                if (csv[i] == '"') ++i;
            }
            ++i;
            CHECK(i < len && (csv[i] == sep || csv[i] == '\r'));
        } else {
            /* Unquoted: no quote, no line break; never a formula after the header */
            CHECK(seen == 0 || !strchr("=+-@", csv[i]) || csv[i] == sep);
            while (i < len && csv[i] != sep && csv[i] != '\r') {
                CHECK(csv[i] != '"' && csv[i] != '\n');
                ++i;
            }
            CHECK(i < len);
        }
        if (csv[i] == sep) {
            ++fields;
            ++i;
            continue;
        }
        CHECK(i + 1 < len && csv[i + 1] == '\n' && fields == 5);
        i += 2;
        fields = 1;
        ++seen;
    }
    CHECK(seen == records);
}

static void fuzz_tree(const char* data, size_t size, const char* format) {
    SnapshotTree* tree = NULL;
    SnapshotTreeStatus st = snapshot_tree_build(data, size, format, &tree);
    CHECK((st == SNAPSHOT_TREE_OK) == (tree != NULL));
    if (!tree) return;
    const SnapshotNode* root = snapshot_tree_root(tree);
    CHECK(root->child_count == 4);
    walk_tree(root, 0);
    static const char SEPARATORS[] = {',', ';', '|', '\t'};
    char sep  = SEPARATORS[size % sizeof(SEPARATORS)];
    size_t len = 0;
    char* csv  = snapshot_tree_csv(tree, sep, &len);
    CHECK(csv != NULL);
    check_csv(csv, len, sep, snapshot_tree_snapshot_count(tree) + 1);
    free(csv);
    snapshot_tree_free(tree);
}

/* Walks every folder from the root, finding each one again by its path */
static void walk_index(const SnapshotIndex* x, long long dir, char* path, size_t len, size_t cap, int depth) {
    size_t n = snapshot_index_child_count(x, dir);
    for (size_t i = 0; i < n; ++i) {
        SnapshotIndexEntry e = snapshot_index_child(x, dir, i);
        CHECK(e.name != NULL && !(e.is_dir && e.size != 0));
        size_t name_len = strlen(e.name);
        if (!e.is_dir || depth > 64 || name_len == 0 || strchr(e.name, '/') || len + 1 + name_len + 1 > cap) continue;
        path[len] = '/';
        memcpy(path + len + 1, e.name, name_len + 1);
        long long child = snapshot_index_find_dir(x, path);
        /* A folder is always found by its path (a sibling of the same name may be found instead) */
        CHECK(child >= 0);
        walk_index(x, child, path, len + 1 + name_len, cap, depth + 1);
        path[len] = '\0';
    }
}

static void fuzz_index(const char* data, size_t size, size_t limit) {
    SnapshotIndex* x = snapshot_index_new(limit);
    if (!x) return;
    SnapshotIndexStatus st = SNAPSHOT_INDEX_OK;
    for (size_t start = 0; start < size;) {
        const char* nl = (const char*)memchr(data + start, '\n', size - start);
        size_t len = nl ? (size_t)(nl - (data + start)) : size - start;
        st = snapshot_index_add_line(x, data + start, len);
        start += len + 1;
    }
    if (snapshot_index_finish(x) == SNAPSHOT_INDEX_OK) {
        CHECK(st == SNAPSHOT_INDEX_OK);
        CHECK(snapshot_index_find_dir(x, "/") == 0);
        static char path[1 << 16];
        path[0] = '\0';
        walk_index(x, 0, path, 0, sizeof(path), 0);
    }
    if (limit) CHECK(snapshot_index_memory(x) <= limit);
    snapshot_index_free(x);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    unsigned mode = data[0];
    const char* text = (const char*)data + 1;
    if (mode & 1) {
        fuzz_index(text, size - 1, (mode >> 1) & 1 ? 0 : (size_t)(mode >> 2) * 1024u + 4096u);
    } else {
        fuzz_tree(text, size - 1, FORMATS[(mode >> 1) % (sizeof(FORMATS) / sizeof(FORMATS[0]))]);
    }
    return 0;
}
