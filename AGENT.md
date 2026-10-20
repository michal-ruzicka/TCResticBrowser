# AGENT.md - Restic File System Plugin for Total Commander (WFX)

Technical specification, operational requirements, compiler hardening rules, and architectural guidelines for developing the Total Commander Restic plugin in standard C (C11).

---

## 1. Project Goal

Build a **File System Plugin (WFX)** extension for **Total Commander** written strictly in **pure standard C (C11)** (no C++ or other languages).

The primary goal of Phase 1 (MVP) is to provide interactive, read-only browsing of **Restic** backup repositories ([https://restic.net](https://restic.net)), allowing users to navigate seamlessly across configured repositories, snapshots, and directory trees directly within Total Commander's dual-pane interface.

**Current scope:** read-only browsing **plus extraction** of files and whole folders through Total Commander's native copy (`F5`), viewer (`F3`) and Quick View (`Ctrl+Q`). Repository modification (upload, delete, move/rename, mkdir) is deliberately out of scope and not exported.

---

## 2. Core Architectural Decisions

1. **Interface Choice: WFX (File System Extension) instead of WCX (Packer)**
   - *Rationale:* The WCX archive model requires sequential, exhaustive enumeration of every file in an archive upon opening (`OpenArchive` / `ReadHeader` loop), freezing the UI on repositories containing hundreds of thousands of files. The WFX model enables lazy, on-demand directory-by-directory loading through the `FsFindFirstW` / `FsFindNextW` / `FsFindClose` lifecycle.

2. **Native CLI Integration**
   - No direct parsing of Restic's proprietary binary repository formats.
   - Invokes `restic.exe` as a hidden child process (`CREATE_NO_WINDOW`) via Win32 API (`CreateProcessW`).

3. **JSON & NDJSON Parsing**
   - A minimal, strict, read-only JSON reader of the project's own (`src/json.c`, `src/json.h`, ~370 lines), written for exactly what Restic prints. No third-party JSON library: the plugin parses untrusted repository content inside Total Commander's process, and a reader that does only what is needed is code that can be read in full.
   - Strict RFC 8259 (no comments, trailing commas, leading zeros), nesting limited to `JSON_MAX_DEPTH` (64), strings containing `\u0000` rejected so every decoded string is a plain C string, lone surrogate escapes decoded as U+FFFD. Integers are read exactly (`json_u64`), so file sizes above 2^53 are not rounded through a `double`. One arena per document: a single `json_free()` releases everything.
   - Portable C11 without Win32: `src/tests/test_json.c` builds with any hosted compiler (e.g. `gcc -fsanitize=address,undefined`), not only MSVC.
   - Verified beyond the unit tests: the full JSONTestSuite (all n_ rejected, all y_ accepted except the two with `\u0000`), differential testing against Python's `json`, and coverage-guided fuzzing (`src/tests/fuzz_json.c`). Re-run the fuzz target after any change to `json.c`.
   - Uses line-by-line NDJSON streaming/parsing for `restic ls --json` output.

4. **Caching Policy (MVP)**
   - **No persistent or in-memory directory cache.**
   - Every directory navigation queries the CLI directly to keep the codebase minimal, deterministic, and free from cache invalidation complexity.

5. **Credential Lifecycle**
   - No passwords are stored on disk (neither in `TCResticBrowser.ini` nor in external files).
   - Credentials are requested at runtime when entering a repository via Total Commander's `RequestProcW` (`RT_Password`) callback.
   - Credentials remain in RAM only, in a small per-repository session table (8 slots, so two panels can browse two repositories without being re-prompted). Entries are encrypted (`CryptProtectMemory`) and wiped on wrong password, on `FsInitW`, on DLL unload, after `PasswordTimeoutSeconds` (default 30 minutes) and through the `🗑 Forget N Passwords` root entry; the least recently cached slot is recycled when the table is full.

---

## 3. Configuration: `TCResticBrowser.ini`

### 3.1 INI File Path Resolution

The first existing file is used in full - keys are never merged across files. This order is shared with the author's other Total Commander plugins (TCOfficeView, TCNewPlus); keep it identical.

1. **Per-user override:** `%APPDATA%\GHISLER\TCResticBrowser.ini` (`config_get_user_ini_path()`, built from the `APPDATA` environment variable on every lookup).
2. **System-wide default:** `<plugin_dir>\TCResticBrowser.ini`, next to the plugin DLL, resolved at load time in `DllMain` via `GetModuleFileNameW`. This is where the commented sample ships, so a plugin upgrade overwrites it; user settings belong in the per-user file.
3. **`DefaultIniName` is ignored:** `FsSetDefaultParams` does nothing. Total Commander's own settings folder is usually `%APPDATA%\GHISLER` anyway; a deviating one (portable install, redirected `wincmd.ini`) is deliberately not consulted, for the same lookup as the sibling plugins.
4. **Tests must isolate `%APPDATA%`:** `test_config` and `test_plugin_dll` point `APPDATA` at an empty temporary directory first, otherwise a developer's real per-user INI would change the results.
5. **No Auto-Creation at Runtime (Sample Template in Repo):** the plugin DLL never creates or modifies `TCResticBrowser.ini` on disk. If neither file exists: the root listing (`\`) returns `ERROR_NO_MORE_FILES` (empty folder), `\<Alias>[...]` listings return `ERROR_PATH_NOT_FOUND`, and `FsGetFileW` returns `FS_FILE_NOTFOUND`; no password prompt and no popup are shown. A fully commented sample `src\TCResticBrowser.ini` is tracked in the repository, staged next to the built DLL by CMake, and shipped in the release ZIP.

### 3.2 INI File Structure

INI key-value syntax, read by the plugin's own reader in `config.c` (not `GetPrivateProfile*`): `[Section]` names and keys compare case-insensitively, lines starting with `;` or `#` are comments, keys and values are trimmed and a value is unquoted (`"..."` or `'...'`), CRLF, LF and CR line breaks are accepted. `ResticPath` takes its first occurrence in `[Settings]`; a repeated alias in `[Repositories]` updates the earlier one. Entries longer than the fixed buffers (alias `MAX_PATH`, target `CONFIG_MAX_PATH`) are skipped, never truncated, and a file above `CONFIG_MAX_INI_BYTES` (4 MiB) is not read.

```ini
[Settings]
; Optional path to restic.exe (if empty, checks plugin folder then system PATH)
ResticPath=restic.exe
; Optional Network Neighborhood root name (default "TCResticBrowser")
RootName=Backups

[Repositories]
; <Display_Alias>=<Restic_Repository_Target>
BackupLocale=D:\Backups\ResticRepo
ServerLinux=rest:https://backup.lan:8000/
CloudS3=s3:s3.us-east-1.amazonaws.com/company-backup/restic
NAS_SMB=\\nas\share\restic_repo
```

### 3.3 Restic Binary Resolution Order

1. Explicit path or custom executable configured in `ResticPath` under `[Settings]`.
2. Local `restic.exe` binary located in the plugin directory (`<plugin_dir>\restic.exe`).
3. Fallback to `restic.exe` found in the directories listed in the `PATH` environment variable.

Rules enforced by `config_resolve_restic_path()`:
- **The current directory is never searched.** `SearchPathW` and bare-name `CreateProcess` lookups include the process working directory (which follows the Total Commander panel), so a `restic.exe` planted in a browsed folder could hijack the plugin. `PATH` is therefore walked manually (`find_in_path_env`): empty entries and entries that are not absolute (`.`, `bin`, `C:tools`, an unexpanded `%SystemRoot%\...`) are skipped, quoted entries are unquoted, an extension-less name gets `.exe` appended, and the hit is returned as an absolute path.
- Only absolute paths (`config_is_absolute_path`: `X:\...` or `\\server\...`) are ever used. A relative `ResticPath` is joined to the plugin folder; a drive-relative (`C:restic.exe`) or root-relative (`\tools\restic.exe`) one is ignored. A relative `%APPDATA%` disables the per-user INI.
- When nothing is found the function returns `FALSE` and the bare name `restic.exe`. `restic_exec_command()` and `restic_dump_file()` refuse every path that is not absolute (`RESTIC_ERR_SPAWN_FAILED`), so the caller must report "restic.exe not found" instead of spawning anything.
- An explicit `ResticPath` that does not exist silently falls through to steps 2-3, and so does one too long for the path buffers: paths are never truncated, and `join_path` checks the length before `wcscat_s`, whose overflow handler would terminate Total Commander.

### 3.4 INI File Encoding

UTF-8 by default, like the author's sibling plugins; a UTF-8, UTF-16 LE or UTF-16 BE byte-order mark is honoured (`read_ini_text`). A BOM-less file that is not valid UTF-8 is decoded in the ANSI code page, because the 1.x plugin read its INI through `GetPrivateProfile*W` and documented ANSI. That API is deliberately not used: it reads a BOM-less file as ANSI and garbles UTF-8 paths. Alias lookup (`_wcsicmp`) folds ASCII case only; Total Commander always passes back the exact name the plugin listed.

### 3.5 Diagnostic Log (`[Logging] LogPath`, `src/log.c`)

Off by default, like TCOfficeView. `LogPath` (first occurrence in `[Logging]`) is applied by every `config_load()` through `log_configure()`, so it follows the INI without a restart; environment variables are expanded, a relative or empty path means off, missing parent directories are created. Each line is opened, appended as UTF-8 (no BOM) with a local time / pid / tid prefix, and closed, so the file is never locked. Logged: `FsInitW` (version, bitness, plugin dir), the effective configuration on every root listing, each `restic` command line with its exit code, duration, result and stderr, password prompts (**length only**, plus the `restic_describe_password` note when the entry starts or ends with whitespace or contains invisible characters), copies, and every error dialog. **Never log a password or `RESTIC_PASSWORD`;** repository targets go through `log_redact_target()`, which masks everything between `scheme://` and the **last** `@` (over-masking a path is preferred to logging a password that contains an unencoded `/`). `test_plugin_dll` asserts that no password, right or wrong, reaches the log.

---

## 4. Virtual Navigation Hierarchy

Total Commander passes backslash-delimited paths rooted at `\`. The plugin maps virtual paths as follows:

| Level | WFX Path | Data Source | Action / Presentation |
| --- | --- | --- | --- |
| **0. Root** | `\` | `[Repositories]` section of `TCResticBrowser.ini` | While passwords are cached, returns the virtual file `🗑 Forget N Password(s)` (`FORGET_ENTRY_PREFIX` + count, U+1F5D1) first; then each configured alias (e.g., `BackupLocale`, `ServerLinux`) as a virtual directory (`FILE_ATTRIBUTE_DIRECTORY`); an alias starting with the forget prefix is not listed (nor reachable through `FsFindFirstW`/`FsGetFileW`). With no INI or no repositories the root stays empty, except for the forget entry while passwords are still cached. |
| **1. Repository** | `\<RepoAlias>\` | `restic snapshots --json --no-lock` | Executes Restic with `RESTIC_REPOSITORY` and `RESTIC_PASSWORD`. Returns each snapshot as a directory (`YYYY-MM-DD_HH-MM-SS_[short_id]`). |
| **2. Snapshot/Subdir** | `\<RepoAlias>\<SnapshotDir>\<SubPath>` | `restic ls --json --no-lock <snapshot_id> <filter_path>` | Lists direct child files and folders at the requested directory depth without recursive traversal, populating `WIN32_FIND_DATAW` (64-bit file sizes, UTC `FILETIME` timestamps, attributes). |

---

## 5. Implemented WFX API (Read-Only Browsing + File Extraction)

All exported functions use the `__stdcall` calling convention. Both Unicode (`W`) entry points and mandatory ANSI stubs are exported for full compatibility with the Total Commander plugin loader:

- **`FsInitW` / `FsInit`**
  - Stores the assigned `PluginNr` identifier and host callback pointers (`pProgressProcW`, `pLogProcW`, `pRequestProcW`).
- **`FsSetDefaultParams`**
  - Exported but empty: `DefaultIniName` is not used (see 3.1).
- **`FsGetDefRootName`**
  - Returns the root label displayed in Network Neighborhood: `[Settings] RootName` from the active INI, or `CONFIG_DEFAULT_ROOT_NAME` (`"TCResticBrowser"`) when it is missing, empty, not a valid folder name, not representable in the ANSI code page (the API is `char*`), or longer than `maxlen`. Total Commander calls it only while installing the plugin (without `FsInitW`) and stores the answer in `wincmd.ini` `[FileSystemPlugins]`, so a changed `RootName` needs the plugin to be re-added. Error dialogs (`RT_MsgOK`) use the same name as their title.
- **`FsFindFirstW` / `FsFindFirst`**
  - Initializes directory enumeration for `Path`, allocates a reentrant `ResticFindHandle`, and populates the first `WIN32_FIND_DATAW` record.
  - On empty directories, calls `SetLastError(ERROR_NO_MORE_FILES)` and returns `INVALID_HANDLE_VALUE`.
- **`FsFindNextW` / `FsFindNext`**
  - Advances the `ResticFindHandle` cursor and populates the next `WIN32_FIND_DATAW` entry, or returns `FALSE` when exhausted.
- **`FsFindClose`**
  - Frees the `ResticFindHandle` and all associated buffers.
- **`FsExecuteFileW` / `FsExecuteFile`**
  - `open` (Enter) on a root name starting with `🗑 Forget ` (the count may be stale) wipes every cached password, shows how many were remembered (`RT_MsgOK`), then sets `RemoteName` to `\` and returns `FS_EXEC_SYMLINK`: Total Commander changes to the root and re-reads it, so the entry goes away without `Ctrl+R` (the ANSI `FsExecuteFile` copies the path back into its buffer); `open` on any other file returns `FS_EXEC_YOURSELF`, so Total Commander downloads and opens it as it did before the function was exported. `properties` is a no-op (`FS_EXEC_OK`); `chmod` and `quote` are refused (`FS_EXEC_ERROR`).
- **`FsGetFileW` / `FsGetFile`**
  - Extracts a single file from a snapshot (`restic dump --no-lock <snapshot_id> <filter_path>`) via `restic_dump_file()`, streaming Restic's stdout in 64 KB chunks (constant memory) into a **temporary file** `<target dir>\~rp<thread id>.tmp` (created with `CREATE_NEW`, so an existing file or a planted link of that name is never truncated or followed; `~rp<thread id>-<n>.tmp` is tried next) that atomically replaces `LocalName` (`MoveFileExW`) only after Restic exits with code 0. On any failure or `ESC` only the temporary file is deleted and a pre-existing target stays untouched (an interrupted overwrite never destroys the old file). The temporary file is created on the first byte, so authentication/path errors never touch the disk; 0-byte files are created at commit time. A read-only target loses the attribute only for the replacement and gets it back if the move fails.
  - Integrates with Total Commander's native copy/extract workflow: when the user copies a folder (`F5`), Total Commander walks the folder tree via `FsFindFirstW`/`FsFindNextW`/`FsFindClose`, creates the local directory hierarchy, and calls `FsGetFileW` for each file. Viewing files with `F3` (Lister) and `Ctrl+Q` (Quick View) also works automatically.
  - Implements Total Commander's two-step overwrite protocol (`FS_FILE_EXISTS` when `FS_COPYFLAGS_OVERWRITE` is not set; a read-only target is replaced when the flag is set), preserves `LastWriteTime` from `RemoteInfoStruct` (the "unknown time" marker is ignored), and reports progress to `ProgressProcW`: 0 % before the transfer, then the first chunk and at most one call per ~100 ms (capped at 99 %), 100 % on success. `ESC` -> `FS_FILE_USERABORT`. The timeout is an **inactivity** timeout (`WFX_RESTIC_TIMEOUT_MS`, 30 min without a single byte on stdout/stderr), so a slow multi-GB transfer is never killed.
  - Result mapping: `RESTIC_ERR_LOCAL_WRITE` (disk full, missing destination directory, destination is a directory) -> `FS_FILE_WRITEERROR` without popup; `PATH_NOT_FOUND`, invalid virtual path, unknown alias, root/repository/snapshot-level path -> `FS_FILE_NOTFOUND`; cancelled or empty password dialog -> `FS_FILE_USERABORT`; wrong password after 3 attempts and other Restic failures -> `FS_FILE_READERROR` with one popup. The password session (7.5) is shared with the listing code.
  - Rejects `FS_COPYFLAGS_MOVE` (`F6`, even combined with `OVERWRITE`) and `FS_COPYFLAGS_RESUME` with `FS_FILE_NOTSUPPORTED`; entries whose `RemoteInfoStruct.Attr` has `FILE_ATTRIBUTE_DIRECTORY` are refused too. Known limitation: Total Commander always passes `ri`, but a caller passing `ri == NULL` for a directory path would receive a tar stream (`restic dump` archives folders).

*Note:* Modification, deletion, and move functions (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`) are intentionally omitted to enforce strict read-only repository access.

### 5.1 Plugin Icon Specification (Total Commander WFX Standard)

- **Network Neighborhood Root Icon (implemented):** Total Commander automatically displays the **first `ICON` resource (by index)** embedded inside the plugin DLL (`TCResticBrowser.wfx` / `TCResticBrowser.wfx64`) to the left of `"TCResticBrowser"` in Network Neighborhood (no extra exported function is required). Per the official WFX specification:
  - Format: `src\TCResticBrowser.ico` linked into the DLL via `src\TCResticBrowser.rc`.
  - Included frames inside `src\TCResticBrowser.ico` (5 resolutions): **16×16, 24×24, 32×32, 48×48** (32-bit BGRA uncompressed BMP/DIB frames) plus **256×256** (32-bit RGBA PNG-compressed), rendered from the SVG sources `logo\TCResticBrowser-16.svg` (16 px) and `logo\TCResticBrowser.svg` (all other sizes); the regeneration commands are in CONTRIBUTING.md, *Plugin Icon*. Keep the icon simple enough to read at 16 px.
- **Per-Item Custom Icons (`FsExtractCustomIconW`, optional):** only needed if custom icons are desired for individual repositories or snapshots inside the file list (`FS_ICONFLAG_SMALL` requests 16×16, otherwise 32×32). When omitted, Total Commander uses standard folder icons and normal Windows file-extension associations.
- **DLL Version Resource (`VS_VERSION_INFO`, implemented):** `version=` in `src\pluginst.inf` is the single source of truth for the plugin version. CMake turns it into `version.h` in the build directory (`src\version.h.in`: `MAJOR,MINOR,PATCH,0` / `"MAJOR.MINOR.PATCH"`, plus the per-bitness file name), which `src\TCResticBrowser.rc` embeds so Windows Explorer and Total Commander display the version and metadata in file properties ("Details" tab).

---

## 6. Source Tree Structure

```text
TCResticBrowser/
├── build.cmd                   # Windows build driver: x64 + x86 Release, tests, reproducible ZIP; "check" = Debug/analyze/ASan
├── setup-build-environment-example.cmd  # Fresh-machine bootstrap: Build Tools from .vsconfig, then build.cmd
├── .vsconfig                   # Pinned Visual Studio Build Tools components (toolset 14.51, SDK 26100, CMake, ASan)
├── .github/workflows/build.yml # CI on windows-latest: build.cmd, build.cmd check, live tests (pinned restic)
├── .clang-format               # Code formatting rules (C11, 4 spaces, 100 cols)
├── logo/                       # SVG sources of src/TCResticBrowser.ico (TCResticBrowser.svg, TCResticBrowser-16.svg for 16 px)
├── src/                        # Every build input, and nothing else
│   ├── CMakeLists.txt          # Targets, compiler hardening, reproducibility flags, version.h generation
│   ├── pluginst.inf            # Total Commander auto-install manifest; version= and release-date= are canonical
│   ├── TCResticBrowser.ini     # Commented sample configuration (staged next to the DLL and into the ZIP)
│   ├── TCResticBrowser.def     # Explicit DLL exports (undecorated names, also for x86 __stdcall)
│   ├── TCResticBrowser.rc      # Windows resource script (ICON #1 + VS_VERSION_INFO metadata)
│   ├── TCResticBrowser.ico     # Multi-resolution plugin icon
│   ├── version.h.in            # Template for the generated version.h
│   ├── fsplugin.h              # Total Commander WFX API declarations (+ FS_SET_TIME_UNKNOWN macro)
│   ├── main.c                  # DllMain, WFX entry points, virtual path routing
│   ├── config.c / config.h     # TCResticBrowser.ini resolution & parsing, binary resolution
│   ├── restic_cli.c / .h       # Child process execution, Win32 pipes, snapshot/NDJSON parsing
│   ├── json.c / json.h         # Minimal strict read-only JSON reader (portable C11, no Win32)
│   ├── log.c / log.h           # Optional diagnostic log ([Logging] LogPath), off by default
│   └── tests/
│       ├── test_json.c         # JSON reader: strictness, escapes, surrogates, limits, exact integers
│       ├── test_config.c       # Configuration, PATH-lookup security, 300-repo scaling, root enumeration
│       ├── test_restic_cli.c   # Parsers, quoting, process execution (cmd.exe stand-in) + live restic.exe tests
│       ├── test_plugin_dll.c   # Black-box smoke test: LoadLibrary(plugin) + exported WFX surface
│       └── fuzz_json.c         # libFuzzer target for the JSON reader (clang; not part of build.cmd)
├── build/                      # (git-ignored) build/x64, build/x86, build/check-*, build/stage
└── dist/                       # (git-ignored) TCResticBrowser.v<version>.zip
```

---

## 7. Low-Level Implementation Details

### 7.1 Reentrant Search Handle (`ResticFindHandle`)

Total Commander may open directories concurrently across both panels or during recursive operations. Each `FsFindFirstW` / `FsFindNextW` cycle maintains isolated state:

```c
typedef struct {
    WIN32_FIND_DATAW* items; /* Array of entries discovered for the target path */
    size_t count;            /* Total number of entries in items */
    size_t current_index;    /* Next entry index to return in FsFindNextW */
} ResticFindHandle;
```

### 7.2 Invoking `restic.exe` via Win32 Pipes (`src/restic_cli.c`)

`restic_exec_command(exe, repo, password, args, options, out)` is the single process-spawning primitive:

- **Executable:** must contain a path separator (bare names refused, see 3.3). The command line is built in a heap buffer sized from the inputs (limit 32767 chars, `RESTIC_ERR_INVALID_ARG` beyond it) - never in a fixed stack buffer, because `swprintf_s` aborts the host process on overflow.
- **Two pipes, two streams:** stdout and stderr use separate anonymous pipes. With `--json` stdout carries only the payload, while `{"message_type":"exit_error",...}` and warnings go to **stderr** (verified against restic 0.19.1). Merging them would corrupt JSON parsing whenever Restic prints a warning.
- **stdin = `NUL`** so Restic can never block on an interactive prompt.
- **Explicit handle whitelist:** `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` makes the child inherit exactly {stdin NUL, stdout pipe, stderr pipe}, so no unrelated inheritable handle of Total Commander leaks into Restic. It does not stop a child that another Total Commander thread starts with `bInheritHandles` at the same moment from inheriting our write ends; EOF may then never arrive, which is why both output loops end when **Restic exits**, never at EOF - keep it that way. Parent-side read ends are non-inheritable.
- **Environment block** (`CREATE_UNICODE_ENVIRONMENT`): the parent environment (Go needs `SystemRoot`, `APPDATA`, `TEMP`...) minus any inherited `RESTIC_REPOSITORY`, `RESTIC_REPOSITORY_FILE`, `RESTIC_PASSWORD`, `RESTIC_PASSWORD_FILE`, `RESTIC_PASSWORD_COMMAND` (they would conflict), plus `RESTIC_REPOSITORY` and `RESTIC_PASSWORD`. The block is wiped with `SecureZeroMemory` right after `CreateProcessW`. The password is never placed on the command line.
- **Output collection without deadlock:** one loop polls both pipes (`PeekNamedPipe` + `ReadFile`), waits at most 20 ms on the process handle only when idle, and performs a final drain after the child exits. A pass reads at most 1 MiB per pipe, so the timeout and `ESC` are checked even while Restic floods its output. Buffers grow geometrically and are always NUL-terminated and non-NULL after a successful spawn.
- **Timeout and cancellation:** `ResticExecOptions { timeout_ms, cancel_proc, cancel_ctx }`. `cancel_proc` is polled about every 100 ms (intended to forward Total Commander's `ProgressProc` abort result); on timeout/cancel the child is terminated (`RESTIC_ERR_TIMEOUT` / `RESTIC_ERR_CANCELLED`). `NULL` options mean no limit. `main.c` must always pass a cancel callback: an unreachable `rest:`/cloud repository makes Restic retry for minutes and would otherwise freeze Total Commander.
- **Exit codes:** `0` -> `RESTIC_OK`, `10` -> `RESTIC_ERR_REPO_NOT_FOUND`, `11` -> `RESTIC_ERR_REPO_LOCKED`, `12` -> `RESTIC_ERR_WRONG_PASSWORD`, anything else -> `RESTIC_ERR_CLI_FAILED`. `error_message` receives the `"message"` of the first JSON line on stderr (then stdout), or the raw trimmed text, safely truncated to 511 characters.

### 7.3 Argument Safety (`restic_list_directory`)

- Snapshot IDs go on the command line unquoted, so they are validated as hexadecimal (max 64 chars) before anything is spawned; `"--help"`, whitespace or `NULL` yield `RESTIC_ERR_INVALID_ARG`.
- The `ls` filter path (arbitrary bytes from the backed-up file system, e.g. Linux names containing a double quote) goes through `restic_quote_arg()`, which implements the `CommandLineToArgvW` escaping rules (backslashes before quotes doubled, quotes escaped). A round-trip test against `CommandLineToArgvW` covers it.
- Paths up to 1023 UTF-16 units (Total Commander's limit) are supported end to end (UTF-8 buffer of 3073 bytes).
- Known limitation: a Linux file name containing a literal backslash cannot be addressed, because the backslash is the WFX path separator. Such entries are not listed at all (see 7.4).

### 7.4 Parsing Rules (`restic_parse_*`)

- Timestamps (`restic_parse_iso8601_time`) must be complete RFC 3339 values as restic writes them: a fraction needs a digit, the zone is `Z`, `+HH`, `+HHMM` or `+HH:MM`, and nothing may follow.
- `snapshots --json`: a literal `null` is an empty list; entries with an invalid time or non-hex ID are skipped (the directory name must never contain separators). The name carries the 8-character `short_id`; when snapshots share it (likely from tens of thousands of snapshots on), they carry the shortest unique prefix of their full IDs instead (`unique_id_prefixes`, neighbours in sorted order), because `restic ls <prefix>` refuses an ambiguous prefix. Directory name = snapshot time **as recorded by Restic (its own UTC offset)**; the `FILETIME` fields are converted to UTC.
- `ls --json` NDJSON: strict - a malformed line aborts with `RESTIC_ERR_JSON_PARSE` (stdout contains JSON only). Only direct children of the filter path are returned. The filtered directory's own node is consumed to prove existence: a filter other than `/` whose node is missing or is not a `dir` yields `RESTIC_ERR_PATH_NOT_FOUND`. This distinguishes "missing" from "empty directory", which Total Commander needs (`ERROR_NO_MORE_FILES` = exists but empty, any other error = does not exist).
- Name safety (`is_safe_component`): only names that are exactly one path component are reported. Total Commander appends the reported name to build both the next virtual path and, on `F5`, the local target path, so a name containing `\` or `/` (a legal Linux file name such as `..\..\x`) would let a recursive copy write outside the chosen target directory. Names consisting only of dots and spaces (`.`, `..`, `...`, `.. `) are skipped too, because Win32 strips trailing dots and spaces from path components. Dot files and names merely ending in a dot are kept.
- Entry mapping: `dir` -> `FILE_ATTRIBUTE_DIRECTORY`; everything else (files, **symlinks**, devices) -> plain file, as the WFX specification requires ("Links should be returned as files"). All entries carry `FILE_ATTRIBUTE_READONLY`. 64-bit sizes are split into `nFileSizeHigh/Low`.
- Unknown dates: when `mtime` is missing or unparsable the entry gets Total Commander's "no date" marker (`FS_SET_TIME_UNKNOWN`: high `0xFFFFFFFF`, low `0xFFFFFFFE`); repository aliases at the root use it too.

### 7.5 Virtual Path Routing, Password Session & Host Callbacks (`src/main.c`)

- **Path decomposition (`parse_wfx_path`):** splits the backslash-delimited `Path` passed to `FsFindFirstW` into:
  - `WFX_PATH_ROOT` (`\`): calls `enumerate_root_repositories()`.
  - `WFX_PATH_REPO` (`\<RepoAlias>`): resolves `RepoAlias` via `config_find_repo()` and calls `restic_list_snapshots()`.
  - `WFX_PATH_SNAPSHOT_DIR` (`\<RepoAlias>\<SnapshotDir>[\<SubPath>]`): extracts the hexadecimal `snapshot_id` via `restic_extract_snapshot_id()` (rejecting malformed folder names before prompting or spawning) and calls `restic_list_directory()`.
- **Pre-flight binary check:** if `cfg.restic_path` contains no path separator (`config_resolve_restic_path` returned `FALSE`), `execute_repo_query()` logs and displays `"restic.exe not found..."` and returns `ERROR_FILE_NOT_FOUND` without prompting for a password.
- **In-memory password sessions:** `g_Sessions[8]` (`SessionSlot { alias, password }`) is guarded by an `SRWLOCK` (statically initialized, so it also works in unit tests where `DllMain` never runs) because Total Commander may call the plugin from worker threads. `execute_repo_query()` copies the password into a local buffer (`session_get`), runs Restic **without** holding the lock, and wipes the local copy with `SecureZeroMemory`. A password is cached (`session_put`) only when restic demonstrably accepted it (`RESTIC_OK`, `REPO_LOCKED`, `JSON_PARSE`, `PATH_NOT_FOUND`: restic checks the password before it locks the repository or reads a snapshot); after a local write error, `ESC`, a timeout, a missing repository or a spawn failure an unverified password is not kept, while one cached earlier stays. `session_forget` / `clear_password_session` wipe one slot / all slots. Each `session_put` gives the slot a new `generation`; `session_get` returns it, and `session_forget` (wrong password) and `session_touch` act only if the slot still has it, so a password another thread verified in the meantime is neither wiped nor extended by mistake. A full table recycles the slot with the oldest `cached_at`. **Memory protection:** the slot table is `VirtualAlloc`ed on first use, `VirtualLock`ed (no paging), excluded from WER crash dumps (`WerRegisterExcludedMemoryBlock`, looked up at run time), and each cached password is stored as `CryptProtectMemory(CRYPTPROTECTMEMORY_SAME_PROCESS)` ciphertext; `session_get` decrypts into the caller's stack buffer, which is wiped after the restic call. If encryption fails the password is not cached; a failed lock is logged and tolerated. `release_password_session` (DLL unload) wipes, unlocks and frees the table. **Password timeout:** `[Settings] PasswordTimeoutSeconds` (default 1800, `0` = until exit, anything but a plain decimal number = default) is applied by every `load_config()`; a slot expires `cached_at` (`GetTickCount64`, which counts through sleep) + timeout after restic first accepted the password — a password taken from the cache is never `session_put` again, so use does not extend it — unless `[Settings] PasswordTimeoutResetOnUse` is yes (yes/true/on/1, default no), when `session_touch` restarts the interval after each operation restic completes with the cached password. A thread-pool timer (`session_expire_locked` re-arms it for the next expiry) wipes expired slots even while Total Commander is idle; on `FreeLibrary` it is stopped and `WaitForThreadpoolTimerCallbacks` runs outside `g_SessionLock` (the callback takes it), at process exit (`lpvReserved != NULL`) the timer is left alone and the lock is only tried (`TryAcquireSRWLockExclusive`): the other threads are already terminated, possibly one holding it. `DllMain` does not call `DisableThreadLibraryCalls`, which Microsoft advises against with the static CRT. Plain copies remain only transiently: the stack buffer during one operation, the child environment block until `CreateProcessW` returns, and restic's own process.
- **Password prompt & retry:** when no password is cached for the alias, `prompt_repository_password()` calls `RequestProcW(RT_Password)` with `ReturnedText[0] = 0` (the buffer is the dialog's default text). An **empty** password or a cancelled dialog yields `ERROR_CANCELLED` without any popup (Restic rejects empty passwords, and accepting one would only produce a confusing generic failure). Up to `MAX_PASSWORD_ATTEMPTS` (3) prompts are shown when Restic keeps answering `RESTIC_ERR_WRONG_PASSWORD` (exit code 12); a stale cached password counts as the first attempt and is forgotten.
- **User abort (`ESC`):** `wfx_cancel_callback()` polls `ProgressProcW` every ~100 ms while Restic runs and terminates `restic.exe` on a non-zero result (`ERROR_CANCELLED`, no popup, a valid cached password is kept). Per the WFX SDK, calls made during the `FsFindFirst/Next/Close` loop are ignored by Total Commander for the first 5 seconds and only then show a progress dialog, so fast listings never flicker. The call uses `PercentDone = 0` (indeterminate) and passes the **alias, never the repository URL**, as `TargetName`: the URL can embed credentials (`rest:https://user:pass@host`) and would be shown on screen. A 30-minute `WFX_RESTIC_TIMEOUT_MS` is a safety net against hung commands.
- **Win32 error mapping:** empty listings (`RESTIC_OK` with `count == 0`) set `ERROR_NO_MORE_FILES`; missing paths or repositories set `ERROR_PATH_NOT_FOUND`; wrong passwords after retries set `ERROR_ACCESS_DENIED`; locked repositories set `ERROR_LOCK_VIOLATION`. User-facing errors are reported via both `g_LogProc(MSGTYPE_IMPORTANTERROR)` and `g_RequestProc(RT_MsgOK)`.

---

## 8. Compilation, Hardening & Quality Standards (MSVC `cl.exe`)

Because the plugin runs in-process inside `TOTALCMD64.EXE` and handles dynamic memory, string/JSON parsing, and external processes, strict compiler and linker practices are mandatory:

### 8.1 Strict Compiler Warning & Standard Flags
- `/TC`: Force compilation strictly as pure C (reject any C++ constructs).
- `/std:c11`: Enforce modern ISO C11 standard.
- `/permissive-`: Disable non-conforming MSVC language extensions.
- `/W4`: Enable high warning level across all project source files.
- `/WX`: Treat all warnings as errors so implicit conversions or uninitialized variables cannot slip through.
- `/we4013`: Treat implicit function declarations (missing prototypes) as errors—critical on 64-bit targets to prevent pointer truncation.

### 8.2 Static CRT Linking (`/MT` & `/MTd`)
- **Release (`/MT`):** Links the C Runtime statically into the plugin DLL. The plugin works out-of-the-box on any clean Windows installation without requiring the user to install matching *Visual C++ Redistributable* packages (`VCRUNTIME140.dll`).
- **Debug (`/MTd`):** Static Debug CRT enabling automatic heap integrity and leak checks (`_CrtCheckMemory`, `_CRTDBG_LEAK_CHECK_DF`).

### 8.3 Runtime Bug Detection & Sanitizers
- **AddressSanitizer (`/fsanitize=address /Zi`):** Detects out-of-bounds accesses, use-after-free, and double-free bugs at runtime (`build.cmd check`).
- **CRT Debug Heap (`<crtdbg.h>`):** Enabled in Debug builds (when ASan is not active) via `_CRTDBG_MAP_ALLOC` and `_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF)`.

### 8.4 Static Analysis
- **MSVC Code Analysis (`/analyze`):** Deep data-flow inspection for `NULL` pointer dereferences, potential buffer overruns, and unchecked return values (`build.cmd check`).
- Warnings from the SDK's own annotations are disabled where they hit, never globally: `main.c` disables C6553 because SDK 10.0.26100 annotates a `DWORD` parameter of `SetThreadpoolTimer` `_In_opt_`. Functions keep their stack below the 16 KB C6262 limit (large test buffers are `static`); `-Wstack-usage=13000` under MinGW approximates it.

### 8.5 Binary Security Hardening
- **Compiler:** `/GS` (Buffer Security Check / stack cookie protection).
- **Linker:** `/DYNAMICBASE` (ASLR - Address Space Layout Randomization) and `/NXCOMPAT` (DEP - Data Execution Prevention), plus `TCResticBrowser.def` to export undecorated symbol names (on x86 the `__stdcall` entry points would otherwise be exported as `_FsInit@16` and similar).
- **Export surface:** only what `TCResticBrowser.def` lists is exported. `src/tests/test_plugin_dll.c` asserts the exact surface: only the read-only `Fs*` functions are exported; `PutFile/DeleteFile/MkDir/RemoveDir/RenMovFile/SetAttr/SetTime` (and internal helpers such as `json_parse`) must stay absent, because Total Commander enables an operation as soon as the symbol exists.
- **Imports:** only `KERNEL32.dll` (static CRT). No extra import libraries are linked into the plugin (`CMakeLists.txt` replaces CMake's default import-library list with `kernel32.lib`; only `test_restic_cli` adds `shell32` for `CommandLineToArgvW`).

### 8.6 Build & Test Commands (`build.cmd` & `src/CMakeLists.txt`)

Windows-native only: `build.cmd` (cmd.exe + PowerShell, both part of Windows) drives CMake with the NMake generator through the pinned Visual Studio Build Tools (`.vsconfig`). No Bash, Python or other tools.

```bat
build.cmd          :: x64 + x86 Release builds, all four test programs for both, then dist\TCResticBrowser.v<version>.zip
build.cmd check    :: x64 only, no ZIP: Debug build + tests (CRT heap & leak checks), /analyze build, ASan test run
```

Every variant has its own build directory (`build\x64`, `build\x86`, `build\check-debug`, `build\check-analyze`, `build\check-asan`), so nothing needs cleaning between them. The test programs are built next to the plugin, where CMake also stages `pluginst.inf` and `TCResticBrowser.ini`; they locate those files through their own executable path and run with the repository root as the current directory (for `tmp\restic.exe`). Under ASan the plugin DLL is not built and `test_plugin_dll` reports `[SKIP]`.

**Reproducible builds:** `/Brepro /experimental:deterministic` (compiler), `/BREPRO` (linker), `/pathmap:<src>=./`, no PDB in Release, pinned toolset (`EXPECTED_CL`, `CMAKE_T`, `CMAKE_SDK` in `build.cmd`, mirrored in `.vsconfig`). The ZIP is made deterministic by normalizing staged text files to CRLF, writing `sha256sums.sha256`, and setting every staged timestamp to `release-date=` from `src\pluginst.inf`. Bump `version=` and `release-date=` together.

**Release package:** contents (all in the ZIP root, otherwise Total Commander does not detect `pluginst.inf`): `pluginst.inf`, `TCResticBrowser.wfx`, `TCResticBrowser.wfx64`, `TCResticBrowser.ini`, every repo-root Markdown file (`*.md`: README, CHANGELOG, CONTRIBUTING, AGENT, CLAUDE, LICENSE), `sha256sums.sha256`. `pluginst.inf` names the 32-bit file (`file=TCResticBrowser.wfx`); 64-bit Total Commander appends `64` itself. `type=wfx`, `defaultdir=TCResticBrowser`. The DLL is named after the project like the sibling plugins (`TCNewPlus.wfx`, `TCOfficeView.wlx`); so are the CMake target and the `.def`/`.rc`/`.ico` files in `src\`.

If Total Commander still has a freshly built DLL loaded, the link fails; run `cm_UnloadPlugins` in Total Commander (or restart it) before rebuilding.

### 8.7 Test Suites (`src/tests/`)

| Program | Scope |
| --- | --- |
| `test_json.c` | The JSON reader on its own (no Win32, builds with any C compiler): rejection of malformed input (trailing commas, leading zeros, bad escapes, unescaped control characters, `\u0000`, trailing garbage, comments), scalars and number lexemes, exact 64-bit integers (`2^53 + 1`, `UINT64_MAX`, overflow refused), escape and surrogate decoding (pairs combined, lone ones -> U+FFFD), member lookup (case-sensitive, first duplicate wins), the nesting limit at exactly `JSON_MAX_DEPTH`, a 20 000-element document spanning several arena blocks, real `snapshots`/`ls` payload shapes, and pinned JSONTestSuite cases (n_/y_ edge cases not covered otherwise, 100 000 opening brackets) (154 assertions). |
| `test_config.c` | Directory normalization, INI lookup order (`%APPDATA%\GHISLER` over the plugin directory, missing `%APPDATA%`, `DefaultIniName` ignored, no INI file auto-created when missing), INI encodings (UTF-8 with and without BOM, UTF-16 LE/BE, ANSI fallback, non-ASCII and CJK aliases, targets and `ResticPath`), INI syntax (CR/LF/CRLF, case-insensitive sections, foreign sections ignored, over-long entries skipped, oversized file refused), `RootName` (default, first occurrence, invalid names, `FsGetDefRootName` truncation and ANSI fallback), `restic.exe` resolution order, **PATH-only lookup (a planted `restic.exe` in the cwd must be ignored)**, `[Repositories]` parsing edge cases, 301-alias scaling (array and INI-section buffer growth), root enumeration through `FsFindFirstW/Next/Close`, WFX path validation, missing-`restic.exe` diagnostic, password prompt cancellation, the **missing-INI scenario** (`FsInitW`/`FsSetDefaultParams`/`FsFindFirstW`/`FsGetFileW` stay inert, no password prompt or popup, and the plugin and per-user directories remain empty), and verification that the sample `TCResticBrowser.ini` staged next to the test is valid and has 0 active repositories, and the diagnostic log (`[Logging] LogPath` first occurrence, environment expansion, missing directories created, UTF-8 without BOM, file never kept open, relative/empty path or missing INI means off, URL credential redaction), and `PasswordTimeoutSeconds` parsing (default, `0`, first occurrence, overflow/negative/text keep the default) and `PasswordTimeoutResetOnUse` values (202 assertions). |
| `test_restic_cli.c` | ISO-8601 edge cases (exact epoch value, offsets, fractions, invalid calendar dates), snapshot naming, path normalization (1023-char CJK path), `restic_quote_arg` round-trip, `restic_describe_password` (leading/trailing space, tab, line break, NBSP, zero-width and control characters; inner ordinary spaces not reported; truncation), snapshot/NDJSON parsers (prefix-sharing siblings, missing/empty/file targets, symlinks, 64-bit sizes, corrupt lines), process execution with `cmd.exe` as a deterministic stand-in (stdout/stderr separation, exit codes, environment injection/stripping/inheritance, timeout, cancellation, >100 KB output, >6000-char command line, spawn failures), and a **self-contained live test** against `tmp\restic.exe` including `restic_dump_file` streaming extraction, mtime preservation, 0-byte and 100 KB files, overwrite protection, binary fidelity (`00/0A/0D/1A` bytes), existing target preserved on every failure or abort, no leftover `~rp*.tmp`, local-side failures, argument validation, plus `test_dump_with_fake_restic` (a fake `restic` batch file that deterministically covers exit codes 10/11/12/other, stderr classification, partial output then failure, empty stderr, environment injection and the inactivity timeout: a silent child is killed, a slow but steady one survives) (286 assertions). |
| `test_plugin_dll.c` | Loads the built plugin (`TCResticBrowser.wfx64` or `TCResticBrowser.wfx`, matching its own bitness) like Total Commander: exact 13-function export surface (`FsGetFile`/`FsGetFileW` and `FsExecuteFile`/`FsExecuteFileW` included; write/delete/move functions forbidden), embedded `RT_GROUP_ICON` #1 + 5 `RT_ICON` frames (`16x16`..`256x256`), embedded `RT_VERSION` (`VS_VERSION_INFO` / `VS_FIXEDFILEINFO` matching the generated `version.h`), root name, INI next to the DLL, per-user `%APPDATA%\GHISLER` INI taking over in full without a reload while `DefaultIniName` is ignored, `RootName` reported by `FsGetDefRootName` (no file auto-created when missing), two concurrent find handles, ANSI stub behavior, and **end-to-end live navigation & extraction** across Level 0 -> Level 1 -> Level 2+, `FsGetFileW` single-file and recursive folder copy (`F5`), overwrite prompt protocol (`FS_FILE_EXISTS` -> `FS_COPYFLAGS_OVERWRITE`), `FS_COPYFLAGS_MOVE` rejection, binary fidelity and mtime, failed overwrite keeps the old file, local-side `FS_FILE_WRITEERROR`, invalid remote names, NULL arguments, `ESC` at start and mid-transfer, password flows inside `FsGetFileW` (cancel, 3 wrong, retry, cache reuse), password retry, per-repository sessions (8-slot eviction), empty/cancelled password, 3-strike failure, missing repository, 250-character alias error message, `ProgressProc` (`ESC`) cancellation, 6 concurrent worker threads, paths with spaces (`restic.exe` under `Program Files`, a repository in a folder with spaces, unquoted, quoted and with a trailing backslash), and the diagnostic log of the whole run (command lines, exit code 12, prompts, error dialogs; **no password, right or wrong, in the log**), the retry prompt and log line for a password with a trailing space, the password-caching policy (an unverified password is dropped after `ESC`, a verified one survives it), `PasswordTimeoutSeconds` (reuse within the timeout, the timer wipes the password without a plugin call, the next access prompts again; with `PasswordTimeoutResetOnUse=yes` a password in use outlives the timeout and expires once left alone), and the forget entry (absent while nothing is cached, `Forget N Passwords` / `Forget 1 Password` listed first as a file, a stale name still works, wipes the cache, counts in the message, returns `FS_EXEC_SYMLINK` to `\` so the root is re-read, `FsExecuteFileW` verbs) (208 assertions). |

The live test needs only `tmp\restic.exe` (git-ignored; reported as `[SKIP]` when absent). On every run it rebuilds `tmp\test_fixture\{data,repo}` from scratch (300-file directory, Unicode and shell-metacharacter names, empty directory, 100 KB file, fixed mtime), initializes a repository whose password contains quotes, `%PATH%` and CJK text, takes two snapshots and verifies per-snapshot listings, timestamps and the error mapping (wrong password, missing repository, unknown snapshot, missing path, file used as directory, quote-injection attempt). The fixture stays on disk for inspection.

Test-quality note: the security-relevant tests were mutation-checked (reverting to `SearchPathW`, or removing the quote escaping, makes them fail).
