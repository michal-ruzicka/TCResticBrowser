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
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "fsplugin.h"
#include "config.h"
#include "restic_cli.h"
#include "repo_cache.h"
#include "snapshot_tree.h"
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

/* Total Commander's connection log (see connection_announce) */
static volatile LONG g_ConnectionLogOn = FALSE; /* [Settings] ConnectionLog, applied by load_config */
static volatile LONG g_Connected       = FALSE; /* The connection was announced and not disconnected */
static void conn_log(BOOL deferred, const WCHAR* alias, const WCHAR* format, ...);

/*
 * In-memory credential sessions, one slot per repository alias (two panels can browse two
 * repositories alternately without being prompted again). Guarded by an SRW lock because Total
 * Commander may call into the plugin from background threads. Slots are wiped on wrong password,
 * FsInitW, DLL unload, PasswordTimeoutSeconds expiry and the forget entry; the least recently
 * cached one makes room when all slots are taken. SRWLOCK_INIT keeps the lock usable even if
 * DllMain never ran (unit tests).
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
    ULONGLONG generation;                 /* Unique per session_put: tells a replaced password apart */
} SessionSlot;

_Static_assert(sizeof(((SessionSlot*)0)->password) % CRYPTPROTECTMEMORY_BLOCK_SIZE == 0,
               "CryptProtectMemory works on whole blocks");

typedef HRESULT (WINAPI *PFN_WerRegisterExcludedMemoryBlock)(const void*, DWORD);
typedef HRESULT (WINAPI *PFN_WerUnregisterExcludedMemoryBlock)(const void*);

static SessionSlot* g_Sessions         = NULL; /* SESSION_SLOTS slots in locked pages, or NULL */
static SIZE_T       g_SessionBytes     = 0;
static BOOL         g_SessionLocked    = FALSE; /* VirtualLock succeeded */
static BOOL         g_SessionWerExcluded = FALSE;
static ULONGLONG    g_SessionGeneration = 0;
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

/*
 * Wipes one slot and drops everything read from its repository (repo_cache.h): what was read with
 * a password is never shown once the password is gone. Caller holds g_SessionLock.
 */
static void session_wipe_slot_locked(SessionSlot* slot) {
    if (slot->used) repo_cache_forget(slot->alias);
    SecureZeroMemory(slot, sizeof(*slot));
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
    WFX_PATH_ROOT = 0, /* "\" -> list configured repository aliases */
    WFX_PATH_REPO,     /* "\<RepoAlias>[\...]" -> the restic mount tree of that repository */
    WFX_PATH_INVALID   /* Malformed path */
} WfxPathLevel;

/**
 * Decomposed components of a WFX virtual path.
 */
typedef struct {
    WfxPathLevel level;
    WCHAR repo_alias[MAX_PATH];
    const WCHAR* rest; /* Below the alias: L"" or "\..." (points into the caller's Path buffer) */
} WfxParsedPath;

/*
 * The file beside snapshots.csv holding the repository's ID (`restic cat config`) and a Windows line
 * end: always 64 + 2 bytes, so it is listed without asking restic
 */
#define REPOSITORY_ID_FILE_NAME  "repository_id.txt"
#define REPOSITORY_ID_FILE_BYTES (RESTIC_REPOSITORY_ID_CHARS + 2)

/* Where a path below a repository alias leads (resolve_repo_path) */
typedef enum {
    REPO_PATH_FOLDER = 0, /* A folder of the restic mount tree above the snapshots */
    REPO_PATH_SNAPSHOT,   /* Inside a snapshot: snapshot_id + subpath */
    REPO_PATH_CSV,        /* SNAPSHOT_TREE_CSV_NAME in the repository's folder: csv, csv_len */
    REPO_PATH_REPOSITORY_ID, /* REPOSITORY_ID_FILE_NAME in the repository's folder */
    REPO_PATH_NOT_FOUND
} RepoPathKind;

typedef struct {
    RepoPathKind kind;
    WIN32_FIND_DATAW* items; /* REPO_PATH_FOLDER when a listing was asked for: heap array */
    size_t count;
    char snapshot_id[SNAPSHOT_TREE_ID_CHARS + 1];
    const WCHAR* subpath;    /* REPO_PATH_SNAPSHOT: L"" or "\..." inside the snapshot */
    char* csv;               /* REPO_PATH_CSV: the file's content (heap) */
    size_t csv_len;
} RepoPath;

/*
 * One operation on a repository, run by run_with_password() with a password that is either the
 * cached one (trusted: restic accepted it before, so cached data read with it may be shown) or one
 * just typed in (data must come from restic, which checks it).
 */
typedef struct {
    const ResticConfig* cfg;
    const ResticRepoEntry* repo;
    const WCHAR* password;
    BOOL trusted;
    const ResticExecOptions* exec;
    ULONGLONG generation;    /* repo_cache_generation() before the operation */
    WCHAR* err;
    size_t err_cap;
} RepoCall;

typedef ResticResult (*RepoOperation)(const RepoCall* call, void* ctx);

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
            conn_log(TRUE, slot->alias, L"Password forgotten after PasswordTimeoutSeconds (%llu s), and what was read "
                                        L"with it", g_PasswordTimeoutMs / 1000);
            session_wipe_slot_locked(slot);
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
 * wipes it) and returns its generation for session_touch/session_forget. Returns FALSE if none is
 * cached.
 */
