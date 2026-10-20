# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

First release of the fork. Version 2.0.0 keeps its releases apart from the
upstream 1.x line.

### Added

- **32-bit plugin.** The release ZIP now contains `TCResticBrowser.wfx` for
  32-bit Total Commander next to `TCResticBrowser.wfx64`; the auto-installer
  picks the right one.
- **Configurable root folder name.** `RootName=` under `[Settings]` names the
  plugin's folder in Network Neighborhood. Total Commander reads it only
  while installing the plugin.
- **Optional diagnostic log**, like in the author's other Total Commander
  plugins: `LogPath=` under `[Logging]` (off by default) records every
  `restic` command line with its exit code, duration and error output, the
  configuration in use, password prompts and error messages. Passwords are
  never logged, only their length, and credentials in repository URLs are
  masked.
- **Hint about invisible characters in a rejected password.** When restic
  rejects a password that starts or ends with a space, tab or line break,
  or contains a line break, tab, non-breaking space, zero-width or other
  invisible character, the next password prompt says so (for example
  "the password you entered ends with a space"), and the diagnostic log
  notes it as well. Inner ordinary spaces of a passphrase are not reported,
  and the password itself is never shown or logged.
- **Reproducible builds.** The same sources and pinned toolchain produce a
  byte-identical ZIP (`/Brepro`, `/BREPRO`, `/pathmap`, pinned MSVC toolset
  and Windows SDK, ZIP timestamps from `release-date=` in `pluginst.inf`).
  The ZIP carries `sha256sums.sha256` for its files.
- **Continuous integration.** Every push is built and tested on GitHub
  Actions for both bitnesses, including the Debug/CRT leak checks, MSVC
  static analysis, AddressSanitizer and the live tests against a real
  `restic.exe` (pinned by SHA-256).

### Changed

- **Windows-native build.** `build.cmd` drives CMake and MSVC; the Bash
  wrapper, NMake Makefile and Python packaging step are gone. All build
  inputs live under `src\`, and `version=` in `src\pluginst.inf` is the only
  place the version is written.
- **Own JSON reader instead of cJSON.** The cJSON submodule is replaced by a
  strict, read-only reader of about 370 lines (`src\json.c`), so the plugin
  contains no third-party code. File sizes are now read as exact 64-bit
  integers. See *Why the Plugin Has Its Own JSON Reader* in the README.
- **Configuration file renamed and looked up like in the author's other
  Total Commander plugins.** `restic_plugin.ini` became `TCResticBrowser.ini`,
  and `%APPDATA%\GHISLER\TCResticBrowser.ini` (per-user override) wins over
  the file next to the plugin, which is only the shipped sample. Previously
  the plugin-folder file won and the folder of Total Commander's
  `wincmd.ini` was the fallback. **Upgrading from 1.x:** move the
  `restic_plugin.ini` you edited to `%APPDATA%\GHISLER\TCResticBrowser.ini`;
  the new version does not read the old name.
- **`TCResticBrowser.ini` is read as UTF-8**, like in the author's other
  Total Commander plugins, so repository aliases and paths may contain any
  characters. A UTF-8 or UTF-16 byte-order mark is honoured, and a file that
  is not valid UTF-8 is still read in the ANSI code page, as before.
  Entries too long for the plugin's buffers are now skipped instead of
  being silently truncated.
- **New plugin icon.** The goals were a vector source, a design simple
  enough to stay legible at the small sizes Total Commander shows it in, and
  content that refers to both Total Commander and versioned backups. It is a
  floppy disk, Total Commander's symbol, whose label carries a "history"
  arrow around a clock, the time-machine sign for going back to earlier
  versions. The sources are SVG files in `logo\`, including a separate
  version fitted to the 16 px pixel grid; the detailed raster illustration
  they replace was illegible at 16 px and had no editable source.
- **Plugin files renamed after the project**, like the author's other
  Total Commander plugins: `restic.wfx`/`restic.wfx64` became
  `TCResticBrowser.wfx`/`TCResticBrowser.wfx64`, and the auto-installer's
  default folder is `plugins\wfx\TCResticBrowser` instead of
  `plugins\wfx\restic`. **Upgrading from 1.x** therefore installs the plugin
  next to the old one: after moving your configuration (see above), remove
  the old `restic` entry under *Configuration → Options → Plugins → File
  system plugins* and delete its folder.
- **Default root folder name is *TCResticBrowser*** instead of
  *Restic Backups*, matching the plugin, its files and its configuration
  file. To keep the old name, set `RootName=Restic Backups` before
  installing the new version.
- **Release ZIP** is named `TCResticBrowser.vX.Y.Z.zip` and, like the
  author's other Total Commander plugins, ships all the Markdown
  documentation (README, CHANGELOG, CONTRIBUTING, AGENT, CLAUDE) and the
  license, now `LICENSE.md` instead of `LICENSE`.
- **Documentation** restructured (README, CONTRIBUTING, this changelog), and
  the Python project template and the Pi coding-agent tooling (scripts and
  instructions in Italian) were removed.

### Fixed

- **Cached passwords are protected in memory.** They are encrypted with
  `CryptProtectMemory`, kept in pages locked out of the page file and
  excluded from Windows Error Reporting crash dumps, so a memory dump or
  the hibernation file no longer contains them in plain text.
- **A password is cached only once restic has accepted it.** After a
  local write error, `ESC` or a timeout an unverified password was kept;
  now the next attempt asks again. A password that worked before is not
  affected, and "repository locked" no longer drops a correct one.

- **Snapshot entries whose name is not a single path component are no
  longer listed.** Restic stores names from non-Windows systems verbatim, so
  a snapshot could contain a file named, for example, `..\..\evil.exe`;
  copying its folder with `F5` could then write outside the chosen target
  directory. Names containing `\` or `/`, or consisting only of dots and
  spaces, are now skipped.
- **Documentation of a relative `ResticPath`.** It is resolved against the
  plugin folder, not the folder of the configuration file as documented
  before.

## [v1.0.0] – 2026-10-18

Upstream release of [Restic WFX Plugin](https://github.com/goldfix/Restic_WFX_Plugin)
by Pietro Partescano, the base of this fork was commit [c8481b43c9ab2867eee824719e3a9c6785db6c61](https://github.com/goldfix/Restic_WFX_Plugin/commit/c8481b43c9ab2867eee824719e3a9c6785db6c61).

### Added

- **Read-only browsing of Restic repositories** in Total Commander's
  Network Neighborhood: repositories from `restic_plugin.ini`, their
  snapshots, and the snapshot directory trees, loaded one level at a time.
- **Extraction** of files and folders with `F5`, `F3` and `Ctrl+Q`, streamed
  through `restic dump` with atomic replacement of the target file.
- **Passwords kept in memory only**, asked for through Total Commander's
  password dialog and passed to `restic.exe` in an isolated environment
  block.
