# Third-party dependencies. Each is taken from the system when a usable
# version is installed, otherwise fetched at a pinned commit (the release tag it is, in a comment). A commit
# cannot be cloned shallow, so these are full clones; the first configure takes longer.

include(FetchContent)

# SDL3: window, input, audio device, Vulkan surface.
find_package(SDL3 3.2 CONFIG QUIET)
if(NOT SDL3_FOUND)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG a8589a84226a6202831a3d49ff4edda4acab9acd # release-3.2.24
    SYSTEM)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

# Vulkan headers and the volk loader (no link-time libvulkan dependency).
FetchContent_Declare(VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG b5c8f996196ba4aa6d8f97e52b5d3b6e70f7e4e2 # vulkan-sdk-1.4.341.0
  SYSTEM)
FetchContent_MakeAvailable(VulkanHeaders)

set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VOLK_HEADERS_ONLY ON CACHE BOOL "" FORCE)
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG d979819fd03de3a7606d6e23d6ff4968942599da # vulkan-sdk-1.4.341.0
  SYSTEM)
FetchContent_MakeAvailable(volk)
target_link_libraries(volk_headers INTERFACE Vulkan::Headers)

FetchContent_Declare(VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG 1d8f600fd424278486eade7ed3e877c99f0846b1 # v3.3.0
  SYSTEM)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

find_package(glm 1.0 CONFIG QUIET)
if(NOT glm_FOUND)
  FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863 # 1.0.1
    SYSTEM)
  FetchContent_MakeAvailable(glm)
endif()

if(HAILSTONE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG 6804767ee637789db8a5cb281381cae98dc36906 # v2.5.2
    SYSTEM)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(doctest)
endif()

# zlib: PFS archive blocks and the SOE session layer.
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
  set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(zlibng
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG 860e4cff7917d93f54f5d7f0bc1d0e8b1a3cb988 # 2.2.4
    SYSTEM)
  FetchContent_MakeAvailable(zlibng)
  add_library(ZLIB::ZLIB ALIAS zlib)
endif()

# Dear ImGui: the temporary developer UI until the SIDL UI engine (M5).
# Built with the SDL3 platform backend and the Vulkan backend on our volk loader.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 97428e8ac99e339ce05eee531cf55b77b29ea709 # v1.91.9
  SYSTEM)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Headers volk::volk_headers)

# Audio decoding and synthesis (src/audio): dr_wav and dr_mp3 (public domain / MIT-0) decode
# the game's WAV and MP3 files; TinySoundFont (MIT) synthesises the XMI music with a SoundFont.
# Single headers: populated only, never added as subprojects.
FetchContent_Declare(dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG fa931f3285ced10ace628f7f1ac951e1951e7ea6 # wav-0.14.5
  SOURCE_SUBDIR hailstone-headers-only
  SYSTEM)
FetchContent_MakeAvailable(dr_libs)
FetchContent_Declare(tinysoundfont
  GIT_REPOSITORY https://github.com/schellingb/TinySoundFont.git
  GIT_TAG 853a0a171759f1ddba0de1442133a75912bbeffa
  SOURCE_SUBDIR hailstone-headers-only
  SYSTEM)
FetchContent_MakeAvailable(tinysoundfont)
add_library(hailstone_audio_headers INTERFACE)
target_include_directories(hailstone_audio_headers SYSTEM INTERFACE ${dr_libs_SOURCE_DIR} ${tinysoundfont_SOURCE_DIR})

# bcdec (MIT): BC7 block decoding, for texture packs on devices without BC formats (src/assets/texture.cpp).
# A single header: populated only, never added as a subproject.
FetchContent_Declare(bcdec
  GIT_REPOSITORY https://github.com/iOrange/bcdec.git
  GIT_TAG 80859ed3b7afb1c527a2a99d70c61457bea72d0c
  SOURCE_SUBDIR hailstone-headers-only
  SYSTEM)