static BOOL session_get(const WCHAR* alias, WCHAR* out_pwd, size_t cap, ULONGLONG* out_generation) {
    BOOL found      = FALSE;
    *out_generation = 0;
    if (cap < SESSION_MAX_PASSWORD) return FALSE;
    AcquireSRWLockExclusive(&g_SessionLock);
    session_expire_locked();
    SessionSlot* slot = session_find_locked(alias);
    if (slot) {
        memcpy(out_pwd, slot->password, sizeof(slot->password));
        found = CryptUnprotectMemory(out_pwd, (DWORD)sizeof(slot->password), CRYPTPROTECTMEMORY_SAME_PROCESS);
        if (!found) {
            SecureZeroMemory(out_pwd, sizeof(slot->password));
            session_wipe_slot_locked(slot);
        } else {
            *out_generation = slot->generation;
        }
        out_pwd[SESSION_MAX_PASSWORD - 1] = L'\0';
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
    return found;
}

/*
 * Stores (or replaces) the password of `alias`; the least recently cached slot makes room if
 * needed. FALSE when it could not be kept.
 */
static BOOL session_put(const WCHAR* alias, const WCHAR* password) {
    AcquireSRWLockExclusive(&g_SessionLock);
    if (!session_table_locked()) {
        ReleaseSRWLockExclusive(&g_SessionLock);
        return FALSE;
    }
    SessionSlot* slot = session_find_locked(alias);
    for (int i = 0; !slot && i < SESSION_SLOTS; ++i) {
        if (!g_Sessions[i].used) slot = &g_Sessions[i];
    }
    if (!slot) {
        slot = &g_Sessions[0];
        for (int i = 1; i < SESSION_SLOTS; ++i) {
            const SessionSlot* s = &g_Sessions[i];
            if (s->cached_at < slot->cached_at ||
                (s->cached_at == slot->cached_at && s->generation < slot->generation)) { /* Same tick */
                slot = &g_Sessions[i];
            }
        }
    }
    if (slot->used && _wcsicmp(slot->alias, alias) != 0) {
        conn_log(TRUE, slot->alias, L"Password forgotten, and what was read with it, to make room for another "
                                    L"repository's (%d are remembered at most)", SESSION_SLOTS);
        session_wipe_slot_locked(slot); /* Recycled: the other repository's data goes with it */
    }
    SecureZeroMemory(slot, sizeof(*slot));
    slot->used       = TRUE;
    slot->generation = ++g_SessionGeneration;
    wcsncpy_s(slot->alias, MAX_PATH, alias, _TRUNCATE);
    wcsncpy_s(slot->password, SESSION_MAX_PASSWORD, password, _TRUNCATE);
    slot->cached_at = GetTickCount64();
    BOOL kept = TRUE;
    if (!CryptProtectMemory(slot->password, (DWORD)sizeof(slot->password), CRYPTPROTECTMEMORY_SAME_PROCESS)) {
        DWORD err = GetLastError();
        SecureZeroMemory(slot, sizeof(*slot)); /* Never keep a plaintext password */
        repo_cache_forget(alias);              /* Nor what was read with it */
        log_write(L"Password for '%ls' not cached: CryptProtectMemory failed (Windows error %lu)", alias, err);
        kept = FALSE;
    }
    session_expire_locked(); /* Arms the timer for this password's expiry */
    ReleaseSRWLockExclusive(&g_SessionLock);
    return kept;
}

/* TRUE while a password is cached for `alias` */
static BOOL session_exists(const WCHAR* alias) {
    AcquireSRWLockExclusive(&g_SessionLock);
    session_expire_locked();
    BOOL found = session_find_locked(alias) != NULL;
    ReleaseSRWLockExclusive(&g_SessionLock);
    return found;
}

/*
 * PasswordTimeoutResetOnUse: restic completed an operation with the cached password of `alias`
 * (generation from session_get; a password replaced meanwhile is left alone)
 */
static void session_touch(const WCHAR* alias, ULONGLONG generation) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SessionSlot* slot = g_PasswordResetOnUse ? session_find_locked(alias) : NULL;
    if (slot && slot->generation == generation) {
        slot->cached_at = GetTickCount64();
        session_expire_locked(); /* Re-arms the timer for the later expiry */
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/*
 * Wipes the cached password of a single repository that restic rejected, unless another thread
 * has replaced it with a verified one since session_get returned `generation`
 */
static void session_forget(const WCHAR* alias, ULONGLONG generation) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SessionSlot* slot = session_find_locked(alias);
    if (slot && slot->generation == generation) session_wipe_slot_locked(slot);
    ReleaseSRWLockExclusive(&g_SessionLock);
}


/*
 * Total Commander's connection log ([Settings] ConnectionLog, on unless "no"). When a repository is first used,
 * before its password is asked for (as an FTP plugin announces a connection when it starts
 * connecting, so that the log shows how it went), the plugin announces one connection for all its
 * repositories (LogProc MSGTYPE_CONNECT, "CONNECT \\", the WFX SDK's root "for a single possible
 * connection"): Total Commander then shows its FTP connections toolbar with a log window, where
 * passwords accepted, refused and forgotten, snapshot lists and folders read by restic, files
 * copied (MSGTYPE_TRANSFERCOMPLETE) and the background work are reported (MSGTYPE_DETAILS), and a
 * Disconnect button, which calls FsDisconnectW.
 *
 * Only Total Commander can end a connection, one per click of Disconnect, each time switching the
 * panel back to where it was before; logging MSGTYPE_DISCONNECT alone changes nothing. A single
 * connection therefore needs one click, whatever the number of repositories in use, and it is
 * announced only once until FsDisconnectW or FsInitW: a password that times out, and the forget
 * entry, leave it listed, and the next use of a repository reuses it.
 *
 * LogProc is never called with a lock of the plugin held, nor from the password timer, whose
 * callbacks DLL unload waits for.
 */

/* Announces the connection unless ConnectionLog is off or it already is announced */
static void connection_announce(void) {
    if (!InterlockedCompareExchange(&g_ConnectionLogOn, 0, 0) || !g_LogProc) return;
    if (InterlockedExchange(&g_Connected, TRUE)) return;
    WCHAR text[] = L"CONNECT \\";
    g_LogProc(g_PluginNr, MSGTYPE_CONNECT, text);
}

/* RepoStatusProc: background work reported in the connection log, while there is one */
static void report_status(const WCHAR* text) {
    tLogProcW log_proc = g_LogProc;
    if (log_proc && InterlockedCompareExchange(&g_Connected, 0, 0)) log_proc(g_PluginNr, MSGTYPE_DETAILS, (WCHAR*)text);
}

/*
 * "[<alias>] <message>" in the connection log, while there is one. `deferred` for callers holding
 * a lock or running in the password timer: a thread-pool worker logs it for them.
 */
static void conn_log(BOOL deferred, const WCHAR* alias, const WCHAR* format, ...) {
    if (!InterlockedCompareExchange(&g_Connected, 0, 0) || !g_LogProc) return;
    /* Deferred lines come with the session lock held; during DLL unload no worker is started for them */
    if (deferred && g_SessionShuttingDown) return;
    WCHAR text[1024];
    int n = _snwprintf_s(text, _countof(text), _TRUNCATE, L"[%ls] ", alias);
    if (n < 0) return;
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text + n, _countof(text) - (size_t)n, _TRUNCATE, format, args);
    va_end(args);
    if (deferred) {
        repo_cache_report_async(text);
    } else {
        report_status(text);
    }
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
    repo_cache_clear();
    session_arm_timer_locked(0);
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/*
 * Wipes and frees the password table (DLL unload). On FreeLibrary the expiry timer is stopped and
 * pending callbacks are cancelled or waited for, outside g_SessionLock (a running callback needs
 * it), before the code they would run is unmapped.
 *
 * At process exit Windows has already terminated every other thread, possibly one holding
 * g_SessionLock (an SRW lock has no owner to release it), so the lock is never waited for there:
 * the table holds ciphertext only and goes with the process; it is wiped if the lock is free.
 */
static void release_password_session(BOOL process_exit) {
    if (process_exit) {
        if (TryAcquireSRWLockExclusive(&g_SessionLock)) {
            if (g_Sessions) SecureZeroMemory(g_Sessions, g_SessionBytes);
            ReleaseSRWLockExclusive(&g_SessionLock);
        }
        return;
    }

    AcquireSRWLockExclusive(&g_SessionLock);
    g_SessionShuttingDown = TRUE;
    PTP_TIMER timer       = g_SessionTimer;
    g_SessionTimer        = NULL;
    if (timer) SetThreadpoolTimer(timer, NULL, 0, 0);
    ReleaseSRWLockExclusive(&g_SessionLock);

    if (timer) {
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }

    AcquireSRWLockExclusive(&g_SessionLock);
    session_table_release_locked();
    repo_cache_clear();
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* config_load() plus the settings the plugin applies globally (PasswordTimeoutSeconds, SnapshotCacheMB) */
static BOOL load_config(ResticConfig* cfg) {
    BOOL ok = config_load(cfg);
    apply_password_timeout(cfg->password_timeout_s, cfg->password_timeout_reset_on_use);
    ULONGLONG bytes = (ULONGLONG)cfg->snapshot_cache_mb * 1024ull * 1024ull;
    /* 32-bit Total Commander has 2 GiB of address space for everything: at most 512 MiB here */
    ULONGLONG cap = sizeof(void*) == 4 ? 512ull * 1024ull * 1024ull : (ULONGLONG)(SIZE_MAX / 2);
    repo_cache_set_index_limit((size_t)(bytes > cap ? cap : bytes));
    InterlockedExchange(&g_ConnectionLogOn, cfg->connection_log);
    return ok;
}

/* RepoPasswordSource for the background listings: the cached password, never a prompt */
static BOOL cached_password(const WCHAR* alias, WCHAR* out, size_t cap) {
    ULONGLONG generation = 0;
    return session_get(alias, out, cap, &generation);
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

    /*
     * The alias in the window title lets password managers that pick an entry by the title of the
     * active window (KeePassXC Auto-Type, KeePass) offer the right password
     */
    WCHAR title[MAX_PATH + 64];
    _snwprintf_s(title, _countof(title), _TRUNCATE, L"%ls - Restic repository password", repo_alias);

    out_pwd[0] = L'\0'; /* RequestProc shows ReturnedText as the default value */
    BOOL accepted = g_RequestProc(
        g_PluginNr,
        RT_Password,
        title,
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
    out->level = WFX_PATH_INVALID;
    out->rest  = L"";

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
    out->rest = p - 1; /* The separator before the next segment */

    /*
     * The folders below an alias are fixed (restic mount's four, see snapshot_tree.h), and so are the
     * two files beside them: anything else fails here, before a password is asked for
     */
    seg_start = p;
    while (*p != L'\0' && *p != L'\\') {
        p++;
    }
    seg_len = (size_t)(p - seg_start);
    WCHAR first[MAX_PATH];
    if (seg_len >= MAX_PATH) return FALSE;
    wcsncpy_s(first, MAX_PATH, seg_start, seg_len);
    if (_wcsicmp(first, L"" SNAPSHOT_TREE_CSV_NAME) == 0 || _wcsicmp(first, L"" REPOSITORY_ID_FILE_NAME) == 0) {
        while (*p == L'\\') p++;
        if (*p != L'\0') return FALSE; /* A file, not a folder */
    } else if (_wcsicmp(first, L"hosts") != 0 && _wcsicmp(first, L"ids") != 0 &&
               _wcsicmp(first, L"snapshots") != 0 && _wcsicmp(first, L"tags") != 0) {
        return FALSE;
    }
    out->level = WFX_PATH_REPO;
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

/*
 * TRUE when config_resolve_restic_path found restic.exe; otherwise shows the reason (a configured
 * ResticPath that is missing, or no restic.exe at all) before any password prompt
 */
static BOOL restic_path_usable(const ResticConfig* cfg, const WCHAR* repo_alias) {
    if (config_is_absolute_path(cfg->restic_path)) return TRUE;
    report_plugin_error(repo_alias, cfg->restic_path_error[0] != L'\0' ? cfg->restic_path_error
                                                                       : L"restic.exe not found.");
    return FALSE;
}

/*
 * Runs `op` with the repository's password: the cached one, or one asked for (up to
 * MAX_PASSWORD_ATTEMPTS prompts while restic answers "wrong password"). A typed password is cached
 * once restic accepted it (password_worth_caching), a cached one wiped when restic refuses it.
 * Returns RESTIC_ERR_CANCELLED when the user cancels the prompt or enters nothing.
 */
static ResticResult run_with_password(
    const ResticConfig* cfg,
    const ResticRepoEntry* repo,
    const ResticExecOptions* exec,
    RepoOperation op,
    void* ctx,
    WCHAR* err_msg,
    size_t err_cap
) {
    WCHAR password[SESSION_MAX_PASSWORD] = {0}; /* Local copy, wiped before returning */
    WCHAR wrong_hint[PASSWORD_HINT_CHARS] = {0}; /* What looked odd in a rejected password */
    ULONGLONG cached_generation          = 0;
    /* Before the password is taken: if it is wiped meanwhile, nothing read with it is cached */
    ULONGLONG cache_generation = repo_cache_generation();
    /* As an FTP plugin announces a connection when it starts connecting: the log then shows how it went */
    connection_announce();
    BOOL have_password = session_get(repo->alias, password, _countof(password), &cached_generation);
    BOOL from_cache    = have_password; /* Cached: not stored again (see session_touch) */
    ResticResult res   = RESTIC_ERR_WRONG_PASSWORD;

    for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS; ++attempt) {
        if (!have_password) {
            if (!prompt_repository_password(repo->alias, attempt > 0, wrong_hint, password, _countof(password))) {
                SecureZeroMemory(password, sizeof(password));
                return RESTIC_ERR_CANCELLED;
            }
            have_password = TRUE;
            from_cache    = FALSE;
        }

        err_msg[0] = L'\0';
        RepoCall call = {cfg, repo, password, from_cache, exec, cache_generation, err_msg, err_cap};
        res = op(&call, ctx);

        if (res == RESTIC_ERR_WRONG_PASSWORD) {
            if (from_cache) {
                session_forget(repo->alias, cached_generation);
                /* Forgetting it dropped the repository's cache: what the typed password reads is kept */
                cache_generation = repo_cache_generation();
                conn_log(FALSE, repo->alias, L"The remembered password was refused by restic and forgotten");
            } else {
                conn_log(FALSE, repo->alias, L"Password refused by restic");
            }
            restic_describe_password(password, wrong_hint, _countof(wrong_hint));
            SecureZeroMemory(password, sizeof(password));
            have_password = FALSE;
            continue;
        }
        break;
    }

    BOOL kept = from_cache;
    if (have_password && password_worth_caching(res)) {
        if (from_cache) {
            session_touch(repo->alias, cached_generation);
        } else {
            kept = session_put(repo->alias, password);
            if (kept) conn_log(FALSE, repo->alias, L"Password accepted");
        }
    }
    SecureZeroMemory(password, sizeof(password));
    /*
     * A typed-in password that is not kept (ESC, a timeout or a local write error before restic
     * proved it right, or no memory to keep it in) takes along what the operation cached with it,
     * unless another thread has cached a password for the repository meanwhile
     */
    if (have_password && !kept && !session_exists(repo->alias)) repo_cache_forget(repo->alias);
    return res;
}

/* Converts one component of a WFX path (len UTF-16 units) to UTF-8; FALSE if it does not fit */
static BOOL component_to_utf8(const WCHAR* comp, size_t len, char* out, size_t cap) {
    if (len == 0 || len >= MAX_PATH) return FALSE;
    int n = WideCharToMultiByte(CP_UTF8, 0, comp, (int)len, out, (int)cap - 1, NULL, NULL);
    if (n <= 0) return FALSE;
    out[n] = '\0';
    return TRUE;
}

/*
 * The field separator of snapshots.csv: [Settings] CsvSeparator, or else the list separator of the
 * user's regional settings - the one Excel expects in a CSV file it opens - or else a comma
 */
static char csv_separator(const ResticConfig* cfg) {
    if (cfg->csv_separator) return cfg->csv_separator;
    WCHAR list[8] = {0};
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLIST, list, (int)_countof(list)) > 0 && list[0] > L' ' &&
        list[0] < 0x7F && list[0] != L'"') {
        return (char)list[0];
    }
    return ',';
}

static void set_find_time(WIN32_FIND_DATAW* fd, uint64_t time) {
    if (time == 0) {
        FS_SET_TIME_UNKNOWN(fd->ftLastWriteTime);
        return;
    }
    fd->ftLastWriteTime.dwLowDateTime  = (DWORD)(time & 0xFFFFFFFFULL);
    fd->ftLastWriteTime.dwHighDateTime = (DWORD)(time >> 32);
    fd->ftCreationTime                 = fd->ftLastWriteTime;
    fd->ftLastAccessTime               = fd->ftLastWriteTime;
}

/*
 * The children of a tree folder as Total Commander directory entries (heap array); the repository's
 * own folder (`repo_folder`) also holds REPOSITORY_ID_FILE_NAME and, with `csv` set,
 * SNAPSHOT_TREE_CSV_NAME, a file of `csv_len` bytes
 */
static ResticResult tree_folder_items(const SnapshotNode* dir, BOOL repo_folder, BOOL csv, size_t csv_len,
                                      WIN32_FIND_DATAW** out_items, size_t* out_count) {
    *out_items = NULL;
    *out_count = 0;
    if (dir->child_count == 0 && !repo_folder) return RESTIC_OK;
    WIN32_FIND_DATAW* items = (WIN32_FIND_DATAW*)calloc(dir->child_count + 2, sizeof(WIN32_FIND_DATAW));
    if (!items) return RESTIC_ERR_OUT_OF_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < dir->child_count; ++i) {
        const SnapshotNode* c = dir->children[i];
        WIN32_FIND_DATAW* fd  = &items[n];
        if (MultiByteToWideChar(CP_UTF8, 0, c->name, -1, fd->cFileName, MAX_PATH) <= 0) continue;
        fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        set_find_time(fd, c->time);
        n++;
    }
    if (csv) {
        WIN32_FIND_DATAW* fd = &items[n++];
        MultiByteToWideChar(CP_UTF8, 0, SNAPSHOT_TREE_CSV_NAME, -1, fd->cFileName, MAX_PATH);
        fd->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
        fd->nFileSizeLow     = (DWORD)((ULONGLONG)csv_len & 0xFFFFFFFFULL);
        fd->nFileSizeHigh    = (DWORD)((ULONGLONG)csv_len >> 32);
        set_find_time(fd, dir->time);
    }
    if (repo_folder) { /* Its size is known before restic is asked for the ID; its time is not */
        WIN32_FIND_DATAW* fd = &items[n++];
        wcscpy_s(fd->cFileName, MAX_PATH, L"" REPOSITORY_ID_FILE_NAME);
        fd->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
        fd->nFileSizeLow     = REPOSITORY_ID_FILE_BYTES;
        FS_SET_TIME_UNKNOWN(fd->ftLastWriteTime);
    }
    *out_items = items;
    *out_count = n;
    return RESTIC_OK;
}

