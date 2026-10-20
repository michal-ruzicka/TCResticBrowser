#pragma once
#include <windows.h>
#include <stddef.h>

/*
 * Optional diagnostic log, off by default ([Logging] LogPath= in TCResticBrowser.ini).
 *
 * Lines are appended as UTF-8 to the configured file, prefixed with the local time, process and
 * thread ID. The file is opened for each line and never kept open, so it can be read, moved or
 * deleted while Total Commander runs. Every function is thread-safe and does nothing while
 * logging is off.
 *
 * Never log a password. Repository targets go through log_redact_target() first.
 */

/* Upper bound of one log line in UTF-16 units; longer lines are truncated */
#define LOG_MAX_LINE 2048

/*
 * Sets the log file from the raw LogPath value: environment variables (%APPDATA%, %TEMP%, ...)
 * are expanded. NULL, an empty value or a path that is not absolute (after expansion) turns
 * logging off. Missing parent directories are created on the first write.
 */
void log_configure(const WCHAR* raw_path);

/* TRUE while a log file is configured */
BOOL log_enabled(void);

/* Copies the active log file path (empty when logging is off) */
void log_get_path(WCHAR* out, size_t cap);

/* Appends one formatted line (printf-style, %ls for wide strings) */
void log_write(const WCHAR* fmt, ...);

/*
 * Copies a repository target with the URL credentials replaced by three asterisks: in
 * "rest:https://user:secret@host/repo" the "user:secret" part is masked. Targets without a
 * "scheme://...@" authority (local paths, sftp:user@host:/path) are copied unchanged.
 */
void log_redact_target(const WCHAR* target, WCHAR* out, size_t cap);
