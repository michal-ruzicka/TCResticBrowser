#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "repo_cache.h"
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <string.h>
#include <wchar.h>
#include "config.h"
#include "log.h"
#include "restic_cli.h"

#include <stdarg.h>
#include <stdio.h>

/* Snapshots read at the same time: one per repository (a second one would only share its line and
   storage), different repositories side by side, all within SnapshotCacheMB together */
#define MAX_CONCURRENT_LOADS 3
#define PROGRESS_REPORT_MS (5u * 1000u) /* How often a running snapshot listing reports progress */
/* A background restic that hangs (a stalled share or server) is given up after this long: a
   snapshot listing when no new entry arrived for so long, a snapshot list re-read in total */
#define BACKGROUND_TIMEOUT_MS (30u * 60u * 1000u)
#define LOAD_RETRY_MS (60u * 1000u)   /* A listing that failed is not tried again sooner */
#define SESSION_PASSWORD_CHARS 512    /* main.c SESSION_MAX_PASSWORD */

typedef enum {
    INDEX_LOADING = 0,
    INDEX_READY,
    INDEX_FAILED,     /* Retried after LOAD_RETRY_MS */
    INDEX_TOO_LARGE   /* Never retried while the limit stays */
} IndexState;

/* The complete listing of one snapshot, or the attempt to read it */
typedef struct IndexEntry IndexEntry;
struct IndexEntry {
    char snapshot_id[SNAPSHOT_TREE_ID_CHARS + 1];
    IndexState state;
    SnapshotIndex* index;           /* INDEX_READY */
    volatile LONG64 last_used;      /* GetTickCount64() of the last lookup (LRU); only through Interlocked* */
    ULONGLONG failed_at;
    IndexEntry* next;
};

typedef struct RepoEntry RepoEntry;
struct RepoEntry {
    WCHAR alias[MAX_PATH];
    WCHAR target[CONFIG_MAX_PATH];
    SnapshotTree* tree;
    char tree_format[SNAPSHOT_TREE_TIME_FORMAT_MAX + 1]; /* The time format its folders are named by */
    ULONGLONG tree_at; /* GetTickCount64() when the snapshots were listed */
    BOOL refreshing;   /* A background `restic snapshots` is running */
    ULONGLONG refresh_tried_at; /* When the last one started */
    IndexEntry* indexes;
    char repository_id[RESTIC_REPOSITORY_ID_CHARS + 1]; /* From `restic cat config`; "" until read */
    ULONGLONG repository_id_at;                         /* FILETIME ticks: when restic was asked for it */
    RepoEntry* next;
};

/* One background listing */
typedef struct {
    WCHAR alias[MAX_PATH];
    WCHAR target[CONFIG_MAX_PATH];
    WCHAR restic_exe[CONFIG_MAX_PATH];
    char snapshot_id[SNAPSHOT_TREE_ID_CHARS + 1];
    ULONGLONG generation;
    RepoPasswordSource password_source;
} LoadJob;

static SRWLOCK    g_CacheLock       = SRWLOCK_INIT;
static RepoEntry* g_Repos           = NULL;
/*
 * Generations: repo_cache_generation() hands out the current value of a counter that every forget
 * and clear advances. Data read under generation g is still good for alias A unless A was
 * forgotten (g_ForgottenAt) or everything was cleared (g_ClearedAt) at or after g, so forgetting
 * one repository leaves the work in progress for the others alone.
 */
typedef struct ForgetMark ForgetMark;
struct ForgetMark {
    WCHAR alias[MAX_PATH];
    ULONGLONG at;
    ForgetMark* next;
};

static ULONGLONG   g_CacheGeneration = 1; /* Advanced by every forget and clear */
static ULONGLONG   g_ClearedAt       = 0;
static ForgetMark* g_ForgottenAt     = NULL;
static size_t     g_IndexLimit      = (size_t)CONFIG_DEFAULT_SNAPSHOT_CACHE_MB * 1024u * 1024u;
static size_t     g_IndexBytes      = 0; /* Memory of all ready listings */
static size_t     g_LoadingBytes    = 0; /* Memory of the listings being read, as their workers last saw it */
static volatile LONG g_LoadsRunning = 0; /* Only through Interlocked*: workers change it without the lock */

