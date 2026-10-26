#pragma once
#include <stddef.h>
#include <stdint.h>
#include "timestamp.h"

/*
 * The folder tree `restic mount` presents above the snapshots, built from the output of
 * `restic snapshots --json`:
 *
 *   ids/<short id>/            one folder per snapshot
 *   snapshots/<time>/          one folder per snapshot, plus latest/
 *   hosts/<host>/<time>/       per host, plus latest/ in each host
 *   tags/<tag>/<time>/         per tag (a snapshot with several tags is in each), plus latest/
 *
 * This is restic mount's default set of path templates ("ids/%i", "snapshots/%T", "hosts/%h/%T",
 * "tags/%t/%T"). The time is written by a configurable format (snapshot_tree_build), by default
 * restic's own colon-free time template "2006-01-02_15-04-05" (see `restic help mount`), because
 * Windows file names cannot contain the colons of the default RFC 3339 one. As in
 * restic (internal/fuse/snapshots_dirstruct.go):
 * - snapshots are inserted in ascending order of time, ties broken by ID, and a name already taken
 *   in a folder gets "-1", "-2", ... appended, so the older snapshot keeps the plain name;
 * - "latest" in a folder of snapshots is the one inserted last with the newest time; next to it
 *   "latest → <folder>" (restic's link target, the snapshot's own folder name) and
 *   "latest → <short id>" show the same snapshot and tell which one it is (restic's latest is a
 *   symlink, whose target Total Commander would not show);
 * - the time is the snapshot's own wall-clock time (the zone it was taken in), and the four
 *   top-level folders exist even when the repository has no snapshots.
 * Host names and tags become a single Windows-safe path component the way restic's filenameFromTag
 * treats tags ("" and "." become "_", ".." becomes "__", "/" becomes "_"), extended to characters
 * Windows does not allow in file names (\ : * ? " < > | and control characters), trailing dots and
 * spaces (which Win32 strips), invalid UTF-8 and names longer than 255 UTF-16 units.
 *
 * Portable C11 with no Win32 dependency, like json.c, so it can be tested on any platform.
 */

/* What restic prints as a snapshot's short ID and uses for ids/ */
#define SNAPSHOT_TREE_SHORT_ID_CHARS 8
/* A full snapshot ID: 64 hexadecimal characters */
#define SNAPSHOT_TREE_ID_CHARS 64
/* "latest → " (U+2192 RIGHTWARDS ARROW between spaces), followed by the latest snapshot's folder name
   or short ID */
#define SNAPSHOT_TREE_LATEST_LINK_PREFIX "latest \xe2\x86\x92 "

/*
 * Time format of the snapshot folders: placeholders YYYY (year), YY (year without century), MM
 * (month), DD (day), hh (hour, 00-23), mm (minute), ss (second), all zero-padded, and zzzz (the
 * zone offset, "+0200", "-0500", "+0000"), case-sensitive; everything else is copied as it is.
 * The time is the snapshot's own wall-clock time, in the zone it was taken in.
 */
#define SNAPSHOT_TREE_DEFAULT_TIME_FORMAT "YYYY-MM-DD_hh-mm-ss" /* restic's 2006-01-02_15-04-05 */
#define SNAPSHOT_TREE_TIME_FORMAT_MAX 64                           /* Bytes of UTF-8 */

/*
 * 1 when `format` can name folders: valid UTF-8 of 1 to SNAPSHOT_TREE_TIME_FORMAT_MAX bytes with at
 * least one placeholder, none of \ / : * ? " < > | or control characters, not ending in a dot or
 * space (Win32 strips those)
 */
int snapshot_tree_time_format_valid(const char* format);

typedef struct SnapshotNode SnapshotNode;
struct SnapshotNode {
    const char* name;                    /* UTF-8, one Windows-safe path component */
    uint64_t time;                       /* FILETIME ticks (UTC): the snapshot's time, or the newest
                                            one below a folder; 0 for a folder without snapshots */
    const char* snapshot_id;             /* Full ID when this folder shows a snapshot, else NULL */
    const SnapshotNode* const* children; /* Sorted by strcmp() of the names; none for a snapshot */
    size_t child_count;
};

