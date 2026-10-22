# TCResticBrowser: Restic Backup Browser for Total Commander

[![Build](https://github.com/michal-ruzicka/TCResticBrowser/actions/workflows/build.yml/badge.svg)](https://github.com/michal-ruzicka/TCResticBrowser/actions/workflows/build.yml)
[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-GitHub%20Sponsors-ea4aaa?style=flat&logo=githubsponsors&logoColor=white)](https://github.com/sponsors/michal-ruzicka)
[![Ko-fi](https://img.shields.io/badge/Tip-Ko--fi-FF5E5B?style=flat&logo=kofi&logoColor=white)](https://ko-fi.com/michal_ruzicka)
[![Revolut](https://img.shields.io/badge/Pay-Revolut-191C1F?style=flat&logo=revolut&logoColor=white)](https://revolut.me/ruzicka_michal)

> **A fork of [Restic WFX Plugin](https://github.com/goldfix/Restic_WFX_Plugin)
> by Pietro Partescano ([@goldfix](https://github.com/goldfix)).** All the
> basic functionality — browsing Restic repositories, snapshots and their file
> trees, extraction with `F5`, the read-only and password-handling design —
> is his work, and many thanks to him for it, above all for the idea of the
> plugin itself.
>
> This fork started mainly to have **reproducible builds**: the same source
> and the pinned toolchain give a byte-identical release ZIP, so anyone can
> check that a published binary was built from the published source — see
> [Reproducible Builds](#reproducible-builds).
>
> Since then, **many more changes and improvements** were proposed and implemented.
> The **full list**, including upgrade notes for original 1.*x* users, is **in the
> [CHANGELOG.md](CHANGELOG.md)**.

**A [Total Commander](https://www.ghisler.com/) File System plugin (WFX)
that lets you browse [Restic](https://restic.net) backup repositories and
copy files out of their snapshots, right in Total Commander's panels.**

![TCResticBrowser Logo – a floppy disc with time machine symbol](logo/TCResticBrowser.svg)

The plugin adds `\\\TCResticBrowser` to Network Neighborhood (the name is
[configurable](#root-folder-name)). Its top level
lists the repositories configured in `TCResticBrowser.ini`; inside each one you
find its snapshots arranged the way [`restic mount`](https://restic.readthedocs.io/en/stable/050_restore.html#restore-using-mount)
shows them - by ID, by time, by host and by tag - and inside a snapshot the
backed-up directory tree.
Everything is strictly read-only: the plugin runs the official `restic.exe`
to list and extract data and never modifies a repository.

```text
\\\TCResticBrowser\
├── MyLocalBackup\                          <-- Repository aliases (from TCResticBrowser.ini)
│   ├── hosts\
│   │   └── laptop\
│   │       ├── 2026-10-17_21-15-42\
│   │       ├── 2026-10-18_09-30-00\
│   │       ├── latest\
│   │       ├── latest → 2026-10-18_09-30-00\
│   │       └── latest → a1b2c3d4\
│   ├── ids\
│   │   ├── a1b2c3d4\
│   │   └── e5f60718\
│   ├── snapshots\
│   │   ├── 2026-10-17_21-15-42\
│   │   ├── 2026-10-18_09-30-00\            <-- A snapshot (named by its time)
│   │   │   └── C\
│   │   │       └── Users\
│   │   │           └── alice\
│   │   │               └── report.pdf      <-- The backed-up files
│   │   ├── latest\                         <-- The newest snapshot
│   │   ├── latest → 2026-10-18_09-30-00\   <-- The same, saying which folder it is
│   │   └── latest → a1b2c3d4\              <-- The same, saying which snapshot ID
│   ├── tags\
│   │   └── daily\
│   │       ├── 2026-10-18_09-30-00\
│   │       ├── latest\
│   │       ├── latest → 2026-10-18_09-30-00\
│   │       └── latest → a1b2c3d4\
│   ├── snapshots.csv                       <-- All snapshots, for a spreadsheet
│   └── repository_id.txt                   <-- The repository's ID
└── CloudS3Backup\
```

The folders above the snapshots are the ones `restic mount` creates, with the
same names and the same rules, so a path you know from a mounted repository
works here too:

- **Snapshot folder names** are the time the snapshot was taken, in the time
  zone of the computer that took it. `restic mount` writes
  `2026-10-18T09:30:00+02:00` by default, but Windows does not allow colons
  in file names, so the plugin uses the colon-free form restic itself
  suggests (`--time-template "2006-01-02_15-04-05"`), `2026-10-18_09-30-00`;
  you can choose another one, see [Snapshot Folder Names](#snapshot-folder-names).
  Two snapshots with the same name get `-1`, `-2`, ... appended, the older
  one keeping the plain name, as in `restic mount`.
- **`latest`** is the newest snapshot of its folder, exactly as in
  `restic mount`: in `snapshots` the newest of the whole repository, whatever
  host took it; in `hosts\<host>` and `tags\<tag>` the newest of that host
  or tag; `ids` has none. `restic mount` makes it a link to the snapshot's
  folder, whose target `ls -l` shows; Total Commander cannot show where a
  link points, so next to `latest` (which keeps its name, for scripts and
  hotlist entries) the plugin lists the same snapshot twice more, as
  `latest → <folder>` - the link target, e.g.
  `latest → 2026-10-18_09-30-00` - and as `latest → <short ID>`.
- **Host names and tags** that contain characters Windows does not allow in
  file names (`\ / : * ? " < > |`, or a dot or space at the end) show them
  as `_`, as `restic mount` does with `/` in tags.

Paths saved by version 2.0 (`2026-10-18_09-30-00_[a1b2c3d4]` directly in
the repository) no longer exist; Total Commander's history and hotlist
entries pointing there need to be recreated.

### Key Features

- **Fast Browsing of Large and Remote Repositories:** Every `restic` run costs about a second before it prints anything (deriving the key from the password and loading the repository index), more for big or remote (e.g. S3) repositories. The plugin therefore reads each repository's snapshot list once and answers from it at once, re-reading it in the background once it is a minute old (as often as `restic mount` does), and when you enter a snapshot it reads the list of *all* its files and folders in the background with a single `restic ls`. Until that is done, each folder is listed on its own (`restic ls <snapshot> <path>`), as fast as before; afterwards every folder of the snapshot opens instantly and `restic` runs only to copy or view files. See [What Is Instant and What Takes a While](#what-is-instant-and-what-takes-a-while) and [Snapshot Cache](#snapshot-cache).
- **Native File & Folder Extraction (`F5`, `F3`, `Ctrl+Q`):**
  - Copy individual files or entire directory trees out of any snapshot using **`F5`** (preserving original modification timestamps and 64-bit file sizes).
  - Inspect files directly with Total Commander's **Lister (`F3`)** or **Quick View (`Ctrl+Q`)**.
  - Constant-memory streaming (`64 KB` chunks via `restic dump --no-lock`) into a temporary file with **atomic replacement** on completion: an interrupted or failed overwrite never corrupts or deletes your existing local file.
- **Strictly Read-Only by Design:** Every Restic command runs with `--no-lock`. Repository modification entry points (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`) are intentionally not exported, and `F6` (Move) is rejected.
- **Zero Passwords on Disk:** Repository passwords are requested interactively via Total Commander's native password dialog (`RequestProcW`), kept only in RAM per repository alias for 30 minutes by default (see [Password Timeout](#password-timeout)), passed to `restic.exe` via an isolated environment block (`RESTIC_PASSWORD`), and wiped with `SecureZeroMemory`. Cached passwords are encrypted in memory (`CryptProtectMemory`), in pages locked out of the page file and excluded from Windows Error Reporting crash dumps, so a memory dump or the hibernation file does not reveal them.
- **Responsive Cancellation (`ESC`):** Long operations on remote backends can be aborted with `ESC`. Extraction uses a 30-minute **inactivity** timeout (so slow multi-gigabyte transfers are never cut off as long as bytes keep flowing); a folder listing is stopped after 30 minutes in total.

**Project page:** <https://github.com/michal-ruzicka/TCResticBrowser> — source
code, releases and issue tracker.

**Binary releases:** <https://github.com/michal-ruzicka/TCResticBrowser/releases>

**Support:** If you find this plugin useful, consider supporting its development.

- <https://github.com/sponsors/michal-ruzicka> — GitHub Sponsors (GitHub account needed).

  [![GitHub Sponsors](https://img.shields.io/github/sponsors/michal-ruzicka)](https://github.com/sponsors/michal-ruzicka)

- <https://ko-fi.com/michal_ruzicka> — Buy me a coffee with no specific account needed, card payment is possible.

  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/michal_ruzicka)

- <https://revolut.me/ruzicka_michal> — Donate via Revolut, debit/credit card or Apple Pay.

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
4. Copy `TCResticBrowser.ini` from the plugin folder to
   `%APPDATA%\GHISLER\TCResticBrowser.ini` and add your repositories to the
   copy (see [Configuration](#configuration)). For a different folder name
   than *TCResticBrowser*, see [Root Folder Name](#root-folder-name): it is
   read only while the plugin is being installed.
5. Open **Network Neighborhood** (`\\\` in the command line, or the `\`
   drive button → Network) and enter **TCResticBrowser**.

If you prefer to install manually, unzip the archive into a persistent
folder (for example `C:\Tools\TCResticBrowser\`) and add `TCResticBrowser.wfx`
(32-bit TC) or `TCResticBrowser.wfx64` (64-bit TC) under **Configuration →
Options → Plugins → File system plugins → Configure → Add**.

> Upgrading replaces the files in the plugin folder, including the sample
> `TCResticBrowser.ini` there; your own copy in `%APPDATA%\GHISLER` is not
> touched.

### Getting Restic

This plugin uses the official **Restic** command-line binary (`restic.exe`) under the hood to communicate with local, network, and cloud repositories. It needs **restic 0.17.1 or newer**: older versions report a wrong password and a missing repository with the same exit code and print `ls` output the plugin does not read.

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
| **List Repositories** | Open **Network Neighborhood** (`\\\`) -> **`TCResticBrowser`**. Changes to `TCResticBrowser.ini` take effect immediately on panel refresh (`Ctrl+R` / `F2`). |
| **Browse Snapshots** | Enter a repository folder (`\<Alias>`). Prompted once for the repository password; once restic accepts it, it is kept in RAM (encrypted) for that alias across both panels for 30 minutes (see [Password Timeout](#password-timeout)). Up to 3 password attempts are allowed. The dialog's title starts with the repository alias (`MyLocalBackup - Restic repository password`), so a password manager that picks an entry by the title of the active window, such as KeePassXC Auto-Type, can offer the right password. |
| **Connection Log** | Total Commander's FTP connections toolbar (unless `ConnectionLog=no`), whose log window shows what the plugin is doing in the background - reading a whole snapshot (with progress every few seconds) or re-reading the snapshot list - and whose **Disconnect** forgets all passwords and everything read with them. See [Connection Log](#connection-log). |
| **Snapshot List (`snapshots.csv`)** | Every repository folder also holds the file **`snapshots.csv`**: all snapshots with host, paths, size, time, tags and full ID, for Excel or another spreadsheet (`F3` to view, `F5` to copy, or open it). See [Snapshot List as a Spreadsheet](#snapshot-list-as-a-spreadsheet). Next to it, **`repository_id.txt`** holds the repository's ID (see [Repository ID](#repository-id)). |
| **Forget Passwords** | While passwords are remembered, the plugin's root folder starts with **`🗑 Forget N Passwords`** (N is their number; `🗑 Forget 1 Password` for one). Press **Enter** on it to wipe them all at once; a message says how many were forgotten, the root folder is then re-read without the entry, and the next access asks again. The entry is not removed by itself when the passwords expire; it disappears the next time the root folder is read (`Ctrl+R`). |
| **Explore Files** | Enter a snapshot through `snapshots`, `ids`, `hosts` or `tags` (or `latest`) and navigate the backed-up directory tree. The snapshot list is read once and then shown at once from memory; when it is older than a minute (`SnapshotListRefreshSeconds=`), as `restic mount` would re-read it, restic reads a new one in the background and the next listing (`Ctrl+R`) shows any new snapshots (see [Snapshot List Refresh](#snapshot-list-refresh)). |
| **View File (`F3` / `Ctrl+Q`)** | Press **`F3`** (Lister) or **`Ctrl+Q`** (Quick View) on any file to stream it to a temporary location and view its contents. |
| **Restore / Extract (`F5`)** | Select one or more files or directories and press **`F5`** to copy them to the target panel, preserving original `LastWriteTime` timestamps. Standard Total Commander overwrite prompts are supported. |
| **Cancel Operation (`ESC`)** | Press **`ESC`** in Total Commander's progress dialog to terminate the underlying `restic.exe` child process and clean up any temporary file. |

### What Is Instant and What Takes a While

Every run of `restic` costs about a second before it shows anything (it
derives the key from the password and loads the repository index), and
much longer for a big or remote repository. The plugin runs it as rarely as
it can, so the same folder can take a moment once and open instantly later:

| You open | What happens |
| --- | --- |
| A repository for the first time (or after its password was forgotten) | Waits for `restic snapshots`. |
| `snapshots`, `hosts`, `tags`, `ids` and the folders below them | Instant, from the snapshot list read before. When that list is older than a minute (`SnapshotListRefreshSeconds=`), a new one is read in the background; `Ctrl+R` a moment later shows snapshots taken meanwhile. |
| The first folders inside a snapshot | Each one waits for its own `restic ls`, as long as for the repository. Entering the snapshot also starts reading the list of *all* its files in the background. |
| Any folder of that snapshot once the background read is done | Instant, from memory. The read takes from seconds to minutes depending on the number of files, and runs while you browse. |
| A snapshot with more files than `SnapshotCacheMB=` holds | Every folder waits for its own `restic ls`; it is never read as a whole. |
| A file you view (`F3`, `Ctrl+Q`) or copy (`F5`) | Always runs `restic dump`, about a second per file plus the transfer. |

After the password timeout (30 minutes by default) or `🗑 Forget N
Passwords`, everything read from that repository is dropped, so the next
visit starts from the top of the table again. Total Commander shows its
progress dialog, with `ESC` to cancel, only for operations that take more
than a few seconds. The [connection log](#connection-log) shows the
background reads as they happen, and the [diagnostic log](#logging) records every restic
run with its duration.

### Connection Log

Unless `ConnectionLog=no` is set under `[Settings]` (also `false`, `off`
or `0`; anything else, or no setting, means yes) the plugin announces itself as a
connection when a repository is first used, as an FTP plugin announces a
server, and Total Commander shows its FTP connections toolbar with two
buttons that matter here:

- **The log window** shows what the plugin does and what takes time:
  passwords accepted, refused and forgotten (with the reason: timeout,
  making room for another repository's), snapshot lists and folders read
  by restic (with how long it took), files copied, and the work in the
  background:

  ```text
  [MyLocalBackup] Password accepted
  [MyLocalBackup] Snapshot list read: 42 snapshots, 1.3 s
  [MyLocalBackup] \snapshots\latest\C\Users: 12 entries listed by restic in 1.1 s
  [MyLocalBackup] Reading every file and folder of snapshot a1b2c3d4 in the background
  [MyLocalBackup] Snapshot a1b2c3d4: 120000 files and folders read so far (25 s)
  [MyLocalBackup] Snapshot a1b2c3d4 read: 209750 files and folders, 18 MiB, 48 s; its folders now open without restic
  [MyLocalBackup] Snapshot list re-read in the background: 42 snapshots, 1 s
  ```

  There is no percentage: restic does not tell in advance how many files a
  snapshot holds.
- **Disconnect** forgets all remembered passwords and everything read with
  them, like `🗑 Forget N Passwords`, and closes the toolbar; Total
  Commander then switches the panel back to where it was before, as it does
  for FTP.

There is one connection for all repositories, so one Disconnect is always
enough. Only Disconnect closes it: `🗑 Forget N Passwords` or a password
timeout forget the passwords but cannot close Total Commander's toolbar, so
it stays (the log says so) until you press Disconnect; the next use of a
repository reuses it. If the toolbar is in the way, `ConnectionLog=no`
turns it off.

The toolbar is Total Commander's own FTP toolbar, so it says *FTP* and has
a transfer mode list; the mode has no effect here: files always come out
of the snapshot byte for byte, whatever it says, and the plugin changes no
setting of Total Commander.

### Snapshot List as a Spreadsheet

Next to `hosts`, `ids`, `snapshots` and `tags`, every repository folder
holds the file `snapshots.csv`, made by the plugin from the snapshot list
it has already read (no extra restic run). It lists every snapshot, one per
line, sorted by host, then paths (both regardless of upper and lower
case), then time:

| Column | Content |
| --- | --- |
| `short ID` | The short ID, as `restic snapshots` prints it and as in `ids` |
| `host` | Host name as restic recorded it |
| `paths` | The backed-up paths, one per line within the cell when there are several |
| `size` | The size of the backed-up data, as `restic snapshots` shows it (`1.234 GiB`; `B`, `KiB`, `MiB`, `GiB` or `TiB` with three decimals); empty for a snapshot taken by restic older than 0.17, which recorded no size |
| `timestamp` | `YYYY-MM-DD hh:mm:ss`, the snapshot's own clock time (the time zone it was taken in), which spreadsheets read as a date and time |
| `tags` | The tags, one per line within the cell; empty when there are none |
| `full ID` | The full 64-character ID, which every restic command accepts and which spreadsheets never take for a number |

The file is UTF-8 with a byte-order mark, so Excel shows non-ASCII host
names and paths correctly, and follows RFC 4180: a cell that contains the
separator, a quote or a line break is quoted, and quotes in it are
doubled. Where a host name, path or tag could start a cell with a
spreadsheet formula (`=`, `+`, `-`, `@` at its start, at the start of one
of its lines, or after a `,` `;` `|` tab or quote, should the file be read
with another separator), an apostrophe is put before that character, so
opening the file never evaluates anything a backup put into its tags.

Excel separates the columns of a CSV file by the *list separator* of your
Windows regional settings (a semicolon in many European countries, a comma
in the US), so the plugin uses that separator too. `CsvSeparator=` under
`[Settings]` sets another one: `,`, `;`, `|` or `tab`.

```ini
[Settings]
CsvSeparator=;
```

Opened by a double click, Excel guesses the type of every cell: a short
ID made only of digits (`01234567`) or looking like a number with an
exponent (`123456e7`), about 3 % of them, is turned into a number, and a
host name or tag such as `1-2` into a date. Quoting does not prevent it
(Excel removes the quotes before it guesses), so the last column,
`full ID`, holds the full ID, which Excel keeps as it is. To keep every
value as it is, import the file instead (*Data* -> *From Text/CSV*) and
set those columns to *Text*.

The file's date is the moment restic was asked for the snapshot list it
is made from. The size and date Total Commander shows are those of the
list at the time of the listing; the file copied is made from the list
cached at the time of the copy and carries that list's date, so after a
background refresh it may already include newer snapshots.

### Repository ID

Every repository folder also holds `repository_id.txt`: one line with
the repository's full 64-character ID, as `restic cat config` reports it,
ended by a Windows line break (CRLF). The ID tells two repositories apart
whatever their location or alias, for example a copy of a repository
from its original, and it is what tools that track repositories
(monitoring, backup inventories) use to identify one.

The file is listed at once, since its size is always the same (66 bytes);
restic is asked for the ID only when you view or copy it, and the answer
is remembered as long as the repository's password is. The file's date
is the moment restic was asked for the ID; until then the listing shows
the date of `snapshots.csv`.

### Dates in the Panels

| Entry | Date shown |
| --- | --- |
| Repository folders, `🗑 Forget N Passwords` | None |
| `hosts`, `ids`, `snapshots`, `tags`, a host's or a tag's folder | When the newest snapshot in it was taken; none if it holds no snapshot (`tags` when no snapshot has a tag) |
| A snapshot's folder, also `latest` and `latest → …` | When the snapshot was taken |
| Files and folders in a snapshot | Their modification time as restic recorded it (none if restic has none); creation and access time from restic's `ctime` and `atime` |
| `snapshots.csv` | When restic was asked for the snapshot list it is made from |
| `repository_id.txt` | When restic was asked for the ID; until then the date of `snapshots.csv` |

Total Commander shows every date in your local time zone, while a
snapshot's folder is named by the time on the clock where the snapshot
was taken, so the two differ for a snapshot from another time zone. A
copied file (`F5`) keeps its modification time. On Linux and macOS,
restic's `ctime` is the time a file's metadata last changed, not when it
was created.

### File Names from Other Systems

Snapshots made on Linux or macOS can contain names that are legal there but
not on Windows. The plugin shows them exactly as they are in the backup and
never renames anything, but it will not copy such a file under that name:

- Names containing `:` (`12:30.log`) are refused, because Windows would write
  the alternate data stream `30.log` of a file `12` instead.
- Names containing `<`, `>`, `"`, `|`, `?`, `*` or control characters cannot
  be created on Windows at all.
- Reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`,
  `LPT1`–`LPT9`, also with an extension such as `nul.txt`) are refused, because
  Windows would write to the device.
- Names ending in a dot or a space (`notes.`) are refused, because Windows
  would silently store them as a different file (`notes`).

Total Commander then reports a write error for that file. To restore a single
such file, press `F5` on it alone and type another name in the copy dialog.
Names containing `\` or `/`, or consisting only of dots and spaces, are not
listed at all, so a snapshot can never make a copy leave the target folder.

### Configuration

A fully commented sample configuration file
([`src/TCResticBrowser.ini`](src/TCResticBrowser.ini)) ships in the release ZIP.

#### Where to Place `TCResticBrowser.ini`

Settings live in `TCResticBrowser.ini`. The plugin reads it from two locations
and uses the **first one that exists** — values are not merged across
files:

1. `%APPDATA%\GHISLER\TCResticBrowser.ini` — per-user override
2. `<plugin install dir>\TCResticBrowser.ini` — system-wide default, shipped
   with the plugin

The shipped INI has every option commented out, so no repositories are
listed until you add some. To configure the plugin for your user account,
copy that file to `%APPDATA%\GHISLER\TCResticBrowser.ini` and edit the copy;
the system-wide file is then ignored entirely. The chosen file is re-read on
every root listing and repository operation — edits take effect on the next
refresh (**Ctrl+R**). The plugin never creates or modifies either file.

The INI is read as **UTF-8** (a UTF-8 or UTF-16 byte-order mark is also
honoured), so aliases and paths with accented or other non-ASCII characters
work — just save the file as UTF-8 (the default in most editors). A file
that is not valid UTF-8 is read in the Windows ANSI code page, so an INI
saved for version 1.x keeps working.

**Portable Total Commander:** keep your configuration in the
`TCResticBrowser.ini` in the plugin folder, which travels with Total
Commander. Back it up before installing a new version of the plugin, as the
installation replaces it with the sample. A `%APPDATA%\GHISLER\TCResticBrowser.ini`
on the computer you run it on would take precedence.

#### Password Timeout

Passwords are kept in memory, encrypted, for 30 minutes from when restic
first accepted them, then wiped; the next access asks again. Using a
repository does not extend the time. `PasswordTimeoutSeconds=` under
`[Settings]` changes it, `0` keeps passwords until Total Commander exits.

With `PasswordTimeoutResetOnUse=yes` (also `true`, `on` or `1`; anything
else means no) every operation that uses a password
starts the interval again: while you work with a repository you are not
asked again, and once you leave it alone the password goes soon. A short
timeout suits this best:

```ini
[Settings]
PasswordTimeoutSeconds=300
PasswordTimeoutResetOnUse=yes
```

#### Snapshot Cache

Measured with a local repository whose snapshot holds 200,000 files, a
`restic ls` of one folder took 1.0 s (1.3 s through a REST server with a
40 ms round trip), almost all of it spent before restic lists anything: a
`restic ls` of a folder and all ten of its subfolders together took the
same. Listing the whole snapshot took 8 s and printed 87 MB of JSON. So
the plugin lists the whole snapshot once, in the background, the first
time you enter it (directly or through a folder deep inside it), and
answers every later folder of that snapshot from memory - about 100 bytes
per file, 18 MB for those 200,000 files.

`SnapshotCacheMB=` under `[Settings]` sets how much memory these lists may
take (default 512 MiB, about five million files; in 32-bit Total Commander
at most 512 MiB). One snapshot of a repository is read at a time - a
second would only share its line and storage - and up to three different
repositories side by side; the least recently used snapshots make room
while they are read, and if the ones being read need more than the limit
together, one of them is set aside and read later. A snapshot larger than
the whole limit is not cached at all - its folders are then simply listed one
by one, as before, and the logs say so - and `0` turns the cache off.
Nothing is written to disk; unlike passwords, the lists (names, sizes and
times of files) are not encrypted in memory. They are dropped together with
the repository's password (timeout, `🗑 Forget N Passwords`, Disconnect
in the connection log, a wrong password), so what was read with a password
is never shown without it.

#### Snapshot List Refresh

The list of a repository's snapshots (`restic snapshots`) is read once and
then shown at once from memory, also when it is old: when you open
`snapshots`, `hosts`, `tags` or `ids` and the list is older than
`SnapshotListRefreshSeconds=` (default 60, as often as `restic mount`
re-reads it), restic reads a new one in the background, and the next
listing (`Ctrl+R`) shows snapshots taken meanwhile. You wait for restic
only on the first visit, for a folder name the old list does not know, and
after typing a password, which restic has to check. `0` re-reads the list
on every visit, still in the background. Inside a snapshot folder,
including `latest`, you stay in the snapshot the folder showed when you
entered it.

#### Snapshot Folder Names

`SnapshotTimeFormat=` under `[Settings]` sets how the snapshot folders
under `snapshots`, `hosts\<host>` and `tags\<tag>` are named, like
`restic mount --time-template` but with simpler placeholders:

| Placeholder | Meaning | Example |
| --- | --- | --- |
| `YYYY` | Year | `2026` |
| `YY` | Year without the century | `26` |
| `MM` | Month, `01`-`12` | `10` |
| `DD` | Day of the month, `01`-`31` | `18` |
| `hh` | Hour, `00`-`23` | `09` |
| `mm` | Minute | `30` |
| `ss` | Second | `00` |
| `zzzz` | Time zone offset | `+0200`, `-0500`, `+0000` |

Placeholders are case-sensitive (`MM` is the month, `mm` the minute) and
every other character is copied as it is. The time is the snapshot's own,
in the time zone it was taken in; add `zzzz` if your backups come from
several time zones. The default is `YYYY-MM-DD_hh-mm-ss`
(`2026-10-18_09-30-00`); for example `YYYY-MM-DDThh.mm.ss` gives
`2026-10-18T09.30.00` and `YYYY-MM-DD` gives one folder per day, with
`-1`, `-2`, ... for further snapshots of that day. A format needs at least
one placeholder, at most 64 bytes, none of the characters Windows forbids
in file names (`\ / : * ? " < > |`), and must not end with a dot or space;
otherwise the default is used and the diagnostic log says so.

```ini
[Settings]
SnapshotTimeFormat=YYYY-MM-DDThh.mm.ss
```

#### Root Folder Name

`RootName=` under `[Settings]` sets the name of the plugin's folder in
Network Neighborhood; without it (or when it is empty or not a valid folder
name) the folder is called **TCResticBrowser**. Total Commander asks the
plugin for this name only while installing it and then remembers it, so a
later change takes effect only after removing the plugin under
**Configuration → Options → Plugins → File system plugins** and installing
it again. The name has to be representable in the Windows ANSI code page,
because Total Commander asks for it through an ANSI function; otherwise the
default is used.

#### How `restic.exe` Is Located

If `ResticPath=` under `[Settings]` is set, the plugin uses exactly that
binary or none at all:

- a full path (`C:\Tools\restic\restic.exe`) must point to an existing file;
- a relative path (`tools\restic.exe`) is taken from the plugin folder;
- a bare name (`restic-custom`, `.exe` is added when there is no extension) is
  looked up in the plugin folder, then in the folders listed in `PATH`.

When that binary is missing, the plugin says so (naming the setting, the
configuration file and the path it tried) and does not look for another
`restic.exe`, so you never end up running a different restic than you chose.

Without `ResticPath=` (or with an empty value), the plugin uses
`<plugin_dir>\restic.exe` next to the plugin DLL, and otherwise `restic.exe`
from the system `PATH`. For security against working-directory hijacking,
the current directory is never searched, and neither are relative `PATH`
entries or `ResticPath` values relative to the current drive or folder
(`C:restic.exe`, `\tools\restic.exe`).

#### Paths with Spaces

Write paths with spaces as they are, without quotes: everything after `=` up
to the end of the line is the value, with surrounding spaces trimmed.
Quotes around the whole value are allowed and removed. A backslash at the
end of a path is kept and does not continue the line, so
`Local Time Machine=C:\Users\me\AppData\Local\Time Machine.restic-repo\` works as well.

#### Logging

Diagnostic logging is **off by default**. To find out why a repository does
not open, set `LogPath` under `[Logging]` to a file; environment variables
are expanded and missing folders are created. The path must be absolute
(after expanding the variables); a relative one leaves logging off:

```ini
[Logging]
LogPath=%TEMP%\TCResticBrowser.log
```

The log records each `restic` command line with its exit code, duration and
error output, the configuration in use, password prompts, the error
messages shown, and the snapshot lists and whole-snapshot listings read in
the background (how many entries, how much memory, how long it took, or
why one was not kept). It never contains a password, only how many characters were
entered and whether the entry starts or ends with whitespace or contains
invisible characters (the password prompt after a wrong password points
these out too), and credentials in repository URLs are masked. Clear the value to
turn logging off again; the file is not rotated, so delete it when done.

#### Example `TCResticBrowser.ini`

```ini
; ==============================================================================
; Restic File System Plugin (WFX) for Total Commander - Configuration File
; ==============================================================================

[Settings]
; Optional path to restic.exe.
; If omitted or commented out, the plugin checks <plugin_dir>\restic.exe first
; and then searches the system PATH.
ResticPath=C:\Program Files\Restic\restic.exe
; Optional name of the plugin's folder in Network Neighborhood
; (default: TCResticBrowser; read only when the plugin is installed).
RootName=Backups
; Optional: how long a password is remembered, in seconds (default: 1800).
PasswordTimeoutSeconds=600
; Optional: Total Commander's connections toolbar with a log (default: yes).
ConnectionLog=no
; Optional snapshot folder names (default: YYYY-MM-DD_hh-mm-ss).
SnapshotTimeFormat=YYYY-MM-DDThh.mm.ss
; Optional: re-read the snapshot list in the background once it is this many
; seconds old (default: 60).
SnapshotListRefreshSeconds=600
; Optional memory for complete snapshot listings in MiB (default: 512, 0 = off).
SnapshotCacheMB=1024
; Optional field separator of snapshots.csv: , ; | or tab
; (default: the list separator of the Windows regional settings).
CsvSeparator=;

[Repositories]
; Format: <Alias>=<Repository Path or URL>
; Each <Alias> is displayed as a top-level folder inside "\\\Backups\".

; 1. Local disk repository (spaces in aliases and paths need no quotes)
LocalDocs=D:\Backups\restic_docs
Local Time Machine=C:\Users\me\AppData\Local\Time Machine.restic-repo\

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

- **Windows** — the plugin imports only `KERNEL32.dll` and `CRYPT32.dll`
  (for `CryptProtectMemory`) and links the C runtime statically, so no
  Visual C++ Redistributable is needed.
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
`time`, `hostname`, `tags`, `paths`, `summary.total_bytes_processed`, `name`, `type`, `size`, `mtime`, `atime`,
`ctime`, `path`, `message_type`, `message`), so fields and message types that a newer
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

## Expected Future Work

Planned for later versions, in no particular order:

- **S3 credentials asked for like the repository password.** Instead of
  relying on `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` in Total
  Commander's environment, the plugin would ask for the access key and
  secret key in a dialog and keep them in memory only, as it does with the
  Restic password.
- **Verified rclone backends.** Check (and add tests) that `rclone:`
  repositories work through the plugin; restic handles them itself, so no
  problems are expected.
- **`--password-file` and `--password-command` per repository.** An INI
  setting whose value the plugin passes to `restic` as the ordinary
  `--password-file` / `--password-command` argument, so such repositories
  open without a password prompt. Total Commander and the plugin never see
  the password.
- **Stop restic's helper processes on `ESC`.** Cancelling terminates
  `restic.exe`, but an `rclone` or `ssh` process it started lives on until
  it notices the closed pipe; so does a `restic` still reading a snapshot in
  the background when Total Commander exits. A job object with
  `KILL_ON_JOB_CLOSE` would end the whole process tree at once.
- **Check the restic version.** Tell the user when `restic.exe` is older
  than 0.17.1 instead of misreading its exit codes and output.

## Contributing

Build instructions, repo layout and architecture notes for developers live
in [CONTRIBUTING.md](CONTRIBUTING.md); the detailed technical specification,
shared with the coding agents used on this project, is in
[AGENT.md](AGENT.md).

## License

This project is licensed under the MIT License, like the upstream project it
is forked from — see [LICENSE.md](LICENSE.md) for the full text. Release notes are
in [CHANGELOG.md](CHANGELOG.md).
