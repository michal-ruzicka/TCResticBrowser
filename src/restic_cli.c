#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "restic_cli.h"
#include "fsplugin.h"
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "json.h"
#include "config.h"
#include "log.h"
#include "timestamp.h"

/* Maximum accepted WFX path length in UTF-16 units (Total Commander allows 1023) */
#define MAX_WFX_PATH 1024
/* Worst-case UTF-8 size of a MAX_WFX_PATH UTF-16 path, plus NUL */
#define MAX_UTF8_PATH (MAX_WFX_PATH * 3 + 1)
/* CreateProcessW command line limit in UTF-16 units */
#define MAX_CMDLINE_CHARS 32767
/* Buffer size requested for the child's stdout pipe */
#define STDOUT_PIPE_BYTES (1024u * 1024u)
/* Poll interval for the cancellation callback while Restic is running */
#define CANCEL_POLL_MS 100

/* ------------------------------------------------------------------------------------------ */
/* Small helpers                                                                              */
/* ------------------------------------------------------------------------------------------ */

static BOOL is_hex_string(const char* s) {
    if (!s || s[0] == '\0') return FALSE;
    for (; *s != '\0'; ++s) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Converts UTF-8 to UTF-16, truncating (never failing) when dst is too small */
static void utf8_to_wide_trunc(const char* src, WCHAR* dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = L'\0';
    if (!src) return;

    size_t len = strlen(src);
    if (len > cap - 1) len = cap - 1;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, (int)len, dst, (int)(cap - 1));
    dst[n > 0 ? n : 0] = L'\0';
}

/* ------------------------------------------------------------------------------------------ */
/* Time, naming and path helpers                                                              */
/* ------------------------------------------------------------------------------------------ */

BOOL restic_parse_iso8601_time(const char* iso_str, FILETIME* out_ft) {
    if (!out_ft) return FALSE;
    memset(out_ft, 0, sizeof(FILETIME));
    Timestamp ts;
    if (!timestamp_parse(iso_str, &ts)) return FALSE;
    out_ft->dwLowDateTime  = (DWORD)(ts.utc_ticks & 0xFFFFFFFFULL);
    out_ft->dwHighDateTime = (DWORD)(ts.utc_ticks >> 32);
    return TRUE;
}

BOOL restic_normalize_filter_path(
    const WCHAR* subpath_w,
    char* out_utf8,
    size_t out_capacity
) {
    if (!out_utf8 || out_capacity < 2) return FALSE;
    out_utf8[0] = '\0';

    if (!subpath_w || subpath_w[0] == L'\0') {
        strcpy_s(out_utf8, out_capacity, "/");
        return TRUE;
    }

    char raw_utf8[MAX_UTF8_PATH] = {0};
    if (WideCharToMultiByte(CP_UTF8, 0, subpath_w, -1, raw_utf8, (int)sizeof(raw_utf8), NULL, NULL) <= 0) {
        return FALSE;
    }

    size_t out_idx = 0;
    out_utf8[out_idx++] = '/';

    for (const char* p = raw_utf8; *p != '\0'; ++p) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && out_utf8[out_idx - 1] == '/') {
            continue; /* Collapse consecutive separators */
        }
        if (out_idx + 1 >= out_capacity) return FALSE;
        out_utf8[out_idx++] = c;
    }

    while (out_idx > 1 && out_utf8[out_idx - 1] == '/') {
        out_idx--;
    }
    out_utf8[out_idx] = '\0';
    return TRUE;
}

typedef enum {
    PWC_PLAIN = 0,
    PWC_SPACE,
    PWC_TAB,
    PWC_LINE_BREAK,
    PWC_CONTROL,
    PWC_NBSP,
    PWC_UNUSUAL_SPACE,
    PWC_INVISIBLE,
    PWC_COUNT
} PasswordCharClass;

static PasswordCharClass classify_password_char(WCHAR c) {
    if (c == L' ') return PWC_SPACE;
    if (c == L'\t') return PWC_TAB;
    if (c == L'\r' || c == L'\n' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029) {
        return PWC_LINE_BREAK;
    }
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return PWC_CONTROL;
    if (c == 0xA0 || c == 0x2007 || c == 0x202F) return PWC_NBSP;
    if (c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x205F || c == 0x3000) return PWC_UNUSUAL_SPACE;
    if (c == 0xAD || c == 0x180E || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
        (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF) {
        return PWC_INVISIBLE;
    }
    return PWC_PLAIN;
}

static const WCHAR* const kPasswordCharNames[PWC_COUNT] = {
    L"", L"a space", L"a tab", L"a line break", L"a control character", L"a non-breaking space",
    L"an unusual space character", L"an invisible character (such as a zero-width space)"
};

/* Appends "; <verb> <name>" (truncating, never failing) */
static void append_phrase(WCHAR* out, size_t cap, const WCHAR* verb, PasswordCharClass cls) {
    size_t len = wcslen(out);
    if (len + 1 >= cap) return;
    _snwprintf_s(out + len, cap - len, _TRUNCATE, L"%ls%ls %ls", len > 0 ? L"; " : L"", verb,
                 kPasswordCharNames[cls]);
}

BOOL restic_describe_password(const WCHAR* password, WCHAR* out, size_t cap) {
    if (!out || cap == 0) return FALSE;
    out[0] = L'\0';
    size_t n = password ? wcslen(password) : 0;
    if (n == 0) return FALSE;

    PasswordCharClass first = classify_password_char(password[0]);
    PasswordCharClass last  = n > 1 ? classify_password_char(password[n - 1]) : PWC_PLAIN;
    BOOL inner[PWC_COUNT]   = {FALSE};
    for (size_t i = 1; i + 1 < n; ++i) {
        inner[classify_password_char(password[i])] = TRUE;
    }

    if (first != PWC_PLAIN) append_phrase(out, cap, L"starts with", first);
    if (last != PWC_PLAIN) append_phrase(out, cap, L"ends with", last);
    for (int cls = PWC_TAB; cls < PWC_COUNT; ++cls) { /* Inner ordinary spaces are fine */
        if (inner[cls] && cls != (int)first && cls != (int)last) {
            append_phrase(out, cap, L"contains", (PasswordCharClass)cls);
        }
    }
    return out[0] != L'\0';
}

WCHAR* restic_quote_arg(const WCHAR* arg) {
    if (!arg) return NULL;

    /* Worst case: every character doubled (quotes/backslashes) plus 2 quotes and NUL */
    WCHAR* out = (WCHAR*)malloc((wcslen(arg) * 2 + 3) * sizeof(WCHAR));
    if (!out) return NULL;

    size_t o = 0;
    out[o++] = L'"';
    for (size_t i = 0;;) {
        size_t backslashes = 0;
        while (arg[i] == L'\\') {
            backslashes++;
            i++;
        }
        if (arg[i] == L'\0') { /* Backslashes before the closing quote must be doubled */
            for (size_t k = 0; k < backslashes * 2; ++k) out[o++] = L'\\';
            break;
        }
        if (arg[i] == L'"') { /* Backslashes before an embedded quote are doubled, quote escaped */
            for (size_t k = 0; k < backslashes * 2 + 1; ++k) out[o++] = L'\\';
        } else {
            for (size_t k = 0; k < backslashes; ++k) out[o++] = L'\\';
        }
        out[o++] = arg[i++];
    }
    out[o++] = L'"';
    out[o] = L'\0';
    return out;
}

/* ------------------------------------------------------------------------------------------ */
/* JSON / NDJSON parsing                                                                      */
/* ------------------------------------------------------------------------------------------ */

static BOOL append_find_data(
    WIN32_FIND_DATAW** items,
    size_t* count,
    size_t* capacity,
    const WIN32_FIND_DATAW* entry
) {
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 16 : (*capacity * 2);
        WIN32_FIND_DATAW* new_buf = (WIN32_FIND_DATAW*)realloc(*items, new_cap * sizeof(WIN32_FIND_DATAW));
        if (!new_buf) return FALSE;
        *items = new_buf;
        *capacity = new_cap;
    }
    (*items)[(*count)++] = *entry;
    return TRUE;
}

