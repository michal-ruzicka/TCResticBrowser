# For Contributors

**This document is for developers who want to build, modify or extend the plugin.**

The project lives at <https://github.com/michal-ruzicka/TCResticBrowser>, a
fork of [Restic WFX Plugin](https://github.com/goldfix/Restic_WFX_Plugin) by
Pietro Partescano. Bug reports and patches are welcome via the project's
[issue tracker](https://github.com/michal-ruzicka/TCResticBrowser/issues) and
[pull requests](https://github.com/michal-ruzicka/TCResticBrowser/pulls).
Fixes that are not specific to this fork's build and packaging are worth
offering upstream as well.

The detailed technical specification — design decisions, the virtual path
model, process handling, hardening rules and the full test inventory — is
[AGENT.md](AGENT.md). It doubles as the instructions for the coding agents
used on this project ([CLAUDE.md](CLAUDE.md) points Claude Code at it); keep
it up to date with every change.

## Prerequisites

- [**Visual Studio 2026 Build Tools**](https://visualstudio.microsoft.com/)
  with the components listed in [`.vsconfig`](.vsconfig): the MSVC toolset,
  the Windows SDK, CMake and AddressSanitizer. For a build that matches the
  CI build byte for byte, use the pinned release, 18.11.1 (see
  [Reproducible Builds](#reproducible-builds)). Installing with
  `vs_buildtools.exe --config .vsconfig` (as
  `setup-build-environment-example.cmd` and the CI workflow do) selects
  exactly those. The full Visual Studio IDE works too, but is not required.
- *(Optional, but CI has it)* `tmp\restic.exe`: with it the live end-to-end
  tests run against a real repository they create in `tmp\test_fixture\`;
  without it they report `[SKIP]`, and a local `build.cmd` or
  `build.cmd check` covers much less than CI does (a test bug that only the
  Debug CRT of `build.cmd check` reports once hung CI while every local run
  passed). Use the version CI pins: `RESTIC_VERSION` in
  `.github/workflows/build.yml`, from
  `restic_<version>_windows_amd64.zip` of the restic GitHub release, checked
  against `RESTIC_ZIP_SHA256` there (`Get-FileHash -Algorithm SHA256`), the
  `.exe` inside copied to `tmp\restic.exe` (the 64-bit `restic.exe` serves the
  x86 tests too).
- *(Optional, but CI has it)* `tmp\rclone.exe`: the live tests of an `rclone:`
  repository (rclone's local backend over the test repository) and of an S3
  repository (on `rclone serve s3`) need it and report `[SKIP]` without it. Again the version CI pins, `PINNED_RCLONE_VERSION`
  and `PINNED_RCLONE_ZIP_SHA256`, `rclone.exe` from
  `rclone-v<version>-windows-amd64.zip` of the rclone GitHub release.

Nothing else: no Bash, no Python, no .NET. The plugin is pure C11 / Win32,
linked against the static MSVC runtime, and contains no third-party code.

## Repo Layout

| Path | Description |
|---|---|
| `.github/` | GitHub Actions CI workflow (`workflows/build.yml`) and Dependabot configuration |
| `build\` | CMake out-of-source build trees and the ZIP staging folder (gitignored) |
| `dist\` | Released ZIP bundles (gitignored) |
| `logo\` | SVG sources of the plugin icon `src\TCResticBrowser.ico` (see [Plugin Icon](#plugin-icon)) |
| `src\` | C sources and headers, `CMakeLists.txt`, `.def`/`.rc`/`.ico`, `pluginst.inf`, the sample `TCResticBrowser.ini` |
| `src\tests\` | Test programs and the libFuzzer targets |
| `tmp\` | Optional live-test inputs `restic.exe` and `rclone.exe` and the generated test repository (gitignored) |
| `.gitattributes` | Line-ending normalization rules |
| `.gitignore` | Excludes build output and `tmp\` from version control |
| `.vsconfig` | Visual Studio component selection; pins the MSVC toolset and Windows SDK versions for the CI workflow and `setup-build-environment-example.cmd` |
| `build.cmd` | Top-level build driver |
| `build-fingerprint.ps1` | Describes one build for comparing two of them (see [When the hashes differ](#when-the-hashes-differ)); called by `build.cmd` |
| `setup-build-environment-example.cmd` | Builds from scratch on a fresh Windows installation |
| `AGENT.md` | Technical specification and coding-agent instructions; bundled in every release ZIP |
| `CHANGELOG.md` | Release notes in Keep a Changelog format; bundled in every release ZIP |
| `CLAUDE.md` | Points Claude Code at `AGENT.md`; bundled in every release ZIP |
| `CONTRIBUTING.md` | Developer documentation (this file); bundled in every release ZIP |
| `LICENSE.md` | MIT License; bundled in every release ZIP |
| `README.md` | End-user documentation; bundled in every release ZIP |

## Build

From any command prompt where `vswhere.exe` can be found:

```cmd
build.cmd
```

The script locates Visual Studio Build Tools via `vswhere` and activates the
compiler environment itself — no Developer Command Prompt is needed. It drives
CMake (NMake Makefiles generator) for both bitnesses (x86 and x64) using the
sources under `src\`, **runs the five test programs for each bitness** (a
failing test fails the build), and packages the artifacts into
`dist\TCResticBrowser.v<version>.zip` — the same auto-install bundle
described in *Installation*. The version is read from `src\pluginst.inf` as
the single source of truth; CMake generates the `version.h` behind the DLL's
version resource from it.

```cmd
build.cmd check
```

runs the additional x64 checks, each in its own build directory and without
producing a ZIP: a Debug build whose tests run with the CRT debug heap and
leak checker, a build with the MSVC static analyzer (`/analyze`, warnings are
errors), and the unit tests under AddressSanitizer. It needs the
AddressSanitizer component from `.vsconfig` (`C++ AddressSanitizer` in the
Visual Studio Installer), which a Visual Studio installed by other means may
lack; `build.cmd check` stops right away with that hint when it is missing.

To set up a build environment from scratch on a fresh Windows installation
(VM, [Windows Sandbox](https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/),
or similar), `setup-build-environment-example.cmd` in the repo root downloads
the sources, installs the required toolchain, and runs `build.cmd`
automatically. Open it in a text editor before running — it is intentionally
readable so you can see and trust every step.

The GitHub Actions CI workflow runs `build.cmd` and `build.cmd check` on every
push and pull request, with `restic.exe` and `rclone.exe` downloaded for the
live tests and verified against SHA-256 hashes pinned in the workflow. The workflow uploads the
resulting ZIP as a downloadable artifact but does **not** publish GitHub
Releases — that step is done locally after GPG-signing the artifact (see
*Release process* below).

All actions in the workflow are pinned to a full commit SHA. Dependabot is
configured to open monthly PRs that bump those SHAs when upstream actions
release new versions — do not update the SHAs by hand. The pinned `restic`
and `rclone` versions are bumped by hand, each together with its SHA-256,
after verifying the release's signed `SHA256SUMS` (the workflow comments name
the keys). The rclone pins must not be named `RCLONE_*`: rclone reads such
variables as its options.

### Tests

The test programs in `src\tests\` are built next to the plugin DLL (they
are not shipped in the ZIP) and run from the repository root:

| Program | Scope |
|---|---|
| `test_json` | The JSON reader on its own: malformed input, escapes, surrogates, exact 64-bit integers, the nesting limit, real Restic output, pinned JSONTestSuite edge cases |
| `test_snapshot_tree` | RFC 3339 timestamps, the `restic mount` folder tree and the whole-snapshot index (portable C, also builds with gcc/clang and sanitizers): naming, `SnapshotTimeFormat`, `-1` suffixes, `latest`, `latest → <folder>` and `latest → <short ID>`, host and tag names made Windows-safe, `snapshots.csv` (quoting, sort order, formula defusing) and `snapshots.json`, the index's walk order, limits and lookups, a file's versions from `restic find` |
| `test_config` | `TCResticBrowser.ini` lookup and parsing, `[Repository: <alias>]` sections, `restic.exe` resolution (never from the current directory), root listing, the inert plugin without an INI |
| `test_restic_cli` | `ls` parser, argument quoting, child-process handling with `cmd.exe` and a fake `restic` stand-in (`fake_restic.exe`), line streaming, the credentials in the environment, `-o` options, the job object, the version check, refused S3 credentials, and live tests against `tmp\restic.exe` (including every folder listed identically from the whole-snapshot index and from `restic ls`) |
| `test_plugin_dll` | Loads the built DLL like Total Commander does: exact export surface, icon and version resources, INI lookup, and live navigation (the `restic mount` folders, background listings and refreshes) and extraction through the WFX API, also of `[Repository: <alias>]` settings, an `rclone:` repository and an S3 repository on `rclone serve s3` with `tmp\rclone.exe`, and `Alt+Enter` (the versions of a file and their dialog, snapshot details) |

[AGENT.md](AGENT.md#87-test-suites-srctests) lists every case. Run them
manually after a build:

```cmd
build\x64\test_plugin_dll.exe
build\x86\test_plugin_dll.exe
```

`src\tests\fuzz_json.c` is a libFuzzer target for the JSON reader, and
`src\tests\fuzz_snapshot.c` one for the parsers that turn restic's output
into the snapshot tree, the whole-snapshot index and a file's versions. They are not part of
`build.cmd` because they need a fuzzing-capable compiler; their headers show
how to build and run them with clang.

### Reproducible Builds

Every build from the same source commit produces byte-identical binaries,
regardless of when or where the build runs. Users can therefore independently
verify that a published ZIP was built from the published source code without
having to trust the build infrastructure.

#### What makes the build deterministic

Four independent sources of non-determinism are eliminated:

| Source | Mechanism |
|---|---|
| PE `TimeDateStamp` field in `.dll` / `.exe` | Linker flag `/BREPRO` zeros it |
| Non-deterministic hash in MSVC `.obj` files | Compiler flags `/Brepro /experimental:deterministic` |
| Absolute source paths in debug info | Compiler flag `/pathmap:<src-dir>=./` strips the machine-local prefix |
| File modification times in ZIP entries | Normalized to `release-date=` read from `src\pluginst.inf` |

Four additional parameters are pinned so that "same source" also means "same
compiler output":

| Parameter | Pinned to | Files that must match |
|---|---|---|
| Visual Studio release | `18.11.1` | `build.cmd` (`EXPECTED_VS`), `.github\workflows\build.yml` (`VS_BUILDTOOLS_VERSION`, `_URL`, `_SHA256`), `setup-build-environment-example.cmd` (`VS_BUILDTOOLS_URL`, `_SHA256`) |
| Exact `cl.exe` version | `19.51.36260` | `build.cmd` (`EXPECTED_CL`) |
| MSVC toolset (major.minor) | `14.51` | `build.cmd` (`CMAKE_T`), `.vsconfig` (also used by the CI workflow) |
| Windows SDK | `10.0.26100.0` | `build.cmd` (`CMAKE_SDK`), `.vsconfig` (also used by the CI workflow) |

The Visual Studio release matters on its own: an update can service the
toolset libraries (`libcmt.lib`, `libvcruntime.lib`) and the Windows SDK
(`libucrt.lib`, headers) without changing the `cl.exe` version or the
folder names. The CI workflow therefore installs Build Tools with the
fixed-version bootstrapper of the pinned release (its SHA-256 is checked),
not the latest one. Locally, keep Visual Studio at that release, or install
the pinned Build Tools next to it with the same bootstrapper and point
`build.cmd` at them:

```
set TCRB_VSINSTALL=C:\BuildTools
build.cmd
```

Without `TCRB_VSINSTALL`, `build.cmd` uses the newest Visual Studio that
`vswhere` reports. It warns when its version (`VSCMD_VER`, shown in the
Developer Command Prompt banner) is not `EXPECTED_VS`.

`build.cmd` checks `cl.exe` against `EXPECTED_CL` and prints a warning if
they differ — the build continues regardless. The check exists so that when
a local and a CI build produce different hashes, the first thing to look at
in both logs is the `cl.exe` version line. Two builds from the same toolset
major.minor but different `cl.exe` patch versions will produce different
binaries; this happens when VS patches land at different times on local
machines and CI runner images. Update `EXPECTED_CL` (with a matching VS
update on both sides) when you want to re-establish bit-for-bit parity.

The toolset is selected by passing `-vcvars_ver=%CMAKE_T%` to `VsDevCmd.bat`
before invoking CMake. The SDK is enforced by CMake:

```
cmake ... -DCMAKE_SYSTEM_VERSION=10.0.26100.0
```

If the SDK is missing, CMake fails with a descriptive error rather than
silently falling back to a different version.

#### Verifying a build

The independent reference point is the SHA-256 of the ZIP itself, not files
inside it — `sha256sums.sha256` is bundled inside the same archive, so it is only
as trustworthy as the ZIP it came from.

To verify a release:

1. Run `build.cmd` locally on the release commit. The script prints the SHA-256
   of the produced ZIP at the end of its output.
2. Compare that hash against the SHA-256 printed in the *Build and test (x86 + x64)*
   step of the [GitHub Actions run](https://github.com/michal-ruzicka/TCResticBrowser/actions)
   for the same commit.
3. Optionally, compare against the published release ZIP's SHA-256:
   ```
   certutil -hashfile dist\TCResticBrowser.vX.Y.Z.zip SHA256
   ```

If all three match, the released ZIP is identical to what the source builds.

`sha256sums.sha256` inside the ZIP is useful as a convenience check after
extraction — to confirm that an individual file was not corrupted during
download or extraction — but it is not an independent trust anchor.

#### When the hashes differ

After each bitness, `build.cmd` runs `build-fingerprint.ps1`, which writes
`build\fingerprint-x64.txt` and `build\fingerprint-x86.txt` and prints them
into the log; the CI workflow also uploads them as the
`TCResticBrowser-fingerprints` artifact. A fingerprint lists everything that
decides the bytes of the plugin DLL, with machine-specific folders replaced by
placeholders (`<VS>`, `<VCTools>`, `<WindowsSdkDir>`, `<src>`, `<build>`), so
two builds of the same commit should have identical fingerprints apart from
the lines marked *informational*. Compare a local one with the one from CI:

```
fc build\fingerprint-x64.txt %USERPROFILE%\Downloads\TCResticBrowser-fingerprints\fingerprint-x64.txt
```

Read the differences from the top; the first section that differs is usually
the cause:

| Section | What a difference means |
|---|---|
| `[toolchain]` | Another Visual Studio, MSVC toolset or Windows SDK build. The same version number with another hash of `cl.exe`, `c1.dll`, `c2.dll` or `link.exe` means a serviced toolset. Install the same Visual Studio version on both sides; a Visual Studio update can update the Windows SDK too. |
| `[libraries]` | The static C runtime (`libcmt.lib` and `libvcruntime.lib` from the toolset, `libucrt.lib` from the Windows SDK) or an import library differs, and their code is linked into the DLL. `libucrt.lib` changes with the SDK revision (10.0.26100.*x*), which the SDK folder name `10.0.26100.0` does not show; the SDK is installed machine-wide, so another product (or a newer Visual Studio instance) can update it under a pinned Visual Studio. |
| `[options]` | CMake generated other compiler, resource compiler or linker options: another CMake version, or a changed `CMakeLists.txt`. |
| `[objects]` | The toolchain and options are the same but an object file is not: the headers differ (another SDK revision) or the sources do. |
| `[dll]` | Only the linked result differs: the linker or a library. The section hashes narrow it down (`.text` code, `.rdata` constants and the import table, `.rsrc` the icon and version resource). The `rich:` lines (the Rich header) list, per tool build, how many linked-in objects it produced: a library built by another compiler shows up as another `build` number. |

#### Upgrading the pinned toolchain

When you adopt a new MSVC toolset or Windows SDK version, update **both
locations** listed below to keep them in sync. A mismatch causes either a
build failure (CMake cannot find the requested toolset) or silent divergence
between local and CI builds.

##### Visual Studio release

Take the new release's *Build Tools* fixed-version bootstrapper from the
[Visual Studio 2026 release history](https://learn.microsoft.com/visualstudio/releases/2026/release-history).
The long hexadecimal folder in its URL is the SHA-256 of the file; check it
after downloading (`certutil -hashfile vs_BuildTools.exe SHA256`), and check
that the file's *Product version* (Properties, Details) is the release.
Update, in one commit:

1. **`build.cmd`** — `set EXPECTED_VS=18.XX.Y`
2. **`.github\workflows\build.yml`** — `VS_BUILDTOOLS_VERSION`,
   `VS_BUILDTOOLS_URL`, `VS_BUILDTOOLS_SHA256`
3. **`setup-build-environment-example.cmd`** — `VS_BUILDTOOLS_URL`,
   `VS_BUILDTOOLS_SHA256`

Then update your local Visual Studio to the same release. A new release can
bring a new `cl.exe`, so check the toolset below as well.

##### MSVC toolset

Identify the installed toolset version:

```
cl.exe /Bv
```

The path in the output contains the toolset directory, e.g.:
`...\VC\Tools\MSVC\`**`14.51`**`.36231\bin\...`

Use the first two numbers (`14.XX`) as the toolset token and the full
three-part `cl.exe` version (e.g. `19.51.36244`) as the exact compiler version.
Update:

1. **`build.cmd`** — the three pinned variables at the top of the file:
   ```
   set EXPECTED_CL=19.51.XXXXX
   set CMAKE_T=14.XX
   set CMAKE_SDK=10.0.XXXXX.X
   ```
2. **`.vsconfig`** — the `VC.Tools` component entry (this also covers the
   CI workflow, which installs the toolchain from `.vsconfig`):
   ```json
   "Microsoft.VisualStudio.Component.VC.14.XX.x86.x64"
   ```

##### Windows SDK

The SDK version is a five-digit build number, visible in VS Installer under
the installed *Windows 11 SDK* component (e.g. `10.0.`**`26100`**`.0`).
Update:

1. **`build.cmd`** — the `CMAKE_SDK` variable at the top of the file, which
   every `cmake` configure line passes on:
   ```
   set CMAKE_SDK=10.0.NNNNN.0
   ```
2. **`.vsconfig`** — the SDK component entry (this also covers the
   CI workflow, which installs the toolchain from `.vsconfig`):
   ```json
   "Microsoft.VisualStudio.Component.Windows11SDK.NNNNN"
   ```

##### Visual Studio major version

Each major Visual Studio release increments the product version number in the
installation path (`\17\` for VS 2022, `\18\` for VS 2026, …). The build
uses the NMake Makefiles generator driven by `VsDevCmd.bat`, so no CMake
generator flag needs updating. What does change:

1. **`.vsconfig`** — the toolset and SDK component IDs change with each major
   VS release; update those using the guidance above.
2. **The pinned release** — a new major version has its own release history
   page; pin one of its releases as described in *Visual Studio release*
   above. `build.cmd` finds the installation through `vswhere`, which does not
   depend on the major version.

CMake comes with the Build Tools (`VC.CMake.Project` in `.vsconfig`), so a
new Visual Studio version also brings a matching CMake.

##### After any toolchain change

Delete the stale CMake build trees so CMake picks up the new selection on the
next run:

```
rmdir /S /Q build
build.cmd
```

### Plugin Icon

The icon is a floppy disk, Total Commander's symbol, whose label carries a
"history" arrow around a clock: backups you can go back in time through.
Two hand-written SVG files in `logo\` are its source:

| File | Frames |
| --- | --- |
| `logo\TCResticBrowser.svg` | 24, 32, 48 and 256 px (32-unit grid) |
| `logo\TCResticBrowser-16.svg` | 16 px, the same design fitted to whole pixels so it stays sharp |

`src\TCResticBrowser.ico` is a committed binary generated from them; the build
only embeds it, so it does not affect reproducibility. After editing an SVG,
regenerate it with `rsvg-convert` (librsvg) and `icotool` (icoutils), for
example in WSL or a Debian container. The 16–48 px frames are stored as
32-bit bitmaps and the 256 px frame as PNG (`-r`), which is what
`test_plugin_dll` expects:

```sh
rsvg-convert -w 16 -h 16 logo/TCResticBrowser-16.svg -o 16.png
for z in 24 32 48 256; do rsvg-convert -w $z -h $z logo/TCResticBrowser.svg -o $z.png; done
icotool -c -o src/TCResticBrowser.ico 16.png 24.png 32.png 48.png -r 256.png
```

The committed icon was made with librsvg 2.60.0 and icoutils 0.32.3 (Debian
13); with those versions the commands reproduce it byte for byte. Check
every size at 100 % zoom: the small frames matter most, because the icon
appears at 16 px in Network Neighborhood.

## Architecture

A single artifact: the plugin DLL (`TCResticBrowser.wfx` 32-bit / `TCResticBrowser.wfx64`
64-bit), a Total Commander **File System plugin** (WFX). The DLL does no
repository access itself; it runs the official `restic.exe` as a hidden
child process and parses its JSON output.

```
Total Commander process
└── TCResticBrowser.wfx[64]
    ├── FsFindFirstW / FsFindNextW   \                   -> aliases from TCResticBrowser.ini
    │                                \<alias>\{ids,snapshots,hosts,tags}\…
    │                                    -> restic mount's tree from `restic snapshots --json`,
    │                                       cached, re-read in the background after 60 s
    │                                \<alias>\snapshots.csv -> generated from that tree
    │                                \<alias>\repository_id.txt -> fixed size, ID read on copy
    │                                …\<snapshot>\…      -> restic ls --json (one level) until
    │                                    the background `restic ls` of the whole snapshot is
    │                                    read, then from memory
    ├── FsGetFileW                   restic dump (or the generated snapshots.csv,
    │                                or the ID from `restic cat config --json`)
    │                                -> temporary file -> atomic replace of the
    │                                target (F5, F3, Ctrl+Q)
    ├── RequestProcW (TC callback)   repository password and S3 secret key, kept in
    │                                memory per alias, passed as RESTIC_PASSWORD and
    │                                AWS_SECRET_ACCESS_KEY (none with PasswordFile/
    │                                PasswordCommand, which restic reads itself)
    └── LogProcW (TC callback)       unless ConnectionLog=no: one CONNECT \ for all
                                     repositories when one is first used, before
                                     its password is asked for; passwords, restic
                                     runs and background work as MSGTYPE_DETAILS,
                                     copies as MSGTYPE_TRANSFERCOMPLETE;
                                     FsDisconnectW forgets every password and cache
```

| Source | Responsibility |
|---|---|
| `main.c` | `DllMain`, the exported WFX entry points, virtual path routing, password sessions |
| `config.c` | `TCResticBrowser.ini` lookup and parsing, `[Repository: <alias>]` settings, `restic.exe` resolution |
| `restic_cli.c` | `CreateProcessW` with an explicit handle list, environment block and job object, `-o` options, pipes (also streamed line by line, stderr watched for refused credentials), timeouts and cancellation, the version check, `ls` parsing |
| `repo_cache.c` | Per-repository cache: snapshot trees and whole-snapshot listings, background workers, dropped with the password |
| `snapshot_tree.c` | The `restic mount` folder tree from `restic snapshots --json` (portable C11, no Win32) |
| `snapshot_index.c` | Compact listing of a whole snapshot from streamed `restic ls --json` (portable C11, no Win32) |
| `timestamp.c` | RFC 3339 timestamps as restic writes them (portable C11, no Win32) |
| `json.c` | Strict read-only JSON reader (portable C11, no Win32) |
| `log.c` | Optional diagnostic log (`[Logging] LogPath`), credential masking |

**Read-only by omission:** a WFX plugin declares which operations it
supports by which functions it exports. `FsPutFile`, `FsMkDir`,
`FsDeleteFile`, `FsRemoveDir` and `FsRenMovFile` are deliberately not
exported (`src\TCResticBrowser.def`, enforced by `test_plugin_dll`), so Total
Commander itself disables writing into the plugin; `F6` move arrives as
`FsGetFileW` with `FS_COPYFLAGS_MOVE` and is refused. Every Restic command
also runs with `--no-lock`.

**Untrusted names:** file names in a snapshot come from whatever system was
backed up. Entries whose name is not a single Windows path component (it
contains `\` or `/`, or consists only of dots and spaces) are never
reported to Total Commander, so a recursive copy cannot be steered outside
its target directory.

## Signing Policy

All commits merged into `main` must be signed. Tags `v*` must also be
signed. The repository's branch and tag protection rules enforce this
server-side — unsigned commits and tags are rejected on push.

Two distinct signing mechanisms are in use:

| What | Method | Key type |
|------|--------|----------|
| Git commits and annotated tags | SSH key signing | Your SSH key |
| Release ZIP files | GPG detached signature | Separate GPG key |

### Setting up SSH commit signing

You can reuse the same SSH key you already use to authenticate to GitHub.

**1. Register the key as a signing key on GitHub.**

Go to **Settings → SSH and GPG keys → New signing key** and paste your
public key. This is a separate entry from the authentication key, but
both entries can use the same key material.

**2. Configure Git for this repository.**

```
git config gpg.format ssh
git config user.signingkey "~/.ssh/id_ed25519.pub"
git config commit.gpgsign true
```

Replace `id_ed25519.pub` with your actual public key filename if
different. Omitting `--global` scopes these settings to this repository
only.

**3. Verify.**

```
git commit --allow-empty -m "test signing"
git log -1 --show-signature
```

Git should report `Good "git" signature` with your key fingerprint.

The same `gpg.format = ssh` setting is picked up by `git tag -s`, so
the release tagging step in *Release Process* below also uses your SSH
key automatically — no separate GPG key is needed for tags.

### GPG signing of release ZIPs

The distributable ZIP is signed with a GPG key (not the SSH key) to
produce a detached `.asc` signature that end users can verify without
having to trust GitHub's infrastructure. The signing key fingerprint is
`489C 5EC8 0FD6 2BE8 9E59  B4F7 19C1 3E8C E0F5 DB61` (available on
<https://keys.openpgp.org/>). The exact signing steps are in the
*Release Process* section below.

## Release Process

Releases are built and signed locally; no private key ever leaves the
developer's machine.

1. On a feature branch, set `version=X.Y.Z` (dropping the `-devel` suffix)
   and `release-date=` in `src\pluginst.inf`, and turn the `[Unreleased]`
   section of `CHANGELOG.md` into `## [vX.Y.Z] – YYYY-MM-DD` (and update the
   link references at its end), in a final
   "Bump plugin version to vX.Y.Z" commit. Development after a release
   continues as `X.Y.Z-devel` of the next version.
2. Open a PR, get it reviewed, and merge into `main`.
3. On `main`, tag and push:
   ```
   git fetch && git checkout main && git pull
   git tag -s vX.Y.Z -m "Release X.Y.Z"
   git push origin vX.Y.Z
   ```
4. Run `build.cmd` locally to produce `dist\TCResticBrowser.vX.Y.Z.zip`.
5. GPG-sign the ZIP:
   ```
   gpg --detach-sign --armor dist\TCResticBrowser.vX.Y.Z.zip
   ```
   This creates `dist\TCResticBrowser.vX.Y.Z.zip.asc`.
6. On the GitHub repository page, go to **Releases → Draft a new release**,
   select the `vX.Y.Z` tag, paste the CHANGELOG entry as the description,
   and attach both files (`.zip` and `.zip.asc`).

The CI workflow also produces the ZIP as a downloadable Actions artifact,
but that copy is unsigned and is intended for testing PRs only.

## License

This project is licensed under the MIT License — see [LICENSE.md](LICENSE.md) for
the full text. Release notes are in [CHANGELOG.md](CHANGELOG.md).
