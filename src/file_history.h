#pragma once
#include <stddef.h>
#include <stdint.h>
#include "timestamp.h"

/*
 * The versions of one file across the snapshots of a repository, from `restic find --json <path>`.
 *
 * restic find takes an absolute pattern and walks every snapshot only along that path (a pattern
 * of a file has no descendants to visit), printing a JSON array with one object per snapshot that
 * has a match: {"matches":[{"path":..,"type":..,"size":..,"mtime":..},...],"hits":N,
 * "snapshot":"<full id>"}, the youngest snapshot first. A pattern of a folder would also print
 * everything below it, in every snapshot (restic matches an absolute pattern as a prefix), so only
 * files are looked up this way.
 *
 * A version is what restic itself compares when it decides whether a file changed: the same size
 * and modification time in two snapshots is one version. Versions are named by their modification
 * time, in the file's own zone, written by the snapshot folders' time format, then the short ID of
 * the oldest snapshot holding the version in parentheses and the file's name
 * ("2026-10-17_14-02-11 (1a2b3c4d) report.docx"), so they sort by time, keep the extension and
 * name the snapshot in which that version was first backed up - the one to look in for the files
 * that changed with it.
 *
 * Portable C11 with no Win32 dependency, like snapshot_tree.c, so it can be tested and fuzzed on
 * any platform.
 */

#define FILE_HISTORY_ID_CHARS 64

typedef struct {
    const char* name;         /* "<modification time> (<short ID>) <file name>", UTF-8, unique in the history */
    uint64_t size;
    Timestamp mtime;          /* The file's modification time */
    char type[16];            /* restic's node type: "file", "symlink", ... */
    char snapshot_id[FILE_HISTORY_ID_CHARS + 1]; /* The oldest snapshot holding this version: named and dumped */
    size_t snapshot_count;    /* How many snapshots hold it */
} FileVersion;

typedef struct FileHistory FileHistory;

typedef enum {
    FILE_HISTORY_OK = 0,
    FILE_HISTORY_BAD_JSON,   /* Not what restic find --json prints */
    FILE_HISTORY_NO_MEMORY,
    FILE_HISTORY_NOT_A_FILE  /* `in_snapshot` holds no file (or other non-folder) at the path */
} FileHistoryStatus;

/*
 * Builds the versions of the file at `path` (UTF-8, absolute inside the snapshots: "/C/Users/me/a.txt")
 * from `len` bytes of `restic find --json` output. Matches at other paths (what lies below a folder
 * of that name) and folders are ignored. With `in_snapshot` (a full ID) the path must be a
 * non-folder in that snapshot, otherwise FILE_HISTORY_NOT_A_FILE: a path that is a folder there is
 * listed as a folder, not as a history. Versions are named by `time_format` (NULL or one that
 * snapshot_tree_time_format_valid() refuses: the default of snapshot_tree.h); two versions with the
 * same name (same time and short ID) get "-1", "-2"... after the time; a name is cut
 * so that it stays within 255 UTF-16 units. Release with file_history_free().
 */
FileHistoryStatus file_history_build(const char* json, size_t len, const char* path, const char* in_snapshot,
                                     const char* time_format, FileHistory** out);

void file_history_free(FileHistory* history);

/* The versions, oldest modification time first */
size_t file_history_count(const FileHistory* history);
const FileVersion* file_history_version(const FileHistory* history, size_t i);

/* The version named `name` (exactly), or NULL */
const FileVersion* file_history_find(const FileHistory* history, const char* name);

/*
 * The IDs of the snapshots holding version `i`, the youngest first: the k-th of
 * file_history_version(history, i)->snapshot_count, or NULL
 */
const char* file_history_snapshot(const FileHistory* history, size_t i, size_t k);

/*
 * `path` as a restic find pattern that matches exactly that path: every '*', '?' and '[' put into
 * a character class ("[*]"), the only escape restic's patterns have on Windows, where a backslash
 * separates path components. Returns the bytes written without the NUL, or 0 when `cap` is too
 * small (the result is at most three times as long as `path`).
 */
size_t file_history_pattern(const char* path, char* out, size_t cap);