/* Interlocked reads, so /analyze (C28112) sees every access to these variables go the same way */
static LONG loads_running(void) {
    return InterlockedCompareExchange(&g_LoadsRunning, 0, 0);
}

static LONG64 last_used_of(IndexEntry* e) {
    return InterlockedCompareExchange64(&e->last_used, 0, 0);
}

static RepoStatusProc g_StatusProc = NULL;

void repo_cache_set_status_proc(RepoStatusProc proc) {
    g_StatusProc = proc;
}

/* "[<alias>] <message>" to the status callback; never called with g_CacheLock held */
static void report(const WCHAR* alias, const WCHAR* format, ...) {
    RepoStatusProc proc = g_StatusProc;
    if (!proc) return;
    WCHAR text[1024];
    int n = _snwprintf_s(text, _countof(text), _TRUNCATE, L"[%ls] ", alias);
    if (n < 0) return;
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text + n, _countof(text) - (size_t)n, _TRUNCATE, format, args);
    va_end(args);
    proc(text);
}

/* TRUE while data read under `generation` may still be kept for `alias`; caller holds g_CacheLock */
static BOOL generation_valid_locked(const WCHAR* alias, ULONGLONG generation) {
    if (generation <= g_ClearedAt) return FALSE;
    for (const ForgetMark* m = g_ForgottenAt; m; m = m->next) {
        if (_wcsicmp(m->alias, alias) == 0) return generation > m->at;
    }
    return TRUE;
}

/* Caller holds g_CacheLock */
static RepoEntry* find_repo_locked(const WCHAR* alias, const WCHAR* target) {
    for (RepoEntry* r = g_Repos; r; r = r->next) {
        if (_wcsicmp(r->alias, alias) == 0 && wcscmp(r->target, target) == 0) return r;
    }
    return NULL;
}

static void free_index_entry(IndexEntry* e) {
    if (e->index) g_IndexBytes -= snapshot_index_memory(e->index);
    snapshot_index_free(e->index);
    free(e);
}

/* Caller holds g_CacheLock exclusively */
static void free_repo(RepoEntry* r) {
    while (r->indexes) {
        IndexEntry* e = r->indexes;
        r->indexes = e->next;
        free_index_entry(e);
    }
    snapshot_tree_free(r->tree);
    free(r);
}

/* Caller holds g_CacheLock exclusively; NULL when out of memory */
static RepoEntry* get_repo_locked(const WCHAR* alias, const WCHAR* target) {
    RepoEntry* r = find_repo_locked(alias, target);
    if (r || wcslen(alias) >= MAX_PATH || wcslen(target) >= CONFIG_MAX_PATH) return r;
    r = (RepoEntry*)calloc(1, sizeof(RepoEntry));
    if (!r) return NULL;
    wcscpy_s(r->alias, MAX_PATH, alias);
    wcscpy_s(r->target, CONFIG_MAX_PATH, target);
    r->next = g_Repos;
    g_Repos = r;
    return r;
}

static IndexEntry* find_index_locked(const RepoEntry* r, const char* snapshot_id) {
    for (IndexEntry* e = r ? r->indexes : NULL; e; e = e->next) {
        if (strcmp(e->snapshot_id, snapshot_id) == 0) return e;
    }
    return NULL;
}

const SnapshotTree* repo_cache_lock_tree(const WCHAR* alias, const WCHAR* target, const char* time_format,
                                         ULONGLONG* out_age_ms) {
    AcquireSRWLockShared(&g_CacheLock);
    const RepoEntry* r = find_repo_locked(alias, target);
    if (!r || !r->tree || strcmp(r->tree_format, time_format) != 0) { /* SnapshotTimeFormat changed */
        ReleaseSRWLockShared(&g_CacheLock);
        return NULL;
    }
    if (out_age_ms) *out_age_ms = GetTickCount64() - r->tree_at;
    return r->tree;
}

