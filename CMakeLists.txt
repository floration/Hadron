cmake_minimum_required(VERSION 3.20)
project(Hadron VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(HADRON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include_directories("${HADRON_ROOT}")   # so #include "core/..." resolves from the repo root

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0A00)

if(MINGW)
  add_compile_options(-municode -Wall -Wextra -Wno-unused-parameter)
  add_link_options(-municode -static -static-libgcc -static-libstdc++)
elseif(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # /MT, no runtime DLL (CLAUDE.md)
  add_link_options(/MANIFEST:NO)                     # our .rc owns the manifest
endif()

# --- core ---------------------------------------------------------------------------------------
add_library(hadron_core STATIC
  core/sys/sys.cpp
  core/sys/settings.cpp
  core/sys/log.cpp
  core/sys/png.cpp
  core/ui/theme.cpp
  core/ui/widgets.cpp
  core/host/cmdline.cpp
  core/host/host.cpp)
target_link_libraries(hadron_core PUBLIC
  dwmapi dbghelp shell32 ole32 gdi32 user32 comdlg32 comctl32)

include(cmake/HadronModule.cmake)

# --- modules (each builds a static lib + its standalone exe) -------------------------------------
hadron_add_module(hello NAME HadronHello DESC "Hadron Hello module")
# Dev/reference gallery for core/ui; builds HadronUikit.exe but is not in the suite catalogue.
hadron_add_module(uikit NAME HadronUikit DESC "Hadron core/ui gallery")
hadron_add_module(diskmap NAME HadronDiskMap DESC "Hadron DiskMap: drive-space treemap and folder list")
# The scan engine and rules are separate files so they stay testable headless (docs/design/diskmap.md).
target_sources(mod_diskmap PRIVATE modules/diskmap/scan.cpp modules/diskmap/classify.cpp)
target_link_libraries(mod_diskmap PUBLIC msimg32)   # GradientFill for the treemap blocks (system lib)

hadron_add_module(runas NAME HadronRunAs DESC "Hadron Run As: launch a file as a chosen identity and integrity level")
# launch.cpp is the token/launch logic, split out so it stays testable headless (docs/design/runas.md).
target_sources(mod_runas PRIVATE modules/runas/launch.cpp)
# Windows SDK libraries only (hard rule 3): CredUI for the password prompt, NetApi for account
# enumeration, Userenv for the environment block, shlwapi for AssocQueryString, plus the shell.
target_link_libraries(mod_runas PUBLIC credui netapi32 wtsapi32 userenv shlwapi advapi32)

hadron_add_module(bios NAME HadronBios DESC "Hadron BIOS Settings Explorer: UEFI setup settings, with guarded writes")
# Firmware access, snapshots, the settings.db model and the zip reader are separate files so they stay
# testable headless (docs/design/bios.md). fwwrite.cpp is the one guarded firmware write path (hard rule 2,
# docs/design/bios-write.md); CI fails if the write API appears in any other file.
target_sources(mod_bios PRIVATE modules/bios/db.cpp modules/bios/firmware.cpp modules/bios/snapshot.cpp
                                modules/bios/zip.cpp modules/bios/fwwrite.cpp)
# The embedded settings.db (RCDATA BIOS_SETTINGS). An INTERFACE source, so every exe that links mod_bios
# (HadronBios.exe and Hadron.exe) gets the resource; an .rc object inside a static library would never be
# pulled in by the linker.
target_sources(mod_bios INTERFACE "${HADRON_ROOT}/modules/bios/resources/bios.rc")
set_property(SOURCE "${HADRON_ROOT}/modules/bios/resources/bios.rc" APPEND PROPERTY OBJECT_DEPENDS
             "${HADRON_ROOT}/modules/bios/data/settings.db")
# Windows SDK libraries only (hard rule 3): advapi32 for tokens and the registry, ole32/uuid for the
# folder picker, comdlg32 for the open/save dialogs, shell32 for ShellExecute and the known-folder path,
# msimg32 for AlphaBlend (the dimmed backdrop behind the write plan).
target_link_libraries(mod_bios PUBLIC advapi32 ole32 uuid comdlg32 shell32 msimg32)

# --- suite --------------------------------------------------------------------------------------
# registry.cpp is compiled into the suite exe (not core) so the module factories resolve without a
# circular static-library dependency.
hadron_generate_resources("Hadron" "Hadron suite" "requireAdministrator" HADRON_SUITE_RC)
add_executable(Hadron WIN32
  app/hadron.cpp
  core/host/registry.cpp
  "${HADRON_SUITE_RC}")
target_link_libraries(Hadron PRIVATE mod_hello mod_diskmap mod_runas mod_bios hadron_core)
