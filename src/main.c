#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifdef _MSC_VER
/* Windows SDK 10.0.26100 annotates the fourth parameter of SetThreadpoolTimer, a plain DWORD, as
   _In_opt_, which /analyze reports as C6553 at every call. This file has no SAL annotations of its
   own, so the check has nothing else to find here. */
#pragma warning(disable : 6553)
#endif
#include <windows.h>
#include <dpapi.h>
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "fsplugin.h"
#include "config.h"
#include "restic_cli.h"
#include "log.h"
#include "version.h"

#define SESSION_MAX_PASSWORD  512
#define SESSION_SLOTS         8
#define MAX_PASSWORD_ATTEMPTS 3
/*
 * Virtual file in the plugin root, listed only while passwords are cached and named after their
 * number ("<wastebasket> Forget 2 Passwords"): Enter on it (FsExecuteFileW "open") wipes them all
 * and has Total Commander re-read the root.
 * Any root name starting with FORGET_ENTRY_PREFIX is the command (the count may have changed since
 * the listing). The wastebasket emoji (U+1F5D1) keeps it apart from the repository folders.
 */
#define FORGET_ENTRY_PREFIX L"\U0001F5D1 Forget "
#define WFX_RESTIC_TIMEOUT_MS (30u * 60u * 1000u) /* Safety net if a hung command is never aborted */

/* Host callbacks supplied by Total Commander in FsInitW */
static int            g_PluginNr     = 0;
static tProgressProcW g_ProgressProc = NULL;
static tLogProcW      g_LogProc      = NULL;
static tRequestProcW  g_RequestProc  = NULL;

/*
 * In-memory credential sessions, one slot per repository alias (two panels can browse two
 * repositories alternately without being prompted again). Guarded by an SRW lock because Total
 * Commander may call into the plugin from background threads. Slots are wiped on wrong password,
 * FsInitW and DLL unload. SRWLOCK_INIT keeps the lock usable even if DllMain never ran (unit tests).
 *
 * The table lives in its own pages, allocated on first use:
 *  - VirtualLock keeps them out of the page file;
 *  - WerRegisterExcludedMemoryBlock (Windows 10+, looked up at run time) leaves them out of
 *    Windows Error Reporting crash dumps;
 *  - every cached password is encrypted with CryptProtectMemory(CRYPTPROTECTMEMORY_SAME_PROCESS),
 *    whose key never appears in the process, so a memory dump, the hibernation file or a page that
 *    escaped the lock holds ciphertext only. A password is decrypted into a stack buffer just for
 *    the restic call and wiped right after it.
 * If encryption fails, the password is not cached at all (the user is asked again next time).
 */
typedef struct {
    BOOL  used;
    WCHAR alias[MAX_PATH];
    WCHAR password[SESSION_MAX_PASSWORD]; /* CryptProtectMemory ciphertext, never plaintext */
    ULONGLONG cached_at;                  /* GetTickCount64() when restic first accepted it (last used) */
} SessionSlot;

_Static_assert(sizeof(((SessionSlot*)0)->password) % CRYPTPROTECTMEMORY_BLOCK_SIZE == 0,
               "CryptProtectMemory works on whole blocks");

typedef HRESULT (WINAPI *PFN_WerRegisterExcludedMemoryBlock)(const void*, DWORD);
typedef HRESULT (WINAPI *PFN_WerUnregisterExcludedMemoryBlock)(const void*);

static SessionSlot* g_Sessions         = NULL; /* SESSION_SLOTS slots in locked pages, or NULL */
static SIZE_T       g_SessionBytes     = 0;
static BOOL         g_SessionLocked    = FALSE; /* VirtualLock succeeded */
static BOOL         g_SessionWerExcluded = FALSE;
static unsigned     g_SessionEvictNext = 0;
static SRWLOCK      g_SessionLock      = SRWLOCK_INIT;

/*
 * [Settings] PasswordTimeoutSeconds: a cached password is wiped this long after restic first
 * accepted it (0 = keep until Total Commander exits); with PasswordTimeoutResetOnUse=yes, every
 * operation restic completes with the cached password starts the interval again instead. A
 * thread-pool timer fires at the next expiry, so a password does not outlive its timeout while
 * Total Commander sits idle for days. GetTickCount64 keeps counting during sleep and hibernation.
 */
static ULONGLONG    g_PasswordTimeoutMs   = CONFIG_DEFAULT_PASSWORD_TIMEOUT_S * 1000ull;
static BOOL         g_PasswordResetOnUse  = FALSE; /* [Settings] PasswordTimeoutResetOnUse */
static PTP_TIMER    g_SessionTimer        = NULL;
static BOOL         g_SessionShuttingDown = FALSE; /* DLL unload: never create the timer again */

/* Allocates, locks and excludes the slot table from crash dumps; caller holds g_SessionLock */
static BOOL session_table_locked(void) {
    if (g_Sessions) return TRUE;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    SIZE_T page  = si.dwPageSize ? si.dwPageSize : 4096;
    SIZE_T bytes = (sizeof(SessionSlot) * SESSION_SLOTS + page - 1) / page * page;
    SessionSlot* table = (SessionSlot*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!table) {
        log_write(L"Password cache unavailable: VirtualAlloc failed (Windows error %lu)", GetLastError());
        return FALSE;
    }
    g_SessionLocked = VirtualLock(table, bytes);
    if (!g_SessionLocked) {
        log_write(L"Password cache not locked in memory (Windows error %lu); passwords stay encrypted",
                  GetLastError());
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    PFN_WerRegisterExcludedMemoryBlock reg =
        kernel ? (PFN_WerRegisterExcludedMemoryBlock)(void*)GetProcAddress(kernel, "WerRegisterExcludedMemoryBlock")
               : NULL;
    g_SessionWerExcluded = reg && SUCCEEDED(reg(table, (DWORD)bytes));
    g_Sessions     = table;
    g_SessionBytes = bytes;
    return TRUE;
}

/* Releases the slot table (DLL unload); caller holds g_SessionLock */
static void session_table_release_locked(void) {
    if (!g_Sessions) return;
    SecureZeroMemory(g_Sessions, g_SessionBytes);
    if (g_SessionWerExcluded) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        PFN_WerUnregisterExcludedMemoryBlock unreg =
            kernel ? (PFN_WerUnregisterExcludedMemoryBlock)(void*)GetProcAddress(kernel, "WerUnregisterExcludedMemoryBlock")
                   : NULL;
        if (unreg) unreg(g_Sessions);
    }
    if (g_SessionLocked) VirtualUnlock(g_Sessions, g_SessionBytes);
    VirtualFree(g_Sessions, 0, MEM_RELEASE);
    g_Sessions           = NULL;
    g_SessionBytes       = 0;
    g_SessionLocked      = FALSE;
    g_SessionWerExcluded = FALSE;
}