const SnapshotIndex* repo_cache_lock_index(const WCHAR* alias, const WCHAR* target, const char* snapshot_id) {
    AcquireSRWLockShared(&g_CacheLock);
    IndexEntry* e = find_index_locked(find_repo_locked(alias, target), snapshot_id);
    if (!e || e->state != INDEX_READY) {
        ReleaseSRWLockShared(&g_CacheLock);
        return NULL;
    }
    InterlockedExchange64(&e->last_used, (LONG64)GetTickCount64());
    return e->index;
}

void repo_cache_unlock(void) {
    ReleaseSRWLockShared(&g_CacheLock);
}

/* Frees ready listings, least recently used first, until `need` more bytes fit; caller holds the
   lock exclusively. `keep` is never freed. */
static void make_room_locked(size_t need, const IndexEntry* keep) {
    while (g_IndexBytes + need > g_IndexLimit) {
        RepoEntry* victim_repo = NULL;
        IndexEntry** victim    = NULL;
        for (RepoEntry* r = g_Repos; r; r = r->next) {
            for (IndexEntry** link = &r->indexes; *link; link = &(*link)->next) {
                IndexEntry* e = *link;
                if (e == keep || e->state != INDEX_READY) continue;
                if (!victim || last_used_of(e) < last_used_of(*victim)) {
                    victim      = link;
                    victim_repo = r;
                }
            }
        }
        if (!victim) return;
        IndexEntry* e = *victim;
        log_write(L"Listing of snapshot %hs of '%ls' dropped to make room (%u entries)", e->snapshot_id,
                  victim_repo->alias, (unsigned)snapshot_index_entry_count(e->index));
        *victim = e->next;
        free_index_entry(e);
    }
}

/*
 * Polled by a background restic run: TRUE once the repository was forgotten. A whole-snapshot
 * listing (`index` set) also reports how far it got every PROGRESS_REPORT_MS; the poll runs on the
 * worker's own thread, the one that fills the index.
 */
typedef struct {
    ULONGLONG generation;
    const WCHAR* alias;
    const LoadJob* job;          /* NULL for a snapshot-list re-read */
    const SnapshotIndex* index;
    ULONGLONG started, reported;
    size_t entries;              /* Entries at the last progress (inactivity timeout) */
    ULONGLONG progressed;
    size_t accounted;            /* This load's share of g_LoadingBytes */
    BOOL crowded;                /* Given up: the loads running side by side need more than the limit */
    BOOL stopped;                /* Told to stop; its share is no longer counted */
} LoadCancel;

static BOOL load_cancelled(void* ctx) {
    LoadCancel* c = (LoadCancel*)ctx;
    if (c->stopped) return TRUE;
    BOOL gone;
    if (c->index) {
        /* The listings kept and the ones being read stay within SnapshotCacheMB together: kept ones
           make room, least recently used first; if the loads alone are too much, this one gives up */
        size_t growing = snapshot_index_memory(c->index);
        AcquireSRWLockExclusive(&g_CacheLock);
        gone = !generation_valid_locked(c->alias, c->generation);
        g_LoadingBytes += growing - c->accounted;
        c->accounted = growing;
        if (!gone && g_IndexBytes + g_LoadingBytes > g_IndexLimit) {
            make_room_locked(g_LoadingBytes, NULL);
            if (g_LoadingBytes > g_IndexLimit && g_LoadingBytes > growing) c->crowded = TRUE;
        }
        if (gone || c->crowded) {
            /* Not counted while restic is being stopped, or the next load polling would give up too */
            g_LoadingBytes -= c->accounted;
            c->accounted = 0;
            c->stopped   = TRUE;
        }
        ReleaseSRWLockExclusive(&g_CacheLock);
        if (c->crowded) return TRUE;
    } else {
        AcquireSRWLockShared(&g_CacheLock);
        gone = !generation_valid_locked(c->alias, c->generation);
        ReleaseSRWLockShared(&g_CacheLock);
    }
    ULONGLONG now = GetTickCount64();
    if (c->index) {
        size_t entries = snapshot_index_entry_count(c->index);
        if (entries != c->entries) {
            c->entries    = entries;
            c->progressed = now;
        } else if (now - c->progressed >= BACKGROUND_TIMEOUT_MS) {
            log_write(L"Background listing of '%ls' given up: no new entry for %u minutes", c->alias,
                      BACKGROUND_TIMEOUT_MS / 60000u);
            return TRUE;
        }
    }
    if (!gone && c->job && c->index && now - c->reported >= PROGRESS_REPORT_MS) {
        c->reported = now;
        report(c->job->alias, L"Snapshot %.8hs: %u files and folders read so far (%llu s)", c->job->snapshot_id,
               (unsigned)snapshot_index_entry_count(c->index), (now - c->started) / 1000);
    }
    return gone;
}