typedef enum {
    SNAPSHOT_TREE_OK = 0,
    SNAPSHOT_TREE_BAD_JSON,  /* Not the JSON array (or null) `restic snapshots --json` prints */
    SNAPSHOT_TREE_NO_MEMORY,
    SNAPSHOT_TREE_BAD_FORMAT /* time_format fails snapshot_tree_time_format_valid() */
} SnapshotTreeStatus;

typedef struct SnapshotTree SnapshotTree;

/*
 * Builds the tree from `len` bytes of `restic snapshots --json` output (a literal null is an empty
 * repository), naming the snapshot folders by `time_format` (NULL: the default). Entries without
 * a valid time or a lowercase hexadecimal ID of 8 to 64 characters are skipped. The tree keeps a
 * copy of the output (snapshot_tree_json). Release the tree with snapshot_tree_free().
 */
SnapshotTreeStatus snapshot_tree_build(const char* json, size_t len, const char* time_format, SnapshotTree** out);

/* The file listing the snapshots, in a repository's folder beside ids, snapshots, hosts and tags */
#define SNAPSHOT_TREE_CSV_NAME "snapshots.csv"

/*
 * The snapshots as CSV text for spreadsheets (heap, NUL-terminated; `*out_len` bytes without the
 * NUL), or NULL when out of memory or for a separator that cannot separate (NUL, quote, line
 * break): a UTF-8 byte-order mark, then RFC 4180 records with CRLF line ends and `separator`
 * between the fields "short ID", host, paths, size (as `restic snapshots` prints it, "1.234 GiB";
 * empty for a snapshot without a summary), timestamp ("YYYY-MM-DD hh:mm:ss", the snapshot's own
 * clock time), tags and "full ID", after a header line; host, paths and tags as restic reports
 * them, not made Windows-safe, several paths or tags one per line in their field. Sorted by host,
 * then paths (both ignoring ASCII case), then time. A field is quoted when it holds the separator,
 * a quote or a line break (quotes doubled). A spreadsheet formula character (= + - @ tab CR) gets an
 * apostrophe before it at the start of a value and after any of , ; | tab LF CR " - wherever a
 * reader, also one splitting at another separator, could start a cell with it.
 */
char* snapshot_tree_csv(const SnapshotTree* tree, char separator, size_t* out_len);

/* The file holding restic's own list of the snapshots, beside SNAPSHOT_TREE_CSV_NAME */
#define SNAPSHOT_TREE_JSON_NAME "snapshots.json"

/*
 * The `restic snapshots --json` output the tree was built from, byte for byte (`*out_len` bytes,
 * followed by a NUL), owned by the tree; every snapshot restic listed, also those the tree skips
 */
const char* snapshot_tree_json(const SnapshotTree* tree, size_t* out_len);

/* The time of the snapshot with this full ID into *out; 0 when the tree has no such snapshot */
int snapshot_tree_snapshot_time(const SnapshotTree* tree, const char* id, Timestamp* out);

/* The host name of the snapshot with this full ID, as restic recorded it ("" without one); NULL when there is no such snapshot */
const char* snapshot_tree_snapshot_host(const SnapshotTree* tree, const char* id);

/*
 * The details of the snapshot with this full ID as UTF-8 text for a properties dialog, a line
 * each ("ID: ...", "Time: ...", "Paths: ...", the summary's size, counts and duration, parent,
 * program), lines missing what restic did not record; several paths or tags one per indented line;
 * control characters of the repository's content as '?', so that it cannot make lines of its own.
 * Heap, NUL-terminated; NULL when the tree has no such snapshot or when out of memory.
 */
char* snapshot_tree_describe(const SnapshotTree* tree, const char* id);

/*
 * `t` written by a time format (snapshot_tree_time_format_valid() or any text of placeholders and
 * literals, of at most SNAPSHOT_TREE_TIME_FORMAT_MAX bytes) into `out`, which holds
 * SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 1 bytes
 */
void snapshot_tree_format_time(const Timestamp* t, const char* format, char* out);

/* A size as restic writes it ("1023 B", "1.062 KiB"), as in snapshots.csv */
void snapshot_tree_format_size(uint64_t bytes, char* out, size_t cap);

/* Releases a tree and every node obtained from it. NULL is accepted. */
void snapshot_tree_free(SnapshotTree* tree);