/* Checks whether node_path is an immediate child of filter_path */
static BOOL is_direct_child_path(const char* node_path, const char* filter_path) {
    if (!node_path || !filter_path || node_path[0] != '/') return FALSE;

    if (strcmp(filter_path, "/") == 0) {
        return node_path[1] != '\0' && strchr(node_path + 1, '/') == NULL;
    }

    size_t filter_len = strlen(filter_path);
    if (strncmp(node_path, filter_path, filter_len) != 0) {
        return FALSE;
    }
    if (node_path[filter_len] != '/' || node_path[filter_len + 1] == '\0') {
        return FALSE; /* Either filter_path itself or a sibling sharing the same prefix */
    }
    return strchr(node_path + filter_len + 1, '/') == NULL;
}

/*
 * TRUE when a name taken from a snapshot can be handed to Total Commander as one path component.
 *
 * Total Commander builds both the next virtual path and, on copy (F5), the local target path by
 * appending the reported name. A name containing a separator is therefore not one component but
 * several: a file named `..\..\x` on a Linux machine is a single legitimate name there, and would
 * make a recursive copy write outside the chosen target directory here. A name made only of dots
 * and spaces is refused too, because Win32 strips trailing dots and spaces from path components and
 * `...` or `.. ` would collapse into `.` or `..`.
 */
static BOOL is_safe_component(const WCHAR* name) {
    if (wcschr(name, L'\\') != NULL || wcschr(name, L'/') != NULL) return FALSE;
    for (const WCHAR* p = name; *p != L'\0'; ++p) {
        if (*p != L'.' && *p != L' ') return TRUE;
    }
    return FALSE; /* Empty, or nothing but dots and spaces */
}

static void ticks_to_filetime(uint64_t ticks, FILETIME* ft) {
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
}

/*
 * Fills a WIN32_FIND_DATAW from one snapshot entry, whether it came from a `restic ls` of the
 * folder or from the index of the whole snapshot. Returns FALSE to skip it.
 */
static BOOL entry_to_find_data(const SnapshotIndexEntry* e, WIN32_FIND_DATAW* fd) {
    memset(fd, 0, sizeof(*fd));
    if (!e->name || MultiByteToWideChar(CP_UTF8, 0, e->name, -1, fd->cFileName, MAX_PATH) <= 0) {
        return FALSE;
    }
    /* Empty names, "."/"..", and anything that is not a single path component: never report them */
    if (!is_safe_component(fd->cFileName)) {
        return FALSE;
    }

    if (e->is_dir) {
        fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    } else { /* Files, symlinks, devices... (the WFX API wants links reported as plain files) */
        fd->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
        fd->nFileSizeLow     = (DWORD)(e->size & 0xFFFFFFFFULL);
        fd->nFileSizeHigh    = (DWORD)(e->size >> 32);
    }

    FS_SET_TIME_UNKNOWN(fd->ftLastWriteTime);
    if (e->mtime != SNAPSHOT_INDEX_NO_TIME) {
        ticks_to_filetime(e->mtime, &fd->ftLastWriteTime);
        fd->ftCreationTime   = fd->ftLastWriteTime;
        fd->ftLastAccessTime = fd->ftLastWriteTime;
    }
    if (e->ctime != SNAPSHOT_INDEX_NO_TIME) ticks_to_filetime(e->ctime, &fd->ftCreationTime);
    if (e->atime != SNAPSHOT_INDEX_NO_TIME) ticks_to_filetime(e->atime, &fd->ftLastAccessTime);
    return TRUE;
}

static uint64_t node_time(const JsonValue* obj, const char* key) {
    Timestamp t;
    return timestamp_parse(json_string(json_object_get(obj, key)), &t) ? t.utc_ticks : SNAPSHOT_INDEX_NO_TIME;
}

/* Fills a WIN32_FIND_DATAW from one `restic ls --json` node object. Returns FALSE to skip it. */
static BOOL node_to_find_data(const JsonValue* obj, WIN32_FIND_DATAW* fd) {
    const char* type = json_string(json_object_get(obj, "type"));
    SnapshotIndexEntry e;
    memset(&e, 0, sizeof(e));
    e.name   = json_string(json_object_get(obj, "name"));
    e.is_dir = type && strcmp(type, "dir") == 0;
    unsigned long long size = 0;
    if (!e.is_dir && json_u64(json_object_get(obj, "size"), &size)) e.size = size;
    e.mtime = node_time(obj, "mtime");
    e.atime = node_time(obj, "atime");
    e.ctime = node_time(obj, "ctime");
    return entry_to_find_data(&e, fd);
}

ResticResult restic_list_index_directory(
    const SnapshotIndex* index,
    const WCHAR* subpath_w,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count
) {
    if (!index || !out_items || !out_count) return RESTIC_ERR_INVALID_ARG;
    *out_items = NULL;
    *out_count = 0;
    char filter_utf8[MAX_UTF8_PATH] = {0};
    if (!restic_normalize_filter_path(subpath_w, filter_utf8, sizeof(filter_utf8))) {
        return RESTIC_ERR_INVALID_ARG;
    }
    long long dir = snapshot_index_find_dir(index, filter_utf8);
    if (dir < 0) return RESTIC_ERR_PATH_NOT_FOUND;

    size_t n = snapshot_index_child_count(index, dir), capacity = 0;
    for (size_t i = 0; i < n; ++i) {
        SnapshotIndexEntry e = snapshot_index_child(index, dir, i);
        WIN32_FIND_DATAW fd;
        if (entry_to_find_data(&e, &fd) && !append_find_data(out_items, out_count, &capacity, &fd)) {
            free(*out_items);
            *out_items = NULL;
            *out_count = 0;
            return RESTIC_ERR_OUT_OF_MEMORY;
        }
    }
    return RESTIC_OK;
}

ResticResult restic_parse_ls_ndjson(
    const char* ndjson_text,
    const char* filter_path,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count
) {
    if (!ndjson_text || !filter_path || !out_items || !out_count) {
        return RESTIC_ERR_INVALID_ARG;
    }
    *out_items = NULL;
    *out_count = 0;

    ResticResult result = RESTIC_OK;
    size_t capacity = 0;
    size_t parsed_objects = 0;
    BOOL self_found = FALSE; /* Restic emits the filtered directory node itself before its children */
    BOOL self_is_dir = FALSE;
    const char* line_start = ndjson_text;

    while (*line_start != '\0' && result == RESTIC_OK) {
        const char* line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        while (line_len > 0 && (line_start[line_len - 1] == '\r' || line_start[line_len - 1] == ' ' ||
                                line_start[line_len - 1] == '\t')) {
            line_len--;
        }
        while (line_len > 0 && (*line_start == ' ' || *line_start == '\t')) {
            line_start++;
            line_len--;
        }

        if (line_len > 0) {
            JsonDoc* doc = json_parse(line_start, line_len);
            const JsonValue* obj = json_root(doc);
            if (!obj || obj->type != JSON_OBJECT) {
                result = RESTIC_ERR_JSON_PARSE;
            } else {
                parsed_objects++;
                const char* msg_type = json_string(json_object_get(obj, "message_type"));
                const char* path     = json_string(json_object_get(obj, "path"));

                if (msg_type && strcmp(msg_type, "node") == 0 && path) {
                    WIN32_FIND_DATAW fd;

                    if (strcmp(path, filter_path) == 0) {
                        const char* type = json_string(json_object_get(obj, "type"));
                        self_found  = TRUE;
                        self_is_dir = type && strcmp(type, "dir") == 0;
                    } else if (is_direct_child_path(path, filter_path) && node_to_find_data(obj, &fd)) {
                        if (!append_find_data(out_items, out_count, &capacity, &fd)) {
                            result = RESTIC_ERR_OUT_OF_MEMORY;
                        }
                    }
                }
            }
            json_free(doc);
        }

        if (!line_end) break;
        line_start = line_end + 1;
    }

    if (result == RESTIC_OK && parsed_objects == 0 && ndjson_text[0] != '\0') {
        result = RESTIC_ERR_JSON_PARSE;
    }
    /* A missing (or non-directory) filter target yields an empty stream: report it explicitly */
    if (result == RESTIC_OK && strcmp(filter_path, "/") != 0 && (!self_found || !self_is_dir)) {
        result = RESTIC_ERR_PATH_NOT_FOUND;
    }
    if (result != RESTIC_OK) {
        free(*out_items);
        *out_items = NULL;
        *out_count = 0;
    }
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Child process execution                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Growable NUL-terminated byte buffer used to capture the child's output streams */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} ByteBuf;

/* Makes room for n more bytes plus the terminating NUL */
static BOOL bytebuf_reserve(ByteBuf* b, size_t n) {
    if (b->len + n + 1 > b->cap) {
        if (n > SIZE_MAX / 2 - b->len) return FALSE; /* The doubling below would wrap around */
        size_t new_cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > new_cap) new_cap *= 2;
        char* p = (char*)realloc(b->data, new_cap);
        if (!p) return FALSE;
        b->data = p;
        b->cap  = new_cap;
    }
    return TRUE;
}

