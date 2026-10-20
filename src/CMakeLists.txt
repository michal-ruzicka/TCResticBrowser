cmake_minimum_required(VERSION 3.20)
project(TCResticBrowser LANGUAGES C RC)

# Strict ISO C11: the plugin is pure C by design (see AGENT.md, section 8.1).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RESTIC_WFX_ANALYZE "Compile with the MSVC static analyzer (/analyze)" OFF)
option(RESTIC_WFX_ASAN "Compile with AddressSanitizer; the plugin DLL itself is not built" OFF)

# ---------------------------------------------------------------------------
# Version: version= in pluginst.inf is the single source of truth. It is
# turned into version.h (for the VERSIONINFO resource and the DLL test) at
# configure time, and CMake re-runs whenever pluginst.inf changes.
# ---------------------------------------------------------------------------

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS pluginst.inf)
file(STRINGS pluginst.inf _version_line REGEX "^version=")
# A pre-release suffix (1.1.0-devel between releases) is kept in the product
# version string; the numeric FILEVERSION carries MAJOR.MINOR.PATCH.0 only.
if(NOT _version_line MATCHES "^version=(([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.]+)?)[\r]*$")
    message(FATAL_ERROR "pluginst.inf: no version=MAJOR.MINOR.PATCH[-suffix] line")
endif()
set(RESTIC_WFX_VERSION "${CMAKE_MATCH_1}")
set(RESTIC_WFX_VER_MAJOR ${CMAKE_MATCH_2})
set(RESTIC_WFX_VER_MINOR ${CMAKE_MATCH_3})
set(RESTIC_WFX_VER_PATCH ${CMAKE_MATCH_4})

# Total Commander loads <name>.wfx in its 32-bit and <name>.wfx64 in its
# 64-bit edition; pluginst.inf names the 32-bit file for both.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RESTIC_WFX_SUFFIX ".wfx64")
else()
    set(RESTIC_WFX_SUFFIX ".wfx")
endif()
set(RESTIC_WFX_FILE_NAME "TCResticBrowser${RESTIC_WFX_SUFFIX}")

configure_file(version.h.in version.h @ONLY)

# The tests expect the staged sample configuration and installer manifest
# next to the plugin, exactly as the release ZIP ships them.
configure_file(pluginst.inf pluginst.inf COPYONLY)
configure_file(TCResticBrowser.ini TCResticBrowser.ini COPYONLY)

# ---------------------------------------------------------------------------
# Compiler and linker settings
# ---------------------------------------------------------------------------

add_compile_definitions(UNICODE _UNICODE)

# The tests live in tests/ but include the plugin headers; the generated
# version.h lands in the build directory.
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# Link the static C runtime (/MT, /MTd in Debug) so the plugin has no
# vcruntime*.dll dependency and installs by dropping it in place.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    # Drop CMake's default list of Win32 import libraries: the plugin must
    # import nothing but KERNEL32.dll (AGENT.md, section 8.5), and every other
    # system library is named explicitly on the target that needs it.
    set(CMAKE_C_STANDARD_LIBRARIES "kernel32.lib")

    # C-only options: rc.exe shares the compile options but rejects these.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:/permissive-;/W4;/WX;/we4013;/GS;/utf-8>"
    )

    # Reproducible builds: strip per-build noise from object files and zero the
    # PE TimeDateStamp so the same source always produces the same binary.
    # /Brepro removes the non-deterministic hash MSVC writes into every .obj.
    # /BREPRO (linker) zeroes the PE timestamp field.
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/Brepro;/experimental:deterministic>")
    add_link_options(/BREPRO /DYNAMICBASE /NXCOMPAT)

    # Normalize absolute source-file paths (in __FILE__ and debug info) so
    # binaries are identical regardless of where the repo is checked out.
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" _src_native)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/pathmap:${_src_native}=./>")

    if(RESTIC_WFX_ANALYZE)
        add_compile_options("$<$<COMPILE_LANGUAGE:C>:/analyze>")
    endif()

    if(RESTIC_WFX_ASAN)
        # /RTC1 (in CMake's default Debug flags) cannot be combined with ASan.
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        add_compile_options("$<$<COMPILE_LANGUAGE:C>:/fsanitize=address>")
        add_link_options(/INCREMENTAL:NO)
    endif()
endif()

# ---------------------------------------------------------------------------
# Plugin code, shared by the DLL and the white-box tests
# ---------------------------------------------------------------------------

add_library(restic_core OBJECT
    main.c
    config.c
    restic_cli.c
    json.c
    log.c
)

# ---------------------------------------------------------------------------
# Plugin DLL: TCResticBrowser.wfx (32-bit) or TCResticBrowser.wfx64 (64-bit)
#
# Not built under AddressSanitizer: Total Commander never loads an
# instrumented DLL, so the ASan run covers the unit tests only.
# ---------------------------------------------------------------------------

if(NOT RESTIC_WFX_ASAN)
    add_library(TCResticBrowser SHARED
        $<TARGET_OBJECTS:restic_core>
        TCResticBrowser.def
        TCResticBrowser.rc   # plugin icon (Network Neighborhood) + VERSIONINFO
    )
    set_target_properties(TCResticBrowser PROPERTIES
        PREFIX ""
        SUFFIX "${RESTIC_WFX_SUFFIX}"
    )
    target_link_libraries(TCResticBrowser PRIVATE crypt32)  # CryptProtectMemory
endif()

# ---------------------------------------------------------------------------
# Tests: run by build.cmd and the CI workflow from the repository root (the
# live tests look for tmp\restic.exe there); not part of the release ZIP.
# ---------------------------------------------------------------------------

# The JSON reader is portable C: its tests link nothing but the reader itself.
add_executable(test_json tests/test_json.c json.c)

add_executable(test_config tests/test_config.c $<TARGET_OBJECTS:restic_core>)
target_link_libraries(test_config PRIVATE crypt32)

add_executable(test_restic_cli tests/test_restic_cli.c $<TARGET_OBJECTS:restic_core>)
target_link_libraries(test_restic_cli PRIVATE shell32 crypt32)  # CommandLineToArgvW, CryptProtectMemory

# Black-box smoke test: loads the plugin like Total Commander does and links
# no plugin code.
add_executable(test_plugin_dll tests/test_plugin_dll.c)
