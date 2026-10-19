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
  the Windows SDK, CMake and AddressSanitizer. Installing with
  `vs_buildtools.exe --config .vsconfig` (as
  `setup-build-environment-example.cmd` and the CI workflow do) selects
  exactly those. The full Visual Studio IDE works too, but is not required.
- *(Optional)* `tmp\restic.exe`: with it the live end-to-end tests run
  against a real repository they create in `tmp\test_fixture\`; without it
  they report `[SKIP]`.

Nothing else: no Bash, no Python, no .NET. The plugin is pure C11 / Win32,
linked against the static MSVC runtime, and contains no third-party code.

## Repo Layout

| Path | Description |
|---|---|
| `.github/` | GitHub Actions CI workflow (`workflows/build.yml`) and Dependabot configuration |
| `build\` | CMake out-of-source build trees and the ZIP staging folder (gitignored) |
| `dist\` | Released ZIP bundles (gitignored) |
| `logo\` | SVG sources of the plugin icon `src\restic_wfx.ico` (see [Plugin Icon](#plugin-icon)) |
| `src\` | C sources and headers, `CMakeLists.txt`, `.def`/`.rc`/`.ico`, `pluginst.inf`, the sample `restic_plugin.ini` |
| `src\tests\` | Test programs and the JSON fuzz target |
| `tmp\` | Optional live-test input `restic.exe` and the generated test repository (gitignored) |
| `.gitattributes` | Line-ending normalization rules |
| `.gitignore` | Excludes build output and `tmp\` from version control |
| `.vsconfig` | Visual Studio component selection; pins the MSVC toolset and Windows SDK versions for the CI workflow and `setup-build-environment-example.cmd` |
| `build.cmd` | Top-level build driver |
| `setup-build-environment-example.cmd` | Builds from scratch on a fresh Windows installation |
| `AGENT.md` | Technical specification and coding-agent instructions |
| `CHANGELOG.md` | Release notes in Keep a Changelog format |
| `CLAUDE.md` | Points Claude Code at `AGENT.md` |
| `CONTRIBUTING.md` | Developer documentation (this file) |
| `LICENSE` | MIT License; bundled in every release ZIP as `LICENSE.txt` |
| `README.md` | End-user documentation; bundled in every release ZIP |

## Build

From any command prompt where `vswhere.exe` can be found:

```cmd
build.cmd
```

The script locates Visual Studio Build Tools via `vswhere` and activates the
compiler environment itself — no Developer Command Prompt is needed. It drives
CMake (NMake Makefiles generator) for both bitnesses (x86 and x64) using the
sources under `src\`, **runs the four test programs for each bitness** (a
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
errors), and the unit tests under AddressSanitizer.

To set up a build environment from scratch on a fresh Windows installation
(VM, [Windows Sandbox](https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/),
or similar), `setup-build-environment-example.cmd` in the repo root downloads
the sources, installs the required toolchain, and runs `build.cmd`
automatically. Open it in a text editor before running — it is intentionally
readable so you can see and trust every step.

The GitHub Actions CI workflow runs `build.cmd` and `build.cmd check` on every
push and pull request, with `restic.exe` downloaded for the live tests and
verified against a SHA-256 pinned in the workflow. The workflow uploads the
resulting ZIP as a downloadable artifact but does **not** publish GitHub
Releases — that step is done locally after GPG-signing the artifact (see
*Release process* below).

All actions in the workflow are pinned to a full commit SHA. Dependabot is
configured to open monthly PRs that bump those SHAs when upstream actions
release new versions — do not update the SHAs by hand. The pinned `restic`
version is bumped by hand, together with its SHA-256, after verifying the
release's signed `SHA256SUMS`.

### Tests

The test programs in `src\tests\` are built next to the plugin DLL (they
are not shipped in the ZIP) and run from the repository root:

| Program | Scope |
|---|---|
| `test_json` | The JSON reader on its own: malformed input, escapes, surrogates, exact 64-bit integers, the nesting limit, real Restic output, pinned JSONTestSuite edge cases |
| `test_config` | `restic_plugin.ini` lookup and parsing, `restic.exe` resolution (never from the current directory), root listing, the inert plugin without an INI |
| `test_restic_cli` | Snapshot and `ls` parsers, argument quoting, child-process handling with `cmd.exe` and a fake `restic` stand-in, and live tests against `tmp\restic.exe` |
| `test_plugin_dll` | Loads the built DLL like Total Commander does: exact export surface, icon and version resources, INI lookup, and live navigation and extraction through the WFX API |

[AGENT.md](AGENT.md#87-test-suites-srctests) lists every case. Run them
manually after a build:

```cmd
build\x64\test_plugin_dll.exe
build\x86\test_plugin_dll.exe
```

`src\tests\fuzz_json.c` is a libFuzzer target for the JSON reader. It is
not part of `build.cmd` because it needs a fuzzing-capable compiler; its
header shows how to build and run it with clang.

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

Three additional parameters are pinned so that "same source" also means "same
compiler output":

| Parameter | Pinned to | Files that must match |
|---|---|---|
| Exact `cl.exe` version | `19.51.36248` | `build.cmd` (`EXPECTED_CL`) |
| MSVC toolset (major.minor) | `14.51` | `build.cmd` (`CMAKE_T`), `.vsconfig`, CI workflow |
| Windows SDK | `10.0.26100.0` | `build.cmd` (`CMAKE_SDK`), `.vsconfig`, CI workflow |

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

#### Upgrading the pinned toolchain

When you adopt a new MSVC toolset or Windows SDK version, update **all three
locations** listed below to keep them in sync. A mismatch causes either a
build failure (CMake cannot find the requested toolset) or silent divergence
between local and CI builds.

##### MSVC toolset

Identify the installed toolset version:

```
cl.exe /Bv
```

The path in the output contains the toolset directory, e.g.:
`...\VC\Tools\MSVC\`**`14.51`**`.36231\bin\...`

Use the first two numbers (`14.XX`) as the toolset token and the full
four-part version (e.g. `19.51.36244`) as the exact compiler version.
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

1. **`build.cmd`** — both `cmake` configure lines:
   ```
   -DCMAKE_SYSTEM_VERSION=10.0.NNNNN.0
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
2. **`.github/workflows/build.yml`** — update the VS installer URL if the
   stable channel URL changes (currently `https://aka.ms/vs/18/stable/vs_buildtools.exe`).

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
| `logo\restic-wfx.svg` | 24, 32, 48 and 256 px (32-unit grid) |
| `logo\restic-wfx-16.svg` | 16 px, the same design fitted to whole pixels so it stays sharp |

