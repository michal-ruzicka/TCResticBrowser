/*
 * Unit tests for the RFC 3339 parser (src/timestamp.c), the restic-mount folder tree
 * (src/snapshot_tree.c), the index of a whole snapshot (src/snapshot_index.c) and the versions of
 * a file (src/file_history.c).
 *
 * Portable C11 with no Win32 dependency, like the code under test: it builds with MSVC as part of
 * build.cmd and equally with any hosted C compiler, e.g. from src/:
 *   gcc -std=c11 -I. -fsanitize=address,undefined tests/test_snapshot_tree.c snapshot_tree.c snapshot_index.c \
 *       file_history.c timestamp.c json.c
 */
#if defined(_MSC_VER) && defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_history.h"
#include "snapshot_index.h"
#include "snapshot_tree.h"
#include "timestamp.h"

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

/* FILETIME ticks of a UTC date and time, computed independently of timestamp.c */
static unsigned long long ticks_utc(int y, int mo, int d, int h, int mi, int s) {
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long long days = 0;
    for (int year = 1601; year < y; ++year) {
        days += ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
    }
    days += cum[mo - 1] + (d - 1);
    if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return (unsigned long long)(((days * 24 + h) * 60 + mi) * 60 + s) * 10000000ULL;
}

static void test_timestamp_parse(void) {
    Timestamp t;
    EXPECT_TRUE(timestamp_parse("1970-01-01T00:00:00Z", &t) && t.utc_ticks == TIMESTAMP_UNIX_EPOCH_TICKS,
                "Unix epoch is 116444736000000000 ticks");
    EXPECT_TRUE(timestamp_parse("1601-01-01T00:00:00Z", &t) && t.utc_ticks == 0, "FILETIME epoch is 0");
    EXPECT_TRUE(timestamp_parse("2026-10-18T16:28:15.182004+02:00", &t), "restic's format");
    EXPECT_TRUE(t.utc_ticks == ticks_utc(2026, 10, 18, 14, 28, 15) + 1820040, "+02:00 converted to UTC, fraction kept");
    EXPECT_TRUE(t.year == 2026 && t.month == 10 && t.day == 18 && t.hour == 16 && t.minute == 28 && t.second == 15 &&
                t.offset_minutes == 120 && t.nanosecond == 182004000u, "Wall-clock fields as written");
    EXPECT_TRUE(timestamp_parse("2026-10-18T09:28:15-05:00", &t) && t.utc_ticks == ticks_utc(2026, 10, 18, 14, 28, 15) &&
                t.offset_minutes == -300, "-05:00");
    EXPECT_TRUE(timestamp_parse("2026-10-18T10:00:00+0530", &t) && t.utc_ticks == ticks_utc(2026, 10, 18, 4, 30, 0),
                "+0530");
    EXPECT_TRUE(timestamp_parse("2026-10-18T12:00:00+02", &t) && t.utc_ticks == ticks_utc(2026, 10, 18, 10, 0, 0), "+02");
    EXPECT_TRUE(timestamp_parse("2026-10-18 12:00:00", &t) && t.utc_ticks == ticks_utc(2026, 10, 18, 12, 0, 0),
                "Space separator, no zone = UTC");
    EXPECT_TRUE(timestamp_parse("2026-01-01T00:30:00+01:00", &t) && t.utc_ticks == ticks_utc(2025, 12, 31, 23, 30, 0),
                "Offset crossing a year boundary");
    EXPECT_TRUE(timestamp_parse("2020-02-29T12:34:56.123456789Z", &t) && t.nanosecond == 123456789u &&
                t.utc_ticks == ticks_utc(2020, 2, 29, 12, 34, 56) + 1234567, "Nanoseconds kept, ticks truncated");
    EXPECT_TRUE(timestamp_parse("2000-02-29T00:00:00Z", &t), "Feb 29 of 2000 (divisible by 400)");
    EXPECT_TRUE(timestamp_parse("2020-02-29T12:34:60Z", &t) && t.second == 60 &&
                t.utc_ticks == ticks_utc(2020, 2, 29, 12, 34, 59), "Leap second counted as :59");
    EXPECT_TRUE(timestamp_parse("9999-12-31T23:59:59Z", &t), "Year 9999");

    static const char* const bad[] = {
        "", "invalid-date", "2026-10-18", "2026-02-30T10:00:00Z", "2021-02-29T10:00:00Z", "1900-02-29T00:00:00Z",
        "2026-04-31T00:00:00Z", "2026-13-01T10:00:00Z", "2026-00-01T10:00:00Z", "2026-10-00T10:00:00Z",
        "2026-10-18T25:00:00Z", "2026-10-18T10:60:00Z", "2026-10-18T10:00:61Z", "0001-01-01T00:00:00Z",
        "1601-01-01T00:00:00+02:00", "2026-10-18T10:00:00+24:00", "2026-10-18T10:00:00+02:75",
        "2026-10-18T10:00:00+02:5", "2026-10-18T10:00:00.Z", "2026-10-18T10:00:00Zjunk", "2026-10-18X10:00:00Z",
        "2026-1a-18T10:00:00Z", "2026-10-18T10:00:00+2",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        if (timestamp_parse(bad[i], &t)) printf("       accepted: %s\n", bad[i]);
        EXPECT_TRUE(!timestamp_parse(bad[i], &t) && t.utc_ticks == 0, "Invalid timestamp rejected");
    }
    EXPECT_TRUE(!timestamp_parse(NULL, &t), "NULL rejected");
    EXPECT_TRUE(!timestamp_parse("2026-10-18T10:00:00Z", NULL), "NULL output rejected");
}

static void test_timestamp_compare(void) {
    Timestamp a, b;
    EXPECT_TRUE(timestamp_parse("2026-10-18T10:00:00.000000100Z", &a) &&
                timestamp_parse("2026-10-18T10:00:00.000000150Z", &b), "Parse");
    EXPECT_TRUE(a.utc_ticks == b.utc_ticks && timestamp_compare(&a, &b) < 0 && timestamp_compare(&b, &a) > 0,
                "Nanoseconds order instants that share a FILETIME tick");
    EXPECT_TRUE(timestamp_parse("2026-10-18T12:00:00+02:00", &a) && timestamp_parse("2026-10-18T10:00:00Z", &b) &&
                timestamp_compare(&a, &b) == 0, "Same instant in two zones");
    EXPECT_TRUE(timestamp_parse("2026-10-18T11:00:00+02:00", &a) && timestamp_compare(&a, &b) < 0,
                "Earlier instant despite a later wall-clock hour");
}

/* ------------------------------------------------------------------------------------------ */

static SnapshotTree* build(const char* json) {
    SnapshotTree* tree = NULL;
    if (snapshot_tree_build(json, strlen(json), NULL, &tree) != SNAPSHOT_TREE_OK) return NULL;
    return tree;
}

/* Follows a '/'-separated path from the root; NULL if any component is missing */
static const SnapshotNode* walk(const SnapshotTree* tree, const char* path) {
    const SnapshotNode* n = snapshot_tree_root(tree);
    char part[512];
    while (n && *path) {
        size_t len = strcspn(path, "/");
        if (len >= sizeof(part)) return NULL;
        memcpy(part, path, len);
        part[len] = '\0';
        n = snapshot_tree_child(n, part);
        path += len;
        if (*path == '/') path++;
    }
    return n;
}