/**
 * Reentrant directory enumeration state returned as HANDLE by FsFindFirstW.
 */
typedef struct {
    WIN32_FIND_DATAW* items; /* Array of entries discovered for the target path */
    size_t count;            /* Total number of entries in items */
    size_t current_index;    /* Next entry index to return in FsFindNextW */
} ResticFindHandle;

/**
 * Virtual path hierarchy levels inside the WFX plugin.
 */
typedef enum {
    WFX_PATH_ROOT = 0,     /* "\" -> list configured repository aliases */
    WFX_PATH_REPO,         /* "\<RepoAlias>" -> list snapshots */
    WFX_PATH_SNAPSHOT_DIR, /* "\<RepoAlias>\<SnapshotDir>[\<SubPath>]" -> list directory */
    WFX_PATH_INVALID       /* Malformed path or invalid snapshot directory name */
} WfxPathLevel;

/**
 * Decomposed components of a WFX virtual path.
 */
typedef struct {
    WfxPathLevel level;
    WCHAR repo_alias[MAX_PATH];
    char snapshot_id[65];
    const WCHAR* subpath; /* Points into the caller's Path buffer (or L"") */
} WfxParsedPath;

/**
 * Context forwarded to the cancellation polling callback while restic.exe runs.
 */
typedef struct {
    const WCHAR* virtual_path;
    const WCHAR* repo_alias; /* Never the repository URL: it may embed credentials */
} WfxCancelContext;

static void session_expire_locked(void);

