# HadronModule.cmake — generate per-exe resources and add a module built both ways.

# Generate <exe_name>.rc (ICON + manifest + VERSIONINFO) into the binary dir; returns its path.
function(hadron_generate_resources exe_name exe_desc exec_level out_rc_var)
  set(V_MAJOR ${PROJECT_VERSION_MAJOR})
  set(V_MINOR ${PROJECT_VERSION_MINOR})
  set(V_PATCH ${PROJECT_VERSION_PATCH})
  set(EXE_NAME "${exe_name}")
  set(EXE_DESC "${exe_desc}")
  set(EXEC_LEVEL "${exec_level}")
  set(HADRON_ICON "${HADRON_ROOT}/resources/hadron.ico")
  set(GEN_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/${exe_name}.manifest")
  configure_file("${HADRON_ROOT}/cmake/hadron.manifest.in" "${GEN_MANIFEST}" @ONLY)
  set(gen_rc "${CMAKE_CURRENT_BINARY_DIR}/${exe_name}.rc")
  configure_file("${HADRON_ROOT}/cmake/hadron.rc.in" "${gen_rc}" @ONLY)
  set(${out_rc_var} "${gen_rc}" PARENT_SCOPE)
endfunction()

# hadron_add_module(<id> NAME <ExeName> DESC "<text>")
# Builds: static lib mod_<id> (shared by suite + standalone) and the standalone exe <ExeName>.exe.
# Every exe is requireAdministrator (CLAUDE.md hard rule 4).
function(hadron_add_module id)
  cmake_parse_arguments(M "" "NAME;DESC" "" ${ARGN})
  add_library(mod_${id} STATIC "${HADRON_ROOT}/modules/${id}/module.cpp")
  target_link_libraries(mod_${id} PUBLIC hadron_core)

  hadron_generate_resources("${M_NAME}" "${M_DESC}" "requireAdministrator" rc)
  add_executable(${M_NAME} WIN32 "${HADRON_ROOT}/modules/${id}/standalone.cpp" "${rc}")
  target_link_libraries(${M_NAME} PRIVATE mod_${id} hadron_core)
endfunction()
