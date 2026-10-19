<p align="center">
  <img src="logo/logo_256x256.png" alt="Restic WFX Plugin Logo" width="180" />
</p>

<h1 align="center">Restic WFX Plugin for Total Commander</h1>

<p align="center">
  <b>Browse and extract files from <a href="https://restic.net">Restic</a> backup repositories directly inside Total Commander's dual-pane interface.</b>
</p>

<p align="center">
  <a href="https://github.com/goldfix/Restic_WFX_Plugin/blob/main/LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT" /></a>
  <img src="https://img.shields.io/badge/Language-C11-00599C.svg" alt="Language: Pure C11" />
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D6.svg" alt="Platform: Windows x64" />
  <img src="https://img.shields.io/badge/Total%20Commander-WFX64-orange.svg" alt="Total Commander WFX64" />
  <img src="https://img.shields.io/badge/Version-1.0.0-success.svg" alt="Version: 1.0.0" />
</p>

---

## Overview & Purpose

**Restic WFX** (`restic.wfx64`) is a native 64-bit **File System Plugin (WFX)** for [Total Commander](https://www.ghisler.com/) written in pure **C11** (Win32 API, zero external DLL dependencies beyond `KERNEL32.dll`).

It turns Total Commander's **Network Neighborhood** (`\\\Restic Backups\`) into a fast, strictly **read-only** browser and extractor for your [Restic](https://restic.net) backup repositories:

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

- **Lazy On-Demand Directory Loading (WFX Architecture):** Unlike packer plugins (`.wcx`), which must enumerate every file in an archive upon opening, Restic WFX queries **only the single directory level you are currently viewing** (`restic ls --json --no-lock <snapshot> <path>`). Even repositories containing millions of files open responsively.
- **Native File & Folder Extraction (`F5`, `F3`, `Ctrl+Q`):**
  - Copy individual files or entire directory trees out of any snapshot using **`F5`** (preserving original modification timestamps and 64-bit file sizes).
  - Inspect files directly with Total Commander's **Lister (`F3`)** or **Quick View (`Ctrl+Q`)**.
  - Constant-memory streaming (`64 KB` chunks via `restic dump --no-lock`) into a temporary file with **atomic replacement** on completion: an interrupted or failed overwrite never corrupts or deletes your existing local file.
- **Strictly Read-Only by Design:** Every Restic command runs with `--no-lock`. Repository modification entry points (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`) are intentionally not exported, and `F6` (Move) is rejected.
- **Zero Passwords on Disk:** Repository passwords are requested interactively via Total Commander's native password dialog (`RequestProcW`), kept only in volatile RAM per repository alias for the active session, passed to `restic.exe` via an isolated environment block (`RESTIC_PASSWORD`), and wiped with `SecureZeroMemory`.
- **Responsive Cancellation (`ESC`):** Long operations on remote backends can be aborted with `ESC` and use a 30-minute **inactivity** timeout (so slow multi-gigabyte transfers are never cut off as long as bytes keep flowing).

---

## About Restic & Where to Get It

This plugin uses the official **Restic** command-line binary (`restic.exe`) under the hood to communicate with local, network, and cloud repositories.

**[Restic](https://restic.net)** is a fast, secure, cross-platform backup program written in Go that uses authenticated encryption (AES-256 + Poly1305-AES) and content-defined deduplication.

- **Official Website:** [https://restic.net](https://restic.net)
- **Official Documentation:** [https://restic.readthedocs.io/en/latest/](https://restic.readthedocs.io/en/latest/)
- **Download `restic.exe` for Windows (GitHub Releases):** [https://github.com/restic/restic/releases](https://github.com/restic/restic/releases)  
  *(Download `restic_<version>_windows_amd64.zip`, extract the binary, and rename it to **`restic.exe`**)*
- **Install via WinGet / Scoop / Chocolatey:**
  ```powershell
  winget install restic.restic
  # or: scoop install restic
  # or: choco install restic
  ```

---

## Installation in Total Commander

### Option A: Automatic Installation (Recommended)

1. Download `wfx_restic_<version>_x64.zip` from the [GitHub Releases](https://github.com/goldfix/Restic_WFX_Plugin/releases) page.
2. In Total Commander, navigate to the ZIP file and **double-click it** (or press `Enter`).
3. Total Commander reads the bundled `pluginst.inf`, asks where to install the plugin (default: `plugins\wfx\restic`) and registers it automatically as **`Restic Backups`** in Network Neighborhood.
4. Edit `restic_plugin.ini` (extracted next to `restic.wfx64`) and add your repositories (see below).
5. Open **Network Neighborhood** (`Alt+F1` / `Alt+F2` -> `\`) and enter **`Restic Backups`**.

The ZIP contains: `pluginst.inf`, `restic.wfx64`, `restic_plugin.ini` (commented sample), `README.md` and `LICENSE`.

> If a `restic_plugin.ini` already exists in the target folder (upgrade), make sure to keep your own copy of it: Total Commander may ask before overwriting files.

### Option B: Manual Installation

1. Extract `restic.wfx64` and `restic_plugin.ini` (from the ZIP, or from `build\` after building from source) and copy them (optionally together with `restic.exe`) into a dedicated plugin folder, for example:
   ```text
   C:\TotalCmd\plugins\wfx\restic\restic.wfx64
   ```
2. In Total Commander, open **Configuration** -> **Options...** -> **Plugins** -> **File system plugins (.WFX)** -> click **Configure**.
3. Click **Add**, select `restic.wfx64`, and confirm with **OK**.
4. Edit `restic_plugin.ini` (see below) and add your repositories.
5. Open **Network Neighborhood** (`Alt+F1` / `Alt+F2` -> `\`) and enter **`Restic Backups`**.

---

## Configuration (`restic_plugin.ini`)

A fully commented sample configuration file is included in the repository root ([`restic_plugin.ini`](restic_plugin.ini)) and is automatically copied to `build\restic_plugin.ini` during every build.

### Where to Place `restic_plugin.ini`

The plugin never auto-creates or modifies `restic_plugin.ini` at runtime. On every root listing or repository operation, it searches for `restic_plugin.ini` in the following order:

1. **Primary Location (Portable / Next to the Plugin DLL):**
   ```text
   <plugin_dir>\restic_plugin.ini
   ```
   *(e.g., `C:\TotalCmd\plugins\wfx\restic\restic_plugin.ini` — recommended for portable Total Commander installations)*
2. **Fallback Location (Total Commander User Configuration Directory):**
   If no INI file is present next to `restic.wfx64` (for example, when Total Commander is installed under a read-only `C:\Program Files\` directory), the plugin looks in the directory reported by Total Commander via `FsSetDefaultParams` (the folder containing `wincmd.ini` / `fsplugin.ini`, typically):
   ```text
   %APPDATA%\GHISLER\restic_plugin.ini
   ```

> **Encoding Note:** Save `restic_plugin.ini` as **ANSI** (system code page) or **UTF-16 LE (with BOM)**. Standard Win32 INI APIs (`GetPrivateProfile*W`) do not decode UTF-8 when the file contains non-ASCII characters.

### How `restic.exe` Is Located

When executing a command, the plugin searches for `restic.exe` in this order:
1. The path configured in `ResticPath=` under `[Settings]` (absolute path, or relative to the directory containing `restic_plugin.ini`).
2. `<plugin_dir>\restic.exe` (next to `restic.wfx64`).
3. The system `PATH` environment variable *(for security against working-directory hijacking, the process current directory is never searched)*.

### Example `restic_plugin.ini`

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

---

## Usage in Total Commander

| Action | How It Works |
| --- | --- |
| **List Repositories** | Open **Network Neighborhood** (`\`) -> **`Restic Backups`**. Changes to `restic_plugin.ini` take effect immediately on panel refresh (`Ctrl+R` / `F2`). |
| **Browse Snapshots** | Enter a repository folder (`\<Alias>`). Prompted once for the repository password; kept in RAM for that alias across both panels until Total Commander unloads the plugin (or a wrong password is entered). Up to 3 password attempts are allowed. |
| **Explore Files** | Enter any snapshot folder (`YYYY-MM-DD_HH-MM-SS_[short_id]`) and navigate the backed-up directory tree. |
| **View File (`F3` / `Ctrl+Q`)** | Press **`F3`** (Lister) or **`Ctrl+Q`** (Quick View) on any file to stream it to a temporary location and view its contents. |
| **Restore / Extract (`F5`)** | Select one or more files or directories and press **`F5`** to copy them to the target panel, preserving original `LastWriteTime` timestamps. Standard Total Commander overwrite prompts are supported. |
| **Cancel Operation (`ESC`)** | Press **`ESC`** in Total Commander's progress dialog to terminate the underlying `restic.exe` child process and clean up any temporary file. |

---

## Building & Testing from Source

### Prerequisites

- **Windows 10/11 x64**
- **MSVC Build Tools (Visual Studio 2022+)** with x64 `cl.exe`, `link.exe`, `rc.exe`, and `nmake.exe` active in your shell environment.
- **Git**
- *(Optional)* `tmp\restic.exe` placed in the `tmp\` folder to enable the live end-to-end integration tests (otherwise live tests are gracefully reported as `[SKIP]`).

### Build Commands

Using the Git Bash / MSYS2 wrapper (`build.sh`) or directly via `nmake`:

```bash
./build.sh release       # Build optimized Release DLL (build/restic.wfx64, static CRT /MT) + copy build/restic_plugin.ini
./build.sh debug         # Build Debug DLL (build/restic.wfx64, /MTd + CRT debug heap)
./build.sh test          # Build & run all 4 test suites in Debug mode (with CRT memory-leak verification)
./build.sh release-test  # Build & run all 4 test suites against the Release build
./build.sh analyze       # Run MSVC Static Code Analyzer (/analyze) with warnings-as-errors (/W4 /WX)
./build.sh asan          # Run unit tests under MSVC AddressSanitizer (/fsanitize=address)
./build.sh package       # Clean, build Release, run tests and create build/wfx_restic_<version>_x64.zip
./build.sh clean         # Clean build/ directory (run before switching between Debug/Release/ASan)
```

Or with `nmake` from an x64 Native Tools Command Prompt:

```bat
nmake /nologo CONFIG=Release all
nmake /nologo CONFIG=Debug test
```

Build artifacts are generated in `build\`:
- `build\restic.wfx64` — 64-bit Total Commander WFX plugin DLL (with embedded multi-resolution icon and `VERSIONINFO` metadata)
- `build\restic_plugin.ini` — Commented sample configuration file ready for deployment
- `build\pluginst.inf` — Total Commander auto-install manifest (`[plugininstall]`)
- `build\wfx_restic_<version>_x64.zip` — Release package (only with `./build.sh package`), installable by double-clicking it in Total Commander. The version is read from `include\version.h`.

---

## External Libraries & Acknowledgments

- **[Total Commander WFX Plugin SDK](https://www.ghisler.com/plugins.htm)** by Christian Ghisler — File System Plugin interface specification (`include/fsplugin.h`).
- **[Restic](https://restic.net)** ([github.com/restic/restic](https://github.com/restic/restic)) — Invoked as an external CLI process (`restic.exe`, **BSD 2-Clause License**).

---

## Project Links & License

- **GitHub Repository:** [https://github.com/goldfix/Restic_WFX_Plugin](https://github.com/goldfix/Restic_WFX_Plugin)
- **Issue Tracker:** [https://github.com/goldfix/Restic_WFX_Plugin/issues](https://github.com/goldfix/Restic_WFX_Plugin/issues)
- **Architecture & Developer Specification:** [`AGENT.md`](AGENT.md)
- **License:** Licensed under the **[MIT License](LICENSE)** — Copyright (c) 2026 Pietro Partescano.
