#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include "config.h"

static SRWLOCK g_log_lock          = SRWLOCK_INIT;
static WCHAR g_log_path[CONFIG_MAX_PATH] = {0}; /* Empty: logging off */
static BOOL g_log_dirs_ready       = FALSE;     /* Parent directories created for g_log_path */

void log_configure(const WCHAR* raw_path) {
    WCHAR expanded[CONFIG_MAX_PATH] = {0};
    if (raw_path && raw_path[0] != L'\0') {
        DWORD n = ExpandEnvironmentStringsW(raw_path, expanded, CONFIG_MAX_PATH);
        if (n == 0 || n > CONFIG_MAX_PATH || !config_is_absolute_path(expanded)) {
            expanded[0] = L'\0';
        }
    }

    AcquireSRWLockExclusive(&g_log_lock);
    if (wcscmp(g_log_path, expanded) != 0) {
        wcsncpy_s(g_log_path, CONFIG_MAX_PATH, expanded, _TRUNCATE);
        g_log_dirs_ready = FALSE;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

BOOL log_enabled(void) {
    AcquireSRWLockShared(&g_log_lock);
    BOOL on = g_log_path[0] != L'\0';
    ReleaseSRWLockShared(&g_log_lock);
    return on;
}

void log_get_path(WCHAR* out, size_t cap) {
    if (!out || cap == 0) return;
    AcquireSRWLockShared(&g_log_lock);
    wcsncpy_s(out, cap, g_log_path, _TRUNCATE);
    ReleaseSRWLockShared(&g_log_lock);
}

/* Creates every missing directory above file_path (best effort) */
static void create_parent_dirs(const WCHAR* file_path) {
    WCHAR dir[CONFIG_MAX_PATH];
    wcsncpy_s(dir, CONFIG_MAX_PATH, file_path, _TRUNCATE);
    WCHAR* last = wcsrchr(dir, L'\\');
    WCHAR* last_fwd = wcsrchr(dir, L'/');
    if (last_fwd && (!last || last_fwd > last)) last = last_fwd;
    if (!last) return;
    *last = L'\0';

    /* Skip the drive ("X:\") or the "\\server\share\" prefix, which cannot be created */
    WCHAR* p = dir;
    if (dir[0] == L'\\' && dir[1] == L'\\') {
        int seps = 0;
        for (p = dir + 2; *p && seps < 2; ++p) {
            if (*p == L'\\' || *p == L'/') seps++;
        }
    } else if (dir[1] == L':') {
        p = dir + 3;
    }
    for (; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            WCHAR saved = *p;
            *p = L'\0';
            CreateDirectoryW(dir, NULL);
            *p = saved;
        }
    }
    CreateDirectoryW(dir, NULL);
}

void log_write(const WCHAR* fmt, ...) {
    if (!fmt) return;
    AcquireSRWLockExclusive(&g_log_lock); /* Also keeps lines of concurrent threads apart */
    if (g_log_path[0] == L'\0') {
        ReleaseSRWLockExclusive(&g_log_lock);
        return;
    }

    WCHAR line[LOG_MAX_LINE];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int prefix = _snwprintf_s(line, LOG_MAX_LINE, _TRUNCATE,
                              L"%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu tid %lu] ",
                              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                              st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
    if (prefix < 0) prefix = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line + prefix, LOG_MAX_LINE - (size_t)prefix - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    wcscat_s(line, LOG_MAX_LINE, L"\r\n");

    char utf8[LOG_MAX_LINE * 3];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, (int)sizeof(utf8), NULL, NULL);

    if (!g_log_dirs_ready) {
        create_parent_dirs(g_log_path);
        g_log_dirs_ready = TRUE;
    }
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (bytes > 1) WriteFile(h, utf8, (DWORD)(bytes - 1), &written, NULL);
        CloseHandle(h);
    } else {
        g_log_dirs_ready = FALSE; /* Retry the directories next time (e.g. a drive came back) */
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void log_redact_target(const WCHAR* target, WCHAR* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!target) return;
    wcsncpy_s(out, cap, target, _TRUNCATE);

    const WCHAR* scheme_end = wcsstr(target, L"://");
    if (!scheme_end) return;
    const WCHAR* authority = scheme_end + 3;
    /* The last '@' anywhere, not only before the first '/': a password written with an unencoded
       '/', '?' or '#' would otherwise end the authority early and be logged in plain text */
    const WCHAR* at = wcsrchr(authority, L'@');
    if (!at) return;

    _snwprintf_s(out, cap, _TRUNCATE, L"%.*ls***%ls", (int)(authority - target), target, at);
}
