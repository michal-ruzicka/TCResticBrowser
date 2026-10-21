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

#define MAX_CONCURRENT_LOADS 2
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
    IndexEntry* indexes;
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
static ULONGLONG  g_CacheGeneration = 0; /* Incremented by every forget/clear */
static size_t     g_IndexLimit      = (size_t)CONFIG_DEFAULT_SNAPSHOT_CACHE_MB * 1024u * 1024u;
static size_t     g_IndexBytes      = 0; /* Memory of all ready listings */
static volatile LONG g_LoadsRunning = 0; /* Only through Interlocked*: workers change it without the lock */

/* Interlocked reads, so /analyze (C28112) sees every access to these variables go the same way */
static LONG loads_running(void) {
    return InterlockedCompareExchange(&g_LoadsRunning, 0, 0);
}

static LONG64 last_used_of(IndexEntry* e) {
    return InterlockedCompareExchange64(&e->last_used, 0, 0);
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

/* Background worker: one `restic ls` of a whole snapshot */
typedef struct {
    ULONGLONG generation;
} LoadCancel;

static BOOL load_cancelled(void* ctx) {
    const LoadCancel* c = (const LoadCancel*)ctx;
    AcquireSRWLockShared(&g_CacheLock);
    BOOL gone = c->generation != g_CacheGeneration;
    ReleaseSRWLockShared(&g_CacheLock);
    return gone;
}

static void finish_load(const LoadJob* job, SnapshotIndex* index, IndexState state) {
    AcquireSRWLockExclusive(&g_CacheLock);
    IndexEntry* e = job->generation == g_CacheGeneration
                        ? find_index_locked(find_repo_locked(job->alias, job->target), job->snapshot_id)
                        : NULL; /* Forgotten meanwhile: the entry is gone and the result discarded */
    if (e && state == INDEX_READY) {
        size_t bytes = snapshot_index_memory(index);
        make_room_locked(bytes, e);
        if (g_IndexBytes + bytes > g_IndexLimit) state = INDEX_TOO_LARGE;
    }
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
}

static VOID CALLBACK load_index_worker(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    LoadJob* job = (LoadJob*)context;
    if (!job) return; /* Never: repo_cache_load_index always passes a job */
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

    if (job->password_source(job->alias, password, _countof(password)) &&
        (index = snapshot_index_new(limit)) != NULL) {
        LoadCancel cancel = {job->generation};
        ResticExecOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.cancel_proc = load_cancelled;
        opts.cancel_ctx  = &cancel;
        res = restic_list_snapshot_index(job->restic_exe, job->target, password, job->snapshot_id, &opts, index,
                                         &index_status, err, _countof(err));
        if (res == RESTIC_OK) {
            state = INDEX_READY;
        } else if (index_status == SNAPSHOT_INDEX_TOO_LARGE) {
            state = INDEX_TOO_LARGE;
        }
    }
    SecureZeroMemory(password, sizeof(password));

    if (state == INDEX_READY) {
        log_write(L"Listing of snapshot %hs of '%ls' read: %u entries, %u KiB, %llu ms", job->snapshot_id, job->alias,
                  (unsigned)snapshot_index_entry_count(index), (unsigned)(snapshot_index_memory(index) / 1024),
                  GetTickCount64() - started);
    } else {
        log_write(L"Listing of snapshot %hs of '%ls' not kept: %ls", job->snapshot_id, job->alias,
                  state == INDEX_TOO_LARGE ? L"larger than SnapshotCacheMB"
                                           : err[0] ? err : L"restic failed, was cancelled or had no password");
        snapshot_index_free(index);
        index = NULL;
    }
    finish_load(job, index, state);
    free(job);
    InterlockedDecrement(&g_LoadsRunning);

    /* The worker holds a reference to this DLL so FreeLibrary cannot unmap it under the worker */
    HMODULE self = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(const void*)&g_CacheLock, &self)) {
        FreeLibraryWhenCallbackReturns(instance, self);
    }
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
    if (r && loads_running() < MAX_CONCURRENT_LOADS &&
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

    /* A reference to this DLL for the worker, released by FreeLibraryWhenCallbackReturns */
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(const void*)&g_CacheLock, &self)) {
        self = NULL; /* No reference taken: nothing for FreeLibrary below */
    }
    if (!TrySubmitThreadpoolCallback(load_index_worker, job, NULL)) {
        log_write(L"Listing of snapshot %hs of '%ls' not started (Windows error %lu)", snapshot_id, alias, GetLastError());
        if (self) FreeLibrary(self);
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
    RepoEntry* r = generation == g_CacheGeneration ? get_repo_locked(alias, target) : NULL;
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

void repo_cache_forget(const WCHAR* alias) {
    AcquireSRWLockExclusive(&g_CacheLock);
    g_CacheGeneration++;
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
    g_CacheGeneration++;
    while (g_Repos) {
        RepoEntry* r = g_Repos;
        g_Repos = r->next;
        free_repo(r);
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}
