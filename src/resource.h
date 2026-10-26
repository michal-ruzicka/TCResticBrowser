/*
 * Resource IDs shared by TCResticBrowser.rc and the plugin code
 */
#ifndef RESTIC_WFX_RESOURCE_H /* Not #pragma once: rc.exe reads this file too */
#define RESTIC_WFX_RESOURCE_H

/* The dialog naming the folder of a file's versions (Alt+Enter on a file inside a snapshot) */
#define IDD_VERSIONS_FOLDER 100
#define IDC_VERSIONS_PATH   1001 /* Read-only edit field: the folder's full path */
#define IDC_VERSIONS_COPY   1002 /* "Copy to clipboard": copies the path, then closes */
#define IDC_VERSIONS_GO     1003 /* "Go to folder": closes, then Total Commander's panel goes there */

#endif
