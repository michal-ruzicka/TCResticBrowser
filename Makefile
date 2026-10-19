# ==============================================================================
# NMake Makefile for Restic Total Commander WFX Plugin (x64)
# Usage (with MSVC x64 environment configured, or via ./build.sh):
#   nmake /nologo                  -> Build Debug plugin (build\restic.wfx64)
#   nmake /nologo CONFIG=Release   -> Build Release plugin (static CRT /MT)
#   nmake /nologo test             -> Build and run unit tests
#   nmake /nologo test ASAN=1      -> Build and run unit tests with AddressSanitizer
#   nmake /nologo analyze          -> Run MSVC static analysis (/analyze)
#   nmake /nologo package          -> Build Release plugin and create installation ZIP
#   nmake /nologo clean            -> Remove build artifacts
# ==============================================================================

!IF "$(CONFIG)" == ""
CONFIG = Debug
!ENDIF

BUILD_DIR  = build
PLUGIN_OUT = $(BUILD_DIR)\restic.wfx64
PLUGIN_LIB = $(BUILD_DIR)\restic.lib
PLUGIN_RES = $(BUILD_DIR)\restic_wfx.res
TEST_CONFIG_OUT = $(BUILD_DIR)\test_config.exe
TEST_CLI_OUT    = $(BUILD_DIR)\test_restic_cli.exe
TEST_DLL_OUT    = $(BUILD_DIR)\test_plugin_dll.exe
TEST_JSON_OUT   = $(BUILD_DIR)\test_json.exe
DEF_FILE        = restic_wfx.def
RC_FILE         = src\restic_wfx.rc
ICO_FILE        = logo\restic_wfx.ico
INI_TEMPLATE    = restic_plugin.ini
INI_OUT         = $(BUILD_DIR)\restic_plugin.ini
PLUGINST_FILE   = pluginst.inf
PLUGINST_OUT    = $(BUILD_DIR)\pluginst.inf

CC   = cl.exe
LINK = link.exe
RC   = rc.exe

# Strict pure C11 (/TC /std:c11) compiler flags and security hardening (/GS)
CFLAGS_COMMON = /nologo /TC /std:c11 /permissive- /W4 /WX /we4013 /GS \
                /DUNICODE /D_UNICODE /Iinclude \
                /Fo$(BUILD_DIR)\ /Fd$(BUILD_DIR)\vc.pdb

# Linker security hardening (/DYNAMICBASE for ASLR, /NXCOMPAT for DEP)
LFLAGS_COMMON = /NOLOGO /DEBUG /DYNAMICBASE /NXCOMPAT
SYSLIBS       =
TEST_LIBS     = Shell32.lib

!IF "$(CONFIG)" == "Release"
CFLAGS_CFG = /MT /O2 /Zi /DNDEBUG
LFLAGS_CFG = $(LFLAGS_COMMON) /OPT:REF /OPT:ICF
RCFLAGS    = /nologo /Iinclude /DNDEBUG
!ELSE
CFLAGS_CFG = /MTd /Zi /Od /D_DEBUG
LFLAGS_CFG = $(LFLAGS_COMMON)
RCFLAGS    = /nologo /Iinclude /D_DEBUG
!ENDIF

CFLAGS_EXTRA =
CFLAGS_SAN   =
!IF "$(ANALYZE)" == "1"
CFLAGS_EXTRA = $(CFLAGS_EXTRA) /analyze
!ENDIF

!IF "$(ASAN)" == "1"
CFLAGS_EXTRA = $(CFLAGS_EXTRA) /fsanitize=address
CFLAGS_SAN   = /fsanitize=address
LFLAGS_CFG   = $(LFLAGS_CFG) /INCREMENTAL:NO
!ENDIF

CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_CFG) $(CFLAGS_EXTRA)

PLUGIN_OBJS = $(BUILD_DIR)\main.obj $(BUILD_DIR)\config.obj $(BUILD_DIR)\restic_cli.obj $(BUILD_DIR)\json.obj

TEST_CONFIG_OBJS = $(BUILD_DIR)\test_config.obj $(PLUGIN_OBJS)
TEST_CLI_OBJS    = $(BUILD_DIR)\test_restic_cli.obj $(PLUGIN_OBJS)

all: $(BUILD_DIR) $(PLUGIN_OUT) $(INI_OUT) $(PLUGINST_OUT)

$(BUILD_DIR):
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

$(INI_OUT): $(BUILD_DIR) $(INI_TEMPLATE)
	copy /Y "$(INI_TEMPLATE)" "$(INI_OUT)" >nul

$(PLUGINST_OUT): $(BUILD_DIR) $(PLUGINST_FILE)
	copy /Y "$(PLUGINST_FILE)" "$(PLUGINST_OUT)" >nul

$(BUILD_DIR)\main.obj: $(BUILD_DIR) src\main.c include\fsplugin.h include\config.h
	$(CC) /c $(CFLAGS) src\main.c

