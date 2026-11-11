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
  core/host/cmdline.cpp
  core/host/host.cpp)
target_link_libraries(hadron_core PUBLIC
  dwmapi dbghelp shell32 ole32 gdi32 user32 comdlg32 comctl32)

include(cmake/HadronModule.cmake)

# --- modules (each builds a static lib + its standalone exe) -------------------------------------
hadron_add_module(hello NAME HadronHello DESC "Hadron Hello module")

# --- suite --------------------------------------------------------------------------------------
# registry.cpp is compiled into the suite exe (not core) so the module factories resolve without a
# circular static-library dependency.
hadron_generate_resources("Hadron" "Hadron suite" "requireAdministrator" HADRON_SUITE_RC)
add_executable(Hadron WIN32
  app/hadron.cpp
  core/host/registry.cpp
  "${HADRON_SUITE_RC}")
target_link_libraries(Hadron PRIVATE mod_hello hadron_core)