/*
 * Follows `rest` (L"" or "\...") through the tree. The first component that reaches a snapshot
 * ends the walk: the remainder is the path inside it. A folder is listed into out->items when
 * `want_items` is set; SNAPSHOT_TREE_CSV_NAME in the repository's folder is generated into
 * out->csv, with `csv_sep` between the fields, and REPOSITORY_ID_FILE_NAME there is recognised.
 */
static ResticResult walk_tree(const SnapshotTree* tree, const WCHAR* rest, BOOL want_items, char csv_sep,
                              RepoPath* out) {
    const SnapshotNode* root = snapshot_tree_root(tree);
    const SnapshotNode* node = root;
    const WCHAR* p           = rest;
    for (;;) {
        while (*p == L'\\') p++;
        if (*p == L'\0') break;
        const WCHAR* start = p;
        while (*p != L'\0' && *p != L'\\') p++;
        char name[MAX_PATH * 3 + 1];
        BOOL named = component_to_utf8(start, (size_t)(p - start), name, sizeof(name));
        if (named && node == root && _stricmp(name, SNAPSHOT_TREE_CSV_NAME) == 0) {
            while (*p == L'\\') p++;
            if (*p != L'\0') {
                out->kind = REPO_PATH_NOT_FOUND; /* A file, not a folder */
                return RESTIC_OK;
            }
            out->kind = REPO_PATH_CSV;
            out->csv  = snapshot_tree_csv(tree, csv_sep, &out->csv_len);
            return out->csv ? RESTIC_OK : RESTIC_ERR_OUT_OF_MEMORY;
        }
        if (named && node == root && _stricmp(name, REPOSITORY_ID_FILE_NAME) == 0) {
            while (*p == L'\\') p++;
            out->kind = *p == L'\0' ? REPO_PATH_REPOSITORY_ID : REPO_PATH_NOT_FOUND; /* A file, not a folder */
            return RESTIC_OK;
        }
        node = named ? snapshot_tree_child(node, name) : NULL;
        if (!node) {
            out->kind = REPO_PATH_NOT_FOUND;
            return RESTIC_OK;
        }
        if (node->snapshot_id) {
            out->kind = REPO_PATH_SNAPSHOT;
            strcpy_s(out->snapshot_id, sizeof(out->snapshot_id), node->snapshot_id);
            out->subpath = p;
            return RESTIC_OK;
        }
    }
    out->kind = REPO_PATH_FOLDER;
    if (!want_items) return RESTIC_OK;
    size_t csv_len = 0;
    char* csv      = node == root ? snapshot_tree_csv(tree, csv_sep, &csv_len) : NULL;
    BOOL with_csv  = csv != NULL; /* Only its size is listed, for the panel and the copy progress */
    free(csv);                    /* (No memory to make it: not listed) */
    return tree_folder_items(node, node == root, with_csv, csv_len, &out->items, &out->count);
}

