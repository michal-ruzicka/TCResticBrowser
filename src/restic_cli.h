#pragma once
#include <windows.h>
#include <stddef.h>
#include "snapshot_index.h"
#include "snapshot_tree.h"

/**
 * Status codes returned by Restic CLI execution and JSON parsing functions.
 */
typedef enum {
    RESTIC_OK = 0,               /* Operation succeeded */
    RESTIC_ERR_INVALID_ARG,      /* NULL or malformed input parameter */
    RESTIC_ERR_SPAWN_FAILED,     /* Pipe/process creation failed (e.g. binary not found) */
    RESTIC_ERR_OUT_OF_MEMORY,    /* Heap allocation failed */
    RESTIC_ERR_WRONG_PASSWORD,   /* Restic exit code 12: wrong password or no key found */
    RESTIC_ERR_REPO_NOT_FOUND,   /* Restic exit code 10: repository does not exist */
    RESTIC_ERR_REPO_LOCKED,      /* Restic exit code 11: repository is already locked */
    RESTIC_ERR_CLI_FAILED,       /* Other non-zero Restic exit code */
    RESTIC_ERR_JSON_PARSE,       /* Invalid or unexpected JSON/NDJSON payload */
    RESTIC_ERR_PATH_NOT_FOUND,   /* Requested snapshot path does not exist or is not a directory */
    RESTIC_ERR_LOCAL_WRITE,      /* Failed to create or write the local destination file */
    RESTIC_ERR_TIMEOUT,          /* Child process exceeded ResticExecOptions.timeout_ms */
    RESTIC_ERR_CANCELLED         /* ResticExecOptions.cancel_proc requested abort */
} ResticResult;

/**
 * Optional cancellation callback polled (about every 100 ms) while Restic runs.
 * Return TRUE to abort: the child process is terminated and RESTIC_ERR_CANCELLED returned.
 * Typical use: forward Total Commander's ProgressProc "user pressed Esc" result.
 */
typedef BOOL (*ResticCancelProc)(void* user_data);

/**
 * Optional streaming progress/cancellation callback invoked by `restic_dump_file`.
 * `bytes_written` is the cumulative number of bytes written to the destination file so far.
 * Return TRUE to abort the transfer (`RESTIC_ERR_CANCELLED`).
 */
typedef BOOL (*ResticDumpProgressProc)(ULONGLONG bytes_written, void* user_data);

/**
 * Optional consumer of the child's stdout, called with each complete line (without its line
 * break, not NUL-terminated) as soon as it arrives, and with the last line once the child has
 * exited. Anything but RESTIC_OK stops the child and becomes the command's result.
 */
typedef ResticResult (*ResticLineProc)(const char* line, size_t len, void* user_data);

/**
 * Optional execution controls. Pass NULL to use defaults (no timeout, not cancellable).
 */
typedef struct {
    DWORD timeout_ms;            /* Maximum run time in milliseconds (0 = unlimited) */
    ResticCancelProc cancel_proc;/* Optional cancellation poll callback */
    void* cancel_ctx;            /* Opaque pointer forwarded to cancel_proc */
    ResticLineProc line_proc;    /* Optional: stdout goes here line by line and is not kept */
    void* line_ctx;              /* Opaque pointer forwarded to line_proc */
} ResticExecOptions;

/**
 * Captures the raw UTF-8 streams and exit status of a Restic CLI invocation.
 * stdout and stderr are kept separate: with `--json` stdout carries only the JSON payload,
 * while errors (`{"message_type":"exit_error",...}`) and warnings go to stderr.
 */
typedef struct {
    char* output;                /* Heap-allocated NUL-terminated stdout (never NULL on success) */
    size_t length;               /* Byte length of output (excluding NUL terminator) */
    char* error_output;          /* Heap-allocated NUL-terminated stderr */
    size_t error_length;         /* Byte length of error_output (excluding NUL terminator) */
    DWORD exit_code;             /* Process exit code returned by restic.exe */
    WCHAR error_message[512];    /* Human-readable failure reason (truncated, always terminated) */
} ResticCommandOutput;

/**
 * Parses an ISO-8601 / RFC3339 timestamp (e.g. "2026-10-18T16:28:15.182004+02:00")
 * into a UTC WIN32 FILETIME. Rejects malformed values and invalid calendar dates
 * (timestamp_parse() in timestamp.h has the exact rules).
 */
BOOL restic_parse_iso8601_time(const char* iso_str, FILETIME* out_ft);

/**
 * Normalizes a WFX wide subpath (e.g. L"", L"\\", L"\\C\\Users\\Docs\\") into the canonical
 * UTF-8 forward-slash absolute path required by Restic (e.g. "/", "/C/Users/Docs").
 */
BOOL restic_normalize_filter_path(
    const WCHAR* subpath_w,
    char* out_utf8,
    size_t out_capacity
);

/**
 * Quotes one command-line argument following the CommandLineToArgvW rules (embedded quotes and
 * backslashes preceding quotes are escaped). Returns a heap string to release with free().
 */
WCHAR* restic_quote_arg(const WCHAR* arg);