/* Stores the outcome of a load; returns the state kept, or INDEX_LOADING when it was discarded */
static IndexState finish_load(const LoadJob* job, SnapshotIndex* index, IndexState state) {
    AcquireSRWLockExclusive(&g_CacheLock);
    IndexEntry* e = generation_valid_locked(job->alias, job->generation)
                        ? find_index_locked(find_repo_locked(job->alias, job->target), job->snapshot_id)
                        : NULL; /* Forgotten meanwhile: the entry is gone and the result discarded */
    if (e && state == INDEX_READY) {
        size_t bytes = snapshot_index_memory(index);
        make_room_locked(bytes, e);
        if (g_IndexBytes + bytes > g_IndexLimit) state = INDEX_TOO_LARGE;
    }
    if (!e) state = INDEX_LOADING;
    if (e) {
        e->state = state;
        e->failed_at = GetTickCount64();
        InterlockedExchange64(&e->last_used, (LONG64)e->failed_at);
        if (state == INDEX_READY) {
            e->index = index;
            g_IndexBytes += snapshot_index_memory(index);
            index = NULL;
        }
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
    snapshot_index_free(index);
    return state;
}

/*
 * Runs `worker(job)` on the thread pool with a reference to this DLL, so FreeLibrary cannot unmap
 * the code under it; the worker ends with release_worker_module(). FALSE if it did not start.
 */
static BOOL submit_worker(PTP_SIMPLE_CALLBACK worker, void* job) {
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(const void*)&g_CacheLock, &self)) {
        return FALSE; /* Without a reference the DLL could be unmapped under the worker (being unloaded) */
    }
    if (TrySubmitThreadpoolCallback(worker, job, NULL)) return TRUE;
    FreeLibrary(self);
    return FALSE;
}

static void release_worker_module(PTP_CALLBACK_INSTANCE instance) {
    HMODULE self = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(const void*)&g_CacheLock, &self)) {
        FreeLibraryWhenCallbackReturns(instance, self);
    }
}