/* Appends `text` to the NUL-terminated `buf` of `cap` bytes, truncating (strcat is deprecated by MSVC) */
static void append(char* buf, size_t cap, const char* text) {
    size_t used = strlen(buf), len = strlen(text);
    if (used + len >= cap) len = cap - used - 1;
    memcpy(buf + used, text, len);
    buf[used + len] = '\0';
}

/* Names of a folder's children joined by '|' */
static const char* names(const SnapshotNode* n) {
    static char buf[2048];
    buf[0] = '\0';
    for (size_t i = 0; n && i < n->child_count; ++i) {
        if (i) append(buf, sizeof(buf), "|");
        append(buf, sizeof(buf), n->children[i]->name);
    }
    return buf;
}

static int is_snapshot(const SnapshotNode* n, const char* id_prefix) {
    return n && n->snapshot_id && strncmp(n->snapshot_id, id_prefix, strlen(id_prefix)) == 0 && n->child_count == 0;
}

#define X8(c) c c c c c c c c
#define ID(c) X8(X8(c)) /* A full 64-character snapshot ID */
#define Z54 "000000000" "000000000" "000000000" "000000000" "000000000" "000000000"

static void test_empty_repository(void) {
    SnapshotTree* tree = build("null");
    EXPECT_TRUE(tree != NULL, "A literal null is an empty repository");
    EXPECT_TRUE(strcmp(names(snapshot_tree_root(tree)), "hosts|ids|snapshots|tags") == 0,
                "The four top-level folders exist without snapshots");
    EXPECT_TRUE(walk(tree, "snapshots")->child_count == 0 && walk(tree, "snapshots")->time == 0 &&
                snapshot_tree_root(tree)->time == 0, "Empty folders, no time");
    EXPECT_TRUE(snapshot_tree_snapshot_count(tree) == 0, "No snapshots");
    snapshot_tree_free(tree);

    tree = build(" [ ] ");
    EXPECT_TRUE(tree && strcmp(names(snapshot_tree_root(tree)), "hosts|ids|snapshots|tags") == 0, "Empty array");
    snapshot_tree_free(tree);

    SnapshotTree* out = (SnapshotTree*)1;
    EXPECT_TRUE(snapshot_tree_build("{}", 2, NULL, &out) == SNAPSHOT_TREE_BAD_JSON && out == NULL, "Object refused");
    EXPECT_TRUE(snapshot_tree_build("[1,", 3, NULL, &out) == SNAPSHOT_TREE_BAD_JSON, "Malformed JSON refused");
    EXPECT_TRUE(snapshot_tree_build(NULL, 0, NULL, &out) == SNAPSHOT_TREE_BAD_JSON, "NULL refused");
    snapshot_tree_free(NULL);
}

static void test_layout(void) {
    /* Out of order on purpose: the tree sorts by time like restic */
    SnapshotTree* tree = build(
        "[{\"time\":\"2026-10-19T08:00:00.5+02:00\",\"id\":\"" ID("b") "\",\"short_id\":\"bbbbbbbb\","
        "\"hostname\":\"laptop\",\"tags\":[\"daily\",\"home\"],\"paths\":[\"/home\"]},"
        "{\"time\":\"2026-10-18T22:15:30Z\",\"id\":\"" ID("a") "\",\"hostname\":\"server\",\"paths\":[\"/etc\"]},"
        "{\"time\":\"2026-10-20T01:02:03-07:00\",\"id\":\"" ID("c") "\",\"hostname\":\"laptop\",\"tags\":[\"daily\"]}]");
    EXPECT_TRUE(tree != NULL, "Built");
    EXPECT_TRUE(snapshot_tree_snapshot_count(tree) == 3, "Three snapshots");
    EXPECT_TRUE(strcmp(names(walk(tree, "ids")), "aaaaaaaa|bbbbbbbb|cccccccc") == 0, "ids/ holds the short IDs");
    EXPECT_TRUE(is_snapshot(walk(tree, "ids/bbbbbbbb"), ID("b")), "ids/<short> is the snapshot with the full ID");
    EXPECT_TRUE(strcmp(names(walk(tree, "snapshots")),
                       "2026-10-18_22-15-30|2026-10-19_08-00-00|2026-10-20_01-02-03|latest|latest \xe2\x86\x92 " "2026-10-20_01-02-03|latest \xe2\x86\x92 " "cccccccc") == 0,
                "snapshots/ named by the time in the snapshot's own zone, plus latest");
    EXPECT_TRUE(is_snapshot(walk(tree, "snapshots/latest"), ID("c")), "latest is the newest snapshot");
    EXPECT_TRUE(is_snapshot(walk(tree, "snapshots/latest \xe2\x86\x92 " "cccccccc"), ID("c")) &&
                walk(tree, "snapshots/latest \xe2\x86\x92 " "cccccccc")->time == walk(tree, "snapshots/latest")->time,
                "latest → <short id> is the same snapshot and says which");
    EXPECT_TRUE(is_snapshot(walk(tree, "snapshots/latest \xe2\x86\x92 2026-10-20_01-02-03"), ID("c")),
                "latest → <folder> names restic mount's link target, the latest snapshot's folder");
    EXPECT_TRUE(is_snapshot(walk(tree, "hosts/server/latest \xe2\x86\x92 " "aaaaaaaa"), ID("a")) &&
                walk(tree, "ids/latest \xe2\x86\x92 " "cccccccc") == NULL, "Wherever latest is, and only there");
    EXPECT_TRUE(walk(tree, "snapshots/latest")->time == ticks_utc(2026, 10, 20, 8, 2, 3), "latest has its time (UTC)");
    EXPECT_TRUE(strcmp(names(walk(tree, "hosts")), "laptop|server") == 0, "hosts/");
    EXPECT_TRUE(strcmp(names(walk(tree, "hosts/laptop")), "2026-10-19_08-00-00|2026-10-20_01-02-03|latest|latest \xe2\x86\x92 " "2026-10-20_01-02-03|latest \xe2\x86\x92 " "cccccccc") == 0,
                "hosts/laptop/");
    EXPECT_TRUE(is_snapshot(walk(tree, "hosts/server/latest"), ID("a")), "hosts/server/latest");
    EXPECT_TRUE(strcmp(names(walk(tree, "tags")), "daily|home") == 0, "tags/, untagged snapshot absent");
    EXPECT_TRUE(strcmp(names(walk(tree, "tags/daily")), "2026-10-19_08-00-00|2026-10-20_01-02-03|latest|latest \xe2\x86\x92 " "2026-10-20_01-02-03|latest \xe2\x86\x92 " "cccccccc") == 0,
                "tags/daily/");
    EXPECT_TRUE(is_snapshot(walk(tree, "tags/home/2026-10-19_08-00-00"), ID("b")) &&
                is_snapshot(walk(tree, "tags/home/latest"), ID("b")), "A snapshot with two tags is in both");
    EXPECT_TRUE(walk(tree, "hosts/laptop")->time == ticks_utc(2026, 10, 20, 8, 2, 3) &&
                walk(tree, "hosts/server")->time == ticks_utc(2026, 10, 18, 22, 15, 30) &&
                snapshot_tree_root(tree)->time == ticks_utc(2026, 10, 20, 8, 2, 3) &&
                walk(tree, "tags")->time == ticks_utc(2026, 10, 20, 8, 2, 3),
                "A folder carries the time of its newest snapshot");
    EXPECT_TRUE(walk(tree, "hosts/LAPTOP") == walk(tree, "hosts/laptop") && walk(tree, "IDS/BBBBBBBB") != NULL,
                "ASCII case-insensitive fallback");
    EXPECT_TRUE(walk(tree, "hosts/nobody") == NULL && walk(tree, "latest") == NULL && walk(tree, "ids/latest") == NULL,
                "Unknown names, and no latest outside folders of times");
    EXPECT_TRUE(snapshot_tree_child(walk(tree, "ids/aaaaaaaa"), "x") == NULL, "A snapshot has no tree children");
    snapshot_tree_free(tree);
}

