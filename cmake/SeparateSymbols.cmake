# Debug information for the shipped binaries, kept out of what ships (docs/hardening.md "Crash reports").
#
# With HAILSTONE_SEPARATE_SYMBOLS on, a Release (or MinSizeRel) build compiles with debug information and writes each
# shipped binary's symbols to build/symbols/: on Linux <binary>.debug (objcopy --only-keep-debug; the binary itself is
# then stripped of its debug sections and points at the file with a .gnu_debuglink), with MSVC <binary>.pdb and a
# copy of the binary. The install, and so the update zip (tools/make_update.py), holds none of them. The release
# builds turn it on (tools/docker/release_options.sh); tools/docker/build.sh keeps build/symbols/ per version.
#
# Debug information does not change the code GCC, Clang or MSVC generate, so the shipped binary is the one a plain
# Release build makes, apart from the build ID, the debug link and, on Windows, the PDB's name in the header.
# Linux keeps line tables only (-g1, Clang's -gline-tables-only): function names, inlining and source lines, which
# is what a crash report's stack needs, at a fraction of full -g's size and link time.
option(HAILSTONE_SEPARATE_SYMBOLS "Release builds: write the shipped binaries' debug information to build/symbols/" OFF)
set(HAILSTONE_SYMBOLS_DIR ${CMAKE_BINARY_DIR}/symbols)

# Every ELF binary carries a GNU build ID (most distributions' compilers add it anyway): a crash report names it
# (platform/crash_log.cpp) and tools/symbolize_crash.py picks the matching symbol file by it.
if(UNIX AND NOT APPLE)
  add_link_options(-Wl,--build-id)
endif()

if(HAILSTONE_SEPARATE_SYMBOLS)
  if(MSVC)
    # /Z7 keeps each object's debug information in the object (no PDB shared between parallel compiles); the linker
    # writes the program's PDB. CMP0141 (cmake_minimum_required 3.25) makes this the way to choose it.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<IF:$<CONFIG:Release,MinSizeRel>,Embedded,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
    # /DEBUG turns /OPT:REF and /OPT:ICF off unless they are named, and the header would name the PDB by its full
    # path on the build machine: %_PDB% is its file name alone.
    add_link_options("$<$<CONFIG:Release,MinSizeRel>:/DEBUG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO;/PDBALTPATH:%_PDB%>")
  elseif(APPLE)
    message(STATUS "HAILSTONE_SEPARATE_SYMBOLS does nothing on macOS yet")
  else()
    if(NOT CMAKE_OBJCOPY)
      message(FATAL_ERROR "HAILSTONE_SEPARATE_SYMBOLS needs objcopy")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(_hailstone_line_tables -gline-tables-only)
    else()
      set(_hailstone_line_tables -g1)
    endif()
    add_compile_options("$<$<AND:$<CONFIG:Release,MinSizeRel>,$<COMPILE_LANGUAGE:C,CXX>>:${_hailstone_line_tables}>")
  endif()
  # The version the symbols belong to, for tools/docker/build.sh's per-version folder.
  file(MAKE_DIRECTORY ${HAILSTONE_SYMBOLS_DIR})
  file(WRITE ${HAILSTONE_SYMBOLS_DIR}/version.txt "${HAILSTONE_RELEASE_VERSION}\n")
endif()

# Moves a shipped binary's debug information to build/symbols/ (see above); nothing when the option is off or the
# build has none to move (not Release or MinSizeRel; macOS).
function(hailstone_separate_symbols target)
  if(NOT HAILSTONE_SEPARATE_SYMBOLS OR APPLE)
    return()
  endif()
  set(_release "$<CONFIG:Release,MinSizeRel>")
  if(MSVC)
    set_target_properties(${target} PROPERTIES PDB_OUTPUT_DIRECTORY ${HAILSTONE_SYMBOLS_DIR}$<0:>)
    # The binary beside its PDB: a debugger reading a minidump wants both.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "$<${_release}:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_FILE:${target}>;${HAILSTONE_SYMBOLS_DIR}>"
      COMMAND_EXPAND_LISTS VERBATIM)
  else()
    set(_debug "${HAILSTONE_SYMBOLS_DIR}/$<TARGET_FILE_NAME:${target}>.debug")
    # The .debug file's sections compressed, about a third of the size: addr2line, llvm-symbolizer and gdb read it so.
    set(_compress --compress-debug-sections=zlib)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "$<${_release}:${CMAKE_OBJCOPY};--only-keep-debug;${_compress};$<TARGET_FILE:${target}>;${_debug}>"
      COMMAND "$<${_release}:${CMAKE_OBJCOPY};--strip-debug;--add-gnu-debuglink=${_debug};$<TARGET_FILE:${target}>>"
      COMMAND_EXPAND_LISTS VERBATIM)
  endif()
endfunction()

# The same for a shared library made in another directory (SDL3 fetched), which add_custom_command(TARGET) cannot
# reach: at install, its symbols go to build/symbols/ and the installed copy (in <destination>) is stripped of them.
# MSVC: its PDB goes to build/symbols/ (set_target_properties reaches any directory's target).
function(hailstone_separate_symbols_at_install target destination)
  if(NOT HAILSTONE_SEPARATE_SYMBOLS OR APPLE)
    return()
  endif()
  if(MSVC)
    set_target_properties(${target} PROPERTIES PDB_OUTPUT_DIRECTORY ${HAILSTONE_SYMBOLS_DIR}$<0:>)
    return()
  endif()
  set(_debug "${HAILSTONE_SYMBOLS_DIR}/$<TARGET_FILE_NAME:${target}>.debug")
  set(_compress --compress-debug-sections=zlib)
  set(_installed "\${CMAKE_INSTALL_PREFIX}/${destination}/$<TARGET_FILE_NAME:${target}>")
  install(CODE "if($<CONFIG:Release,MinSizeRel>)
    execute_process(COMMAND \"${CMAKE_OBJCOPY}\" --only-keep-debug ${_compress}
                            \"$<TARGET_FILE:${target}>\" \"${_debug}\" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND \"${CMAKE_OBJCOPY}\" --strip-debug \"--add-gnu-debuglink=${_debug}\" \"${_installed}\"
                    COMMAND_ERROR_IS_FATAL ANY)
  endif()")
endfunction()