/*
 * Resolves a path below a repository alias. With a trusted password the cached tree answers
 * whatever it knows, however old: a folder above the snapshots is listed from it at once, and if
 * the tree is older than SnapshotListRefreshSeconds a fresh list is read in the background for the
 * next listing; a path into a snapshot keeps meaning the snapshot it was listed for. Only a name
 * the cached tree does not know, a missing tree, or a password just typed in (restic must check
 * it) list the snapshots while the user waits.
 */
static ResticResult resolve_repo_path(const RepoCall* call, const WCHAR* rest, BOOL want_items, RepoPath* out) {
    memset(out, 0, sizeof(*out));
    out->subpath = L"";

    if (call->trusted) {
        ULONGLONG age = 0;
        const SnapshotTree* cached =
            repo_cache_lock_tree(call->repo->alias, call->repo->target, call->cfg->snapshot_time_format, &age);
        if (cached) {
            ResticResult res = walk_tree(cached, rest, want_items, csv_separator(call->cfg), out);
            repo_cache_unlock();
            if (res != RESTIC_OK || out->kind == REPO_PATH_SNAPSHOT) return res;
            if (out->kind == REPO_PATH_FOLDER || out->kind == REPO_PATH_CSV ||
                out->kind == REPO_PATH_REPOSITORY_ID) {
                ULONGLONG refresh_ms = (ULONGLONG)call->cfg->snapshot_list_refresh_s * 1000ull;
                if (age >= refresh_ms) {
                    repo_cache_refresh_tree(call->repo->alias, call->repo->target, call->cfg->restic_path,
                                            call->cfg->snapshot_time_format, refresh_ms, cached_password);
                }
                return res;
            }
            memset(out, 0, sizeof(*out));
            out->subpath = L"";
        }
    }

    SnapshotTree* tree = NULL;
    ULONGLONG started = GetTickCount64();
    ResticResult res = restic_list_snapshots(call->cfg->restic_path, call->repo->target, call->password,
                                             call->cfg->snapshot_time_format, call->exec, &tree, call->err, call->err_cap);
    if (res != RESTIC_OK) return res;
    log_write(L"Snapshots of '%ls': %u", call->repo->alias, (unsigned)snapshot_tree_snapshot_count(tree));
    conn_log(FALSE, call->repo->alias, L"Snapshot list read: %u snapshots, %.1f s",
             (unsigned)snapshot_tree_snapshot_count(tree), (double)(GetTickCount64() - started) / 1000.0);
    res = walk_tree(tree, rest, want_items, csv_separator(call->cfg), out);
    repo_cache_put_tree(call->repo->alias, call->repo->target, call->cfg->snapshot_time_format, tree, call->generation);
    return res;
}

