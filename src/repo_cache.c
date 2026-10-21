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
#include <wchar.h>
#include "config.h"

typedef struct RepoEntry RepoEntry;
struct RepoEntry {
    WCHAR alias[MAX_PATH];
    WCHAR target[CONFIG_MAX_PATH];
    SnapshotTree* tree;
    ULONGLONG tree_at; /* GetTickCount64() when the snapshots were listed */
    RepoEntry* next;
};

static SRWLOCK    g_CacheLock       = SRWLOCK_INIT;
static RepoEntry* g_Repos           = NULL;
static ULONGLONG  g_CacheGeneration = 0; /* Incremented by every forget/clear */

/* Caller holds g_CacheLock */
static RepoEntry* find_repo_locked(const WCHAR* alias, const WCHAR* target) {
    for (RepoEntry* r = g_Repos; r; r = r->next) {
        if (_wcsicmp(r->alias, alias) == 0 && wcscmp(r->target, target) == 0) return r;
    }
    return NULL;
}

static void free_repo(RepoEntry* r) {
    snapshot_tree_free(r->tree);
    free(r);
}

const SnapshotTree* repo_cache_lock_tree(const WCHAR* alias, const WCHAR* target, ULONGLONG* out_age_ms) {
    AcquireSRWLockShared(&g_CacheLock);
    const RepoEntry* r = find_repo_locked(alias, target);
    if (!r || !r->tree) {
        ReleaseSRWLockShared(&g_CacheLock);
        return NULL;
    }
    if (out_age_ms) *out_age_ms = GetTickCount64() - r->tree_at;
    return r->tree;
}

void repo_cache_unlock_tree(void) {
    ReleaseSRWLockShared(&g_CacheLock);
}

ULONGLONG repo_cache_generation(void) {
    AcquireSRWLockShared(&g_CacheLock);
    ULONGLONG g = g_CacheGeneration;
    ReleaseSRWLockShared(&g_CacheLock);
    return g;
}

void repo_cache_put_tree(const WCHAR* alias, const WCHAR* target, SnapshotTree* tree, ULONGLONG generation) {
    if (!tree) return;
    if (wcslen(alias) >= MAX_PATH || wcslen(target) >= CONFIG_MAX_PATH) {
        snapshot_tree_free(tree);
        return;
    }
    AcquireSRWLockExclusive(&g_CacheLock);
    RepoEntry* r = generation == g_CacheGeneration ? find_repo_locked(alias, target) : NULL;
    if (!r && generation == g_CacheGeneration && (r = (RepoEntry*)calloc(1, sizeof(RepoEntry))) != NULL) {
        wcscpy_s(r->alias, MAX_PATH, alias);
        wcscpy_s(r->target, CONFIG_MAX_PATH, target);
        r->next = g_Repos;
        g_Repos = r;
    }
    if (r) {
        snapshot_tree_free(r->tree);
        r->tree    = tree;
        r->tree_at = GetTickCount64();
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