static BOOL bytebuf_append(ByteBuf* b, const char* src, size_t n) {
    if (!bytebuf_reserve(b, n)) return FALSE;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return TRUE;
}

/*
 * Moves the bytes currently available in a pipe into buf, at most DRAIN_SLICE_BYTES per call so that
 * the caller still checks the timeout and ESC while a child floods its output. Sets *eof once the
 * pipe is closed. Beyond `keep_max` bytes in buf (0: no limit) the rest is read and dropped; every
 * byte read, kept or not, is added to *total.
 */
#define DRAIN_SLICE_BYTES (1024u * 1024u)
#define DRAIN_READ_BYTES  (64u * 1024u) /* At most per ReadFile, straight into the buffer */
#define STDERR_KEEP_BYTES (1024u * 1024u)       /* Enough for any error message; the rest is read and dropped */
#define STREAM_LINE_MAX_BYTES (16u * 1024u * 1024u) /* A streamed stdout line longer than this fails the command */
static BOOL drain_pipe(HANDLE pipe, ByteBuf* buf, BOOL* eof, size_t keep_max, size_t* total) {
    char discard[4096];
    for (size_t taken = 0; !*eof && taken < DRAIN_SLICE_BYTES;) {
        DWORD avail = 0;
        DWORD got = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
            *eof = TRUE; /* ERROR_BROKEN_PIPE: writer closed */
            break;
        }
        if (avail == 0) break;
        BOOL keep = keep_max == 0 || buf->len < keep_max;
        DWORD want = avail < DRAIN_READ_BYTES ? avail : DRAIN_READ_BYTES;
        if (!keep && want > sizeof(discard)) want = (DWORD)sizeof(discard);
        if (keep && !bytebuf_reserve(buf, want)) return FALSE;
        if (!ReadFile(pipe, keep ? buf->data + buf->len : discard, want, &got, NULL) || got == 0) {
            *eof = TRUE;
            break;
        }
        if (keep) {
            buf->len += got;
            buf->data[buf->len] = '\0';
        }
        taken += got;
        *total += got;
    }
    return TRUE;
}

static BOOL starts_with_env_key(const WCHAR* entry, const WCHAR* prefix) {
    return _wcsnicmp(entry, prefix, wcslen(prefix)) == 0;
}

/* Inherited variables that would conflict with the credentials injected by the plugin */
static const WCHAR* const kStrippedEnvKeys[] = {
    L"RESTIC_REPOSITORY=", L"RESTIC_REPOSITORY_FILE=",
    L"RESTIC_PASSWORD=",   L"RESTIC_PASSWORD_FILE=",   L"RESTIC_PASSWORD_COMMAND="
};

/*
 * Builds a UTF-16 environment block: the parent environment (Go needs SystemRoot, APPDATA, TEMP...)
 * minus conflicting RESTIC_* credential variables, plus RESTIC_REPOSITORY and RESTIC_PASSWORD.
 * The caller must wipe the block with SecureZeroMemory before releasing it.
 */