/* FsFindFirstW below a repository alias */
typedef struct {
    const WCHAR* rest;
    WIN32_FIND_DATAW* items;
    size_t count;
    char snapshot_id[SNAPSHOT_TREE_ID_CHARS + 1]; /* Set when the folder is inside a snapshot */
} ListContext;

static ResticResult list_operation(const RepoCall* call, void* ctx_ptr) {
    ListContext* ctx = (ListContext*)ctx_ptr;
    RepoPath where;
    ResticResult res = resolve_repo_path(call, ctx->rest, TRUE, &where);
    if (res != RESTIC_OK) return res;
    switch (where.kind) {
        case REPO_PATH_FOLDER:
            ctx->items = where.items;
            ctx->count = where.count;
            return RESTIC_OK;
        case REPO_PATH_SNAPSHOT: {
            strcpy_s(ctx->snapshot_id, sizeof(ctx->snapshot_id), where.snapshot_id);
            /* The whole snapshot read before: no restic at all */
            const SnapshotIndex* index =
                call->trusted ? repo_cache_lock_index(call->repo->alias, call->repo->target, where.snapshot_id) : NULL;
            if (index) {
                res = restic_list_index_directory(index, where.subpath, &ctx->items, &ctx->count);
                repo_cache_unlock();
                return res;
            }
            /* Until then one `restic ls` per folder, as before */
            ULONGLONG started = GetTickCount64();
            res = restic_list_directory(call->cfg->restic_path, call->repo->target, call->password,
                                        where.snapshot_id, where.subpath, call->exec, &ctx->items, &ctx->count,
                                        call->err, call->err_cap);
            if (res == RESTIC_OK) {
                conn_log(FALSE, call->repo->alias, L"%ls: %u entries listed by restic in %.1f s", ctx->rest,
                         (unsigned)ctx->count, (double)(GetTickCount64() - started) / 1000.0);
            }
            return res;
        }
        case REPO_PATH_CSV:
            free(where.csv);
            return RESTIC_ERR_PATH_NOT_FOUND; /* A file, not a folder */
        case REPO_PATH_REPOSITORY_ID:
            return RESTIC_ERR_PATH_NOT_FOUND;
        default:
            return RESTIC_ERR_PATH_NOT_FOUND;
    }
}