/* The folder holding ids, snapshots, hosts and tags */
const SnapshotNode* snapshot_tree_root(const SnapshotTree* tree);

/* Number of snapshots in the tree */
size_t snapshot_tree_snapshot_count(const SnapshotTree* tree);

/* When restic was asked for the snapshots, as FILETIME ticks (UTC): the time of snapshots.csv; 0 unset */
void snapshot_tree_set_listed_at(SnapshotTree* tree, uint64_t time);
uint64_t snapshot_tree_listed_at(const SnapshotTree* tree);

/*
 * The child of `dir` named `name` (UTF-8); an exact match first, otherwise the first one equal
 * ignoring ASCII case (a path typed into Total Commander). NULL if there is none, or if `dir` is a
 * snapshot.
 */
const SnapshotNode* snapshot_tree_child(const SnapshotNode* dir, const char* name);

/*
 * Network shares inside a snapshot
 *
 * restic names the top folder of a backed-up Windows path after its volume (pathComponents in its
 * internal/archiver/tree.go): "C:\Users" is stored below "/C", but "\\server\share\dir" below
 * one folder named "\\server\share" and "\\?\UNC\server\share\dir" below
 * "\\?\UNC\server\share", backslashes and all. A name holding a separator is never listed (it
 * would be several components on Windows), so such a folder is shown as UNC\<server>\<share>
 * instead, as `restic mount` shows "/C" for "C:", and the paths below it are mapped back for
 * restic. Which volumes a snapshot holds comes from the paths it backed up (`restic snapshots`), so
 * no restic run is needed for it. A server or share name that is empty, made only of dots and
 * spaces, or holds a control character, one of : * ? " < > | or a '/' (restic writes backslashes)
 * cannot name a folder: its volume stays hidden, as before. Volumes that differ only in ASCII case
 * or in their prefix ("\\" or "\\?\UNC\") are listed once and reached through the first of them in
 * the snapshot's paths.
 */
#define SNAPSHOT_TREE_UNC_NAME "UNC"

typedef enum {
    SNAPSHOT_UNC_NONE = 0, /* Nothing to do with the snapshot's network shares: as it is */
    SNAPSHOT_UNC_ROOT,     /* The snapshot's root ("/"), which holds network shares: also list "UNC" */
    SNAPSHOT_UNC_FOLDER,   /* "/UNC" or "/UNC/<server>", folders only the plugin shows: snapshot_tree_unc_list() */
    SNAPSHOT_UNC_MAPPED,   /* "/UNC/<server>/<share>[/...]": restic's path to it written into `out` */
    SNAPSHOT_UNC_TOO_LONG  /* SNAPSHOT_UNC_MAPPED, but `out` is too small */
} SnapshotUncKind;

/*
 * What `path`, inside the snapshot with this full ID (UTF-8, "/" or "/a/b" without a trailing '/',
 * as restic_normalize_filter_path() writes it), is to the snapshot's network shares. "UNC", the
 * server and the share are matched exactly first, otherwise ignoring ASCII case. For
 * SNAPSHOT_UNC_MAPPED `out` (`cap` bytes) receives "/<volume as restic named it>" and the rest of
 * the path ("/\\server\share/dir"); otherwise it is "". SNAPSHOT_UNC_NONE also when the tree has
 * no such snapshot or for a NULL argument.
 */
SnapshotUncKind snapshot_tree_unc_path(const SnapshotTree* tree, const char* id, const char* path, char* out,
                                       size_t cap);

/*
 * The folders in `path`, a SNAPSHOT_UNC_FOLDER of the snapshot with this full ID: the servers in
 * "/UNC", the shares of the server in "/UNC/<server>", each once (ignoring ASCII case), passed to
 * `add` (`len` bytes of UTF-8, not NUL-terminated) unless it is NULL. Returns how many there are.
 */
size_t snapshot_tree_unc_list(const SnapshotTree* tree, const char* id, const char* path,
                              void (*add)(void* ctx, const char* name, size_t len), void* ctx);

/*
 * The length of the network share volume ("\\server\share" or "\\?\UNC\server\share") that
 * `path` (`len` bytes) starts with, followed by a separator or nothing; 0 when it starts with none
 * that snapshot_tree_unc_path() would map to
 */
size_t snapshot_tree_unc_volume(const char* path, size_t len);