static WCHAR* build_child_environment(const ResticRepo* repo, size_t* out_chars) {
    *out_chars = 0;
    LPWCH parent_env = GetEnvironmentStringsW();
    size_t parent_chars = 0;

    for (const WCHAR* p = parent_env; p && *p != L'\0'; p += wcslen(p) + 1) {
        parent_chars += wcslen(p) + 1;
    }

    size_t total_cap = parent_chars + wcslen(L"RESTIC_REPOSITORY=") + wcslen(repo->target) + 1 +
                       wcslen(L"RESTIC_PASSWORD=") + wcslen(repo->password) + 1 + 1;
    WCHAR* block = (WCHAR*)calloc(total_cap, sizeof(WCHAR));
    if (!block) {
        if (parent_env) FreeEnvironmentStringsW(parent_env);
        return NULL;
    }

    size_t offset = 0;
    for (const WCHAR* p = parent_env; p && *p != L'\0'; p += wcslen(p) + 1) {
        BOOL stripped = FALSE;
        for (size_t i = 0; i < _countof(kStrippedEnvKeys); ++i) {
            if (starts_with_env_key(p, kStrippedEnvKeys[i])) {
                stripped = TRUE;
                break;
            }
        }
        if (!stripped) {
            size_t len = wcslen(p) + 1;
            wmemcpy(block + offset, p, len);
            offset += len;
        }
    }
    if (parent_env) FreeEnvironmentStringsW(parent_env);

    int n = swprintf_s(block + offset, total_cap - offset, L"RESTIC_REPOSITORY=%ls", repo->target);
    if (n < 0) goto fail;
    offset += (size_t)n + 1;
    n = swprintf_s(block + offset, total_cap - offset, L"RESTIC_PASSWORD=%ls", repo->password);
    if (n < 0) goto fail;
    offset += (size_t)n + 1;

    block[offset++] = L'\0'; /* Block terminator */
    *out_chars = offset;
    return block;

fail:
    SecureZeroMemory(block, total_cap * sizeof(WCHAR));
    free(block);
    return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Diagnostic log (off unless [Logging] LogPath is set). Never logs the password itself.      */
/* ------------------------------------------------------------------------------------------ */

static const WCHAR* result_name(ResticResult r) {
    switch (r) {
        case RESTIC_OK: return L"OK";
        case RESTIC_ERR_INVALID_ARG: return L"invalid argument";
        case RESTIC_ERR_SPAWN_FAILED: return L"restic.exe could not be started";
        case RESTIC_ERR_OUT_OF_MEMORY: return L"out of memory";
        case RESTIC_ERR_WRONG_PASSWORD: return L"wrong password or no key found";
        case RESTIC_ERR_REPO_NOT_FOUND: return L"repository not found";
        case RESTIC_ERR_REPO_LOCKED: return L"repository locked";
        case RESTIC_ERR_CLI_FAILED: return L"restic failed";
        case RESTIC_ERR_JSON_PARSE: return L"unexpected restic output";
        case RESTIC_ERR_PATH_NOT_FOUND: return L"path not found";
        case RESTIC_ERR_LOCAL_WRITE: return L"local write failed";
        case RESTIC_ERR_TIMEOUT: return L"timeout";
        case RESTIC_ERR_CANCELLED: return L"cancelled";
    }
    return L"unknown";
}

/* Logs the command about to run: the repository (credentials redacted) and the password length only */
static void log_restic_start(const WCHAR* cmd_line, const ResticRepo* repo) {
    if (!log_enabled()) return;
    WCHAR target[1024];
    log_redact_target(repo->target, target, _countof(target));
    log_write(L"restic run: %ls", cmd_line);
    log_write(L"  repository: %ls; password: %u characters", target, (unsigned)wcslen(repo->password));
}

/* Logs how a run ended, with restic's stderr (line breaks shown as " | ") */
static void log_restic_end(ResticResult result, BOOL exited, DWORD exit_code, ULONGLONG start, const ByteBuf* err) {
    if (!log_enabled()) return;
    ULONGLONG ms = GetTickCount64() - start;
    if (exited) {
        log_write(L"  exit code %lu after %llu ms: %ls", exit_code, ms, result_name(result));
    } else {
        log_write(L"  ended after %llu ms: %ls", ms, result_name(result));
    }
    if (err && err->data && err->len > 0) {
        WCHAR text[LOG_MAX_LINE - 128];
        utf8_to_wide_trunc(err->data, text, _countof(text));
        WCHAR flat[LOG_MAX_LINE - 128];
        size_t o = 0;
        for (const WCHAR* p = text; *p && o + 4 < _countof(flat); ++p) {
            if (*p == L'\r') continue;
            if (*p == L'\n') {
                if (p[1] != L'\0') {
                    flat[o++] = L' ';
                    flat[o++] = L'|';
                    flat[o++] = L' ';
                }
                continue;
            }
            flat[o++] = *p;
        }
        flat[o] = L'\0';
        log_write(L"  stderr: %ls", flat);
    }
}

/*
 * Starts cmd_line hidden with stdin=NUL and stdout/stderr redirected to two anonymous pipes.
 * Restic inherits only these three handles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), never unrelated
 * inheritable handles of Total Commander. The list does not protect the other direction: a child
 * another thread of the host starts with bInheritHandles in the meantime can inherit the write
 * ends and keep them open. That is harmless only because collect_output() and the dump loop end
 * when Restic exits, not at EOF; keep it that way.
 */
static BOOL spawn_hidden(
    WCHAR* cmd_line,
    WCHAR* env_block,
    HANDLE* out_read,
    HANDLE* err_read,
    PROCESS_INFORMATION* pi
) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    HANDLE in_nul = INVALID_HANDLE_VALUE;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    BOOL attrs_ready = FALSE;
    BOOL ok = FALSE;

    /* A large stdout pipe lets restic write a whole snapshot listing without waiting on every 4 KB */
    if (!CreatePipe(&out_r, &out_w, &sa, STDOUT_PIPE_BYTES) || !CreatePipe(&err_r, &err_w, &sa, 0)) goto done;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    in_nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in_nul == INVALID_HANDLE_VALUE) goto done;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size); /* Fails by design: size query */
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) goto done;
    attrs_ready = TRUE;

    HANDLE inherit[3] = {in_nul, out_w, err_w};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   sizeof(inherit), NULL, NULL)) {
        goto done;
    }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb          = sizeof(si);
    si.StartupInfo.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput   = in_nul;
    si.StartupInfo.hStdOutput  = out_w;
    si.StartupInfo.hStdError   = err_w;
    si.lpAttributeList         = attrs;

    memset(pi, 0, sizeof(*pi));
    ok = CreateProcessW(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        env_block, NULL, &si.StartupInfo, pi);

done:
    if (attrs_ready) DeleteProcThreadAttributeList(attrs);
    free(attrs);
    /* Child-side ends must be closed in the parent so EOF is observable */
    if (out_w) CloseHandle(out_w);
    if (err_w) CloseHandle(err_w);
    if (in_nul != INVALID_HANDLE_VALUE) CloseHandle(in_nul);
    if (ok) {
        *out_read = out_r;
        *err_read = err_r;
    } else {
        if (out_r) CloseHandle(out_r);
        if (err_r) CloseHandle(err_r);
    }
    return ok;
}

/*
 * Reads both pipes until the child exits (no deadlock if one stream fills up), honouring the
 * optional timeout and cancellation callback. The child is NOT terminated here.
 */
/*
 * Passes the complete lines in `buf` to `proc` and drops them from the buffer (all of it when the
 * child has exited, `last`), so a streamed stdout never grows beyond one line
 */
static ResticResult deliver_lines(ByteBuf* buf, ResticLineProc proc, void* ctx, BOOL last, size_t* delivered) {
    size_t start = 0;
    for (;;) {
        const char* nl = (const char*)memchr(buf->data + start, '\n', buf->len - start);
        if (!nl) break;
        size_t len = (size_t)(nl - (buf->data + start));
        ResticResult r = proc(buf->data + start, len, ctx);
        if (r != RESTIC_OK) return r;
        start += len + 1;
    }
    if (last && start < buf->len) {
        ResticResult r = proc(buf->data + start, buf->len - start, ctx);
        if (r != RESTIC_OK) return r;
        start = buf->len;
    }
    if (start > 0) {
        memmove(buf->data, buf->data + start, buf->len - start);
        buf->len -= start;
        buf->data[buf->len] = '\0';
        *delivered += start;
    }
    return RESTIC_OK;
}

static ResticResult collect_output(
    HANDLE process,
    HANDLE out_pipe,
    HANDLE err_pipe,
    const ResticExecOptions* options,
    ByteBuf* out,
    ByteBuf* err
) {
    ULONGLONG start = GetTickCount64();
    ULONGLONG last_poll = start;
    BOOL out_eof = FALSE, err_eof = FALSE;
    ResticLineProc line_proc = options ? options->line_proc : NULL;
    size_t delivered = 0; /* Bytes of stdout passed to line_proc and no longer kept */

    for (BOOL exited = FALSE;;) {
        size_t got = 0; /* Bytes read in this pass, also those of stderr beyond STDERR_KEEP_BYTES */
        if (!drain_pipe(out_pipe, out, &out_eof, 0, &got) ||
            !drain_pipe(err_pipe, err, &err_eof, STDERR_KEEP_BYTES, &got)) {
            return RESTIC_ERR_OUT_OF_MEMORY;
        }
        BOOL got_data = got != 0;
        if (line_proc) {
            ResticResult r = deliver_lines(out, line_proc, options->line_ctx, FALSE, &delivered);
            if (r != RESTIC_OK) return r;
            if (out->len > STREAM_LINE_MAX_BYTES) return RESTIC_ERR_JSON_PARSE; /* No line is that long */
        }

        /*
         * After the exit, read on until the pipes hold nothing more (a slice is at most 1 MiB); the
         * timeout and ESC still apply, as a helper restic started may hold the pipes and keep writing
         */
        if (exited) {
            if (!got_data) {
                return line_proc ? deliver_lines(out, line_proc, options->line_ctx, TRUE, &delivered) : RESTIC_OK;
            }
        } else if (!got_data && WaitForSingleObject(process, 20) == WAIT_OBJECT_0) {
            /* Only sleep when idle; wake immediately when the child exits */
            exited = TRUE;
            continue;
        }

        if (options) {
            ULONGLONG now = GetTickCount64();
            if (options->timeout_ms != 0 && now - start >= options->timeout_ms) {
                return RESTIC_ERR_TIMEOUT;
            }
            if (options->cancel_proc && now - last_poll >= CANCEL_POLL_MS) {
                last_poll = now;
                if (options->cancel_proc(options->cancel_ctx)) return RESTIC_ERR_CANCELLED;
            }
        }
    }
}