/* Lists a folder below a repository alias, with password prompt/retry */
static HANDLE execute_repo_query(
    const WCHAR* virtual_path,
    const ResticConfig* cfg,
    const ResticRepoEntry* repo,
    const WfxParsedPath* parsed,
    WIN32_FIND_DATAW* FindData
) {
    /* Verify that restic.exe was resolved to an explicit path before prompting for a password */
    if (!restic_path_usable(cfg, repo->alias)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    WfxCancelContext cancel_ctx = {virtual_path, repo->alias};
    ResticExecOptions exec_opts = {WFX_RESTIC_TIMEOUT_MS, wfx_cancel_callback, &cancel_ctx};
    ListContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rest           = parsed->rest;
    WCHAR err_msg[512] = {0};

    ResticResult res = run_with_password(cfg, repo, &exec_opts, list_operation, &ctx, err_msg, _countof(err_msg));
    if (ctx.snapshot_id[0] != '\0' && (res == RESTIC_OK || res == RESTIC_ERR_PATH_NOT_FOUND)) {
        /* Inside a snapshot, wherever: read all of it in the background (the password is cached by now) */
        repo_cache_load_index(repo->alias, repo->target, cfg->restic_path, ctx.snapshot_id, cached_password);
    }
    if (res != RESTIC_OK) {
        free(ctx.items);
        handle_restic_failure(res, repo->alias, err_msg);
        return INVALID_HANDLE_VALUE;
    }
    return wrap_find_items(ctx.items, ctx.count, FindData);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        /* No DisableThreadLibraryCalls: Microsoft advises against it with the static CRT (/MT) */
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
    InterlockedExchange(&g_Connected, FALSE); /* A new session of Total Commander knows of no connection */
    g_PluginNr     = PluginNr;
    g_ProgressProc = pProgressProcW;
    g_LogProc      = pLogProcW;
    g_RequestProc  = pRequestProcW;
    repo_cache_set_status_proc(report_status);

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
    if (cfg->restic_path_error[0] != L'\0') {
        log_write(L"  restic.exe: %ls", cfg->restic_path_error);
    } else {
        log_write(L"  ResticPath resolved to: %ls", cfg->restic_path);
    }
    log_write(L"  RootName: %ls; LogPath: %ls", cfg->root_name, log_path);
    WCHAR time_format[SNAPSHOT_TREE_TIME_FORMAT_MAX + 1] = {0};
    MultiByteToWideChar(CP_UTF8, 0, cfg->snapshot_time_format, -1, time_format, (int)_countof(time_format));
    BOOL capped = sizeof(void*) == 4 && cfg->snapshot_cache_mb > 512; /* See load_config */
    log_write(L"  SnapshotTimeFormat: %ls%ls; SnapshotCacheMB: %lu%ls", time_format,
              cfg->snapshot_time_format_invalid ? L" (the configured one is unusable, default used)" : L"",
              (unsigned long)cfg->snapshot_cache_mb, capped ? L" (512 used: 32-bit Total Commander)" : L"");
    char sep          = csv_separator(cfg);
    WCHAR sep_text[8] = {(WCHAR)sep, L'\0'};
    log_write(L"  CsvSeparator: %ls%ls", sep == '\t' ? L"tab" : sep_text,
              cfg->csv_separator ? L"" : L" (the Windows list separator)");
    log_write(L"  PasswordTimeoutSeconds: %lu; PasswordTimeoutResetOnUse: %ls; SnapshotListRefreshSeconds: %lu; "
              L"ConnectionLog: %ls", (unsigned long)cfg->password_timeout_s,
              cfg->password_timeout_reset_on_use ? L"yes" : L"no", (unsigned long)cfg->snapshot_list_refresh_s,
              cfg->connection_log ? L"yes" : L"no");
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
    unsigned cached = cached_password_count(); /* Listed even if the INI has lost its repositories */
    if ((!loaded || cfg.repo_count == 0) && cached == 0) {
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
        report_status(L"All remembered passwords and cached listings forgotten; Disconnect closes this toolbar");
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
    /*
     * The transfer mode of the connections toolbar (undocumented verb "MODE I", "MODE A", or
     * "MODE X <text-file extensions>" for automatic): Total Commander only tells the plugin, and
     * restic dump always delivers the bytes unchanged, so every mode is accepted and ignored
     */
    if (_wcsnicmp(Verb, L"mode", 4) == 0 && (Verb[4] == L'\0' || Verb[4] == L' ')) {
        log_write(L"Transfer mode '%ls' ignored: files are always copied byte for byte", Verb);
        return FS_EXEC_OK;
    }
    return FS_EXEC_ERROR;
}

/*
 * The Disconnect button of Total Commander's connections toolbar (the connection announced by
 * connection_announce): forgets every password and everything read with them, as the forget entry
 * does. Total Commander then switches the panel back to where it was before the connection.
 */
BOOL __stdcall FsDisconnectW(WCHAR* DisconnectRoot) {
    if (!InterlockedExchange(&g_Connected, FALSE)) return TRUE; /* Nothing announced: nothing open */
    unsigned n = cached_password_count();
    clear_password_session();
    log_write(L"Disconnect %ls: %u cached password(s) and the cached listings forgotten",
              DisconnectRoot ? DisconnectRoot : L"(null)", n);
    if (g_LogProc) {
        WCHAR details[] = L"All remembered passwords and cached listings forgotten";
        WCHAR text[] = L"DISCONNECT \\";
        g_LogProc(g_PluginNr, MSGTYPE_DETAILS, details);
        g_LogProc(g_PluginNr, MSGTYPE_DISCONNECT, text);
    }
    return TRUE;
}

BOOL __stdcall FsDisconnect(char* DisconnectRoot) {
    WCHAR root_w[MAX_PATH];
    if (!DisconnectRoot || MultiByteToWideChar(CP_ACP, 0, DisconnectRoot, -1, root_w, MAX_PATH) == 0) return TRUE;
    return FsDisconnectW(root_w);
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
    HANDLE hdl = execute_repo_query(Path, &cfg, repo, &parsed, FindData);
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

/* FsGetFileW below a repository alias */
typedef struct {
    const WCHAR* rest;
    const WCHAR* local_name;
    BOOL overwrite;
    const FILETIME* mtime;
    WfxDumpProgressContext* progress;
} GetFileContext;

/* TRUE when `rest` (L"" or "\...") is the single component `name` (ASCII, any case) */
static BOOL rest_is_file(const WCHAR* rest, const WCHAR* name) {
    while (*rest == L'\\') rest++;
    size_t n = wcslen(name);
    if (_wcsnicmp(rest, name, n) != 0) return FALSE;
    rest += n;
    while (*rest == L'\\') rest++;
    return *rest == L'\0';
}

/* The repository's ID and CRLF into the local file: from the cache, or else `restic cat config` */
static ResticResult write_repository_id(const RepoCall* call, const GetFileContext* ctx) {
    char text[REPOSITORY_ID_FILE_BYTES + 1];
    if (!call->trusted || !repo_cache_get_repository_id(call->repo->alias, call->repo->target, text)) {
        ULONGLONG started = GetTickCount64();
        ResticResult res  = restic_read_repository_id(call->cfg->restic_path, call->repo->target, call->password,
                                                      call->exec, text, call->err, call->err_cap);
        if (res != RESTIC_OK) return res;
        conn_log(FALSE, call->repo->alias, L"Repository ID read by restic in %.1f s",
                 (double)(GetTickCount64() - started) / 1000.0);
        repo_cache_put_repository_id(call->repo->alias, call->repo->target, text, call->generation);
    }
    memcpy(text + RESTIC_REPOSITORY_ID_CHARS, "\r\n", 3);
    return restic_write_local_file(ctx->local_name, text, REPOSITORY_ID_FILE_BYTES, ctx->overwrite, ctx->mtime);
}

static ResticResult get_file_operation(const RepoCall* call, void* ctx_ptr) {
    GetFileContext* ctx = (GetFileContext*)ctx_ptr;
    if (rest_is_file(ctx->rest, L"" REPOSITORY_ID_FILE_NAME)) return write_repository_id(call, ctx);
    RepoPath where;
    ResticResult res = resolve_repo_path(call, ctx->rest, FALSE, &where);
    if (res != RESTIC_OK) return res;
    if (where.kind == REPO_PATH_CSV) {
        res = restic_write_local_file(ctx->local_name, where.csv, where.csv_len, ctx->overwrite, ctx->mtime);
        free(where.csv);
        return res;
    }
    const WCHAR* sp = where.subpath;
    while (*sp == L'\\') sp++;
    if (where.kind != REPO_PATH_SNAPSHOT || *sp == L'\0') {
        return RESTIC_ERR_PATH_NOT_FOUND; /* A folder above the snapshots, or a snapshot itself */
    }
    return restic_dump_file(call->cfg->restic_path, call->repo->target, call->password, where.snapshot_id,
                            where.subpath, ctx->local_name, ctx->overwrite, ctx->mtime, WFX_RESTIC_TIMEOUT_MS,
                            wfx_dump_progress_callback, ctx->progress, call->err, call->err_cap);
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
    if (!parse_wfx_path(RemoteName, &parsed) || parsed.level != WFX_PATH_REPO || parsed.rest[0] == L'\0') {
        return FS_FILE_NOTFOUND;
    }

    /*
     * A name legal where the backup was made may mean something else here ("a:b" is a stream of
     * "a", "CON" a device, "a." the file "a"); the snapshot keeps the name, the copy is refused
     */
    if (!config_is_plain_file_name(LocalName)) {
        log_write(L"Copy %ls refused: Windows cannot store a file named as in %ls", RemoteName, LocalName);
        return FS_FILE_WRITEERROR;
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
    const ResticRepoEntry* repo = is_forget_entry_name(parsed.repo_alias) ? NULL
                                                                          : config_find_repo(&cfg, parsed.repo_alias);
    if (!repo) {
        config_free(&cfg);
        return FS_FILE_NOTFOUND;
    }
    if (!restic_path_usable(&cfg, repo->alias)) {
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

    /* Resolving the path may list the snapshots: ESC and the timeout apply to that too */
    WfxCancelContext cancel_ctx = {RemoteName, repo->alias};
    ResticExecOptions exec_opts = {WFX_RESTIC_TIMEOUT_MS, wfx_cancel_callback, &cancel_ctx};
    GetFileContext ctx = {parsed.rest, LocalName, overwrite, mtime, &prog_ctx};
    WCHAR err_msg[512] = {0};
    ResticResult res = run_with_password(&cfg, repo, &exec_opts, get_file_operation, &ctx, err_msg, _countof(err_msg));

    int ret = FS_FILE_READERROR;
    switch (res) {
        case RESTIC_OK:
            if (g_ProgressProc) {
                g_ProgressProc(g_PluginNr, RemoteName, LocalName, 100);
            }
            if (g_LogProc && InterlockedCompareExchange(&g_Connected, 0, 0)) { /* WFX SDK: "source -> target" */
                WCHAR done[MAX_PATH * 2 + 8];
                _snwprintf_s(done, _countof(done), _TRUNCATE, L"%ls -> %ls", RemoteName, LocalName);
                g_LogProc(g_PluginNr, MSGTYPE_TRANSFERCOMPLETE, done);
            }
            ret = FS_FILE_OK;
            break;
        case RESTIC_ERR_CANCELLED: /* Also a cancelled or empty password prompt */
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
