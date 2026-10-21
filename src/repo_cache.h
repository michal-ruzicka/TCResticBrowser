#pragma once
#include <windows.h>
#include "snapshot_index.h"
#include "snapshot_tree.h"

/*
 * What the plugin keeps in memory about each repository between two Total Commander calls: the
 * restic mount folder tree built from its last `restic snapshots` (snapshot_tree.h), and the
 * complete listing of the snapshots the user has entered (snapshot_index.h), read by one
 * `restic ls` per snapshot in the background (repo_cache_load_index).
 *
 * A repository is identified by its alias and target together, so editing the INI never mixes two
 * repositories. Everything about a repository is dropped together with its cached password
 * (repo_cache_forget, repo_cache_clear): what was read with a password is never shown once the
 * password is gone. Thread-safe; callers never hold another lock of the plugin's except the
 * password session lock, which is always taken first.
 */

/* restic mount re-reads the snapshot list at most once a minute (minSnapshotsReloadTime) */
#define REPO_CACHE_TREE_TTL_MS (60u * 1000u)

/*
 * Returns the cached tree of the repository and keeps it readable (a shared lock) until
 * repo_cache_unlock(); NULL when nothing is cached (no lock is then held). `*out_age_ms` receives
 * how long ago restic listed the snapshots.
 */
const SnapshotTree* repo_cache_lock_tree(const WCHAR* alias, const WCHAR* target, ULONGLONG* out_age_ms);

/*
 * Returns the complete listing of a snapshot when it has been read, and keeps it readable until
 * repo_cache_unlock(); NULL otherwise (no lock is then held).
 */
const SnapshotIndex* repo_cache_lock_index(const WCHAR* alias, const WCHAR* target, const char* snapshot_id);

void repo_cache_unlock(void);

/* Reads the cached password of a repository into `out` (cap characters); FALSE if there is none */
typedef BOOL (*RepoPasswordSource)(const WCHAR* alias, WCHAR* out, size_t cap);

/*
 * Starts reading the complete listing of a snapshot in the background with the repository's
 * cached password, unless it is already there or being read, at most two of them at a time. The
 * worker gives up when the repository is forgotten (its password wiped) and its result is
 * discarded then. A snapshot whose listing needs more than the memory limit is not tried again;
 * one that failed otherwise not before a minute has passed. Listings of other snapshots make room
 * for a new one, least recently used first.
 */
void repo_cache_load_index(
    const WCHAR* alias,
    const WCHAR* target,
    const WCHAR* restic_exe,
    const char* snapshot_id,
    RepoPasswordSource password_source
);

/* Sets the memory for snapshot listings ([Settings] SnapshotCacheMB); 0 disables them */
void repo_cache_set_index_limit(size_t bytes);

/* Waits until no background listing is running (tests), at most timeout_ms; FALSE on timeout */
BOOL repo_cache_wait_idle(DWORD timeout_ms);

/*
 * The value to pass to repo_cache_put_tree() for data read from now on: a repository forgotten
 * after this call does not get its data back from a restic call that was already running.
 */
ULONGLONG repo_cache_generation(void);

/* Caches `tree` (ownership passes to the cache, also when it is dropped) unless a repository was
 * forgotten since `generation` was obtained */
void repo_cache_put_tree(const WCHAR* alias, const WCHAR* target, SnapshotTree* tree, ULONGLONG generation);

/* Drops everything cached for the alias (any target), e.g. when its password is wiped */
void repo_cache_forget(const WCHAR* alias);

/* Drops everything (FsInitW, the forget command, DLL unload) */
void repo_cache_clear(void);