/* Copies the "message" field of the first JSON line found in text. Returns TRUE if found. */
static BOOL find_json_message(const char* text, WCHAR* dst, size_t cap) {
    for (const char* line = text; line && *line != '\0';) {
        const char* next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len > 0 && *line == '{') {
            JsonDoc* doc = json_parse(line, len);
            const char* msg = json_string(json_object_get(json_root(doc), "message"));
            if (msg && msg[0] != '\0') {
                utf8_to_wide_trunc(msg, dst, cap);
                json_free(doc);
                return TRUE;
            }
            json_free(doc);
        }
        line = next ? next + 1 : NULL;
    }
    return FALSE;
}

/*
 * Says which binary could not be started and why ("Cannot start C:\x\restic.exe: The specified
 * executable is not a valid application for this OS platform. (Windows error 193)"), so a configured
 * binary that exists but does not run is reported as such; the reason is in the system language
 */
static void describe_spawn_failure(const WCHAR* restic_exe, DWORD error, WCHAR* dst, size_t cap) {
    if (!dst || cap == 0) return;
    WCHAR reason[256] = {0};
    /* Some texts name the file as %1 ("%1 is not a valid Win32 application."): fill in the path */
    DWORD_PTR inserts[9];
    for (size_t i = 0; i < _countof(inserts); ++i) inserts[i] = (DWORD_PTR)restic_exe;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, error, 0, reason,
                             _countof(reason), (va_list*)inserts);
    while (n > 0 && (reason[n - 1] == L'\r' || reason[n - 1] == L'\n' || reason[n - 1] == L' ')) {
        reason[--n] = L'\0';
    }
    if (n > 0) {
        _snwprintf_s(dst, cap, _TRUNCATE, L"Cannot start %ls: %ls (Windows error %lu)", restic_exe, reason, error);
    } else {
        _snwprintf_s(dst, cap, _TRUNCATE, L"Cannot start %ls (Windows error %lu).", restic_exe, error);
    }
    log_write(L"  %ls", dst);
}

/*
 * TRUE when restic_exe may be handed to CreateProcessW; otherwise `err` says why. A path that is
 * not absolute would make CreateProcess search the current directory. A file without a PE header
 * (the "MZ" stub pointing at "PE\0\0") is not a Windows program: 64-bit Windows would take it for a
 * 16-bit DOS program and show its modal "Unsupported 16-Bit Application" dialog, which stops the
 * calling thread - Total Commander's, or a CI test run - until someone clicks it. A truncated
 * download or an HTML page saved as restic.exe looks like that. A file that cannot be opened is
 * left to CreateProcessW, which reports why.
 */
