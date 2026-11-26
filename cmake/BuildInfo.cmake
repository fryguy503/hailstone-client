# Run at every build (the hailstone_build_info target): writes OUT, the build's version line for core/build_info.cpp
# ("0.1.0-alpha (f6a07d7)", "-dirty" with uncommitted changes), the release and display versions, and the build
# date (20261123, local time; SOURCE_DATE_EPOCH when set). The display version does not affect update comparisons.
set(display "${VERSION}")
if(NOT "${DISPLAY_VERSION}" STREQUAL "")
  string(REGEX REPLACE "-.*$" "" version_core "${VERSION}")
  string(FIND "${DISPLAY_VERSION}" "${version_core}-" version_prefix)
  if(NOT version_prefix EQUAL 0 OR NOT "${DISPLAY_VERSION}" MATCHES "^[0-9.]+-[A-Za-z0-9]+([.-][A-Za-z0-9]+)*$")
    message(FATAL_ERROR "HAILSTONE_DISPLAY_VERSION must be ${version_core}- followed by ASCII letters and digits separated by hyphens or dots")
  endif()
  set(display "${DISPLAY_VERSION}")
endif()
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE hash
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(failed OR hash STREQUAL "")
  set(line "${display}")
else()
  execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${SRC}"
                  OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT changes STREQUAL "")
    set(hash "${hash}-dirty")
  endif()
  set(line "${display} (${hash})")
endif()
string(TIMESTAMP date "%Y%m%d")
string(CONCAT text
  "#define HAILSTONE_BUILD_VERSION \"${line}\"\n"
  "#define HAILSTONE_RELEASE_VERSION \"${VERSION}\"\n"
  "#define HAILSTONE_DISPLAY_VERSION \"${display}\"\n"
  "#define HAILSTONE_BUILD_DATE ${date}\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
