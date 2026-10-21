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


/*
 * Returns the cached tree of the repository, built with `time_format`, and keeps it readable (a
 * shared lock) until repo_cache_unlock(); NULL when no such tree is cached (no lock is then held).
 * `*out_age_ms` receives how long ago restic listed the snapshots.
 */
const SnapshotTree* repo_cache_lock_tree(const WCHAR* alias, const WCHAR* target, const char* time_format,
                                         ULONGLONG* out_age_ms);

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
 * cached password, unless it is already there or being read, one snapshot at a time. The
 * worker gives up when the repository is forgotten (its password wiped) and its result is
 * discarded then. A snapshot whose listing needs more than the memory limit is not tried again;
 * one that failed otherwise not before a minute has passed. Listings of other snapshots make room
 * for a new one, least recently used first, while it is being read, so the listings kept and the
 * one being read stay within the limit together.
 */
void repo_cache_load_index(
    const WCHAR* alias,
    const WCHAR* target,
    const WCHAR* restic_exe,
    const char* snapshot_id,
    RepoPasswordSource password_source
);

/*
 * Re-reads the snapshot list of the repository in the background with its cached password,
 * unless no tree is cached, one is being re-read, or the cached tree (or the last attempt) is
 * younger than `min_interval_ms`. Until the new list replaces it the cached tree stays in use; a
 * repository forgotten meanwhile discards the result.
 */
void repo_cache_refresh_tree(
    const WCHAR* alias,
    const WCHAR* target,
    const WCHAR* restic_exe,
    const char* time_format,
    ULONGLONG min_interval_ms,
    RepoPasswordSource password_source
);

/*
 * Receives one line about the background work ("[<alias>] Snapshot 1a2b3c4d read: ..."), for
 * Total Commander's connection log. Called from the background threads, never with a lock of the
 * cache held; NULL (the default) reports nothing.
 */
typedef void (*RepoStatusProc)(const WCHAR* text);
void repo_cache_set_status_proc(RepoStatusProc proc);

/* Sets the memory for snapshot listings ([Settings] SnapshotCacheMB); 0 disables them */
void repo_cache_set_index_limit(size_t bytes);

/* Waits until no background listing is running (tests), at most timeout_ms; FALSE on timeout */
BOOL repo_cache_wait_idle(DWORD timeout_ms);

/*
 * The value to pass to repo_cache_put_tree() for data read from now on: a repository forgotten
 * after this call does not get its data back from a restic call that was already running.
 * Forgetting one repository does not affect what is read for the others.
 */
ULONGLONG repo_cache_generation(void);

/* Caches `tree`, built with `time_format` (ownership passes to the cache, also when it is dropped)
 * unless a repository was forgotten since `generation` was obtained */
void repo_cache_put_tree(const WCHAR* alias, const WCHAR* target, const char* time_format, SnapshotTree* tree,
                         ULONGLONG generation);

/* Drops everything cached for the alias (any target), e.g. when its password is wiped */
void repo_cache_forget(const WCHAR* alias);

/* Drops everything (FsInitW, the forget command, DLL unload) */
void repo_cache_clear(void);