static BOOL restic_exe_startable(const WCHAR* restic_exe, WCHAR* err, size_t err_capacity) {
    if (!config_is_absolute_path(restic_exe)) {
        describe_spawn_failure(restic_exe, ERROR_BAD_PATHNAME, err, err_capacity);
        return FALSE;
    }
    HANDLE h = CreateFileW(restic_exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return TRUE;
    BYTE dos[64];
    BYTE signature[4];
    DWORD got = 0;
    BOOL is_pe = FALSE;
    if (ReadFile(h, dos, sizeof(dos), &got, NULL) && got == sizeof(dos) && dos[0] == 'M' && dos[1] == 'Z') {
        LONG pe_offset = 0;
        memcpy(&pe_offset, dos + 0x3C, sizeof(pe_offset));
        LARGE_INTEGER at;
        at.QuadPart = pe_offset;
        is_pe = pe_offset > 0 && SetFilePointerEx(h, at, NULL, FILE_BEGIN) &&
                ReadFile(h, signature, sizeof(signature), &got, NULL) && got == sizeof(signature) &&
                memcmp(signature, "PE\0\0", sizeof(signature)) == 0;
    }
    CloseHandle(h);
    if (!is_pe && err && err_capacity > 0) {
        _snwprintf_s(err, err_capacity, _TRUNCATE, L"Cannot start %ls: it is not a Windows program.", restic_exe);
        log_write(L"  %ls", err);
    }
    return is_pe;
}

/* Builds a human-readable failure reason from Restic's streams (JSON message preferred) */
static void build_error_message(const ByteBuf* out, const ByteBuf* err, WCHAR* dst, size_t cap) {
    if (find_json_message(err->data, dst, cap) || find_json_message(out->data, dst, cap)) return;
    utf8_to_wide_trunc(err->len > 0 ? err->data : out->data, dst, cap);
    for (size_t n = wcslen(dst); n > 0 && (dst[n - 1] == L'\r' || dst[n - 1] == L'\n'); --n) {
        dst[n - 1] = L'\0';
    }
}

ResticResult restic_exec_command(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
    const WCHAR* args,
    const ResticExecOptions* options,
    ResticCommandOutput* out_result
) {
    if (!restic_exe || !repo || !repo->target || !repo->password || !args || !out_result) {
        return RESTIC_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(ResticCommandOutput));

    /* Neither a relative name (current directory search) nor a file that is no Windows program */
    if (!restic_exe_startable(restic_exe, out_result->error_message, _countof(out_result->error_message))) {
        return RESTIC_ERR_SPAWN_FAILED;
    }

    size_t cmd_chars = wcslen(restic_exe) + wcslen(args) + 4; /* quotes, space, NUL */
    if (cmd_chars > MAX_CMDLINE_CHARS) {
        return RESTIC_ERR_INVALID_ARG;
    }
    WCHAR* cmd_line = (WCHAR*)malloc(cmd_chars * sizeof(WCHAR));
    if (!cmd_line) {
        return RESTIC_ERR_OUT_OF_MEMORY;
    }
    swprintf_s(cmd_line, cmd_chars, L"\"%ls\" %ls", restic_exe, args);

    size_t env_chars = 0;
    WCHAR* env_block = build_child_environment(repo, &env_chars);
    if (!env_block) {
        free(cmd_line);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }

    HANDLE out_pipe = NULL, err_pipe = NULL;
    PROCESS_INFORMATION pi;
    ULONGLONG started = GetTickCount64();
    log_restic_start(cmd_line, repo);
    BOOL spawned = spawn_hidden(cmd_line, env_block, &out_pipe, &err_pipe, &pi);
    DWORD spawn_error = spawned ? 0 : GetLastError();

    /* Credentials must not outlive the CreateProcess call in this process' memory */
    SecureZeroMemory(env_block, env_chars * sizeof(WCHAR));
    free(env_block);
    free(cmd_line);
    if (!spawned) {
        describe_spawn_failure(restic_exe, spawn_error, out_result->error_message,
                               _countof(out_result->error_message));
        return RESTIC_ERR_SPAWN_FAILED;
    }
    CloseHandle(pi.hThread);

    ByteBuf out = {0}, err = {0};
    ResticResult result = RESTIC_ERR_OUT_OF_MEMORY;
    if (bytebuf_append(&out, "", 0) && bytebuf_append(&err, "", 0)) {
        result = collect_output(pi.hProcess, out_pipe, err_pipe, options, &out, &err);
    }

    BOOL exited = FALSE;
    if (result == RESTIC_OK) {
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        exited = TRUE;
        out_result->exit_code = exit_code;
        if (exit_code != 0) {
            build_error_message(&out, &err, out_result->error_message, _countof(out_result->error_message));
            switch (exit_code) {
                case 10: result = RESTIC_ERR_REPO_NOT_FOUND; break;
                case 11: result = RESTIC_ERR_REPO_LOCKED; break;
                case 12: result = RESTIC_ERR_WRONG_PASSWORD; break;
                default: result = RESTIC_ERR_CLI_FAILED; break;
            }
        }
    } else {
        TerminateProcess(pi.hProcess, 1); /* Timeout, cancellation or memory failure */
        WaitForSingleObject(pi.hProcess, 5000);
    }

    log_restic_end(result, exited, out_result->exit_code, started, &err);
    CloseHandle(out_pipe);
    CloseHandle(err_pipe);
    CloseHandle(pi.hProcess);

    out_result->output       = out.data;
    out_result->length       = out.len;
    out_result->error_output = err.data;
    out_result->error_length = err.len;
    return result;
}

void restic_free_command_output(ResticCommandOutput* result) {
    if (!result) return;
    free(result->output);
    free(result->error_output);
    memset(result, 0, sizeof(ResticCommandOutput));
}

/* ------------------------------------------------------------------------------------------ */
/* High-level helpers                                                                         */
/* ------------------------------------------------------------------------------------------ */

/* Runs a Restic command; on failure copies the reason into out_error_msg and frees the output */
static ResticResult run_and_check(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
    const WCHAR* args,
    const ResticExecOptions* options,
    ResticCommandOutput* cmd_out,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    ResticResult res = restic_exec_command(restic_exe, repo, args, options, cmd_out);
    if (res != RESTIC_OK) {
        if (out_error_msg && err_capacity > 0) {
            wcsncpy_s(out_error_msg, err_capacity, cmd_out->error_message, _TRUNCATE);
        }
        restic_free_command_output(cmd_out);
    }
    return res;
}

ResticResult restic_list_snapshots(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
    const char* time_format,
    const ResticExecOptions* options,
    SnapshotTree** out_tree,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    if (out_tree) *out_tree = NULL;
    if (!restic_exe || !repo || !repo->target || !repo->password || !out_tree) {
        return RESTIC_ERR_INVALID_ARG;
    }

    FILETIME now; /* When restic was asked: the time of what is made from its answer (snapshots.csv) */
    GetSystemTimeAsFileTime(&now);
    ResticCommandOutput cmd_out;
    ResticResult res = run_and_check(restic_exe, repo, L"snapshots --json --no-lock",
                                     options, &cmd_out, out_error_msg, err_capacity);
    if (res != RESTIC_OK) return res;

    switch (snapshot_tree_build(cmd_out.output, cmd_out.length, time_format, out_tree)) {
        case SNAPSHOT_TREE_OK:
            snapshot_tree_set_listed_at(*out_tree, ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime);
            res = RESTIC_OK;
            break;
        case SNAPSHOT_TREE_NO_MEMORY: res = RESTIC_ERR_OUT_OF_MEMORY; break;
        case SNAPSHOT_TREE_BAD_FORMAT: res = RESTIC_ERR_INVALID_ARG; break;
        default: res = RESTIC_ERR_JSON_PARSE; break;
    }
    restic_free_command_output(&cmd_out);
    return res;
}

ResticResult restic_read_repository_id(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
    const ResticExecOptions* options,
    char* out_id,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    if (out_id) out_id[0] = '\0';
    if (!restic_exe || !repo || !repo->target || !repo->password || !out_id) {
        return RESTIC_ERR_INVALID_ARG;
    }

    ResticCommandOutput cmd_out;
    ResticResult res = run_and_check(restic_exe, repo, L"cat config --json --no-lock", options,
                                     &cmd_out, out_error_msg, err_capacity);
    if (res != RESTIC_OK) return res;

    res = RESTIC_ERR_JSON_PARSE;
    JsonDoc* doc = json_parse(cmd_out.output, cmd_out.length);
    const char* id = doc ? json_string(json_object_get(json_root(doc), "id")) : NULL;
    if (id && strlen(id) == RESTIC_REPOSITORY_ID_CHARS &&
        strspn(id, "0123456789abcdef") == RESTIC_REPOSITORY_ID_CHARS) {
        memcpy(out_id, id, RESTIC_REPOSITORY_ID_CHARS + 1);
        res = RESTIC_OK;
    }
    json_free(doc);
    restic_free_command_output(&cmd_out);
    return res;
}

ResticResult restic_list_directory(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
    const char* snapshot_id,
    const WCHAR* subpath_w,
    const ResticExecOptions* options,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    /* The snapshot ID goes on the command line unquoted: only hexadecimal IDs are accepted */
    if (!restic_exe || !repo || !repo->target || !repo->password || !out_items || !out_count ||
        !is_hex_string(snapshot_id) || strlen(snapshot_id) > 64) {
        return RESTIC_ERR_INVALID_ARG;
    }

    char filter_utf8[MAX_UTF8_PATH] = {0};
    WCHAR filter_w[MAX_UTF8_PATH] = {0};
    WCHAR snap_w[65] = {0};
    if (!restic_normalize_filter_path(subpath_w, filter_utf8, sizeof(filter_utf8)) ||
        MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, filter_w, (int)_countof(filter_w)) <= 0) {
        return RESTIC_ERR_INVALID_ARG;
    }
    MultiByteToWideChar(CP_UTF8, 0, snapshot_id, -1, snap_w, (int)_countof(snap_w));

    WCHAR* quoted_filter = restic_quote_arg(filter_w);
    if (!quoted_filter) return RESTIC_ERR_OUT_OF_MEMORY;

    size_t args_chars = wcslen(L"ls --json --no-lock  ") + wcslen(snap_w) + wcslen(quoted_filter) + 1;
    WCHAR* args = (WCHAR*)malloc(args_chars * sizeof(WCHAR));
    if (!args) {
        free(quoted_filter);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }
    swprintf_s(args, args_chars, L"ls --json --no-lock %ls %ls", snap_w, quoted_filter);
    free(quoted_filter);

    ResticCommandOutput cmd_out;
    ResticResult res = run_and_check(restic_exe, repo, args, options, &cmd_out,
                                     out_error_msg, err_capacity);
    free(args);
    if (res != RESTIC_OK) return res;

    res = restic_parse_ls_ndjson(cmd_out.output, filter_utf8, out_items, out_count);
    restic_free_command_output(&cmd_out);
    return res;
}

typedef struct {
    SnapshotIndex* index;
    SnapshotIndexStatus status;
} IndexFeed;

static ResticResult feed_index_line(const char* line, size_t len, void* ctx) {
    IndexFeed* feed = (IndexFeed*)ctx;
    feed->status = snapshot_index_add_line(feed->index, line, len);
    switch (feed->status) {
        case SNAPSHOT_INDEX_OK: return RESTIC_OK;
        case SNAPSHOT_INDEX_BAD_JSON: return RESTIC_ERR_JSON_PARSE;
        default: return RESTIC_ERR_OUT_OF_MEMORY;
    }
}

ResticResult restic_list_snapshot_index(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
    const char* snapshot_id,
    const ResticExecOptions* options,
    SnapshotIndex* index,
    SnapshotIndexStatus* out_index_status,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    if (out_index_status) *out_index_status = SNAPSHOT_INDEX_OK;
    /* The snapshot ID goes on the command line unquoted: only hexadecimal IDs are accepted */
    if (!restic_exe || !repo || !repo->target || !repo->password || !index || !is_hex_string(snapshot_id) ||
        strlen(snapshot_id) > 64) {
        return RESTIC_ERR_INVALID_ARG;
    }
    WCHAR args[96];
    swprintf_s(args, _countof(args), L"ls --json --no-lock %hs", snapshot_id);

    IndexFeed feed = {index, SNAPSHOT_INDEX_OK};
    ResticExecOptions opts;
    memset(&opts, 0, sizeof(opts));
    if (options) opts = *options;
    opts.line_proc = feed_index_line;
    opts.line_ctx  = &feed;

    ResticCommandOutput cmd_out;
    ResticResult res = run_and_check(restic_exe, repo, args, &opts, &cmd_out, out_error_msg,
                                     err_capacity);
    if (res == RESTIC_OK) {
        restic_free_command_output(&cmd_out);
        feed.status = snapshot_index_finish(index);
        res = feed.status == SNAPSHOT_INDEX_OK ? RESTIC_OK : RESTIC_ERR_OUT_OF_MEMORY;
    }
    if (out_index_status) *out_index_status = feed.status;
    return res;
}

