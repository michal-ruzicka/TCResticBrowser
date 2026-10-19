# TCResticBrowser: Restic Backup Browser for Total Commander

[![Build](https://github.com/michal-ruzicka/TCResticBrowser/actions/workflows/build.yml/badge.svg)](https://github.com/michal-ruzicka/TCResticBrowser/actions/workflows/build.yml)
[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-GitHub%20Sponsors-ea4aaa?style=flat&logo=githubsponsors&logoColor=white)](https://github.com/sponsors/michal-ruzicka)
[![Ko-fi](https://img.shields.io/badge/Tip-Ko--fi-FF5E5B?style=flat&logo=kofi&logoColor=white)](https://ko-fi.com/michal_ruzicka)
[![Revolut](https://img.shields.io/badge/Pay-Revolut-191C1F?style=flat&logo=revolut&logoColor=white)](https://revolut.me/ruzicka_michal)

> **A fork of [Restic WFX Plugin](https://github.com/goldfix/Restic_WFX_Plugin)
> by Pietro Partescano ([@goldfix](https://github.com/goldfix)).** All the
> functionality — browsing Restic repositories, snapshots and their file
> trees, extraction with `F5`, the read-only and password-handling design —
> is his work, and many thanks to him for it. This fork exists for three
> reasons:
>
> 1. **Reproducible builds.** The same source and the pinned toolchain give
>    a byte-identical release ZIP, so anyone can check that a published
>    binary was built from the published source — see
>    [Reproducible Builds](#reproducible-builds).
> 2. **Minimal external dependencies.** The only third-party code the plugin
>    carried, the cJSON library (a Git submodule), is replaced by a small
>    JSON reader of its own; nothing is left but Windows itself, `restic.exe`
>    and Total Commander — see [External Dependencies](#external-dependencies).
> 3. **The same build system and CI as my other Total Commander plugins,**
>    [TCOfficeView](https://github.com/michal-ruzicka/TCOfficeView) and
>    [TCNewPlus](https://github.com/michal-ruzicka/TCNewPlus): a
>    Windows-native `build.cmd` driving CMake and MSVC for both 32-bit and
>    64-bit Total Commander, tests in every build, and the same build on
>    GitHub Actions for every push.
>
> Changes useful upstream are offered back to the original project.

**A [Total Commander](https://www.ghisler.com/) File System plugin (WFX)
that lets you browse [Restic](https://restic.net) backup repositories and
copy files out of their snapshots, right in Total Commander's panels.**

The plugin adds `\\\Restic Backups` to Network Neighborhood. Its top level
lists the repositories configured in `restic_plugin.ini`; inside each one you
find its snapshots, and inside a snapshot the backed-up directory tree.
Everything is strictly read-only: the plugin runs the official `restic.exe`
to list and extract data and never modifies a repository.

```text
\\\Restic Backups\
├── MyLocalBackup\                              <-- Level 0: Repository aliases (from restic_plugin.ini)
│   ├── 2026-10-18_09-30-00_[a1b2c3d4]\         <-- Level 1: Snapshots (YYYY-MM-DD_HH-MM-SS_[short_id])
│   │   └── C\
│   │       └── Users\
│   │           └── alice\
│   │               └── Documents\
│   │                   └── report.pdf          <-- Level 2+: Snapshot directory tree & files
│   └── 2026-10-17_21-15-42_[e5f60718]\
└── CloudS3Backup\
```

### Key Features

- **Lazy On-Demand Directory Loading (WFX Architecture):** Unlike packer plugins (`.wcx`), which must enumerate every file in an archive upon opening, the plugin queries **only the single directory level you are currently viewing** (`restic ls --json --no-lock <snapshot> <path>`). Even repositories containing millions of files open responsively.
- **Native File & Folder Extraction (`F5`, `F3`, `Ctrl+Q`):**
  - Copy individual files or entire directory trees out of any snapshot using **`F5`** (preserving original modification timestamps and 64-bit file sizes).
  - Inspect files directly with Total Commander's **Lister (`F3`)** or **Quick View (`Ctrl+Q`)**.
  - Constant-memory streaming (`64 KB` chunks via `restic dump --no-lock`) into a temporary file with **atomic replacement** on completion: an interrupted or failed overwrite never corrupts or deletes your existing local file.
- **Strictly Read-Only by Design:** Every Restic command runs with `--no-lock`. Repository modification entry points (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`) are intentionally not exported, and `F6` (Move) is rejected.
- **Zero Passwords on Disk:** Repository passwords are requested interactively via Total Commander's native password dialog (`RequestProcW`), kept only in volatile RAM per repository alias for the active session, passed to `restic.exe` via an isolated environment block (`RESTIC_PASSWORD`), and wiped with `SecureZeroMemory`.
- **Responsive Cancellation (`ESC`):** Long operations on remote backends can be aborted with `ESC` and use a 30-minute **inactivity** timeout (so slow multi-gigabyte transfers are never cut off as long as bytes keep flowing).

**Project page:** <https://github.com/michal-ruzicka/TCResticBrowser> — source
code, releases and issue tracker.

**Binary releases:** <https://github.com/michal-ruzicka/TCResticBrowser/releases>

**Support:** If you find this plugin useful, consider supporting its development.

- <https://github.com/sponsors/michal-ruzicka> — GitHub Sponsors (GitHub account needed).

  [![GitHub Sponsors](https://img.shields.io/github/sponsors/michal-ruzicka)](https://github.com/sponsors/michal-ruzicka)

- <https://ko-fi.com/michal_ruzicka> — Buy me a Coffee with no specific account needed, card payment is possible.

  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/michal_ruzicka)

- <https://revolut.me/ruzicka_michal> — Donate me via Revolut, debet/credit card or Apple Pay.

  [![Revolut.Me](Revolut.Me.png)](https://revolut.me/ruzicka_michal)

---

## Installation

The plugin needs `restic.exe` (see [Getting Restic](#getting-restic) below).
The recommended way to install the plugin itself is the **Total Commander
auto-installer:**

1. **Download** `TCResticBrowser.vX.Y.Z.zip` from the [**latest release**](https://github.com/michal-ruzicka/TCResticBrowser/releases).
2. In Total Commander, **navigate to the ZIP** file and press **Enter**.
3. TC detects the bundled `pluginst.inf` and asks whether to install the
   plugin — **confirm** (default folder: `plugins\wfx\TCResticBrowser`).
4. Copy `restic_plugin.ini` from the plugin folder to
   `%APPDATA%\GHISLER\restic_plugin.ini` and add your repositories to the
   copy (see [Configuration](#configuration)).
5. Open **Network Neighborhood** (`\\\` in the command line, or the `\`
   drive button → Network) and enter **Restic Backups**.

If you prefer to install manually, unzip the archive into a persistent
folder (for example `C:\Tools\TCResticBrowser\`) and add `TCResticBrowser.wfx`
(32-bit TC) or `TCResticBrowser.wfx64` (64-bit TC) under **Configuration →
Options → Plugins → File system plugins → Configure → Add**.

> Upgrading replaces the files in the plugin folder, including the sample
> `restic_plugin.ini` there; your own copy in `%APPDATA%\GHISLER` is not
> touched.

### Getting Restic

This plugin uses the official **Restic** command-line binary (`restic.exe`) under the hood to communicate with local, network, and cloud repositories.

**[Restic](https://restic.net)** is a fast, secure, cross-platform backup program written in Go that uses authenticated encryption (AES-256 + Poly1305-AES) and content-defined deduplication.

- **Official Website:** [https://restic.net](https://restic.net)
- **Official Documentation:** [https://restic.readthedocs.io/en/latest/](https://restic.readthedocs.io/en/latest/)
- **Download `restic.exe` for Windows (GitHub Releases):** [https://github.com/restic/restic/releases](https://github.com/restic/restic/releases)  
  *(Download `restic_<version>_windows_amd64.zip` — or `windows_386` on 32-bit Windows — extract the binary, and rename it to **`restic.exe`**)*
- **Install via WinGet / Scoop / Chocolatey:**
  ```powershell
  winget install restic.restic
  # or: scoop install restic
  # or: choco install restic
  ```

### Verifying Releases

Each release ZIP is accompanied by a detached GPG signature file
(`TCResticBrowser.vX.Y.Z.zip.asc`). Before installing, verify that the
archive has not been tampered with:

```
gpg --keyserver keys.openpgp.org --recv-keys 489C5EC80FD62BE89E59B4F719C13E8CE0F5DB61
gpg --verify TCResticBrowser.vX.Y.Z.zip.asc TCResticBrowser.vX.Y.Z.zip
```

GPG should report `Good signature from "Michal Růžička <ruzicka.mich@gmail.com>"`.
The full fingerprint of the signing key is `489C 5EC8 0FD6 2BE8 9E59  B4F7 19C1 3E8C E0F5 DB61`.

#### Reproducible Builds

Every release is built from a pinned, identical toolchain, so the same
source code always produces byte-for-byte identical binaries. This means
you are not just trusting the GPG signature — you can independently confirm
that the published binary matches the published source code by building it
yourself and comparing the SHA-256 of the ZIP against the released ZIP archive
or the [CI build log](https://github.com/michal-ruzicka/TCResticBrowser/actions)
for the release commit.

Full details — how the build achieves this, and how to run a verification
build yourself — are in
[Reproducible Builds](CONTRIBUTING.md#reproducible-builds) in the developer
documentation.

## Usage

| Action | How It Works |
| --- | --- |
| **List Repositories** | Open **Network Neighborhood** (`\`) -> **`Restic Backups`**. Changes to `restic_plugin.ini` take effect immediately on panel refresh (`Ctrl+R` / `F2`). |
| **Browse Snapshots** | Enter a repository folder (`\<Alias>`). Prompted once for the repository password; kept in RAM for that alias across both panels until Total Commander unloads the plugin (or a wrong password is entered). Up to 3 password attempts are allowed. |
| **Explore Files** | Enter any snapshot folder (`YYYY-MM-DD_HH-MM-SS_[short_id]`) and navigate the backed-up directory tree. |
| **View File (`F3` / `Ctrl+Q`)** | Press **`F3`** (Lister) or **`Ctrl+Q`** (Quick View) on any file to stream it to a temporary location and view its contents. |
| **Restore / Extract (`F5`)** | Select one or more files or directories and press **`F5`** to copy them to the target panel, preserving original `LastWriteTime` timestamps. Standard Total Commander overwrite prompts are supported. |
| **Cancel Operation (`ESC`)** | Press **`ESC`** in Total Commander's progress dialog to terminate the underlying `restic.exe` child process and clean up any temporary file. |

### Configuration

A fully commented sample configuration file
([`src/restic_plugin.ini`](src/restic_plugin.ini)) ships in the release ZIP.

#### Where to Place `restic_plugin.ini`

Settings live in `restic_plugin.ini`. The plugin reads it from two locations
and uses the **first one that exists** — values are not merged across
files:

1. `%APPDATA%\GHISLER\restic_plugin.ini` — per-user override
2. `<plugin install dir>\restic_plugin.ini` — system-wide default, shipped
   with the plugin

The shipped INI has every option commented out, so no repositories are
listed until you add some. To configure the plugin for your user account,
copy that file to `%APPDATA%\GHISLER\restic_plugin.ini` and edit the copy;
the system-wide file is then ignored entirely. The chosen file is re-read on
every root listing and repository operation — edits take effect on the next
refresh (**Ctrl+R**). The plugin never creates or modifies either file.

The INI is read as **UTF-8** (a UTF-8 or UTF-16 byte-order mark is also
honoured), so aliases and paths with accented or other non-ASCII characters
work — just save the file as UTF-8 (the default in most editors). A file
that is not valid UTF-8 is read in the Windows ANSI code page, so an INI
saved for version 1.x keeps working.

**Portable Total Commander:** keep your configuration in the
`restic_plugin.ini` in the plugin folder, which travels with Total
Commander. Back it up before installing a new version of the plugin, as the
installation replaces it with the sample. A `%APPDATA%\GHISLER\restic_plugin.ini`
on the computer you run it on would take precedence.

#### How `restic.exe` Is Located

When executing a command, the plugin searches for `restic.exe` in this order:
1. The path configured in `ResticPath=` under `[Settings]` (absolute path, or relative to the plugin directory).
2. `<plugin_dir>\restic.exe` (next to the plugin DLL).
3. The system `PATH` environment variable *(for security against working-directory hijacking, the process current directory is never searched)*.

#### Example `restic_plugin.ini`

```ini
; ==============================================================================
; Restic File System Plugin (WFX) for Total Commander - Configuration File
; ==============================================================================

[Settings]
; Optional path to restic.exe.
; If omitted or commented out, the plugin checks <plugin_dir>\restic.exe first
; and then searches the system PATH.
ResticPath=C:\Tools\restic\restic.exe

[Repositories]
; Format: <Alias>=<Repository Path or URL>
; Each <Alias> is displayed as a top-level folder inside "\\\Restic Backups\".

; 1. Local disk repository
LocalDocs=D:\Backups\restic_docs

; 2. Network share (SMB / UNC path)
NasBackup=\\NAS\Backups\workstation_repo

; 3. Restic REST Server (HTTP / HTTPS)
HomeRestServer=rest:http://192.168.1.50:8000/workstation
RemoteRestTLS=rest:https://backup.example.com:8000/prod

; 4. SFTP repository (uses your system OpenSSH configuration / ssh-agent)
LinuxServerSftp=sftp:backupuser@192.168.1.20:/srv/restic/repo

; 5. Amazon S3 / S3-compatible storage (MinIO, Wasabi, Backblaze B2, Cloudflare R2)
;    Backend environment variables (e.g. AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY)
;    are inherited from Total Commander's process environment.
AwsS3=s3:s3.amazonaws.com/my-backup-bucket/restic
LocalMinio=s3:http://192.168.1.60:9000/restic-backups

; 6. Rclone backend (Google Drive, OneDrive, Dropbox, Box, etc.)
GoogleDrive=rclone:gdrive:Backups/restic_repo
```

## External Dependencies

The plugin depends on nothing but what it cannot do without:

- **Windows** — the plugin imports only `KERNEL32.dll` and links the C
  runtime statically, so no Visual C++ Redistributable is needed.
- **[Restic](https://restic.net)** ([github.com/restic/restic](https://github.com/restic/restic),
  BSD 2-Clause License) — run as an external `restic.exe` process, never
  linked in.
- **Total Commander** — the plugin implements the
  [WFX plugin interface](https://www.ghisler.com/plugins.htm) by Christian
  Ghisler; the interface declarations are in `src/fsplugin.h`.

### Why the Plugin Has Its Own JSON Reader

`restic.exe` reports snapshots and directory listings as JSON
(`restic snapshots --json`, `restic ls --json`), so the plugin has to parse
JSON. Upstream used the [cJSON](https://github.com/DaveGamble/cJSON) library
as a Git submodule. This fork replaces it with `src/json.c`, a strict,
read-only JSON reader of about 370 lines, because no component the plugin
needs anyway offers JSON parsing that is practical to use:

- **Windows** has a JSON parser only in WinRT (`Windows.Data.Json`), which
  plain C can reach only through COM-style ABI calls — more code than the
  whole reader.
- **Restic** offers machine-readable output only as JSON; its human-readable
  output is not a stable interface, and file names containing line breaks
  cannot be separated reliably from it.
- **Total Commander** has nothing of the kind.

A library would not be the smaller risk either. The JSON parsed here
describes repository content — file names chosen by whoever created the
backed-up files — and is parsed inside Total Commander's own process, so the
code that parses it should be as small as possible and readable in full.
cJSON is about eight times larger than what the plugin needs (it also builds,
modifies and prints JSON), and was pinned to an untagged development commit.
The reader does only what the plugin uses: it parses one document into an
immutable tree held in a single memory arena, with no serialisation and no
mutation. It is deliberately strict — it rejects anything outside RFC 8259,
nesting deeper than 64 levels, and strings containing `\u0000` (so every
decoded name is a plain C string that cannot be cut short) — and reads file
sizes as exact 64-bit integers instead of going through floating point.

The reader is generic: it understands any valid JSON, not just a list of
expected keywords. The plugin looks up the few fields it uses (`id`,
`short_id`, `time`, `name`, `type`, `size`, `mtime`, `atime`, `ctime`,
`path`, `message_type`, `message`), so fields and message types that a newer
Restic adds are simply ignored; a missing field skips the entry or leaves its
size or time unknown, and a line that is not valid JSON fails the listing
instead of being guessed at.

How the reader is tested:

- **Unit tests** (`src/tests/test_json.c`, run by every build on both
  bitnesses and under AddressSanitizer): malformed input, every escape,
  surrogate pairs, exact 64-bit integers and overflow, the nesting limit,
  documents spanning several memory blocks, real Restic output, and a pinned
  selection of edge cases from JSONTestSuite.
- **[JSONTestSuite](https://github.com/nst/JSONTestSuite)**, the reference
  conformance corpus for JSON parsers, run in full: all 188 inputs a parser
  must reject are rejected, and all 95 it must accept are accepted except the
  two containing `\u0000`, which the reader refuses by design.
- **Differential testing** against Python's `json` module on 80,000 generated
  and mutated documents: identical results apart from the documented
  deliberate differences.
- **Coverage-guided fuzzing** with libFuzzer, AddressSanitizer and
  UndefinedBehaviorSanitizer (`src/tests/fuzz_json.c`), seeded with
  JSONTestSuite and real Restic output: about 90 million inputs without a
  sanitizer finding or a violated invariant; code coverage stopped growing
  within the first minute.

## Contributing

Build instructions, repo layout and architecture notes for developers live
in [CONTRIBUTING.md](CONTRIBUTING.md); the detailed technical specification,
shared with the coding agents used on this project, is in
[AGENT.md](AGENT.md).

## License

This project is licensed under the MIT License, like the upstream project it
is forked from — see [LICENSE](LICENSE) for the full text. Release notes are
in [CHANGELOG.md](CHANGELOG.md).