static VOID CALLBACK session_timer_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer) {
    (void)instance;
    (void)context;
    (void)timer;
    AcquireSRWLockExclusive(&g_SessionLock);
    session_expire_locked();
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* Arms the expiry timer to fire in due_ms (0: stop it); caller holds g_SessionLock */
static void session_arm_timer_locked(ULONGLONG due_ms) {
    if (due_ms == 0) {
        if (g_SessionTimer) SetThreadpoolTimer(g_SessionTimer, NULL, 0, 0);
        return;
    }
    if (!g_SessionTimer) {
        if (g_SessionShuttingDown) return;
        g_SessionTimer = CreateThreadpoolTimer(session_timer_callback, NULL, NULL);
        if (!g_SessionTimer) {
            log_write(L"Password timeout timer unavailable (Windows error %lu); expiry is checked on use only",
                      GetLastError());
            return;
        }
    }
    ULARGE_INTEGER due;
    due.QuadPart = (ULONGLONG)(-(LONGLONG)((due_ms + 100) * 10000)); /* Relative, 100 ns units */
    FILETIME ft;
    ft.dwLowDateTime  = due.LowPart;
    ft.dwHighDateTime = due.HighPart;
    SetThreadpoolTimer(g_SessionTimer, &ft, 0, 1000);
}

/* Wipes the passwords whose timeout elapsed and re-arms the timer; caller holds g_SessionLock */
static void session_expire_locked(void) {
    if (!g_Sessions) return;
    ULONGLONG now = GetTickCount64(), next_due = 0;
    for (int i = 0; i < SESSION_SLOTS; ++i) {
        SessionSlot* slot = &g_Sessions[i];
        if (!slot->used || g_PasswordTimeoutMs == 0) continue;
        ULONGLONG age = now - slot->cached_at;
        if (age >= g_PasswordTimeoutMs) {
            log_write(L"Cached password for '%ls' forgotten: PasswordTimeoutSeconds (%llu s) elapsed",
                      slot->alias, g_PasswordTimeoutMs / 1000);
            SecureZeroMemory(slot, sizeof(*slot));
            continue;
        }
        ULONGLONG left = g_PasswordTimeoutMs - age;
        if (next_due == 0 || left < next_due) next_due = left;
    }
    session_arm_timer_locked(next_due);
}

/*
 * Applies [Settings] PasswordTimeoutSeconds and PasswordTimeoutResetOnUse; a shorter timeout also
 * applies to passwords already cached.
 */
static void apply_password_timeout(DWORD seconds, BOOL reset_on_use) {
    AcquireSRWLockExclusive(&g_SessionLock);
    g_PasswordResetOnUse = reset_on_use;
    ULONGLONG ms = (ULONGLONG)seconds * 1000ull;
    if (ms != g_PasswordTimeoutMs) {
        g_PasswordTimeoutMs = ms;
        session_expire_locked();
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
}

static SessionSlot* session_find_locked(const WCHAR* alias) {
    if (!g_Sessions) return NULL;
    for (int i = 0; i < SESSION_SLOTS; ++i) {
        if (g_Sessions[i].used && _wcsicmp(g_Sessions[i].alias, alias) == 0) {
            return &g_Sessions[i];
        }
    }
    return NULL;
}

/*
 * Decrypts the cached password of `alias` into out_pwd (SESSION_MAX_PASSWORD characters; the caller
 * wipes it). Returns FALSE if none is cached.
 */
static BOOL session_get(const WCHAR* alias, WCHAR* out_pwd, size_t cap) {
    BOOL found = FALSE;
    if (cap < SESSION_MAX_PASSWORD) return FALSE;
    AcquireSRWLockExclusive(&g_SessionLock);
    session_expire_locked();
    SessionSlot* slot = session_find_locked(alias);
    if (slot) {
        memcpy(out_pwd, slot->password, sizeof(slot->password));
        found = CryptUnprotectMemory(out_pwd, (DWORD)sizeof(slot->password), CRYPTPROTECTMEMORY_SAME_PROCESS);
        if (!found) {
            SecureZeroMemory(out_pwd, sizeof(slot->password));
            SecureZeroMemory(slot, sizeof(*slot));
        }
        out_pwd[SESSION_MAX_PASSWORD - 1] = L'\0';
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
    return found;
}

/* Stores (or replaces) the password of `alias`; the oldest slot is recycled when the table is full */
static void session_put(const WCHAR* alias, const WCHAR* password) {
    AcquireSRWLockExclusive(&g_SessionLock);
    if (!session_table_locked()) {
        ReleaseSRWLockExclusive(&g_SessionLock);
        return;
    }
    SessionSlot* slot = session_find_locked(alias);
    for (int i = 0; !slot && i < SESSION_SLOTS; ++i) {
        if (!g_Sessions[i].used) slot = &g_Sessions[i];
    }
    if (!slot) {
        slot = &g_Sessions[g_SessionEvictNext++ % SESSION_SLOTS];
    }
    SecureZeroMemory(slot, sizeof(*slot));
    slot->used = TRUE;
    wcsncpy_s(slot->alias, MAX_PATH, alias, _TRUNCATE);
    wcsncpy_s(slot->password, SESSION_MAX_PASSWORD, password, _TRUNCATE);
    slot->cached_at = GetTickCount64();
    if (!CryptProtectMemory(slot->password, (DWORD)sizeof(slot->password), CRYPTPROTECTMEMORY_SAME_PROCESS)) {
        DWORD err = GetLastError();
        SecureZeroMemory(slot, sizeof(*slot)); /* Never keep a plaintext password */
        log_write(L"Password for '%ls' not cached: CryptProtectMemory failed (Windows error %lu)", alias, err);
    }
    session_expire_locked(); /* Arms the timer for this password's expiry */
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* PasswordTimeoutResetOnUse: restic completed an operation with the cached password of `alias` */
static void session_touch(const WCHAR* alias) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SessionSlot* slot = g_PasswordResetOnUse ? session_find_locked(alias) : NULL;
    if (slot) {
        slot->cached_at = GetTickCount64();
        session_expire_locked(); /* Re-arms the timer for the later expiry */
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* Wipes the cached password of a single repository */
static void session_forget(const WCHAR* alias) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SessionSlot* slot = session_find_locked(alias);
    if (slot) SecureZeroMemory(slot, sizeof(*slot));
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* TRUE for a root entry name that is the forget command (also used to reserve such aliases) */
static BOOL is_forget_entry_name(const WCHAR* name) {
    return name && _wcsnicmp(name, FORGET_ENTRY_PREFIX, wcslen(FORGET_ENTRY_PREFIX)) == 0;
}

/* Number of passwords currently cached */
static unsigned cached_password_count(void) {
    unsigned n = 0;
    AcquireSRWLockExclusive(&g_SessionLock);
    session_expire_locked();
    for (int i = 0; g_Sessions && i < SESSION_SLOTS; ++i) {
        if (g_Sessions[i].used) n++;
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
    return n;
}

/* Wipes every cached password from RAM */
static void clear_password_session(void) {
    AcquireSRWLockExclusive(&g_SessionLock);
    if (g_Sessions) SecureZeroMemory(g_Sessions, sizeof(SessionSlot) * SESSION_SLOTS);
    g_SessionEvictNext = 0;
    session_arm_timer_locked(0);
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/*
 * Wipes and frees the password table (DLL unload). On FreeLibrary the expiry timer is stopped and
 * pending callbacks are cancelled or waited for, outside g_SessionLock (a running callback needs
 * it), before the code they would run is unmapped. At process exit other threads are already gone.
 */
static void release_password_session(BOOL process_exit) {
    AcquireSRWLockExclusive(&g_SessionLock);
    g_SessionShuttingDown = TRUE;
    PTP_TIMER timer       = g_SessionTimer;
    g_SessionTimer        = NULL;
    if (timer) SetThreadpoolTimer(timer, NULL, 0, 0);
    ReleaseSRWLockExclusive(&g_SessionLock);

    if (timer && !process_exit) {
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }

    AcquireSRWLockExclusive(&g_SessionLock);
    session_table_release_locked();
    g_SessionEvictNext = 0;
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* config_load() plus the settings the plugin applies globally (PasswordTimeoutSeconds) */
static BOOL load_config(ResticConfig* cfg) {
    BOOL ok = config_load(cfg);
    apply_password_timeout(cfg->password_timeout_s, cfg->password_timeout_reset_on_use);
    return ok;
}

/* The Network Neighborhood root name: [Settings] RootName, or CONFIG_DEFAULT_ROOT_NAME */
static void load_root_name(WCHAR* out, size_t capacity) {
    ResticConfig cfg;
    load_config(&cfg);
    wcsncpy_s(out, capacity, cfg.root_name, _TRUNCATE);
    config_free(&cfg);
}

/* Logs an important error and displays an OK message dialog in Total Commander */
static void report_plugin_error(const WCHAR* repo_alias, const WCHAR* detail) {
    WCHAR msg[1024]; /* alias (<260) + Restic message (<512) + decoration always fits; _TRUNCATE anyway */
    if (repo_alias && repo_alias[0] != L'\0') {
        _snwprintf_s(
            msg,
            _countof(msg),
            _TRUNCATE,
            L"[%ls] %ls",
            repo_alias,
            (detail && detail[0] != L'\0') ? detail : L"Restic command failed."
        );
    } else {
        wcsncpy_s(
            msg,
            _countof(msg),
            (detail && detail[0] != L'\0') ? detail : L"Restic plugin error.",
            _TRUNCATE
        );
    }

    log_write(L"Error shown to the user: %ls", msg);
    if (g_LogProc) {
        g_LogProc(g_PluginNr, MSGTYPE_IMPORTANTERROR, msg);
    }
    if (g_RequestProc) {
        WCHAR unused[4] = {0};
        WCHAR title[MAX_PATH];
        load_root_name(title, _countof(title));
        g_RequestProc(g_PluginNr, RT_MsgOK, title, msg, unused, _countof(unused));
    }
}

/* Upper bound of restic_describe_password() text (a few short phrases) */
#define PASSWORD_HINT_CHARS 384

/*
 * Prompts the user for the repository password via Total Commander's RequestProcW.
 * `wrong_hint` is the restic_describe_password() text of the password restic just rejected
 * (NULL or empty: none), shown so a stray space or line break can be spotted in a masked field.
 * Returns FALSE when the dialog is cancelled or an empty password is entered (Restic refuses
 * empty passwords, and accepting one would only produce a confusing generic failure).
 */
static BOOL prompt_repository_password(
    const WCHAR* repo_alias,
    BOOL is_retry,
    const WCHAR* wrong_hint,
    WCHAR* out_pwd,
    size_t cap
) {
    if (!g_RequestProc || !repo_alias || repo_alias[0] == L'\0' || cap == 0) {
        return FALSE;
    }

    WCHAR prompt_text[MAX_PATH + PASSWORD_HINT_CHARS + 128];
    if (is_retry && wrong_hint && wrong_hint[0] != L'\0') {
        _snwprintf_s(prompt_text, _countof(prompt_text), _TRUNCATE,
                     L"Wrong password. Note: the password you entered %ls. Enter password for '%ls':",
                     wrong_hint, repo_alias);
    } else {
        _snwprintf_s(prompt_text, _countof(prompt_text), _TRUNCATE,
                     is_retry ? L"Wrong password. Enter password for '%ls':"
                              : L"Enter password for repository '%ls':",
                     repo_alias);
    }

    out_pwd[0] = L'\0'; /* RequestProc shows ReturnedText as the default value */
    BOOL accepted = g_RequestProc(
        g_PluginNr,
        RT_Password,
        L"Restic Repository Password",
        prompt_text,
        out_pwd,
        (int)cap
    );
    out_pwd[cap - 1] = L'\0';
    if (!accepted || out_pwd[0] == L'\0') {
        log_write(L"Password prompt for '%ls'%ls: %ls", repo_alias, is_retry ? L" (retry)" : L"",
                  accepted ? L"empty password, treated as cancel" : L"cancelled");
        SecureZeroMemory(out_pwd, cap * sizeof(WCHAR));
        return FALSE;
    }
    WCHAR hint[PASSWORD_HINT_CHARS];
    BOOL odd = restic_describe_password(out_pwd, hint, _countof(hint));
    log_write(L"Password prompt for '%ls'%ls: %u characters entered%ls%ls", repo_alias,
              is_retry ? L" (retry)" : L"", (unsigned)wcslen(out_pwd), odd ? L"; the password " : L"",
              odd ? hint : L"");
    return TRUE;
}

/*
 * Polled every ~100 ms by restic_exec_command; returns TRUE if the user pressed ESC in TC.
 * Total Commander ignores ProgressProc calls during the first 5 seconds of a directory change and
 * only then shows a progress dialog, so fast listings never flicker. 0 % = indeterminate.
 */
static BOOL wfx_cancel_callback(void* user_data) {
    if (!g_ProgressProc) {
        return FALSE;
    }
    const WfxCancelContext* ctx = (const WfxCancelContext*)user_data;
    WCHAR* src = (ctx && ctx->virtual_path) ? (WCHAR*)ctx->virtual_path : L"\\";
    WCHAR* dst = (ctx && ctx->repo_alias) ? (WCHAR*)ctx->repo_alias : L"";
    return g_ProgressProc(g_PluginNr, src, dst, 0) != 0;
}

/* Splits a WFX path into its hierarchy level, repository alias, snapshot ID, and subpath */
static BOOL parse_wfx_path(const WCHAR* path, WfxParsedPath* out) {
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->level   = WFX_PATH_INVALID;
    out->subpath = L"";

    if (!path) return FALSE;

    const WCHAR* p = path;
    while (*p == L'\\') {
        p++;
    }
    if (*p == L'\0') {
        out->level = WFX_PATH_ROOT;
        return TRUE;
    }

    /* Segment 1: <RepoAlias> */
    const WCHAR* seg_start = p;
    while (*p != L'\0' && *p != L'\\') {
        p++;
    }
    size_t seg_len = (size_t)(p - seg_start);
    if (seg_len == 0 || seg_len >= MAX_PATH) {
        return FALSE;
    }
    wcsncpy_s(out->repo_alias, MAX_PATH, seg_start, seg_len);

    while (*p == L'\\') {
        p++;
    }
    if (*p == L'\0') {
        out->level = WFX_PATH_REPO;
        return TRUE;
    }

    /* Segment 2: <SnapshotDir> ("YYYY-MM-DD_HH-MM-SS_[short_id]") */
    seg_start = p;
    while (*p != L'\0' && *p != L'\\') {
        p++;
    }
    seg_len = (size_t)(p - seg_start);
    if (seg_len == 0 || seg_len >= MAX_PATH) {
        return FALSE;
    }

    WCHAR snapshot_dirname[MAX_PATH] = {0};
    wcsncpy_s(snapshot_dirname, MAX_PATH, seg_start, seg_len);
    if (!restic_extract_snapshot_id(snapshot_dirname, out->snapshot_id, sizeof(out->snapshot_id))) {
        return FALSE;
    }

    /* Remainder (empty or starting with '\') is the subpath inside the snapshot */
    out->subpath = p;
    out->level   = WFX_PATH_SNAPSHOT_DIR;
    return TRUE;
}

/* Wraps a heap-allocated WIN32_FIND_DATAW array into a ResticFindHandle */
static HANDLE wrap_find_items(
    WIN32_FIND_DATAW* items,
    size_t count,
    WIN32_FIND_DATAW* first_out
) {
    if (count == 0) {
        free(items);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    ResticFindHandle* handle = (ResticFindHandle*)calloc(1, sizeof(ResticFindHandle));
    if (!handle) {
        free(items);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    handle->items         = items;
    handle->count         = count;
    handle->current_index = 1;
    *first_out            = items[0];
    return (HANDLE)handle;
}

/* Maps a ResticResult failure to a Win32 error code and notifies Total Commander if needed */
static void handle_restic_failure(
    ResticResult res,
    const WCHAR* repo_alias,
    const WCHAR* err_msg
) {
    switch (res) {
        case RESTIC_ERR_PATH_NOT_FOUND:
            SetLastError(ERROR_PATH_NOT_FOUND);
            break;
        case RESTIC_ERR_CANCELLED:
            SetLastError(ERROR_CANCELLED);
            break;
        case RESTIC_ERR_WRONG_PASSWORD:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Wrong repository password."
            );
            SetLastError(ERROR_ACCESS_DENIED);
            break;
        case RESTIC_ERR_REPO_NOT_FOUND:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Repository does not exist."
            );
            SetLastError(ERROR_PATH_NOT_FOUND);
            break;
        case RESTIC_ERR_REPO_LOCKED:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Repository is locked."
            );
            SetLastError(ERROR_LOCK_VIOLATION);
            break;
        case RESTIC_ERR_SPAWN_FAILED:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Failed to execute restic.exe."
            );
            SetLastError(ERROR_FILE_NOT_FOUND);
            break;
        case RESTIC_ERR_TIMEOUT:
            report_plugin_error(repo_alias, L"Restic command timed out.");
            SetLastError(ERROR_TIMEOUT);
            break;
        case RESTIC_ERR_OUT_OF_MEMORY:
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            break;
        case RESTIC_ERR_INVALID_ARG:
            SetLastError(ERROR_INVALID_PARAMETER);
            break;
        default:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Restic command failed."
            );
            SetLastError(ERROR_GEN_FAILURE);
            break;
    }
}

/*
 * True only when restic demonstrably accepted the password, so it is worth caching for the next
 * call. Restic opens the repository with the password (exit code 12 if it is wrong) before it locks
 * it or reads a snapshot, so these outcomes prove it right: success, "repository locked" (exit
 * code 11), unexpected output after exit code 0, and a path missing from a snapshot (restic had to
 * decrypt the snapshot to tell). Anything that may end before the password was checked - a local
 * write error, ESC, a timeout, a failure to start restic - leaves an unverified password uncached;
 * a password cached earlier stays.
 */
static BOOL password_worth_caching(ResticResult res) {
    return res == RESTIC_OK || res == RESTIC_ERR_REPO_LOCKED || res == RESTIC_ERR_JSON_PARSE ||
           res == RESTIC_ERR_PATH_NOT_FOUND;
}

/* Executes a Level 1 (snapshots) or Level 2 (directory) query with password prompt/retry */
static HANDLE execute_repo_query(
    const WCHAR* virtual_path,
    const WCHAR* restic_exe,
    const ResticRepoEntry* repo,
    const WfxParsedPath* parsed,
    WIN32_FIND_DATAW* FindData
) {
    /* Verify that restic.exe was resolved to an explicit path before prompting for a password */
    if (wcschr(restic_exe, L'\\') == NULL && wcschr(restic_exe, L'/') == NULL) {
        report_plugin_error(
            repo->alias,
            L"restic.exe not found. Set ResticPath in TCResticBrowser.ini or place restic.exe in the plugin directory / PATH."
        );
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    WfxCancelContext cancel_ctx = {virtual_path, repo->alias};
    ResticExecOptions exec_opts = {WFX_RESTIC_TIMEOUT_MS, wfx_cancel_callback, &cancel_ctx};

    WCHAR password[SESSION_MAX_PASSWORD] = {0}; /* Local copy, wiped before returning */
    WCHAR wrong_hint[PASSWORD_HINT_CHARS] = {0}; /* What looked odd in a rejected password */
    BOOL have_password                   = session_get(repo->alias, password, _countof(password));
    BOOL from_cache                      = have_password; /* Cached: not stored again (see session_touch) */
    WIN32_FIND_DATAW* items              = NULL;
    size_t count                         = 0;
    WCHAR err_msg[512]                   = {0};
    ResticResult res                     = RESTIC_ERR_WRONG_PASSWORD;

    for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS; ++attempt) {
        if (!have_password) {
            if (!prompt_repository_password(repo->alias, attempt > 0, wrong_hint, password, _countof(password))) {
                SecureZeroMemory(password, sizeof(password));
                SetLastError(ERROR_CANCELLED);
                return INVALID_HANDLE_VALUE;
            }
            have_password = TRUE;
            from_cache    = FALSE;
        }

        err_msg[0] = L'\0';
        if (parsed->level == WFX_PATH_REPO) {
            res = restic_list_snapshots(
                restic_exe, repo->target, password, &exec_opts, &items, &count, err_msg, _countof(err_msg)
            );
        } else {
            res = restic_list_directory(
                restic_exe,
                repo->target,
                password,
                parsed->snapshot_id,
                parsed->subpath,
                &exec_opts,
                &items,
                &count,
                err_msg,
                _countof(err_msg)
            );
        }

        if (res == RESTIC_ERR_WRONG_PASSWORD) {
            session_forget(repo->alias);
            restic_describe_password(password, wrong_hint, _countof(wrong_hint));
            SecureZeroMemory(password, sizeof(password));
            have_password = FALSE;
            continue;
        }
        break;
    }

    if (have_password && password_worth_caching(res)) {
        if (from_cache) {
            session_touch(repo->alias);
        } else {
            session_put(repo->alias, password);
        }
    }
    SecureZeroMemory(password, sizeof(password));

    if (res != RESTIC_OK) {
        handle_restic_failure(res, repo->alias, err_msg);
        return INVALID_HANDLE_VALUE;
    }

    return wrap_find_items(items, count, FindData);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        config_set_plugin_dir_from_module((HMODULE)hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        release_password_session(lpvReserved != NULL);
    }
    return TRUE;
}

/*
 * Total Commander calls this only while installing the plugin (without FsInitW) and stores the
 * answer in wincmd.ini, so a later change of RootName needs the plugin to be added again. The
 * API is ANSI: a configured name the ANSI code page cannot represent, or one longer than maxlen,
 * yields the default name instead of question marks or a truncated name.
 */
void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    if (DefRootName == NULL || maxlen <= 0) return;

    WCHAR name[MAX_PATH];
    load_root_name(name, _countof(name));

    char ansi[MAX_PATH * 4];
    BOOL lossy = FALSE;
    BOOL utf8_acp = (GetACP() == CP_UTF8); /* lpUsedDefaultChar must be NULL for CP_UTF8 */
    int n = WideCharToMultiByte(CP_ACP, utf8_acp ? 0 : WC_NO_BEST_FIT_CHARS, name, -1,
                                ansi, (int)sizeof(ansi), NULL, utf8_acp ? NULL : &lossy);
    if (n <= 0 || lossy || n > maxlen) {
        WideCharToMultiByte(CP_ACP, 0, CONFIG_DEFAULT_ROOT_NAME, -1, ansi, (int)sizeof(ansi), NULL, NULL);
    }
    strncpy_s(DefRootName, (size_t)maxlen, ansi, _TRUNCATE);
}

int __stdcall FsInit(
    int PluginNr,
    tProgressProc pProgressProc,
    tLogProc pLogProc,
    tRequestProc pRequestProc
) {
    (void)pProgressProc;
    (void)pLogProc;
    (void)pRequestProc;
    g_PluginNr = PluginNr;
    return 0;
}

int __stdcall FsInitW(
    int PluginNr,
    tProgressProcW pProgressProcW,
    tLogProcW pLogProcW,
    tRequestProcW pRequestProcW
) {
    clear_password_session();
    g_PluginNr     = PluginNr;
    g_ProgressProc = pProgressProcW;
    g_LogProc      = pLogProcW;
    g_RequestProc  = pRequestProcW;

    ResticConfig cfg; /* Applies [Logging] LogPath before the first log line */
    load_config(&cfg);
    config_free(&cfg);
    log_write(L"FsInitW: TCResticBrowser %hs (%u-bit), plugin directory %ls", RESTIC_WFX_PRODUCT_VER,
              (unsigned)(sizeof(void*) * 8), config_get_plugin_dir());
    return 0;
}

/*
 * DefaultIniName is deliberately ignored: like the author's other Total Commander plugins, the
 * configuration is looked up in %APPDATA%\GHISLER and the plugin directory (see config.h).
 */
void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps) {
    (void)dps;
}

/* Logs the effective configuration (repository credentials redacted) */
static void log_configuration(const ResticConfig* cfg) {
    if (!log_enabled()) return;
    WCHAR log_path[CONFIG_MAX_PATH];
    log_get_path(log_path, _countof(log_path));
    log_write(L"Configuration: %ls", cfg->ini_path);
    log_write(L"  ResticPath resolved to: %ls", cfg->restic_path);
    log_write(L"  RootName: %ls; LogPath: %ls", cfg->root_name, log_path);
    log_write(L"  %u repositories:", (unsigned)cfg->repo_count);
    for (size_t i = 0; i < cfg->repo_count; ++i) {
        WCHAR target[CONFIG_MAX_PATH];
        log_redact_target(cfg->repos[i].target, target, _countof(target));
        log_write(L"    %ls = %ls", cfg->repos[i].alias, target);
    }
}

/* Populates a ResticFindHandle for the root path "\" from [Repositories] */
static HANDLE enumerate_root_repositories(WIN32_FIND_DATAW* FindData) {
    ResticConfig cfg;
    BOOL loaded = load_config(&cfg);
    log_configuration(&cfg);
    if (!loaded || cfg.repo_count == 0) {
        config_free(&cfg);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    WIN32_FIND_DATAW* items = (WIN32_FIND_DATAW*)calloc(cfg.repo_count + 1, sizeof(WIN32_FIND_DATAW));
    if (!items) {
        config_free(&cfg);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    /* The forget command first (only while passwords are cached), then the repository folders */
    size_t count = 0;
    unsigned cached = cached_password_count();
    if (cached > 0) {
        items[count].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        FS_SET_TIME_UNKNOWN(items[count].ftLastWriteTime);
        _snwprintf_s(items[count].cFileName, MAX_PATH, _TRUNCATE, L"%ls%u Password%ls", FORGET_ENTRY_PREFIX,
                     cached, cached == 1 ? L"" : L"s");
        count++;
    }
    for (size_t i = 0; i < cfg.repo_count; ++i) {
        if (is_forget_entry_name(cfg.repos[i].alias)) {
            log_write(L"Repository alias '%ls' is reserved for the forget command and not listed", cfg.repos[i].alias);
            continue;
        }
        items[count].dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        FS_SET_TIME_UNKNOWN(items[count].ftLastWriteTime); /* Aliases have no date */
        wcsncpy_s(items[count].cFileName, MAX_PATH, cfg.repos[i].alias, _TRUNCATE);
        count++;
    }

    config_free(&cfg);
    return wrap_find_items(items, count, FindData);
}

/*
 * Enter on a file ("open"): the root forget entry wipes every cached password, says how many
 * there were and sends Total Commander back to the root (FS_EXEC_SYMLINK to "\\"), which re-reads
 * it, so the entry disappears without Ctrl+R; any other file is downloaded and opened by
 * Total Commander itself, as before this function was exported. "properties" has nothing to show; "chmod" and "quote"
 * commands are not supported by this read-only plugin.
 */
int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb) {
    (void)MainWin;
    if (!RemoteName || !Verb) return FS_EXEC_ERROR;
    BOOL is_forget = RemoteName[0] == L'\\' && wcschr(RemoteName + 1, L'\\') == NULL &&
                     is_forget_entry_name(RemoteName + 1);

    if (_wcsicmp(Verb, L"open") == 0) {
        if (!is_forget) return FS_EXEC_YOURSELF;
        unsigned n = cached_password_count();
        clear_password_session();
        log_write(L"Forget All Passwords: %u cached password(s) wiped", n);
        if (g_RequestProc) {
            WCHAR msg[128], title[MAX_PATH], unused[4] = {0};
            if (n == 0) {
                wcscpy_s(msg, _countof(msg), L"No repository password was remembered.");
            } else {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Forgot %u remembered repository password%ls.", n,
                             n == 1 ? L"" : L"s");
            }
            load_root_name(title, _countof(title));
            g_RequestProc(g_PluginNr, RT_MsgOK, title, msg, unused, _countof(unused));
        }
        wcscpy_s(RemoteName, MAX_PATH, L"\\");
        return FS_EXEC_SYMLINK;
    }
    if (_wcsicmp(Verb, L"properties") == 0) return FS_EXEC_OK;
    return FS_EXEC_ERROR;
}

int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    WCHAR remote_w[MAX_PATH], verb_w[MAX_PATH];
    if (!RemoteName || !Verb ||
        MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, remote_w, MAX_PATH) == 0 ||
        MultiByteToWideChar(CP_ACP, 0, Verb, -1, verb_w, MAX_PATH) == 0) {
        return FS_EXEC_ERROR;
    }
    int rc = FsExecuteFileW(MainWin, remote_w, verb_w);
    if (rc == FS_EXEC_SYMLINK &&
        WideCharToMultiByte(CP_ACP, 0, remote_w, -1, RemoteName, MAX_PATH, NULL, NULL) == 0) {
        return FS_EXEC_ERROR; /* Cannot hand the new path back */
    }
    return rc;
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
    (void)Path;
    if (FindData) {
        memset(FindData, 0, sizeof(WIN32_FIND_DATAA));
    }
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return INVALID_HANDLE_VALUE;
}

HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
    if (!Path || !FindData) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    memset(FindData, 0, sizeof(WIN32_FIND_DATAW));

    WfxParsedPath parsed;
    if (!parse_wfx_path(Path, &parsed)) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    /* Level 0: Plugin Root "\" -> list configured repository aliases */
    if (parsed.level == WFX_PATH_ROOT) {
        return enumerate_root_repositories(FindData);
    }

    /* Level 1 & Level 2+: resolve repository configuration and invoke Restic */
    ResticConfig cfg;
    if (!load_config(&cfg)) {
        config_free(&cfg);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    const ResticRepoEntry* repo = is_forget_entry_name(parsed.repo_alias)
                                      ? NULL
                                      : config_find_repo(&cfg, parsed.repo_alias);
    if (!repo) {
        log_write(L"List %ls: no repository alias '%ls' in %ls", Path, parsed.repo_alias, cfg.ini_path);
        config_free(&cfg);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    log_write(L"List %ls", Path);
    HANDLE hdl = execute_repo_query(Path, cfg.restic_path, repo, &parsed, FindData);
    config_free(&cfg);
    return hdl;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
    (void)Hdl;
    (void)FindData;
    return FALSE;
}

BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
    if (Hdl == INVALID_HANDLE_VALUE || Hdl == NULL || FindData == NULL) {
        return FALSE;
    }

    ResticFindHandle* handle = (ResticFindHandle*)Hdl;
    if (handle->current_index >= handle->count) {
        return FALSE;
    }

    *FindData = handle->items[handle->current_index++];
    return TRUE;
}

int __stdcall FsFindClose(HANDLE Hdl) {
    if (Hdl != INVALID_HANDLE_VALUE && Hdl != NULL) {
        ResticFindHandle* handle = (ResticFindHandle*)Hdl;
        free(handle->items);
        free(handle);
    }
    return 0;
}

typedef struct {
    WCHAR*    remote_name;
    WCHAR*    local_name;
    ULONGLONG total_size;
} WfxDumpProgressContext;

/* Reports byte-level progress to Total Commander during FsGetFileW; returns TRUE on user abort */
static BOOL wfx_dump_progress_callback(ULONGLONG bytes_written, void* user_data) {
    if (!g_ProgressProc) {
        return FALSE;
    }
    const WfxDumpProgressContext* ctx = (const WfxDumpProgressContext*)user_data;
    int pct = 0;
    if (ctx && ctx->total_size > 0) {
        ULONGLONG raw = (bytes_written * 100ULL) / ctx->total_size;
        pct = (raw >= 99ULL) ? 99 : (int)raw;
    }
    WCHAR* src = (ctx && ctx->remote_name) ? ctx->remote_name : L"";
    WCHAR* dst = (ctx && ctx->local_name) ? ctx->local_name : L"";
    return g_ProgressProc(g_PluginNr, src, dst, pct) != 0;
}

int __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    (void)RemoteName;
    (void)LocalName;
    (void)CopyFlags;
    (void)ri;
    return FS_FILE_NOTSUPPORTED;
}

