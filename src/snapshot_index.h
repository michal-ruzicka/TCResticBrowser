#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * The complete file and folder tree of one snapshot, built from the NDJSON that
 * `restic ls --json <snapshot>` prints when it lists the whole snapshot (one node per line, in the
 * depth-first order restic walks the tree). Once built, any folder of the snapshot is listed
 * without running restic again.
 *
 * Lines are added one at a time as restic prints them, so the output never has to be held in
 * memory as a whole; what is kept is compact (about 60 bytes per entry plus its name). Names are
 * kept exactly as restic reports them: deciding which ones Total Commander may see is up to the
 * caller, as for a single `restic ls`.
 *
 * Portable C11 with no Win32 dependency, like json.c, so it can be tested on any platform.
 */

/* An entry time that restic did not report or that is not a valid timestamp */
#define SNAPSHOT_INDEX_NO_TIME UINT64_MAX

typedef enum {
    SNAPSHOT_INDEX_OK = 0,
    SNAPSHOT_INDEX_BAD_JSON,   /* A line that is not a JSON object, or a node without a usable path */
    SNAPSHOT_INDEX_NO_MEMORY,
    SNAPSHOT_INDEX_TOO_LARGE   /* More than the memory limit given to snapshot_index_new() */
} SnapshotIndexStatus;

typedef struct {
    const char* name;  /* UTF-8, as restic reports it */
    int is_dir;
    uint64_t size;
    uint64_t mtime, atime, ctime; /* FILETIME ticks (UTC), or SNAPSHOT_INDEX_NO_TIME */
} SnapshotIndexEntry;

typedef struct SnapshotIndex SnapshotIndex;

/* An empty index that refuses to grow beyond `max_bytes` (0: no limit); NULL when out of memory */
SnapshotIndex* snapshot_index_new(size_t max_bytes);

/*
 * Adds one line of `restic ls --json` output (`len` bytes, need not be NUL-terminated; a trailing
 * CR or blank line is fine). The snapshot line and other message types are ignored. A node whose
 * parent folder was not listed before it is ignored too (restic always lists a folder first).
 * After an error the index stays failed and every later call returns that error.
 */
SnapshotIndexStatus snapshot_index_add_line(SnapshotIndex* index, const char* line, size_t len);

/* Completes the index after the last line; it can then be looked up and no longer grows */
SnapshotIndexStatus snapshot_index_finish(SnapshotIndex* index);

/* Bytes of memory the index occupies */
size_t snapshot_index_memory(const SnapshotIndex* index);

/* Number of entries (files and folders, the snapshot root not counted) */
size_t snapshot_index_entry_count(const SnapshotIndex* index);

void snapshot_index_free(SnapshotIndex* index);

/*
 * The folder at `path` ("/" or "/a/b", '/'-separated, no trailing '/'), as a handle for
 * snapshot_index_child_*; -1 if the path does not exist or is not a folder. Needs a finished index.
 */
long long snapshot_index_find_dir(const SnapshotIndex* index, const char* path);

/* Number of entries directly in folder `dir` */
size_t snapshot_index_child_count(const SnapshotIndex* index, long long dir);

/* The i-th entry directly in folder `dir`, in the order restic listed them */
SnapshotIndexEntry snapshot_index_child(const SnapshotIndex* index, long long dir, size_t i);