/**
 * Describes characters in a password that are easy to enter by mistake and impossible to see in a
 * password field: a leading or trailing space, tab, line break or invisible character, and line
 * breaks, tabs, control, non-breaking, unusual space or zero-width characters anywhere inside
 * (ordinary inner spaces are normal in passphrases and not reported). Writes a phrase such as
 * "ends with a space; contains a tab" to `out` and returns TRUE, or returns FALSE (empty `out`)
 * when there is nothing to report. The text never contains the password's own characters.
 */
BOOL restic_describe_password(const WCHAR* password, WCHAR* out, size_t cap);

/**
 * Parses the NDJSON stream printed by `restic ls --json <snapshot> <filter_path>` into the
 * direct children of `filter_path` (a normalized forward-slash path such as "/" or "/C/Users").
 * Returns RESTIC_ERR_PATH_NOT_FOUND when `filter_path` (other than "/") is absent from the stream
 * or is not a directory. Caller must free `*out_items` with `free()`.
 */
ResticResult restic_parse_ls_ndjson(
    const char* ndjson_text,
    const char* filter_path,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count
);

/**
 * Lists folder `subpath_w` (a WFX subpath as for restic_list_directory) of a whole-snapshot index
 * (snapshot_index.h) into directory entries exactly as restic_list_directory() would report them.
 * RESTIC_ERR_PATH_NOT_FOUND when it is not a folder of the snapshot. Caller must free `*out_items`.
 */
ResticResult restic_list_index_directory(
    const SnapshotIndex* index,
    const WCHAR* subpath_w,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count
);

/**
 * Runs `restic_exe args` as a hidden child process (CREATE_NO_WINDOW) with RESTIC_REPOSITORY and
 * RESTIC_PASSWORD injected into its environment. `restic_exe` must contain a path separator
 * (bare names are refused so the current directory is never searched). `args` is passed verbatim
 * and must already be quoted. Caller must release `out_result` with restic_free_command_output().
 */
ResticResult restic_exec_command(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const WCHAR* args,
    const ResticExecOptions* options,
    ResticCommandOutput* out_result
);

/**
 * Releases the buffers owned by a ResticCommandOutput.
 */
void restic_free_command_output(ResticCommandOutput* result);

/**
 * Runs `restic snapshots --json --no-lock` and builds the restic mount folder tree from it
 * (snapshot_tree.h); release it with snapshot_tree_free(). On failure `out_error_msg` (optional)
 * receives the reason reported by Restic.
 */
ResticResult restic_list_snapshots(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const ResticExecOptions* options,
    SnapshotTree** out_tree,
    WCHAR* out_error_msg,
    size_t err_capacity
);

/**
 * Runs `restic ls --json --no-lock <snapshot_id> <subpath>` and returns Level 2 directory
 * entries. `snapshot_id` must be hexadecimal. On failure `out_error_msg` (optional) receives
 * the reason reported by Restic.
 */
ResticResult restic_list_directory(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const char* snapshot_id,
    const WCHAR* subpath_w,
    const ResticExecOptions* options,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count,
    WCHAR* out_error_msg,
    size_t err_capacity
);

/**
 * Lists a whole snapshot (`restic ls --json --no-lock <snapshot_id>`) into `index`, line by line as
 * restic prints it, and finishes the index. `snapshot_id` must be hexadecimal. Fails with
 * RESTIC_ERR_JSON_PARSE on unexpected output and RESTIC_ERR_OUT_OF_MEMORY when the index runs out
 * of memory or over its limit (`*out_index_status` tells which). On failure `out_error_msg`
 * (optional) receives the reason reported by Restic.
 */
ResticResult restic_list_snapshot_index(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const char* snapshot_id,
    const ResticExecOptions* options,
    SnapshotIndex* index,
    SnapshotIndexStatus* out_index_status,
    WCHAR* out_error_msg,
    size_t err_capacity
);

/**
 * Extracts a single file from a snapshot (`restic dump --no-lock <snapshot_id> <filter_path>`)
 * and streams its raw content to `local_dest_path` using a 64 KB buffer.
 *
 * - Data is written to a temporary file (`<target dir>\~rp<thread id>.tmp`) that atomically replaces
 *   `local_dest_path` only after Restic exited with code 0. On ANY failure or cancellation the
 *   temporary file is deleted and a pre-existing `local_dest_path` is left untouched.
 * - The temporary file is created only once Restic emits its first byte (or exits with code 0 for
 *   a 0-byte file), so authentication or path errors never touch the local disk.
 * - If `overwrite` is FALSE and `local_dest_path` exists (or is a directory), returns
 *   `RESTIC_ERR_LOCAL_WRITE` before contacting the repository.
 * - If `mtime` is non-NULL and not the "unknown time" marker, it is applied to the new file.
 * - `timeout_ms` is an INACTIVITY timeout (no byte on stdout/stderr for that long; 0 = unlimited):
 *   a slow but progressing multi-GB transfer is never aborted.
 * - `progress_proc` is invoked at most every ~100 ms with the cumulative byte count.
 */
ResticResult restic_dump_file(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const char* snapshot_id,
    const WCHAR* subpath_w,
    const WCHAR* local_dest_path,
    BOOL overwrite,
    const FILETIME* mtime,
    DWORD timeout_ms,
    ResticDumpProgressProc progress_proc,
    void* progress_ctx,
    WCHAR* out_error_msg,
    size_t err_capacity
);