int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    if (!RemoteName || !LocalName || RemoteName[0] == L'\0' || LocalName[0] == L'\0') {
        return FS_FILE_NOTFOUND;
    }

    /* Restic snapshots are read-only: move (delete after copy) and partial resume are unsupported */
    if ((CopyFlags & (FS_COPYFLAGS_MOVE | FS_COPYFLAGS_RESUME)) != 0) {
        return FS_FILE_NOTSUPPORTED;
    }
    if (ri != NULL && (ri->Attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FS_FILE_NOTSUPPORTED;
    }

    WfxParsedPath parsed;
    if (!parse_wfx_path(RemoteName, &parsed) || parsed.level != WFX_PATH_SNAPSHOT_DIR ||
        !parsed.subpath) {
        return FS_FILE_NOTFOUND;
    }
    const WCHAR* sp = parsed.subpath;
    while (*sp == L'\\') sp++;
    if (*sp == L'\0') {
        return FS_FILE_NOTFOUND;
    }

    /* Total Commander calls FsGetFileW first without FS_COPYFLAGS_OVERWRITE to probe for collisions */
    DWORD local_attr = GetFileAttributesW(LocalName);
    if (local_attr != INVALID_FILE_ATTRIBUTES) {
        if ((local_attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return FS_FILE_WRITEERROR;
        }
        if ((CopyFlags & FS_COPYFLAGS_OVERWRITE) == 0) {
            return FS_FILE_EXISTS;
        }
    }

    ResticConfig cfg;
    if (!load_config(&cfg)) {
        config_free(&cfg);
        return FS_FILE_NOTFOUND; /* No configuration file: the repository alias cannot exist */
    }
    const ResticRepoEntry* repo = config_find_repo(&cfg, parsed.repo_alias);
    if (!repo) {
        config_free(&cfg);
        return FS_FILE_NOTFOUND;
    }
    if (wcschr(cfg.restic_path, L'\\') == NULL && wcschr(cfg.restic_path, L'/') == NULL) {
        report_plugin_error(
            repo->alias,
            L"restic.exe not found. Set ResticPath in TCResticBrowser.ini or place restic.exe in the plugin directory / PATH."
        );
        config_free(&cfg);
        return FS_FILE_READERROR;
    }

    if (g_ProgressProc && g_ProgressProc(g_PluginNr, RemoteName, LocalName, 0) != 0) {
        config_free(&cfg);
        return FS_FILE_USERABORT;
    }
    log_write(L"Copy %ls -> %ls%ls", RemoteName, LocalName, (CopyFlags & FS_COPYFLAGS_OVERWRITE) ? L" (overwrite)" : L"");

    ULONGLONG total_size            = ri ? (((ULONGLONG)ri->SizeHigh << 32) | ri->SizeLow) : 0;
    const FILETIME* mtime           = ri ? &ri->LastWriteTime : NULL;
    BOOL overwrite                  = (CopyFlags & FS_COPYFLAGS_OVERWRITE) != 0;
    WfxDumpProgressContext prog_ctx = {RemoteName, LocalName, total_size};

    WCHAR password[SESSION_MAX_PASSWORD] = {0};
    WCHAR wrong_hint[PASSWORD_HINT_CHARS] = {0}; /* What looked odd in a rejected password */
    BOOL have_password                   = session_get(repo->alias, password, _countof(password));
    BOOL from_cache                      = have_password; /* Cached: not stored again (see session_touch) */
    WCHAR err_msg[512]                   = {0};
    ResticResult res                     = RESTIC_ERR_WRONG_PASSWORD;

    for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS; ++attempt) {
        if (!have_password) {
            if (!prompt_repository_password(repo->alias, attempt > 0, wrong_hint, password, _countof(password))) {
                SecureZeroMemory(password, sizeof(password));
                config_free(&cfg);
                return FS_FILE_USERABORT;
            }
            have_password = TRUE;
            from_cache    = FALSE;
        }

        err_msg[0] = L'\0';
        res = restic_dump_file(
            cfg.restic_path,
            repo->target,
            password,
            parsed.snapshot_id,
            parsed.subpath,
            LocalName,
            overwrite,
            mtime,
            WFX_RESTIC_TIMEOUT_MS,
            wfx_dump_progress_callback,
            &prog_ctx,
            err_msg,
            _countof(err_msg)
        );

        if (res == RESTIC_ERR_WRONG_PASSWORD) {
            session_forget(repo->alias);
            restic_describe_password(password, wrong_hint, _countof(wrong_hint));
            SecureZeroMemory(password, sizeof(password));
            have_password = FALSE;
            continue;
        }
        break;
    }

    if (have_password && password_worth_caching(res)) {
        if (from_cache) {
            session_touch(repo->alias);
        } else {
            session_put(repo->alias, password);
        }
    }
    SecureZeroMemory(password, sizeof(password));

    int ret = FS_FILE_READERROR;
    switch (res) {
        case RESTIC_OK:
            if (g_ProgressProc) {
                g_ProgressProc(g_PluginNr, RemoteName, LocalName, 100);
            }
            ret = FS_FILE_OK;
            break;
        case RESTIC_ERR_CANCELLED:
            ret = FS_FILE_USERABORT;
            break;
        case RESTIC_ERR_PATH_NOT_FOUND:
            ret = FS_FILE_NOTFOUND;
            break;
        case RESTIC_ERR_LOCAL_WRITE:
            ret = FS_FILE_WRITEERROR;
            break;
        default:
            handle_restic_failure(res, repo->alias, err_msg);
            ret = FS_FILE_READERROR;
            break;
    }

    config_free(&cfg);
    return ret;
}