FetchContent_MakeAvailable(bcdec)
add_library(hailstone_bcdec INTERFACE)
target_include_directories(hailstone_bcdec SYSTEM INTERFACE ${bcdec_SOURCE_DIR})

# stb_image (public domain / MIT): JPEG decoding for the pre-game screens' pictures (src/assets/jpeg_read.cpp).
# A single header: populated only, never added as a subproject.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
  SOURCE_SUBDIR hailstone-headers-only
  SYSTEM)
FetchContent_MakeAvailable(stb)
add_library(hailstone_stb INTERFACE)
target_include_directories(hailstone_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# cgltf (MIT): binary glTF reading for replacement models (src/assets/gltf_mesh.cpp, docs/graphics.md "Model
# overrides"). A single header: populated only; its implementation is built here as one C file.
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG 360db1a95480fe102ae9c69b27c5d101167ff5ba # v1.15
  SOURCE_SUBDIR hailstone-headers-only
  SYSTEM)
FetchContent_MakeAvailable(cgltf)
file(WRITE ${PROJECT_BINARY_DIR}/generated/cgltf_impl.c "#define CGLTF_IMPLEMENTATION\n#include <cgltf.h>\n")
add_library(hailstone_cgltf STATIC ${PROJECT_BINARY_DIR}/generated/cgltf_impl.c)
target_include_directories(hailstone_cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
set_target_properties(hailstone_cgltf PROPERTIES LINKER_LANGUAGE C)
target_compile_options(hailstone_cgltf PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)

# Monocypher (BSD-2-Clause or CC0): Ed25519 (with SHA-512, RFC 8032) checks the update manifest's signature
# (core/ed25519.h, docs/updater.md). Populated only: its two C files are built here as one static library.
FetchContent_Declare(monocypher
  GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git
  GIT_TAG 0d85f98c9d9b0227e42cf795cb527dff372b40a4 # 4.0.2
  SOURCE_SUBDIR hailstone-headers-only
  SYSTEM)
FetchContent_MakeAvailable(monocypher)
add_library(hailstone_monocypher STATIC ${monocypher_SOURCE_DIR}/src/monocypher.c
            ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
target_include_directories(hailstone_monocypher SYSTEM PUBLIC ${monocypher_SOURCE_DIR}/src
                           ${monocypher_SOURCE_DIR}/src/optional)
set_target_properties(hailstone_monocypher PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(hailstone_monocypher PRIVATE -w)
endif()

# Recast Navigation (zlib): the navigation meshes and their path queries (src/nav). Populated only: the sources are
# built here as one static library so that DT_POLYREF64 (64-bit polygon references, enough tiles for the largest
# zones) reaches every user of Detour's headers.
FetchContent_Declare(recastnavigation
  GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
  GIT_TAG 6dc1667f580357e8a2154c28b7867bea7e8ad3a7 # v1.6.0
  SOURCE_SUBDIR hailstone-sources-only
  SYSTEM)
FetchContent_MakeAvailable(recastnavigation)
file(GLOB hailstone_recast_sources
  ${recastnavigation_SOURCE_DIR}/Recast/Source/*.cpp
  ${recastnavigation_SOURCE_DIR}/Detour/Source/*.cpp
  ${recastnavigation_SOURCE_DIR}/DetourCrowd/Source/*.cpp)
add_library(hailstone_recast STATIC ${hailstone_recast_sources})
target_include_directories(hailstone_recast SYSTEM PUBLIC
  ${recastnavigation_SOURCE_DIR}/Recast/Include
  ${recastnavigation_SOURCE_DIR}/Detour/Include
  ${recastnavigation_SOURCE_DIR}/DetourCrowd/Include)
target_compile_definitions(hailstone_recast PUBLIC DT_POLYREF64)
# Detour lays a tile's sections out on 4-byte boundaries (dtAlign4), so with 64-bit references a tile's dtLink array
# (8-byte aligned by type) can start on a 4-byte one. x86-64 and arm64 load such data fine; the sanitizer build stops
# on the first report, so its alignment check is off for Detour's own sources only.
if(HAILSTONE_SANITIZE AND NOT MSVC)
  target_compile_options(hailstone_recast PRIVATE -fno-sanitize=alignment)
endif()

# bc7enc_rdo (MIT): BC7 (bc7enc) and BC4/BC5 (rgbcx) encoders for tools/texpack's packs; not the client's.
FetchContent_Declare(bc7enc_rdo
  GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
  GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
  SOURCE_SUBDIR hailstone-sources-only
  SYSTEM)
FetchContent_MakeAvailable(bc7enc_rdo)
add_library(hailstone_bc7enc STATIC ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp)
target_include_directories(hailstone_bc7enc SYSTEM PUBLIC ${bc7enc_rdo_SOURCE_DIR})

# CEF (the Chromium Embedded Framework, BSD): the in-game browser's helper (tools/hailstone-web), with HAILSTONE_BROWSER.
# Spotify's minimal binary builds, pinned with their SHA-1 (https://cef-builds.spotifycdn.com/index.json). Populated
# only: tools/hailstone-web/CMakeLists.txt uses its cmake/ and libcef_dll/.
if(HAILSTONE_BROWSER)
  set(HAILSTONE_CEF_VERSION "156.0.14+g4fa4ab5+chromium-156.0.8078.48")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(hailstone_cef_platform linux64)
    set(hailstone_cef_sha1 d091c82a8655113a34616c08cb340bf92310298d)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(hailstone_cef_platform linuxarm64)
    set(hailstone_cef_sha1 7907f1d0319a8d920be9b3ba54c0c60e2d22a7fb)
    # CEF's cmake/cef_variables.cmake takes the architecture from CMAKE_HOST_SYSTEM_PROCESSOR, which Linux reports as
    # aarch64, not arm64, and then compiles libcef_dll_wrapper with -m64 -march=x86-64.
    set(PROJECT_ARCH arm64)
  elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64)$")
    set(hailstone_cef_platform windows64)
    set(hailstone_cef_sha1 72dc1dd1636acc2391609fa6f410aa09f09168d4)
  else()
    message(STATUS "No CEF build for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}: HAILSTONE_BROWSER is off")
    set(HAILSTONE_BROWSER OFF CACHE BOOL "" FORCE)
  endif()
endif()
if(HAILSTONE_BROWSER)
  string(REPLACE "+" "%2B" hailstone_cef_url_version "${HAILSTONE_CEF_VERSION}")
  FetchContent_Declare(cef
    URL "https://cef-builds.spotifycdn.com/cef_binary_${hailstone_cef_url_version}_${hailstone_cef_platform}_minimal.tar.bz2"
    URL_HASH SHA1=${hailstone_cef_sha1}
    SOURCE_SUBDIR hailstone-not-a-subproject
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(cef)
endif()

# MoltenVK (Apache-2.0): Vulkan on Metal. macOS has no Vulkan of its own, so a build ships this beside the client
# (platform/paths.h bundled_vulkan_library). The release's prebuilt universal dylib, pinned with its SHA-256.
# HAILSTONE_MOLTENVK=OFF leaves it out, for a build run against an installed Vulkan SDK.
if(APPLE)
  option(HAILSTONE_MOLTENVK "Ship MoltenVK beside the client (macOS)" ON)
endif()
if(APPLE AND HAILSTONE_MOLTENVK)
  FetchContent_Declare(moltenvk
    URL https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
    URL_HASH SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
    SOURCE_SUBDIR hailstone-not-a-subproject
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(moltenvk)
  set(HAILSTONE_MOLTENVK_DYLIB ${moltenvk_SOURCE_DIR}/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib)
  set(HAILSTONE_MOLTENVK_LICENSE ${moltenvk_SOURCE_DIR}/LICENSE)
endif()