static void test_name_collisions(void) {
    /* Same wall-clock second (different fractions and zones) and the same short ID */
    SnapshotTree* tree = build(
        "[{\"time\":\"2026-10-18T10:00:00.9Z\",\"id\":\"1234567811" Z54 "\",\"hostname\":\"h\"},"
        "{\"time\":\"2026-10-18T10:00:00.1Z\",\"id\":\"1234567822" Z54 "\",\"hostname\":\"h\"},"
        "{\"time\":\"2026-10-18T10:00:00+01:00\",\"id\":\"1234567833" Z54 "\",\"hostname\":\"h\"},"
        "{\"time\":\"2026-10-18T10:00:00.1Z\",\"id\":\"1234567800" Z54 "\",\"hostname\":\"h\"}]");
    EXPECT_TRUE(tree != NULL, "Built");
    /* Sorted: ...33 (09:00 UTC), ...00 and ...22 (10:00:00.1, by ID), ...11 (10:00:00.9) */
    EXPECT_TRUE(strcmp(names(walk(tree, "snapshots")),
                       "2026-10-18_10-00-00|2026-10-18_10-00-00-1|2026-10-18_10-00-00-2|2026-10-18_10-00-00-3|latest|latest \xe2\x86\x92 " "12345678|latest \xe2\x86\x92 " "2026-10-18_10-00-00-3") == 0,
                "Taken names get -1, -2, ... as in restic");
    EXPECT_TRUE(is_snapshot(walk(tree, "snapshots/2026-10-18_10-00-00"), "1234567833") &&
                is_snapshot(walk(tree, "snapshots/2026-10-18_10-00-00-1"), "1234567800") &&
                is_snapshot(walk(tree, "snapshots/2026-10-18_10-00-00-2"), "1234567822") &&
                is_snapshot(walk(tree, "snapshots/2026-10-18_10-00-00-3"), "1234567811"),
                "Older snapshot keeps the plain name; ties broken by ID");
    EXPECT_TRUE(is_snapshot(walk(tree, "snapshots/latest"), "1234567811"), "latest is the newest instant");
    EXPECT_TRUE(strcmp(names(walk(tree, "ids")), "12345678|12345678-1|12345678-2|12345678-3") == 0,
                "Shared short IDs numbered the same way");
    EXPECT_TRUE(is_snapshot(walk(tree, "ids/12345678-1"), "1234567800"), "ids suffix follows the time order");
    snapshot_tree_free(tree);

    /* Two snapshots at the very same instant: latest is the one with the greater ID (inserted last) */
    tree = build("[{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"" ID("f") "\"},"
                 "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"" ID("e") "\"}]");
    EXPECT_TRUE(tree && is_snapshot(walk(tree, "snapshots/latest"), ID("f")) &&
                is_snapshot(walk(tree, "snapshots/2026-10-18_10-00-00"), ID("e")), "Equal times: ID decides");
    snapshot_tree_free(tree);
}

static void test_sanitized_names(void) {
    SnapshotTree* tree = build(
        "[{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"" ID("1") "\",\"hostname\":\"a/b\\\\c:d*e?f\\\"g<h>i|j\","
        "\"tags\":[\"\",\".\",\"..\",\"x/y\",\"tab\\there\",\"trail. \",\"caf\\u00e9\",\"\\ud83d\\ude00\",5,\"x_y\"]},"
        "{\"time\":\"2026-10-18T11:00:00Z\",\"id\":\"" ID("2") "\"},"
        "{\"time\":\"2026-10-18T12:00:00Z\",\"id\":\"" ID("3") "\",\"hostname\":\"\"}]");
    EXPECT_TRUE(tree != NULL, "Built");
    EXPECT_TRUE(strcmp(names(walk(tree, "hosts")), "_|a_b_c_d_e_f_g_h_i_j") == 0,
                "Separators and Windows-reserved characters become '_'; missing and empty host names are '_'");
    EXPECT_TRUE(walk(tree, "hosts/_")->child_count == 5, "Both nameless snapshots under '_' (plus latest and its two aliases)");
    EXPECT_TRUE(strcmp(names(walk(tree, "tags")), "_|__|caf\xc3\xa9|tab_here|trail__|x_y|\xf0\x9f\x98\x80") == 0,
                "restic's tag rules: '' and '.' -> '_', '..' -> '__', '/' -> '_'; control characters and trailing "
                "dots/spaces replaced; non-ASCII kept; non-string tags ignored");
    EXPECT_TRUE(strcmp(names(walk(tree, "tags/x_y")), "2026-10-18_10-00-00|2026-10-18_10-00-00-1|latest|latest \xe2\x86\x92 " "11111111|latest \xe2\x86\x92 " "2026-10-18_10-00-00-1") == 0,
                "Tags that map to the same name share the folder, as in restic");
    EXPECT_TRUE(strcmp(names(walk(tree, "tags/_")), "2026-10-18_10-00-00|2026-10-18_10-00-00-1|latest|latest \xe2\x86\x92 " "11111111|latest \xe2\x86\x92 " "2026-10-18_10-00-00-1") == 0,
                "'' and '.' share '_'");
    snapshot_tree_free(tree);

    /* Invalid UTF-8 (the JSON reader passes raw bytes through) and over-long names */
    char json[2048];
    char host[700];
    memset(host, 'h', 600);
    host[600] = '\0';
    snprintf(json, sizeof(json),
             "[{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"" ID("4") "\",\"hostname\":\"%s\",\"tags\":[\"bad\xff\xc3(\"]}]",
             host);
    tree = build(json);
    EXPECT_TRUE(tree != NULL, "Built");
    const SnapshotNode* hosts = walk(tree, "hosts");
    EXPECT_TRUE(hosts->child_count == 1 && strlen(hosts->children[0]->name) == 255, "Host name cut to 255 UTF-16 units");
    EXPECT_TRUE(strcmp(names(walk(tree, "tags")), "bad__(") == 0, "Invalid UTF-8 bytes replaced");
    snapshot_tree_free(tree);

    /* 200 x U+1F600 (two UTF-16 units each): cut at a code point boundary after 127 of them */
    char emoji_json[2048] = "[{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"" ID("5") "\",\"hostname\":\"";
    for (int i = 0; i < 200; ++i) append(emoji_json, sizeof(emoji_json), "\xf0\x9f\x98\x80");
    append(emoji_json, sizeof(emoji_json), "\"}]");
    tree = build(emoji_json);
    EXPECT_TRUE(tree && strlen(walk(tree, "hosts")->children[0]->name) == 127 * 4, "Surrogate pairs never split");
    snapshot_tree_free(tree);
}