static VOID CALLBACK load_index_worker(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    LoadJob* job = (LoadJob*)context;
    if (!job) { /* Never: submit_worker always passes a job */
        release_worker_module(instance);
        return;
    }
    WCHAR password[SESSION_PASSWORD_CHARS];
    WCHAR err[512] = {0};
    ULONGLONG started = GetTickCount64();

    SnapshotIndex* index = NULL;
    IndexState state     = INDEX_FAILED;
    SnapshotIndexStatus index_status = SNAPSHOT_INDEX_OK;
    ResticResult res     = RESTIC_ERR_INVALID_ARG;
    AcquireSRWLockShared(&g_CacheLock);
    size_t limit = g_IndexLimit;
    ReleaseSRWLockShared(&g_CacheLock);

    BOOL crowded = FALSE;
    if (job->password_source(job->alias, password, _countof(password)) &&
        (index = snapshot_index_new(limit)) != NULL) {
        LoadCancel cancel = {job->generation, job->alias, job, index, started, started, 0, started, 0, FALSE};
        ResticRepo access = {job->target, password};
        ResticExecOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.cancel_proc = load_cancelled;
        opts.cancel_ctx  = &cancel;
        res = restic_list_snapshot_index(job->restic_exe, &access, job->snapshot_id, &opts, index,
                                         &index_status, err, _countof(err));
        AcquireSRWLockExclusive(&g_CacheLock);
        g_LoadingBytes -= cancel.accounted; /* From now on counted as kept, or not at all */
        ReleaseSRWLockExclusive(&g_CacheLock);
        crowded = cancel.crowded;
        if (res == RESTIC_OK) {
            state = INDEX_READY;
        } else if (index_status == SNAPSHOT_INDEX_TOO_LARGE) {
            state = INDEX_TOO_LARGE;
        }
    }
    SecureZeroMemory(password, sizeof(password));

    unsigned entries = index ? (unsigned)snapshot_index_entry_count(index) : 0;
    unsigned kib     = index ? (unsigned)(snapshot_index_memory(index) / 1024) : 0;
    ULONGLONG ms     = GetTickCount64() - started;
    if (state != INDEX_READY) {
        snapshot_index_free(index);
        index = NULL;
    }
    switch (finish_load(job, index, state)) { /* May still refuse a listing that does not fit */
        case INDEX_READY: /* The connection log first: the diagnostic log line is what tests wait for */
            report(job->alias, L"Snapshot %.8hs read: %u files and folders, %u MiB, %llu s; its folders now open "
                               L"without restic", job->snapshot_id, entries, (kib + 1023) / 1024, ms / 1000);
            log_write(L"Listing of snapshot %hs of '%ls' read: %u entries, %u KiB, %llu ms", job->snapshot_id,
                      job->alias, entries, kib, ms);
            break;
        case INDEX_TOO_LARGE:
            log_write(L"Listing of snapshot %hs of '%ls' not kept: larger than SnapshotCacheMB", job->snapshot_id,
                      job->alias);
            report(job->alias, L"Snapshot %.8hs has more files than SnapshotCacheMB holds; its folders stay listed "
                               L"one by one", job->snapshot_id);
            break;
        case INDEX_LOADING:
            log_write(L"Listing of snapshot %hs of '%ls' discarded: the repository was forgotten meanwhile",
                      job->snapshot_id, job->alias);
            break;
        default:
            if (crowded) {
                log_write(L"Listing of snapshot %hs of '%ls' set aside: the snapshots read at the same time need the "
                          L"memory", job->snapshot_id, job->alias);
                report(job->alias, L"Snapshot %.8hs set aside: the snapshots read at the same time need the memory "
                                   L"SnapshotCacheMB allows; it is read again later", job->snapshot_id);
                break;
            }
            log_write(L"Listing of snapshot %hs of '%ls' not kept: %ls", job->snapshot_id, job->alias,
                      err[0] ? err : L"restic failed, was cancelled or had no password");
            report(job->alias, L"Snapshot %.8hs not read: %ls", job->snapshot_id,
                   err[0] ? err : L"restic failed, was cancelled or had no password");
            break;
    }
    free(job);
    InterlockedDecrement(&g_LoadsRunning);
    release_worker_module(instance);
}

