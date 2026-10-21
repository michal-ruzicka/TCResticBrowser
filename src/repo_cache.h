#pragma once
#include <windows.h>
#include "snapshot_tree.h"

/*
 * What the plugin keeps in memory about each repository between two Total Commander calls: the
 * restic mount folder tree built from its last `restic snapshots` (snapshot_tree.h).
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
 * repo_cache_unlock_tree(); NULL when nothing is cached (no lock is then held). `*out_age_ms`
 * receives how long ago restic listed the snapshots.
 */
const SnapshotTree* repo_cache_lock_tree(const WCHAR* alias, const WCHAR* target, ULONGLONG* out_age_ms);
void repo_cache_unlock_tree(void);

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