/* [Settings] SnapshotTimeFormat: placeholders, literals, the zone, and what is refused */
static void test_time_formats(void) {
    static const char* const json =
        "[{\"time\":\"2026-01-02T03:04:05.9+05:30\",\"id\":\"" ID("a") "\"},"
        "{\"time\":\"1999-12-31T23:59:58-08:00\",\"id\":\"" ID("b") "\"},"
        "{\"time\":\"2026-07-08T09:10:11Z\",\"id\":\"" ID("c") "\"}]";
    static const struct { const char* format; const char* names; } cases[] = {
        {"YYYY-MM-DD_hh-mm-ss", "1999-12-31_23-59-58|2026-01-02_03-04-05|2026-07-08_09-10-11|latest|latest \xe2\x86\x92 " "2026-07-08_09-10-11|latest \xe2\x86\x92 " "cccccccc"},
        {"YYYY-MM-DDThh.mm.ss", "1999-12-31T23.59.58|2026-01-02T03.04.05|2026-07-08T09.10.11|latest|latest \xe2\x86\x92 " "2026-07-08T09.10.11|latest \xe2\x86\x92 " "cccccccc"},
        {"YYMMDD-hhmmss zzzz", "260102-030405 +0530|260708-091011 +0000|991231-235958 -0800|latest|latest \xe2\x86\x92 " "260708-091011 +0000|latest \xe2\x86\x92 " "cccccccc"},
        {"DD.MM.YYYY hh-mm", "02.01.2026 03-04|08.07.2026 09-10|31.12.1999 23-59|latest|latest \xe2\x86\x92 " "08.07.2026 09-10|latest \xe2\x86\x92 " "cccccccc"},
        {"YYYY", "1999|2026|2026-1|latest|latest \xe2\x86\x92 " "2026-1|latest \xe2\x86\x92 " "cccccccc"},
        {"Y-Mon h.s z", "Y-Mon h.s z|Y-Mon h.s z-1|Y-Mon h.s z-2|latest"}, /* No full placeholder... */
        {"Café YYYY-MM-DD", "Café 1999-12-31|Café 2026-01-02|Café 2026-07-08|latest|latest \xe2\x86\x92 " "Café 2026-07-08|latest \xe2\x86\x92 " "cccccccc"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        SnapshotTree* tree = NULL;
        SnapshotTreeStatus st = snapshot_tree_build(json, strlen(json), cases[i].format, &tree);
        if (i == 5) { /* ...is refused: every snapshot would get the same name */
            EXPECT_TRUE(st == SNAPSHOT_TREE_BAD_FORMAT && tree == NULL, "A format without a placeholder is refused");
            continue;
        }
        EXPECT_TRUE(st == SNAPSHOT_TREE_OK, cases[i].format);
        if (strcmp(names(walk(tree, "snapshots")), cases[i].names) != 0) printf("       got: %s\n", names(walk(tree, "snapshots")));
        EXPECT_TRUE(strcmp(names(walk(tree, "snapshots")), cases[i].names) == 0, cases[i].format);
        snapshot_tree_free(tree);
    }
    static const char* const bad[] = {
        "", "YYYY:MM", "YYYY/MM", "YYYY\\MM", "YYYY*", "YYYY?", "YYYY\"", "YYYY<", "YYYY>", "YYYY|",
        "YYYY\t", "YYYY.", "YYYY ", "literal only", "bad \xff YYYY",
        "YYYY-MM-DD_hh-mm-ss_YYYY-MM-DD_hh-mm-ss_YYYY-MM-DD_hh-mm-ss_YYYY-", /* 65 bytes */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        EXPECT_TRUE(!snapshot_tree_time_format_valid(bad[i]), "Unusable time format refused");
    }
    EXPECT_TRUE(!snapshot_tree_time_format_valid(NULL), "NULL refused");
    EXPECT_TRUE(snapshot_tree_time_format_valid("YYYY-MM-DD_hh-mm-ss_YYYY-MM-DD_hh-mm-ss_YYYY-MM-DD_hh-mm-ss_YYYY"),
                "64 bytes accepted");
}

/* snapshots.csv: content, quoting, sorting, formula protection */
static void test_csv(void) {
    SnapshotTree* tree = build(
        "[{\"time\":\"2026-10-19T08:00:00+02:00\",\"id\":\"" ID("b") "\",\"hostname\":\"pc,1\","
        "\"paths\":[\"/home\",\"/etc\"],\"tags\":[\"say \\\"hi\\\"\",\"=1+2\"],"
        "\"summary\":{\"total_files_processed\":3,\"total_bytes_processed\":1088}},"
        "{\"time\":\"2026-10-18T07:00:00Z\",\"id\":\"" ID("a") "\",\"hostname\":\"pc,1\",\"paths\":[\"/home\"]},"
        "{\"time\":\"2026-10-17T06:00:00Z\",\"id\":\"" ID("c") "\",\"hostname\":\"alpha\","
        "\"paths\":[\"C:\\\\Users\"],\"tags\":[\"-daily\",\"@x\",\"plain\"],\"summary\":{\"total_bytes_processed\":1023}},"
        "{\"time\":\"2026-10-20T05:04:03-05:00\",\"id\":\"" ID("d") "\",\"hostname\":\"pc,1\",\"paths\":[\"/home\"],"
        "\"summary\":{}},"
        "{\"time\":\"2026-10-16T00:00:00Z\",\"id\":\"" ID("e") "\",\"summary\":{\"total_bytes_processed\":5368709120}}]");
    EXPECT_TRUE(tree != NULL, "Built");
    size_t len = 0;
    char* csv = snapshot_tree_csv(tree, ',', &len);
    static const char expected[] =
        "\xEF\xBB\xBF" "short ID,host,paths,size,timestamp,tags,full ID\r\n"
        "eeeeeeee,,,5.000 GiB,2026-10-16 00:00:00,," ID("e") "\r\n"              /* No host sorts first */
        "cccccccc,alpha,C:\\Users,1023 B,2026-10-17 06:00:00,\"'-daily\n'@x\nplain\"," ID("c") "\r\n"
        "aaaaaaaa,\"pc,1\",/home,,2026-10-18 07:00:00,," ID("a") "\r\n"           /* Same host and paths: by time */
        "dddddddd,\"pc,1\",/home,0 B,2026-10-20 05:04:03,," ID("d") "\r\n"        /* A summary without the size */
        "bbbbbbbb,\"pc,1\",\"/home\n/etc\",1.062 KiB,2026-10-19 08:00:00,\"say \"\"hi\"\"\n'=1+2\"," ID("b") "\r\n"; /* Paths after "/home" */
    if (!csv || strcmp(csv, expected) != 0) printf("       got: %s\n", csv ? csv : "(null)");
    EXPECT_TRUE(csv && strcmp(csv, expected) == 0 && len == strlen(expected),
                "BOM, header, CRLF; sorted by host, paths, time; quoting, doubled quotes, lines in a field, the size as "
                "restic prints it (none without a summary), the full ID last, "
                "a formula character at the start of a value or of a line in it defused");
    free(csv);
    csv = snapshot_tree_csv(tree, ';', &len);
    EXPECT_TRUE(csv && strstr(csv, "short ID;host;paths;size;timestamp;tags;full ID\r\n") && strstr(csv, ";pc,1;/home;") &&
                strstr(csv, "eeeeeeee;;;5.000 GiB;2026-10-16 00:00:00;;" ID("e") "\r\n"),
                "Another separator; a comma then needs no quotes");
    free(csv);
    snapshot_tree_free(tree);

    /* Read with another separator, a comma or a quote in a value could start a cell there */
    tree = build("[{\"time\":\"2026-10-19T08:00:00Z\",\"id\":\"" ID("f") "\",\"hostname\":\"x,=HYPERLINK(1)\","
                 "\"paths\":[\"/a|+b;-c\\t@d\"],\"tags\":[\"q\\\"=1\",\"a - b\"]}]");
    csv = snapshot_tree_csv(tree, ';', &len);
    EXPECT_TRUE(csv && strstr(csv, "\r\nffffffff;x,'=HYPERLINK(1);\"/a|'+b;'-c\t'@d\";;2026-10-19 08:00:00;"
                                   "\"q\"\"'=1\na - b\";" ID("f") "\r\n"),
                "A formula character after , ; | tab or a quote defused too; one after a space is left alone");
    free(csv);
    snapshot_tree_free(tree);

    /* Hosts and paths ignore ASCII case: "alpha" before "Zeta", "/archive" before "/Data" */
    tree = build("[{\"time\":\"2026-10-19T08:00:00Z\",\"id\":\"" ID("1") "\",\"hostname\":\"Zeta\",\"paths\":[\"/x\"]},"
                 "{\"time\":\"2026-10-19T08:00:00Z\",\"id\":\"" ID("2") "\",\"hostname\":\"alpha\",\"paths\":[\"/Data\"]},"
                 "{\"time\":\"2026-10-19T08:00:00Z\",\"id\":\"" ID("3") "\",\"hostname\":\"ALPHA\",\"paths\":[\"/archive\"]}]");
    csv = snapshot_tree_csv(tree, ',', &len);
    const char* a3 = csv ? strstr(csv, "\r\n33333333,") : NULL;
    const char* a2 = csv ? strstr(csv, "\r\n22222222,") : NULL;
    const char* z1 = csv ? strstr(csv, "\r\n11111111,") : NULL;
    EXPECT_TRUE(a3 && a2 && z1 && a3 < a2 && a2 < z1, "Sorted by host and paths ignoring case");
    free(csv);
    EXPECT_TRUE(snapshot_tree_listed_at(tree) == 0, "No listing time until it is set");
    snapshot_tree_set_listed_at(tree, 133000000000000000ull);
    EXPECT_TRUE(snapshot_tree_listed_at(tree) == 133000000000000000ull, "The listing time as set");
    snapshot_tree_free(tree);

    tree = build("[]");
    EXPECT_TRUE(snapshot_tree_csv(tree, '"', &len) == NULL && snapshot_tree_csv(tree, '\n', &len) == NULL &&
                snapshot_tree_csv(NULL, ',', &len) == NULL, "Unusable separators refused");
    snapshot_tree_free(tree);

    tree = build("null");
    csv = snapshot_tree_csv(tree, ',', &len);
    EXPECT_TRUE(csv && strcmp(csv, "\xEF\xBB\xBF" "short ID,host,paths,size,timestamp,tags,full ID\r\n") == 0,
                "An empty repository has the header only");
    free(csv);
    snapshot_tree_free(tree);
}

/* The size column against restic's ui.FormatBytes: unit limits, three decimals rounded half to even */
static void test_csv_size(void) {
    static const struct {
        const char* bytes; /* total_bytes_processed as JSON; NULL: no summary */
        const char* text;
    } cases[] = {
        {"0", "0 B"},
        {"1023", "1023 B"},
        {"1024", "1.000 KiB"},
        {"1088", "1.062 KiB"},       /* 1.0625: a tie, rounded to the even digit */
        {"1216", "1.188 KiB"},       /* 1.1875: a tie, rounded to the even digit */
        {"1048575", "1023.999 KiB"},
        {"1048576", "1.000 MiB"},
        {"1073741823", "1024.000 MiB"}, /* Rounding carries into the whole number, as in restic */
        {"1073741824", "1.000 GiB"},
        {"1099511627776", "1.000 TiB"},
        {"1125899906842624", "1024.000 TiB"}, /* No unit above TiB */
        {"18446744073709551615", "16777216.000 TiB"},
        {"-1", ""}, /* Not a size: unknown */
        {"\"1024\"", ""},
        {"1e3", ""},
        {"18446744073709551616", ""},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char json[256], expected[128];
        snprintf(json, sizeof(json),
                 "[{\"time\":\"2026-10-19T08:00:00Z\",\"id\":\"" ID("a") "\",\"summary\":{\"total_bytes_processed\":%s}}]",
                 cases[i].bytes);
        snprintf(expected, sizeof(expected), "\r\naaaaaaaa,,,%s,2026-10-19 08:00:00,,", cases[i].text);
        SnapshotTree* tree = build(json);
        size_t len = 0;
        char* csv = tree ? snapshot_tree_csv(tree, ',', &len) : NULL;
        if (!csv || !strstr(csv, expected)) printf("       %s: got %s\n", cases[i].bytes, csv ? csv : "(null)");
        EXPECT_TRUE(csv && strstr(csv, expected), "Size written as restic snapshots prints it");
        free(csv);
        snapshot_tree_free(tree);
    }
    SnapshotTree* tree = build("[{\"time\":\"2026-10-19T08:00:00Z\",\"id\":\"" ID("a") "\",\"summary\":null}]");
    size_t len = 0;
    char* csv = tree ? snapshot_tree_csv(tree, ',', &len) : NULL;
    EXPECT_TRUE(csv && strstr(csv, "\r\naaaaaaaa,,,,2026-10-19"), "A summary that is not an object gives no size");
    free(csv);
    snapshot_tree_free(tree);
}

static void test_skipped_entries(void) {
    SnapshotTree* tree = build(
        "[1,\"x\",null,{},"
        "{\"time\":\"2026-10-18T10:00:00Z\"},"
        "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"1234567\"},"
        "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"12345678g\"},"
        "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"ABCDEF1234\"},"
        "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"" ID("1") "1\"},"
        "{\"time\":\"2026-02-30T10:00:00Z\",\"id\":\"" ID("2") "\"},"
        "{\"time\":17,\"id\":\"" ID("3") "\"},"
        "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"deadbeef\",\"tags\":\"notalist\"}]");
    EXPECT_TRUE(tree != NULL, "Built");
    EXPECT_TRUE(snapshot_tree_snapshot_count(tree) == 1 && strcmp(names(walk(tree, "ids")), "deadbeef") == 0,
                "Only the entry with a valid time and a lowercase hex ID of 8-64 characters is used");
    EXPECT_TRUE(walk(tree, "tags")->child_count == 0, "Tags that are not an array are ignored");
    snapshot_tree_free(tree);
}

/* 20 000 snapshots: insertion stays fast (hash index) and every name stays unique */
static void test_many_snapshots(void) {
    size_t n = 20000, cap = n * 200 + 16;
    char* json = (char*)malloc(cap);
    EXPECT_TRUE(json != NULL, "Allocate");
    size_t len = 0;
    json[len++] = '[';
    for (size_t i = 0; i < n; ++i) {
        size_t avail = cap - len;
        int wrote = snprintf(json + len, avail,
                             "%s{\"time\":\"2026-10-%02uT%02u:00:00Z\",\"id\":\"%08x%056x\",\"hostname\":\"h%u\",\"tags\":[\"t\"]}",
                             i ? "," : "", (unsigned)(i % 28 + 1), (unsigned)(i % 3), (unsigned)(i % 1000),
                             (unsigned)i, (unsigned)(i % 7));
        if (wrote < 0 || (size_t)wrote >= avail) {
            free(json);
            EXPECT_TRUE(0, "JSON buffer overflow while formatting test data");
        }
        len += (size_t)wrote;
    }
    if (cap - len < 2) {
        free(json);
        EXPECT_TRUE(0, "JSON buffer overflow while finalizing test data");
    }
    json[len++] = ']';
    json[len] = '\0';
    SnapshotTree* tree = NULL;
    EXPECT_TRUE(snapshot_tree_build(json, strlen(json), NULL, &tree) == SNAPSHOT_TREE_OK, "Built");
    free(json);
    EXPECT_TRUE(snapshot_tree_snapshot_count(tree) == n, "All snapshots");
    const SnapshotNode* ids = walk(tree, "ids");
    EXPECT_TRUE(ids->child_count == n, "One ids/ entry each, colliding short IDs numbered");
    int sorted = 1;
    for (size_t i = 1; i < ids->child_count; ++i) sorted &= strcmp(ids->children[i - 1]->name, ids->children[i]->name) < 0;
    EXPECT_TRUE(sorted, "Children sorted and unique");
    EXPECT_TRUE(walk(tree, "snapshots")->child_count == n + 3 && walk(tree, "tags/t")->child_count == n + 3,
                "snapshots/ and tags/t/ hold every snapshot plus latest and its two aliases");
    EXPECT_TRUE(walk(tree, "hosts")->child_count == 7, "Seven hosts");
    EXPECT_TRUE(snapshot_tree_child(ids, "000003e7") != NULL && snapshot_tree_child(ids, "000003e7-19") != NULL &&
                snapshot_tree_child(ids, "000003e7-20") == NULL, "Twenty snapshots share each short ID");
    snapshot_tree_free(tree);
}

/* ------------------------------------------------------------------------------------------ */

static SnapshotIndexStatus add(SnapshotIndex* x, const char* line) {
    return snapshot_index_add_line(x, line, strlen(line));
}

/* Names of a folder's entries joined by '|', folders marked with a trailing '/' */
static const char* index_names(const SnapshotIndex* x, const char* path) {
    static char buf[2048];
    buf[0] = '\0';
    long long dir = snapshot_index_find_dir(x, path);
    if (dir < 0) return "<none>";
    for (size_t i = 0; i < snapshot_index_child_count(x, dir); ++i) {
        SnapshotIndexEntry e = snapshot_index_child(x, dir, i);
        if (i) append(buf, sizeof(buf), "|");
        append(buf, sizeof(buf), e.name);
        if (e.is_dir) append(buf, sizeof(buf), "/");
    }
    return buf;
}

#define NODE(path, name, type, extra) \
    "{\"name\":\"" name "\",\"type\":\"" type "\",\"path\":\"" path "\"" extra ",\"message_type\":\"node\"}"

/* restic's depth-first order: every folder is followed by its whole subtree */
static void test_index_listing(void) {
    SnapshotIndex* x = snapshot_index_new(0);
    EXPECT_TRUE(x != NULL, "Created");
    static const char* const lines[] = {
        "{\"time\":\"2026-10-18T10:00:00Z\",\"id\":\"abcd\",\"message_type\":\"snapshot\"}\r\n",
        "",
        "   \r",
        NODE("/home", "home", "dir", ",\"mtime\":\"2026-01-02T03:04:05+01:00\""),
        NODE("/home/alice", "alice", "dir", ""),
        NODE("/home/alice/a.txt", "a.txt", "file", ",\"size\":5,\"mtime\":\"2020-02-29T12:34:56Z\",\"atime\":\"bad\""),
        NODE("/home/alice/big.bin", "big.bin", "file", ",\"size\":18446744073709551615"),
        NODE("/home/alice/link", "link", "symlink", ",\"linktarget\":\"a.txt\""),
        NODE("/home/alice/sub", "sub", "dir", ""),
        NODE("/home/alice/sub/deep.txt", "deep.txt", "file", ",\"size\":1"),
        NODE("/home/alice/z", "z", "dir", ""),
        NODE("/home/bob", "bob", "dir", ""),
        NODE("/home/bob/..\\\\..\\\\x", "..\\\\..\\\\x", "file", ""), /* JSON "..\\..\\x" */
        NODE("/tmp", "tmp", "dir", ""),
        NODE("/zz.txt", "zz.txt", "file", ",\"size\":0"),
        "{\"message_type\":\"summary\",\"total_files\":5}",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        EXPECT_TRUE(add(x, lines[i]) == SNAPSHOT_INDEX_OK, "Line added");
    }
    EXPECT_TRUE(snapshot_index_find_dir(x, "/") == -1, "Not usable before it is finished");
    EXPECT_TRUE(snapshot_index_finish(x) == SNAPSHOT_INDEX_OK, "Finished");
    EXPECT_TRUE(snapshot_index_entry_count(x) == 12, "Twelve entries");
    EXPECT_TRUE(strcmp(index_names(x, "/"), "home/|tmp/|zz.txt") == 0, "Root");
    EXPECT_TRUE(strcmp(index_names(x, "/home/alice"), "a.txt|big.bin|link|sub/|z/") == 0,
                "Direct children only, in restic's order");
    EXPECT_TRUE(strcmp(index_names(x, "/home/alice/sub"), "deep.txt") == 0, "Nested folder");
    EXPECT_TRUE(strcmp(index_names(x, "/home/alice/z"), "") == 0, "Empty folder exists");
    EXPECT_TRUE(strcmp(index_names(x, "/home/bob"), "..\\..\\x") == 0, "Names kept as restic reports them");
    EXPECT_TRUE(snapshot_index_find_dir(x, "/home/alice/a.txt") == -1, "A file is not a folder");
    EXPECT_TRUE(snapshot_index_find_dir(x, "/home/carol") == -1 && snapshot_index_find_dir(x, "/hom") == -1 &&
                snapshot_index_find_dir(x, "/home/alice/") == -1 && snapshot_index_find_dir(x, "home") == -1 &&
                snapshot_index_find_dir(x, "//home") == -1, "Missing or malformed paths");

    long long alice = snapshot_index_find_dir(x, "/home/alice");
    SnapshotIndexEntry a = snapshot_index_child(x, alice, 0);
    EXPECT_TRUE(!a.is_dir && a.size == 5 && a.mtime == ticks_utc(2020, 2, 29, 12, 34, 56) &&
                a.atime == SNAPSHOT_INDEX_NO_TIME && a.ctime == SNAPSHOT_INDEX_NO_TIME, "File fields, bad and missing times");
    EXPECT_TRUE(snapshot_index_child(x, alice, 1).size == 18446744073709551615ULL, "64-bit size kept exactly");
    EXPECT_TRUE(snapshot_index_child(x, snapshot_index_find_dir(x, "/"), 0).mtime == ticks_utc(2026, 1, 2, 2, 4, 5),
                "Folder time in UTC");
    EXPECT_TRUE(snapshot_index_child(x, alice, 99).name == NULL && snapshot_index_child_count(x, -1) == 0,
                "Out of range");
    EXPECT_TRUE(add(x, NODE("/late", "late", "file", "")) == SNAPSHOT_INDEX_BAD_JSON, "No lines after finishing");
    snapshot_index_free(x);
    snapshot_index_free(NULL);
}

static void test_index_errors(void) {
    SnapshotIndex* x = snapshot_index_new(0);
    EXPECT_TRUE(add(x, "not json") == SNAPSHOT_INDEX_BAD_JSON, "Malformed line");
    EXPECT_TRUE(add(x, NODE("/a", "a", "file", "")) == SNAPSHOT_INDEX_BAD_JSON, "The error sticks");
    EXPECT_TRUE(snapshot_index_finish(x) == SNAPSHOT_INDEX_BAD_JSON, "And finishing reports it");
    snapshot_index_free(x);

    static const char* const bad[] = {
        "[1]", NODE("relative", "x", "file", ""), NODE("/", "x", "dir", ""), NODE("/a/", "a", "dir", ""),
        "{\"message_type\":\"node\",\"name\":\"x\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        x = snapshot_index_new(0);
        EXPECT_TRUE(add(x, bad[i]) == SNAPSHOT_INDEX_BAD_JSON, "Unusable node refused");
        snapshot_index_free(x);
    }

    /* A node whose folder restic never listed is skipped; so is one below a file */
    x = snapshot_index_new(0);
    EXPECT_TRUE(add(x, NODE("/a/b", "b", "file", "")) == SNAPSHOT_INDEX_OK &&
                add(x, NODE("/f", "f", "file", "")) == SNAPSHOT_INDEX_OK &&
                add(x, NODE("/f/x", "x", "file", "")) == SNAPSHOT_INDEX_OK &&
                add(x, NODE("/d", "d", "dir", "")) == SNAPSHOT_INDEX_OK &&
                add(x, NODE("/e/y", "y", "file", "")) == SNAPSHOT_INDEX_OK &&
                add(x, NODE("/d/z", "z", "file", "")) == SNAPSHOT_INDEX_OK, "Orphans ignored");
    EXPECT_TRUE(snapshot_index_finish(x) == SNAPSHOT_INDEX_OK && snapshot_index_entry_count(x) == 3 &&
                strcmp(index_names(x, "/"), "f|d/") == 0 && strcmp(index_names(x, "/d"), "z") == 0,
                "Only entries below listed folders, unsorted names still found");
    snapshot_index_free(x);

    /* The memory limit */
    x = snapshot_index_new(16 * 1024);
    SnapshotIndexStatus st = SNAPSHOT_INDEX_OK;
    char line[256];
    for (int i = 0; i < 10000 && st == SNAPSHOT_INDEX_OK; ++i) {
        snprintf(line, sizeof(line), "{\"name\":\"f%05d\",\"type\":\"file\",\"path\":\"/f%05d\",\"message_type\":\"node\"}", i, i);
        st = add(x, line);
    }
    EXPECT_TRUE(st == SNAPSHOT_INDEX_TOO_LARGE && snapshot_index_memory(x) <= 16 * 1024, "Growth stops at the limit");
    EXPECT_TRUE(snapshot_index_finish(x) == SNAPSHOT_INDEX_TOO_LARGE, "And stays failed");
    snapshot_index_free(x);
}

/* 100 000 entries in one folder: binary search finds every one of them */
static void test_index_large_folder(void) {
    SnapshotIndex* x = snapshot_index_new(0);
    char line[256];
    EXPECT_TRUE(add(x, NODE("/d", "d", "dir", "")) == SNAPSHOT_INDEX_OK, "Folder");
    for (int i = 0; i < 100000; ++i) {
        snprintf(line, sizeof(line), "{\"name\":\"%06d\",\"type\":\"dir\",\"path\":\"/d/%06d\",\"message_type\":\"node\"}", i, i);
        if (add(x, line) != SNAPSHOT_INDEX_OK) break;
    }
    EXPECT_TRUE(snapshot_index_finish(x) == SNAPSHOT_INDEX_OK && snapshot_index_entry_count(x) == 100001, "Built");
    int found = 1;
    for (int i = 0; i < 100000; i += 7) {
        snprintf(line, sizeof(line), "/d/%06d", i);
        found &= snapshot_index_find_dir(x, line) > 0;
    }
    EXPECT_TRUE(found && snapshot_index_find_dir(x, "/d/100000") == -1, "Every folder found, a missing one not");
    snapshot_index_free(x);
}

/* The properties text of a snapshot, from what restic 0.19.1 prints (restic snapshots --json) */
static void test_describe(void) {
    SnapshotTree* tree = build(
        "[{\"time\":\"2026-10-24T22:15:20.490648083+02:00\",\"tree\":\"23b1\",\"paths\":[\"/data\",\"/etc\"],"
        "\"hostname\":\"pc1\",\"username\":\"agent\",\"uid\":1000,\"gid\":1000,\"program_version\":\"restic 0.19.1\","
        "\"tags\":[\"daily\"],\"parent\":\"" ID("a") "\","
        "\"summary\":{\"backup_start\":\"2026-10-24T22:15:20.490648083+02:00\",\"backup_end\":\"2026-10-24T22:17:25.154510884+02:00\","
        "\"files_new\":2,\"files_changed\":1,\"files_unmodified\":7,\"dirs_new\":2,\"dirs_changed\":0,\"dirs_unmodified\":3,"
        "\"data_blobs\":2,\"tree_blobs\":3,\"data_added\":1924,\"data_added_packed\":1380,\"total_files_processed\":10,"
        "\"total_bytes_processed\":1088},\"id\":\"" ID("b") "\",\"short_id\":\"bbbbbbbb\"},"
        "{\"time\":\"2026-10-23T08:00:00Z\",\"hostname\":\"pc2\",\"paths\":[\"/x\"],\"id\":\"" ID("c") "\"}]");
    EXPECT_TRUE(tree != NULL, "Built");
    char* text = snapshot_tree_describe(tree, ID("b"));
    static const char expected[] =
        "Snapshot bbbbbbbb of pc1\n"
        "ID: " ID("b") "\n"
        "Time: 2026-10-24 22:15:20 +0200\n"
        "User: agent\n"
        "Paths: /data\n    /etc\n"
        "Tags: daily\n"
        "Size: 1.062 KiB in 10 files\n"
        "Files: 2 new, 1 changed, 7 unmodified\n"
        "Folders: 2 new, 0 changed, 3 unmodified\n"
        "Added to the repository: 1.879 KiB (1.348 KiB stored)\n"
        "Backup took: 2 min 4 s\n"
        "Parent: aaaaaaaa\n"
        "Program: restic 0.19.1\n";
    if (!text || strcmp(text, expected) != 0) printf("       got: %s\n", text ? text : "(null)");
    EXPECT_TRUE(text && strcmp(text, expected) == 0, "Every detail restic recorded, a line each");
    free(text);
    text = snapshot_tree_describe(tree, ID("c"));
    EXPECT_TRUE(text && strcmp(text, "Snapshot cccccccc of pc2\nID: " ID("c") "\nTime: 2026-10-23 08:00:00 +0000\n"
                                     "Paths: /x\n") == 0,
                "A snapshot without a summary (restic before 0.17): only what it has");
    free(text);
    EXPECT_TRUE(snapshot_tree_describe(tree, ID("d")) == NULL && snapshot_tree_describe(tree, NULL) == NULL,
                "No such snapshot");
    Timestamp t;
    EXPECT_TRUE(snapshot_tree_snapshot_time(tree, ID("c"), &t) && t.day == 23 && !snapshot_tree_snapshot_time(tree, ID("d"), &t),
                "A snapshot's time by its ID");
    EXPECT_TRUE(strcmp(snapshot_tree_snapshot_host(tree, ID("b")), "pc1") == 0 && !snapshot_tree_snapshot_host(tree, ID("d")),
                "A snapshot's host by its ID");
    snapshot_tree_free(tree);
}

/* One snapshot of restic find --json output with one match */
#define FIND_MATCH(snapshot, path, type, size, mtime) \
    "{\"matches\":[{\"path\":\"" path "\",\"permissions\":\"-rw-------\",\"type\":\"" type "\",\"mode\":384," \
    "\"mtime\":\"" mtime "\",\"uid\":1000,\"gid\":1000,\"size\":" size "}],\"hits\":1,\"snapshot\":\"" snapshot "\"}"

/* The versions of a file across snapshots, from restic find --json (the shape restic 0.19.1 prints) */
static void test_file_history(void) {
    /* Youngest snapshot first, as restic find lists them */
    static const char json[] =
        "[" FIND_MATCH(ID("4"), "/data/report.txt", "file", "11", "2026-10-24T22:15:22.159055432+02:00") ","
        FIND_MATCH(ID("3"), "/data/report.txt", "file", "11", "2026-10-24T22:15:22.159055432+02:00") ","
        "{\"matches\":[{\"path\":\"/data/report.txt\",\"type\":\"dir\",\"mtime\":\"2026-10-21T10:00:00Z\"},"
        "{\"path\":\"/data/report.txt/inner\",\"type\":\"file\",\"size\":3,\"mtime\":\"2026-10-21T10:00:00Z\"}],"
        "\"hits\":2,\"snapshot\":\"" ID("2") "\"},"
        FIND_MATCH(ID("1"), "/data/report.txt", "file", "4", "2026-10-20T08:00:00.5+02:00") ","
        FIND_MATCH(ID("0"), "/data/report.txt", "file", "5", "2026-10-20T08:00:00.7+02:00") "]";
    FileHistory* h = NULL;
    EXPECT_TRUE(file_history_build(json, strlen(json), "/data/report.txt", ID("3"), NULL, &h) == FILE_HISTORY_OK && h,
                "Built");
    EXPECT_TRUE(file_history_count(h) == 3, "Three versions: the folder and what lies below it are not one");
    const FileVersion* v0 = file_history_version(h, 0);
    const FileVersion* v1 = file_history_version(h, 1);
    const FileVersion* v2 = file_history_version(h, 2);
    EXPECT_TRUE(strcmp(v0->name, "2026-10-20_08-00-00 (11111111) report.txt") == 0 && v0->size == 4 &&
                    strcmp(v0->snapshot_id, ID("1")) == 0 && v0->snapshot_count == 1,
                "Oldest first, named by the modification time in the file's own zone and the snapshot");
    EXPECT_TRUE(strcmp(v1->name, "2026-10-20_08-00-00 (00000000) report.txt") == 0 && v1->size == 5,
                "A second version of the same second: told apart by its snapshot");
    EXPECT_TRUE(strcmp(v2->name, "2026-10-24_22-15-22 (33333333) report.txt") == 0 && v2->size == 11 &&
                    v2->snapshot_count == 2 && strcmp(v2->snapshot_id, ID("3")) == 0 && strcmp(v2->type, "file") == 0,
                "The same size and time in two snapshots is one version, named after and dumped from the oldest");
    EXPECT_TRUE(strcmp(file_history_snapshot(h, 2, 0), ID("4")) == 0 && strcmp(file_history_snapshot(h, 2, 1), ID("3")) == 0 &&
                    file_history_snapshot(h, 2, 2) == NULL && strcmp(file_history_snapshot(h, 0, 0), ID("1")) == 0,
                "The snapshots of each version, youngest first");
    EXPECT_TRUE(file_history_find(h, "2026-10-24_22-15-22 (33333333) report.txt") == v2 &&
                    !file_history_find(h, "2026-10-24_22-15-22 report.txt") && !file_history_find(h, "report.txt"),
                "A version found by its name");
    file_history_free(h);

    EXPECT_TRUE(file_history_build(json, strlen(json), "/data/report.txt", ID("2"), NULL, &h) == FILE_HISTORY_NOT_A_FILE &&
                    !h,
                "In a snapshot where the path is a folder, there is no history of a file");
    EXPECT_TRUE(file_history_build("[]", 2, "/data/report.txt", ID("3"), NULL, &h) == FILE_HISTORY_NOT_A_FILE,
                "Nothing found");
    EXPECT_TRUE(file_history_build(json, strlen(json), "/data/report.txt", NULL, "YYYY-MM-DD", &h) == FILE_HISTORY_OK &&
                    strcmp(file_history_version(h, 1)->name, "2026-10-20 (00000000) report.txt") == 0,
                "The time format of the snapshot folders");
    file_history_free(h);
    /* Two snapshots whose IDs share the short ID, two versions of the same second */
    static const char same_short[] =
        "[" FIND_MATCH("1111111122", "/data/report.txt", "file", "5", "2026-10-20T08:00:00.7Z") ","
        FIND_MATCH("1111111133", "/data/report.txt", "file", "4", "2026-10-20T08:00:00.5Z") "]";
    EXPECT_TRUE(file_history_build(same_short, strlen(same_short), "/data/report.txt", NULL, NULL, &h) == FILE_HISTORY_OK &&
                    strcmp(file_history_version(h, 0)->name, "2026-10-20_08-00-00 (11111111) report.txt") == 0 &&
                    strcmp(file_history_version(h, 1)->name, "2026-10-20_08-00-00-1 (11111111) report.txt") == 0,
                "The same time and short ID: -1 after the time");
    file_history_free(h);
    static const char* const bad[] = {"{}", "[{\"matches\":[]}]", "[{\"matches\":{},\"snapshot\":\"" ID("1") "\"}]",
                                      "[{\"matches\":[],\"snapshot\":\"XYZ\"}]", "[", ""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        EXPECT_TRUE(file_history_build(bad[i], strlen(bad[i]), "/a", NULL, NULL, &h) == FILE_HISTORY_BAD_JSON && !h,
                    "Not restic find's output");
    }

    /* A long name is cut so that the version's name stays a valid component (255 UTF-16 units) */
    static char long_json[2048], long_path[600];
    char name[300];
    memset(name, 'n', 250);
    memcpy(name + 250, "\xc3\xa9.txt", 7); /* An e with acute: two bytes, one unit */
    snprintf(long_path, sizeof(long_path), "/%s", name);
    snprintf(long_json, sizeof(long_json),
             "[{\"matches\":[{\"path\":\"%s\",\"type\":\"file\",\"size\":1,\"mtime\":\"2026-10-20T08:00:00Z\"}],"
             "\"snapshot\":\"" ID("1") "\"}]", long_path);
    EXPECT_TRUE(file_history_build(long_json, strlen(long_json), long_path, NULL, NULL, &h) == FILE_HISTORY_OK,
                "A long name");
    const char* n = file_history_version(h, 0)->name;
    EXPECT_TRUE(strlen(n) == 31 + 224 && strncmp(n, "2026-10-20_08-00-00 (11111111) nnnn", 35) == 0, "Cut to 255 units");
    file_history_free(h);

    char pattern[64];
    EXPECT_TRUE(file_history_pattern("/a/b*c?d[e]f", pattern, sizeof(pattern)) == 18 &&
                    strcmp(pattern, "/a/b[*]c[?]d[[]e]f") == 0,
                "Wildcards in a path matched literally");
    EXPECT_TRUE(file_history_pattern("/[[[", pattern, 10) == 0, "No room: nothing");
}

int main(void) {
#if defined(_MSC_VER) && defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    printf("Running timestamp and snapshot tree unit tests...\n");
    test_timestamp_parse();
    test_timestamp_compare();
    test_empty_repository();
    test_layout();
    test_name_collisions();
    test_sanitized_names();
    test_time_formats();
    test_csv();
    test_describe();
    test_file_history();
    test_csv_size();
    test_skipped_entries();
    test_many_snapshots();
    test_index_listing();
    test_index_errors();
    test_index_large_folder();
    printf("[SNAPSHOT TREE] %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