void repo_cache_load_index(
    const WCHAR* alias,
    const WCHAR* target,
    const WCHAR* restic_exe,
    const char* snapshot_id,
    RepoPasswordSource password_source
) {
    if (!alias || !target || !restic_exe || !snapshot_id || !password_source ||
        strlen(snapshot_id) > SNAPSHOT_TREE_ID_CHARS || wcslen(restic_exe) >= CONFIG_MAX_PATH) {
        return;
    }
    LoadJob* job = (LoadJob*)calloc(1, sizeof(LoadJob));
    if (!job) return;

    AcquireSRWLockExclusive(&g_CacheLock);
    BOOL start = FALSE;
    RepoEntry* r = g_IndexLimit > 0 ? get_repo_locked(alias, target) : NULL;
    IndexEntry* e = find_index_locked(r, snapshot_id);
    ULONGLONG now = GetTickCount64();
    BOOL repo_busy = FALSE; /* One snapshot of a repository at a time */
    for (const IndexEntry* other = r ? r->indexes : NULL; other; other = other->next) {
        if (other->state == INDEX_LOADING) repo_busy = TRUE;
    }
    if (r && !repo_busy && loads_running() < MAX_CONCURRENT_LOADS &&
        (!e || (e->state == INDEX_FAILED && now - e->failed_at >= LOAD_RETRY_MS))) {
        if (!e && (e = (IndexEntry*)calloc(1, sizeof(IndexEntry))) != NULL) {
            strcpy_s(e->snapshot_id, sizeof(e->snapshot_id), snapshot_id);
            e->next    = r->indexes;
            r->indexes = e;
        }
        if (e) {
            e->state = INDEX_LOADING;
            start    = TRUE;
            InterlockedIncrement(&g_LoadsRunning);
            wcscpy_s(job->alias, MAX_PATH, r->alias);
            wcscpy_s(job->target, CONFIG_MAX_PATH, r->target);
            wcscpy_s(job->restic_exe, CONFIG_MAX_PATH, restic_exe);
            strcpy_s(job->snapshot_id, sizeof(job->snapshot_id), snapshot_id);
            job->generation      = g_CacheGeneration;
            job->password_source = password_source;
        }
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
    if (!start) {
        free(job);
        return;
    }

    if (!submit_worker(load_index_worker, job)) {
        log_write(L"Listing of snapshot %hs of '%ls' not started (Windows error %lu)", snapshot_id, alias, GetLastError());
        AcquireSRWLockExclusive(&g_CacheLock);
        InterlockedDecrement(&g_LoadsRunning);
        e = find_index_locked(find_repo_locked(job->alias, job->target), snapshot_id);
        if (e && e->state == INDEX_LOADING) {
            e->state     = INDEX_FAILED;
            e->failed_at = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&g_CacheLock);
        free(job);
        return;
    }
    log_write(L"Listing of snapshot %hs of '%ls' started in the background", snapshot_id, alias);
    report(alias, L"Reading every file and folder of snapshot %.8hs in the background", snapshot_id);
}

/* One background `restic snapshots` */
typedef struct {
    WCHAR alias[MAX_PATH];
    WCHAR target[CONFIG_MAX_PATH];
    WCHAR restic_exe[CONFIG_MAX_PATH];
    char time_format[SNAPSHOT_TREE_TIME_FORMAT_MAX + 1];
    ULONGLONG generation;
    RepoPasswordSource password_source;
} RefreshJob;

static VOID CALLBACK refresh_tree_worker(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    RefreshJob* job = (RefreshJob*)context;
    if (!job) { /* Never: submit_worker always passes a job */
        release_worker_module(instance);
        return;
    }
    WCHAR password[SESSION_PASSWORD_CHARS];
    WCHAR err[512] = {0};
    SnapshotTree* tree = NULL;
    ResticResult res = RESTIC_ERR_INVALID_ARG;
    ULONGLONG started = GetTickCount64();

    if (job->password_source(job->alias, password, _countof(password))) {
        LoadCancel cancel = {job->generation, job->alias, NULL, NULL, started, started, 0, started, 0, FALSE};
        ResticRepo access = {job->target, password};
        ResticExecOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.timeout_ms  = BACKGROUND_TIMEOUT_MS;
        opts.cancel_proc = load_cancelled;
        opts.cancel_ctx  = &cancel;
        res = restic_list_snapshots(job->restic_exe, &access, job->time_format, &opts, &tree, err,
                                    _countof(err));
    }
    SecureZeroMemory(password, sizeof(password));
    if (res == RESTIC_OK) {
        log_write(L"Snapshots of '%ls' re-read in the background: %u, %llu ms", job->alias,
                  (unsigned)snapshot_tree_snapshot_count(tree), GetTickCount64() - started);
        report(job->alias, L"Snapshot list re-read in the background: %u snapshots, %llu s",
               (unsigned)snapshot_tree_snapshot_count(tree), (GetTickCount64() - started) / 1000);
        repo_cache_put_tree(job->alias, job->target, job->time_format, tree, job->generation);
    } else {
        log_write(L"Snapshots of '%ls' not re-read in the background, the cached list stays: %ls", job->alias,
                  err[0] ? err : L"restic failed, was cancelled or had no password");
        report(job->alias, L"Snapshot list not re-read, the previous one stays: %ls",
               err[0] ? err : L"restic failed, was cancelled or had no password");
    }

    AcquireSRWLockExclusive(&g_CacheLock);
    RepoEntry* r = find_repo_locked(job->alias, job->target);
    if (r) r->refreshing = FALSE;
    ReleaseSRWLockExclusive(&g_CacheLock);
    free(job);
    release_worker_module(instance);
}

void repo_cache_refresh_tree(
    const WCHAR* alias,
    const WCHAR* target,
    const WCHAR* restic_exe,
    const char* time_format,
    ULONGLONG min_interval_ms,
    RepoPasswordSource password_source
) {
    if (!alias || !target || !restic_exe || !time_format || !password_source ||
        wcslen(restic_exe) >= CONFIG_MAX_PATH || strlen(time_format) > SNAPSHOT_TREE_TIME_FORMAT_MAX) {
        return;
    }
    RefreshJob* job = (RefreshJob*)calloc(1, sizeof(RefreshJob));
    if (!job) return;

    AcquireSRWLockExclusive(&g_CacheLock);
    RepoEntry* r = find_repo_locked(alias, target);
    ULONGLONG now = GetTickCount64();
    BOOL start = r && r->tree && !r->refreshing && now - r->tree_at >= min_interval_ms &&
                 (r->refresh_tried_at == 0 || now - r->refresh_tried_at >= min_interval_ms);
    if (start) {
        r->refreshing       = TRUE;
        r->refresh_tried_at = now;
        wcscpy_s(job->alias, MAX_PATH, r->alias);
        wcscpy_s(job->target, CONFIG_MAX_PATH, r->target);
        wcscpy_s(job->restic_exe, CONFIG_MAX_PATH, restic_exe);
        strcpy_s(job->time_format, sizeof(job->time_format), time_format);
        job->generation      = g_CacheGeneration;
        job->password_source = password_source;
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
    if (!start) {
        free(job);
        return;
    }
    if (!submit_worker(refresh_tree_worker, job)) {
        log_write(L"Snapshots of '%ls' not re-read (Windows error %lu)", alias, GetLastError());
        AcquireSRWLockExclusive(&g_CacheLock);
        r = find_repo_locked(alias, target);
        if (r) r->refreshing = FALSE;
        ReleaseSRWLockExclusive(&g_CacheLock);
        free(job);
        return;
    }
    log_write(L"Snapshots of '%ls' are re-read in the background", alias);
}

static VOID CALLBACK report_worker(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    WCHAR* text = (WCHAR*)context;
    RepoStatusProc proc = g_StatusProc;
    if (text && proc) proc(text);
    free(text);
    release_worker_module(instance);
}

void repo_cache_report_async(const WCHAR* text) {
    if (!text || !g_StatusProc) return;
    size_t len = wcslen(text) + 1;
    WCHAR* copy = (WCHAR*)malloc(len * sizeof(WCHAR));
    if (!copy) return;
    wcscpy_s(copy, len, text);
    if (!submit_worker(report_worker, copy)) free(copy);
}

void repo_cache_set_index_limit(size_t bytes) {
    AcquireSRWLockExclusive(&g_CacheLock);
    if (bytes != g_IndexLimit) {
        g_IndexLimit = bytes;
        make_room_locked(0, NULL);
        /* A listing refused as too large may fit the new limit */
        for (RepoEntry* r = g_Repos; r; r = r->next) {
            for (IndexEntry* e = r->indexes; e; e = e->next) {
                if (e->state == INDEX_TOO_LARGE) e->state = INDEX_FAILED, e->failed_at = 0;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}

BOOL repo_cache_wait_idle(DWORD timeout_ms) {
    ULONGLONG start = GetTickCount64();
    while (loads_running() != 0) {
        if (GetTickCount64() - start >= timeout_ms) return FALSE;
        Sleep(20);
    }
    return TRUE;
}

ULONGLONG repo_cache_generation(void) {
    AcquireSRWLockShared(&g_CacheLock);
    ULONGLONG g = g_CacheGeneration;
    ReleaseSRWLockShared(&g_CacheLock);
    return g;
}

void repo_cache_put_tree(const WCHAR* alias, const WCHAR* target, const char* time_format, SnapshotTree* tree,
                         ULONGLONG generation) {
    if (!tree) return;
    if (strlen(time_format) > SNAPSHOT_TREE_TIME_FORMAT_MAX) {
        snapshot_tree_free(tree);
        return;
    }
    AcquireSRWLockExclusive(&g_CacheLock);
    RepoEntry* r = generation_valid_locked(alias, generation) ? get_repo_locked(alias, target) : NULL;
    if (r) {
        snapshot_tree_free(r->tree);
        r->tree    = tree;
        r->tree_at = GetTickCount64();
        strcpy_s(r->tree_format, sizeof(r->tree_format), time_format);
        tree       = NULL;
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
    snapshot_tree_free(tree); /* Not cached: a repository was forgotten meanwhile */
}

BOOL repo_cache_get_repository_id(const WCHAR* alias, const WCHAR* target, char* out_id, ULONGLONG* out_read_at) {
    AcquireSRWLockShared(&g_CacheLock);
    const RepoEntry* r = find_repo_locked(alias, target);
    BOOL found         = r && r->repository_id[0] != '\0';
    if (found && out_id) memcpy(out_id, r->repository_id, sizeof(r->repository_id));
    if (found && out_read_at) *out_read_at = r->repository_id_at;
    ReleaseSRWLockShared(&g_CacheLock);
    return found;
}

void repo_cache_put_repository_id(const WCHAR* alias, const WCHAR* target, const char* id, ULONGLONG read_at,
                                  ULONGLONG generation) {
    if (strlen(id) != RESTIC_REPOSITORY_ID_CHARS) return;
    AcquireSRWLockExclusive(&g_CacheLock);
    RepoEntry* r = generation_valid_locked(alias, generation) ? get_repo_locked(alias, target) : NULL;
    if (r) {
        memcpy(r->repository_id, id, sizeof(r->repository_id));
        r->repository_id_at = read_at;
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}

void repo_cache_forget(const WCHAR* alias) {
    AcquireSRWLockExclusive(&g_CacheLock);
    ForgetMark* m = g_ForgottenAt;
    while (m && _wcsicmp(m->alias, alias) != 0) m = m->next;
    if (!m && wcslen(alias) < MAX_PATH && (m = (ForgetMark*)calloc(1, sizeof(ForgetMark))) != NULL) {
        wcscpy_s(m->alias, MAX_PATH, alias);
        m->next       = g_ForgottenAt;
        g_ForgottenAt = m;
    }
    if (m) {
        m->at = g_CacheGeneration++;
    } else {
        g_ClearedAt = g_CacheGeneration++; /* Cannot record it for the alias: invalidate everything */
    }
    for (RepoEntry** link = &g_Repos; *link;) {
        RepoEntry* r = *link;
        if (_wcsicmp(r->alias, alias) == 0) {
            *link = r->next;
            free_repo(r);
        } else {
            link = &r->next;
        }
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}

void repo_cache_clear(void) {
    AcquireSRWLockExclusive(&g_CacheLock);
    g_ClearedAt = g_CacheGeneration++;
    while (g_ForgottenAt) { /* All older than g_ClearedAt now */
        ForgetMark* m = g_ForgottenAt;
        g_ForgottenAt = m->next;
        free(m);
    }
    while (g_Repos) {
        RepoEntry* r = g_Repos;
        g_Repos = r->next;
        free_repo(r);
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}
