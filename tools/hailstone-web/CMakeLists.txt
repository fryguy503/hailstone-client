# The in-game browser's helper (docs/browser.md): CEF's off-screen browser behind the protocol of
# src/web/browser_protocol.h. Its own directory, so CEF's cmake (which resets compiler flags on Windows) stays out of
# the client's targets. It and CEF's files go in build/hailstone/browser/ and an install's browser/.

set(CEF_ROOT ${cef_SOURCE_DIR})
# MSVC: the DLL runtime, as the client's targets (hailstone_web_protocol links in); CEF's default is the static one.
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
set(USE_SANDBOX OFF CACHE BOOL "" FORCE) # no Chromium sandbox yet (docs/browser.md)
list(APPEND CMAKE_MODULE_PATH ${CEF_ROOT}/cmake)
find_package(CEF REQUIRED)
# The minimal builds have Release only (cef_variables.cmake would look under the build type's name).
set(CEF_BINARY_DIR ${CEF_BINARY_DIR_RELEASE})
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
# chrome-sandbox is the Linux sandbox's setuid helper; the helper runs without the sandbox (hailstone_web.cpp).
list(REMOVE_ITEM CEF_BINARY_FILES chrome-sandbox)

set(HAILSTONE_BROWSER_DIR ${HAILSTONE_TREE_DIR}/browser)
add_executable(hailstone-web hailstone_web.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(hailstone-web)
add_dependencies(hailstone-web libcef_dll_wrapper)
target_link_libraries(hailstone-web PRIVATE libcef_dll_wrapper hailstone_web_protocol ${CEF_STANDARD_LIBS})
set_target_properties(hailstone-web PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${HAILSTONE_BROWSER_DIR}$<0:>)
hailstone_separate_symbols(hailstone-web)

if(WIN32)
  target_link_libraries(hailstone-web PRIVATE ${CEF_BINARY_DIR}/libcef.lib)
  target_compile_definitions(hailstone-web PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # A Windows program: no console window when the client starts it (its stdin and stdout are the client's pipes).
  set_target_properties(hailstone-web PROPERTIES WIN32_EXECUTABLE TRUE)
  target_sources(hailstone-web PRIVATE hailstone-web.manifest) # MSVC links a listed .manifest in
else()
  target_link_libraries(hailstone-web PRIVATE ${CEF_LIB_RELEASE})
  # libcef.so beside the helper.
  set_target_properties(hailstone-web PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
endif()

COPY_FILES(hailstone-web "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${HAILSTONE_BROWSER_DIR}")
COPY_FILES(hailstone-web "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${HAILSTONE_BROWSER_DIR}")

install(TARGETS hailstone-web RUNTIME DESTINATION browser)
foreach(f ${CEF_BINARY_FILES})
  if(IS_DIRECTORY ${CEF_BINARY_DIR}/${f})
    install(DIRECTORY ${CEF_BINARY_DIR}/${f} DESTINATION browser)
  else()
    install(PROGRAMS ${CEF_BINARY_DIR}/${f} DESTINATION browser)
  endif()
endforeach()
foreach(f ${CEF_RESOURCE_FILES})
  if(IS_DIRECTORY ${CEF_RESOURCE_DIR}/${f})
    install(DIRECTORY ${CEF_RESOURCE_DIR}/${f} DESTINATION browser)
  else()
    install(FILES ${CEF_RESOURCE_DIR}/${f} DESTINATION browser)
  endif()
endforeach()
# CEF's licence and Chromium's third-party notices (THIRD_PARTY.md).
install(FILES ${CEF_ROOT}/LICENSE.txt RENAME CEF-LICENSE.txt DESTINATION browser)
install(FILES ${CEF_ROOT}/CREDITS.html DESTINATION browser)
if(UNIX AND NOT APPLE)
  # Spotify's libcef.so keeps its symbols (1.4 GB); the installed one is stripped (about 260 MB).
  find_program(HAILSTONE_STRIP NAMES strip)
  if(HAILSTONE_STRIP)
    install(CODE "execute_process(COMMAND ${HAILSTONE_STRIP} --strip-unneeded \"\${CMAKE_INSTALL_PREFIX}/browser/libcef.so\")")
  endif()
endif()