$(BUILD_DIR)\config.obj: $(BUILD_DIR) src\config.c include\config.h
	$(CC) /c $(CFLAGS) src\config.c

$(BUILD_DIR)\restic_cli.obj: $(BUILD_DIR) src\restic_cli.c include\restic_cli.h include\fsplugin.h include\json.h
	$(CC) /c $(CFLAGS) src\restic_cli.c

$(BUILD_DIR)\json.obj: $(BUILD_DIR) src\json.c include\json.h
	$(CC) /c $(CFLAGS) src\json.c

$(BUILD_DIR)\test_config.obj: $(BUILD_DIR) tests\test_config.c include\config.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_config.c

$(BUILD_DIR)\test_restic_cli.obj: $(BUILD_DIR) tests\test_restic_cli.c include\restic_cli.h include\fsplugin.h
	$(CC) /c $(CFLAGS) tests\test_restic_cli.c

$(PLUGIN_RES): $(BUILD_DIR) $(RC_FILE) $(ICO_FILE) include\version.h
	$(RC) $(RCFLAGS) /fo$(PLUGIN_RES) $(RC_FILE)

$(PLUGIN_OUT): $(PLUGIN_OBJS) $(PLUGIN_RES) $(DEF_FILE)
	$(CC) /nologo /LD $(CFLAGS_CFG) $(CFLAGS_SAN) $(PLUGIN_OBJS) $(PLUGIN_RES) $(SYSLIBS) /link $(LFLAGS_CFG) /DEF:$(DEF_FILE) /IMPLIB:$(PLUGIN_LIB) /OUT:$(PLUGIN_OUT)

$(TEST_CONFIG_OUT): $(TEST_CONFIG_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_CONFIG_OBJS) $(SYSLIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_CONFIG_OUT)

# The JSON reader is portable C: its tests link nothing but the reader itself
$(BUILD_DIR)\test_json.obj: $(BUILD_DIR) tests\test_json.c include\json.h
	$(CC) /c $(CFLAGS) tests\test_json.c

$(TEST_JSON_OUT): $(BUILD_DIR)\test_json.obj $(BUILD_DIR)\json.obj
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(BUILD_DIR)\test_json.obj $(BUILD_DIR)\json.obj /link $(LFLAGS_CFG) /OUT:$(TEST_JSON_OUT)

$(TEST_CLI_OUT): $(TEST_CLI_OBJS)
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(TEST_CLI_OBJS) $(SYSLIBS) $(TEST_LIBS) /link $(LFLAGS_CFG) /OUT:$(TEST_CLI_OUT)

$(BUILD_DIR)\test_plugin_dll.obj: $(BUILD_DIR) tests\test_plugin_dll.c include\version.h
	$(CC) /c $(CFLAGS) tests\test_plugin_dll.c

# Black-box smoke test: loads build\restic.wfx64 like Total Commander does (links no plugin code)
$(TEST_DLL_OUT): $(BUILD_DIR)\test_plugin_dll.obj
	$(CC) /nologo $(CFLAGS_CFG) $(CFLAGS_SAN) $(BUILD_DIR)\test_plugin_dll.obj /link $(LFLAGS_CFG) /OUT:$(TEST_DLL_OUT)

# The DLL smoke test needs the plugin binary; AddressSanitizer runs only exercise the unit tests
TEST_DEPS = $(INI_OUT) $(PLUGINST_OUT) $(TEST_JSON_OUT) $(TEST_CONFIG_OUT) $(TEST_CLI_OUT) $(TEST_DLL_OUT)
!IF "$(ASAN)" != "1"
TEST_DEPS = $(PLUGIN_OUT) $(TEST_DEPS)
!ENDIF

test: $(TEST_DEPS)
	$(TEST_JSON_OUT)
	$(TEST_CONFIG_OUT)
	$(TEST_CLI_OUT)
	$(TEST_DLL_OUT)

analyze:
	$(MAKE) /nologo ANALYZE=1 $(PLUGIN_OUT) $(INI_OUT) $(PLUGINST_OUT) $(TEST_JSON_OUT) $(TEST_CONFIG_OUT) $(TEST_CLI_OUT) $(TEST_DLL_OUT)

package:
	$(MAKE) /nologo CONFIG=Release all
	python -c "import re, zipfile; ver = re.search(r'RESTIC_WFX_PRODUCT_VER\s+\"([^\"]+)\"', open('include/version.h').read()).group(1); inf = open('build/pluginst.inf').read(); assert f'version={ver}' in inf and 'file=restic.wfx64' in inf; zpath = f'build/wfx_restic_{ver}_x64.zip'; zf = zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED); [zf.write(src, arc) for src, arc in [('build/pluginst.inf','pluginst.inf'),('build/restic.wfx64','restic.wfx64'),('build/restic_plugin.ini','restic_plugin.ini'),('README.md','README.md'),('LICENSE','LICENSE')]]; zf.close(); print(f'[PACKAGE] Created {zpath} ({len(zf.namelist())} files)')"

clean:
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"