#define DUMP_CHUNK_BYTES (64u * 1024u)

/* State of one streaming extraction: data goes to a temporary file that replaces the target on success */
typedef struct {
    const WCHAR* final_path;
    WCHAR* part_path;        /* Heap: <target dir>\~rp<thread id>[-<n>].tmp (short, to stay below MAX_PATH) */
    BOOL overwrite;
    HANDLE hLocal;           /* Handle of the temporary file (INVALID_HANDLE_VALUE until the first byte) */
    BOOL part_created;       /* TRUE while the temporary file exists on disk and must be cleaned up */
    ULONGLONG written;       /* Cumulative payload bytes written */
    ULONGLONG last_poll;     /* Tick of the last progress/cancel callback */
    ULONGLONG last_activity; /* Tick of the last byte received from Restic (inactivity timeout) */
    BOOL first_reported;     /* TRUE once the first payload chunk was reported to the progress callback */
    ResticDumpProgressProc progress_proc;
    void* progress_ctx;
    char* chunk;
} DumpState;

static BOOL is_unknown_time(const FILETIME* ft) {
    return ft->dwHighDateTime == 0xFFFFFFFF && ft->dwLowDateTime == 0xFFFFFFFE;
}

#define PARTIAL_NAME_CHARS 32 /* "~rp" + thread id + "-" + attempt + ".tmp" + NUL, rounded up */
#define PARTIAL_ATTEMPTS   100

/* Length of the directory part of path, including its trailing separator */
static size_t dir_part_length(const WCHAR* path) {
    size_t dir_len = 0;
    for (size_t i = 0; path[i] != L'\0'; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') dir_len = i + 1;
    }
    return dir_len;
}

/* Writes the name of temporary file `attempt` ("~rp<tid>.tmp", then "~rp<tid>-<n>.tmp") into part */
static void set_partial_name(WCHAR* part, size_t dir_len, unsigned attempt) {
    if (attempt == 0) {
        swprintf_s(part + dir_len, PARTIAL_NAME_CHARS, L"~rp%lx.tmp", GetCurrentThreadId());
    } else {
        swprintf_s(part + dir_len, PARTIAL_NAME_CHARS, L"~rp%lx-%x.tmp", GetCurrentThreadId(), attempt);
    }
}

/* Builds "<dir of final_path>\~rp<tid>.tmp" on the heap. Returns NULL on failure. */
static WCHAR* make_partial_path(const WCHAR* final_path) {
    size_t dir_len = dir_part_length(final_path);
    WCHAR* part    = (WCHAR*)malloc((dir_len + PARTIAL_NAME_CHARS) * sizeof(WCHAR));
    if (!part) return NULL;
    wmemcpy(part, final_path, dir_len);
    set_partial_name(part, dir_len, 0);
    return part;
}

/*
 * Returns the open temporary file, creating it on first use. INVALID_HANDLE_VALUE on failure.
 * CREATE_NEW never truncates or follows anything already there (a user's file of that name, or a
 * link planted in a shared target folder); the next name is tried instead.
 */