`src\restic_wfx.ico` is a committed binary generated from them; the build
only embeds it, so it does not affect reproducibility. After editing an SVG,
regenerate it with `rsvg-convert` (librsvg) and `icotool` (icoutils), for
example in WSL or a Debian container. The 16–48 px frames are stored as
32-bit bitmaps and the 256 px frame as PNG (`-r`), which is what
`test_plugin_dll` expects:

```sh
rsvg-convert -w 16 -h 16 logo/restic-wfx-16.svg -o 16.png
for z in 24 32 48 256; do rsvg-convert -w $z -h $z logo/restic-wfx.svg -o $z.png; done
icotool -c -o src/restic_wfx.ico 16.png 24.png 32.png 48.png -r 256.png
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
    ├── FsFindFirstW / FsFindNextW   \                     -> aliases from restic_plugin.ini
    │                                \<alias>              -> restic snapshots --json
    │                                \<alias>\<snapshot>\… -> restic ls --json (one level)
    ├── FsGetFileW                   restic dump -> temporary file -> atomic
    │                                replace of the target (F5, F3, Ctrl+Q)
    └── RequestProcW (TC callback)   repository password, kept in memory
                                     per alias, passed as RESTIC_PASSWORD
```

| Source | Responsibility |
|---|---|
| `main.c` | `DllMain`, the exported WFX entry points, virtual path routing, password sessions |
| `config.c` | `restic_plugin.ini` lookup and parsing, `restic.exe` resolution |
| `restic_cli.c` | `CreateProcessW` with an explicit handle list and environment block, pipes, timeouts and cancellation, snapshot and `ls` parsing |
| `json.c` | Strict read-only JSON reader (portable C11, no Win32) |

**Read-only by omission:** a WFX plugin declares which operations it
supports by which functions it exports. `FsPutFile`, `FsMkDir`,
`FsDeleteFile`, `FsRemoveDir` and `FsRenMovFile` are deliberately not
exported (`src\restic_wfx.def`, enforced by `test_plugin_dll`), so Total
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
   section of `CHANGELOG.md` into `## [vX.Y.Z] – YYYY-MM-DD`, in a final
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

This project is licensed under the MIT License — see [LICENSE](LICENSE) for
the full text. Release notes are in [CHANGELOG.md](CHANGELOG.md).