static HANDLE dump_get_partial(DumpState* st) {
    if (st->hLocal != INVALID_HANDLE_VALUE) return st->hLocal;
    size_t dir_len = dir_part_length(st->final_path);
    HANDLE h       = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < PARTIAL_ATTEMPTS; ++attempt) {
        set_partial_name(st->part_path, dir_len, attempt);
        h = CreateFileW(st->part_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                        NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    st->hLocal       = h;
    st->part_created = TRUE;
    return h;
}

/*
 * Moves every byte available on Restic's stdout into the temporary file (opened lazily on the first
 * byte). The progress callback runs at most every CANCEL_POLL_MS so multi-GB transfers do not flood
 * Total Commander with UI updates.
 */
static ResticResult dump_drain_stdout(HANDLE out_pipe, DumpState* st, BOOL* out_eof) {
    while (!*out_eof) {
        DWORD avail = 0, got = 0;
        if (!PeekNamedPipe(out_pipe, NULL, 0, NULL, &avail, NULL)) {
            *out_eof = TRUE;
            break;
        }
        if (avail == 0) break;
        if (!ReadFile(
                out_pipe,
                st->chunk,
                avail < DUMP_CHUNK_BYTES ? avail : DUMP_CHUNK_BYTES,
                &got,
                NULL
            ) ||
            got == 0) {
            *out_eof = TRUE;
            break;
        }
        ULONGLONG now    = GetTickCount64();
        st->last_activity = now;
        HANDLE hf = dump_get_partial(st);
        if (hf == INVALID_HANDLE_VALUE) {
            return RESTIC_ERR_LOCAL_WRITE;
        }
        DWORD written = 0;
        if (!WriteFile(hf, st->chunk, got, &written, NULL) || written != got) {
            return RESTIC_ERR_LOCAL_WRITE;
        }
        st->written += written;
        if (st->progress_proc && (!st->first_reported || now - st->last_poll >= CANCEL_POLL_MS)) {
            st->first_reported = TRUE;
            st->last_poll = now;
            if (st->progress_proc(st->written, st->progress_ctx)) return RESTIC_ERR_CANCELLED;
        }
    }
    return RESTIC_OK;
}

/* Finalizes a successful transfer: applies the mtime and moves the temporary file over the target */
static ResticResult dump_commit(DumpState* st, const FILETIME* mtime) {
    HANDLE hf = dump_get_partial(st); /* Zero-byte file: nothing was streamed, create it now */
    if (hf == INVALID_HANDLE_VALUE) {
        return RESTIC_ERR_LOCAL_WRITE;
    }
    if (mtime != NULL && !is_unknown_time(mtime)) {
        SetFileTime(hf, NULL, NULL, mtime);
    }
    CloseHandle(hf);
    st->hLocal = INVALID_HANDLE_VALUE;

    DWORD flags          = 0;
    DWORD read_only_attr = 0; /* Attributes of a read-only target, restored if it cannot be replaced */
    if (st->overwrite) {
        DWORD attr = GetFileAttributesW(st->final_path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY) != 0 &&
            (attr & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            SetFileAttributesW(st->final_path, attr & ~FILE_ATTRIBUTE_READONLY)) {
            read_only_attr = attr;
        }
        flags = MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(st->part_path, st->final_path, flags)) {
        if (read_only_attr != 0) SetFileAttributesW(st->final_path, read_only_attr);
        return RESTIC_ERR_LOCAL_WRITE;
    }
    st->part_created = FALSE;
    return RESTIC_OK;
}

ResticResult restic_dump_file(
    const WCHAR* restic_exe,
    const ResticRepo* repo,
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
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    if (!restic_exe || !repo || !repo->target || !repo->password || !snapshot_id || !subpath_w ||
        !local_dest_path || local_dest_path[0] == L'\0' || !is_hex_string(snapshot_id) ||
        strlen(snapshot_id) > 64) {
        return RESTIC_ERR_INVALID_ARG;
    }
    if (!restic_exe_startable(restic_exe, out_error_msg, err_capacity)) {
        return RESTIC_ERR_SPAWN_FAILED;
    }

    /* Fail fast, before contacting the repository, when the target cannot be replaced */
    DWORD existing = GetFileAttributesW(local_dest_path);
    if (existing != INVALID_FILE_ATTRIBUTES &&
        ((existing & FILE_ATTRIBUTE_DIRECTORY) != 0 || !overwrite)) {
        return RESTIC_ERR_LOCAL_WRITE;
    }

    char filter_utf8[MAX_UTF8_PATH] = {0};
    WCHAR filter_w[MAX_UTF8_PATH]   = {0};
    WCHAR snap_w[65]                = {0};
    if (!restic_normalize_filter_path(subpath_w, filter_utf8, sizeof(filter_utf8)) ||
        strcmp(filter_utf8, "/") == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, filter_w, (int)_countof(filter_w)) <= 0) {
        return RESTIC_ERR_INVALID_ARG;
    }
    MultiByteToWideChar(CP_UTF8, 0, snapshot_id, -1, snap_w, (int)_countof(snap_w));

    WCHAR* quoted_filter = restic_quote_arg(filter_w);
    if (!quoted_filter) return RESTIC_ERR_OUT_OF_MEMORY;

    size_t cmd_chars = wcslen(restic_exe) + wcslen(L"\"\" dump --no-lock  ") + wcslen(snap_w) +
                       wcslen(quoted_filter) + 4;
    if (cmd_chars > MAX_CMDLINE_CHARS) {
        free(quoted_filter);
        return RESTIC_ERR_INVALID_ARG;
    }
    WCHAR* cmd_line = (WCHAR*)malloc(cmd_chars * sizeof(WCHAR));
    if (!cmd_line) {
        free(quoted_filter);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }
    swprintf_s(cmd_line, cmd_chars, L"\"%ls\" dump --no-lock %ls %ls", restic_exe, snap_w, quoted_filter);
    free(quoted_filter);

    DumpState st;
    memset(&st, 0, sizeof(st));
    st.final_path    = local_dest_path;
    st.overwrite     = overwrite;
    st.hLocal        = INVALID_HANDLE_VALUE;
    st.progress_proc = progress_proc;
    st.progress_ctx  = progress_ctx;
    st.part_path     = make_partial_path(local_dest_path);
    st.chunk         = (char*)malloc(DUMP_CHUNK_BYTES);

    size_t env_chars = 0;
    WCHAR* env_block = (st.part_path && st.chunk)
                           ? build_child_environment(repo, &env_chars)
                           : NULL;
    if (!env_block) {
        free(cmd_line);
        free(st.part_path);
        free(st.chunk);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }

    HANDLE out_pipe = NULL, err_pipe = NULL;
    PROCESS_INFORMATION pi;
    ULONGLONG started = GetTickCount64();
    log_restic_start(cmd_line, repo);
    BOOL spawned = spawn_hidden(cmd_line, env_block, &out_pipe, &err_pipe, &pi);
    DWORD spawn_error = spawned ? 0 : GetLastError();
    SecureZeroMemory(env_block, env_chars * sizeof(WCHAR));
    free(env_block);
    free(cmd_line);
    if (!spawned) {
        describe_spawn_failure(restic_exe, spawn_error, out_error_msg, err_capacity);
        free(st.part_path);
        free(st.chunk);
        return RESTIC_ERR_SPAWN_FAILED;
    }
    CloseHandle(pi.hThread);

    ByteBuf err         = {0};
    BOOL out_eof        = FALSE;
    BOOL err_eof        = FALSE;
    ResticResult result = RESTIC_ERR_OUT_OF_MEMORY;

    if (bytebuf_append(&err, "", 0)) {
        ULONGLONG start  = GetTickCount64();
        st.last_poll     = start;
        st.last_activity = start;
        result           = RESTIC_OK;

        for (;;) {
            ULONGLONG before_out = st.written;
            size_t err_got       = 0; /* Also the stderr bytes beyond STDERR_KEEP_BYTES: activity too */

            result = dump_drain_stdout(out_pipe, &st, &out_eof);
            if (result != RESTIC_OK) break;
            if (!drain_pipe(err_pipe, &err, &err_eof, STDERR_KEEP_BYTES, &err_got)) {
                result = RESTIC_ERR_OUT_OF_MEMORY;
                break;
            }

            if (st.written != before_out || err_got != 0) {
                st.last_activity = GetTickCount64();
            } else if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
                result = dump_drain_stdout(out_pipe, &st, &out_eof);
                if (result == RESTIC_OK && !drain_pipe(err_pipe, &err, &err_eof, STDERR_KEEP_BYTES, &err_got)) {
                    result = RESTIC_ERR_OUT_OF_MEMORY;
                }
                break;
            }

            ULONGLONG now = GetTickCount64();
            /* Inactivity timeout: a slow but progressing multi-GB transfer is never aborted */
            if (timeout_ms != 0 && now - st.last_activity >= timeout_ms) {
                result = RESTIC_ERR_TIMEOUT;
                break;
            }
            if (progress_proc && now - st.last_poll >= CANCEL_POLL_MS) {
                st.last_poll = now;
                if (progress_proc(st.written, progress_ctx)) {
                    result = RESTIC_ERR_CANCELLED;
                    break;
                }
            }
        }
    }

    BOOL exited = FALSE;
    DWORD exit_code = 1;
    if (result == RESTIC_OK) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
        exited = TRUE;
        if (exit_code != 0) {
            if (out_error_msg && err_capacity > 0) {
                ByteBuf empty_out = {0};
                build_error_message(&empty_out, &err, out_error_msg, err_capacity);
            }
            switch (exit_code) {
                case 10: result = RESTIC_ERR_REPO_NOT_FOUND; break;
                case 11: result = RESTIC_ERR_REPO_LOCKED; break;
                case 12: result = RESTIC_ERR_WRONG_PASSWORD; break;
                default:
                    if (err.data && (strstr(err.data, "not found in snapshot") != NULL ||
                                     strstr(err.data, "does not exist") != NULL)) {
                        result = RESTIC_ERR_PATH_NOT_FOUND;
                    } else {
                        result = RESTIC_ERR_CLI_FAILED;
                    }
                    break;
            }
        } else {
            result = dump_commit(&st, mtime);
        }
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    /* Any failure: discard the partial data. A pre-existing target file stays untouched. */
    if (st.hLocal != INVALID_HANDLE_VALUE) {
        CloseHandle(st.hLocal);
    }
    if (st.part_created) {
        DeleteFileW(st.part_path);
    }

    log_restic_end(result, exited, exit_code, started, &err);
    free(st.part_path);
    free(st.chunk);
    free(err.data);
    CloseHandle(out_pipe);
    CloseHandle(err_pipe);
    CloseHandle(pi.hProcess);
    return result;
}

ResticResult restic_write_local_file(
    const WCHAR* local_dest_path,
    const void* data,
    size_t len,
    BOOL overwrite,
    const FILETIME* mtime
) {
    if (!local_dest_path || local_dest_path[0] == L'\0' || (!data && len > 0)) return RESTIC_ERR_INVALID_ARG;
    if (!overwrite && GetFileAttributesW(local_dest_path) != INVALID_FILE_ATTRIBUTES) return RESTIC_ERR_LOCAL_WRITE;

    DumpState st; /* The same temporary file and atomic replacement as restic_dump_file */
    memset(&st, 0, sizeof(st));
    st.final_path = local_dest_path;
    st.overwrite  = overwrite;
    st.hLocal     = INVALID_HANDLE_VALUE;
    st.part_path  = make_partial_path(local_dest_path);
    if (!st.part_path) return RESTIC_ERR_OUT_OF_MEMORY;

    ResticResult result = RESTIC_ERR_LOCAL_WRITE;
    HANDLE hf = dump_get_partial(&st);
    BOOL written = hf != INVALID_HANDLE_VALUE;
    for (size_t done = 0; written && done < len;) {
        DWORD part = len - done > DUMP_CHUNK_BYTES ? DUMP_CHUNK_BYTES : (DWORD)(len - done), got = 0;
        written = WriteFile(hf, (const char*)data + done, part, &got, NULL) && got == part;
        done += got;
    }
    if (written) result = dump_commit(&st, mtime);

    if (st.hLocal != INVALID_HANDLE_VALUE) CloseHandle(st.hLocal);
    if (st.part_created) DeleteFileW(st.part_path);
    free(st.part_path);
    return result;
}
