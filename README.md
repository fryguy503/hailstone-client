<p align="center">
  <img src="docs/images/logo.jpg" alt="Project Hailstone" width="560">
</p>

# Hailstone Client

A portable, open-source client for EQEmu servers, written in C++20 with Vulkan 1.3. It speaks the RoF2 protocol and runs on Linux, Windows and macOS (via MoltenVK).

> **Beta.** Expect rough edges and missing features. Bug reports are welcome.

Hailstone is an independent, educational project: a study of how a game client of its era works (network protocol, file formats, rendering, game rules) by writing one from scratch. It contains no code or assets from proprietary software. You supply your own RoF2-compatible game folder, which Hailstone only reads and never modifies or redistributes.

**Please subscribe to EverQuest.** Hailstone plays on files from an EverQuest installation (RoF2 or current live), which are Daybreak Game Company's work. If you use them, we highly encourage you to subscribe to [EverQuest](https://www.everquest.com). Hailstone is not affiliated with or endorsed by Daybreak Game Company; EverQuest is its trademark.

## What works

- Server list, login, character create/delete, world and zone entry
- Zone and character rendering, with an optional HD texture pack tool
- Movement, targeting, combat, spells, buffs, pets and groups
- Inventory, loot, merchants, trade, barter and the guild bank
- Chat, hot bar, skills and abilities
- Zone sound and music
- Lua UI packs (`modern`, `gamepad`) with full gamepad support
- MacroQuest-style Lua scripting and compiled plugins ([Plugins and Scripts](https://github.com/projecthailstone/hailstone-client/wiki/Plugins-and-Scripts)), with `/nav` and `/stick` on a navigation mesh ([Navigation](https://github.com/projecthailstone/hailstone-client/wiki/Navigation))
- Add-ons from GitHub, with a store on the login screen that shows what each one asks to do ([Add-ons](https://github.com/projecthailstone/hailstone-client/wiki/Add-ons))
- Automatic, signed updates, with release notes on the login screen ([Updater](https://github.com/projecthailstone/hailstone-client/wiki/Updater))
- An in-game browser for the pages a server sends, on Linux and Windows ([Browser](https://github.com/projecthailstone/hailstone-client/wiki/Browser))
- Optional server-side support: client file delivery, cross-zone guild invites, declined group invitations reaching the inviter, quest givers in the Find window and zone connections for the map's zone finder ([server/README.md](server/README.md))

Work in progress and known gaps are tracked in the [plan](https://github.com/projecthailstone/hailstone-client/wiki/Plan), the [to-do list](https://github.com/projecthailstone/hailstone-client/wiki/To-Do) and the [known gaps](https://github.com/projecthailstone/hailstone-client/wiki/Known-Gaps).

## System requirements

| | Minimum |
|---|---|
| **Operating system** | Linux x64: Ubuntu 24.04 or a distribution of the same age or newer (the release is built against its glibc).<br>Windows x64: Windows 10 version 1903 or later.<br>macOS on Apple Silicon (arm64), 13.3 or later: test builds only (below).<br>Intel Macs and Linux or Windows on arm64: build from source (no release packages). |
| **Graphics** | A GPU and driver with Vulkan 1.3 and the features listed below. On macOS, MoltenVK, which comes with Hailstone. |
| **Game data** | Your own RoF2-compatible game folder: a RoF2 install or a current live one. Hailstone never writes to it. |
| **Server** | An EQEmu server that accepts RoF2 clients. |
| **Network** | UDP to the login and world servers. Automatic updates need HTTPS: on Linux and macOS the system's libcurl (macOS always has it). |
| **Input** | Keyboard and mouse, or a gamepad. |

The GPU needs:

- Vulkan 1.3, dynamic rendering and synchronization2;
- descriptor indexing: runtime descriptor arrays, non-uniform sampled image indexing, partially bound and variable-count bindings;
- separate depth and stencil layouts (every Vulkan 1.2 driver has them).

To check a GPU, run `vulkaninfo --summary` and look for `apiVersion` 1.3 or higher, or look it up on [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org). If no GPU qualifies, Hailstone names each GPU and what it lacks in a dialog and in `hailstone/logs/hailstone.log`. GPUs without BC texture support still run (textures are decoded on the CPU).

Memory, VRAM and CPU minimums have not been measured yet.

## Download

Release packages for Linux x64 and Windows x64 are on the [releases page](https://github.com/projecthailstone/hailstone-client/releases). Unzip one anywhere and run `hailstone` (`hailstone.exe` on Windows). Hailstone updates itself from then on.

macOS on Apple Silicon has test builds only, as pre-releases on the same page; each one's notes say how to start it. They are not signed with an Apple Developer ID, so clear the download's quarantine first (`xattr -dr com.apple.quarantine <folder>`).

## Building from source

```sh
git clone https://github.com/projecthailstone/hailstone-client.git
cd hailstone-client
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/hailstone/hailstone
```

On first start Hailstone asks for your game data folder and remembers it. You can also pass `--game-dir PATH` or set `EQ_GAME_DIR`.

Hailstone is portable: settings, per-server files, characters and logs live in the `hailstone/` folder next to the binary ([Folder Layout](https://github.com/projecthailstone/hailstone-client/wiki/Folder-Layout)).

### Build requirements

- CMake 3.25+
- GCC 13+, Clang 17+ or MSVC 2022
- `glslc` (shaderc) or `glslangValidator`
- A Vulkan 1.3 driver (MoltenVK on macOS)
- Linux, when the system has no SDL 3.2 or newer: pkg-config and the development packages SDL needs for Wayland and X11 (the list is in [tools/docker/linux.Dockerfile](tools/docker/linux.Dockerfile)). Configuring stops if SDL would be built without Wayland.

Dependencies are fetched automatically unless the system provides them. The in-game browser (Linux and Windows) is built on CEF, a few hundred MB to fetch; `-DHAILSTONE_BROWSER=OFF` leaves it out, and pages open in the system's browser.

Run tests with `ctest --test-dir build` and see the client's options with `build/hailstone/hailstone --help`.

## Documentation

The documentation is on the [wiki](https://github.com/projecthailstone/hailstone-client/wiki), built from the [docs/](docs/) folder, which is where to change it ([Wiki](https://github.com/projecthailstone/hailstone-client/wiki/Wiki)).

| | |
|---|---|
| [Plan](https://github.com/projecthailstone/hailstone-client/wiki/Plan) | Goals, rules, architecture and milestones |
| [Folder Layout](https://github.com/projecthailstone/hailstone-client/wiki/Folder-Layout) | The Hailstone folder, settings and the game folder |
| [Gamepad](https://github.com/projecthailstone/hailstone-client/wiki/Gamepad) | Gamepad support and the gamepad UI |
| [Add-ons](https://github.com/projecthailstone/hailstone-client/wiki/Add-ons) | Installing add-ons, and publishing one |
| [Plugins and Scripts](https://github.com/projecthailstone/hailstone-client/wiki/Plugins-and-Scripts) | Lua scripts, compiled plugins and their API |
| [Developer UI](https://github.com/projecthailstone/hailstone-client/wiki/Developer-UI) | Writing UI packs |
| [Mechanics](https://github.com/projecthailstone/hailstone-client/wiki/Mechanics) | Game rules and where they were checked |
| [Graphics](https://github.com/projecthailstone/hailstone-client/wiki/Graphics) | The renderer |
| [Protocol](https://github.com/projecthailstone/hailstone-client/wiki/Protocol-Session) | Session, login and RoF2 packet notes |
| [File formats](https://github.com/projecthailstone/hailstone-client/wiki/Format-WLD) | WLD, EQG, textures, characters, particles, sky, audio |
| [CI and Local Builds](https://github.com/projecthailstone/hailstone-client/wiki/CI-and-Local-Builds) | How builds, tests and releases are run |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md), and [AGENTS.md](AGENTS.md) for AI coding tools. Security issues: [SECURITY.md](SECURITY.md). Changes: [CHANGELOG.md](CHANGELOG.md).

## License

[GPL-2.0-or-later](LICENSE). The plugin SDK ([sdk/](sdk/)) is under MIT, and an exception lets plugins, scripts and add-ons use any licence: see [NOTICE.md](NOTICE.md). Releases up to 0.7.0-beta were MIT. Bundled and fetched third-party code keeps its own licenses, listed in [THIRD_PARTY.md](THIRD_PARTY.md).

## Credits

Hailstone stands on the work of many people and projects:

- [EQEmu](https://github.com/EQEmu/Server): the server this client targets, and the protocol reference for RoF2 packets and sessions
- [OpenEQ](https://github.com/daeken/OpenEQ) (daeken): research notes on asset formats, rendering and audio
- [LanternExtractor](https://github.com/LanternEQ/LanternExtractor) and [libeq](https://github.com/cjab/libeq): file format specifications
- [EQEmu documentation](https://docs.eqemu.dev) and [zone-utilities](https://github.com/EQEmu/zone-utilities): format and behaviour references
- [MacroQuest](https://www.macroquest.org): the scripting API that Hailstone's Lua layer is compatible with
- [eqmac](https://github.com/kevrgithub/eqmac) (kevrgithub): symbol research
- [Dear ImGui](https://github.com/ocornut/imgui), [SDL3](https://libsdl.org), [Lua](https://www.lua.org), [volk](https://github.com/zeux/volk), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [glm](https://github.com/g-truc/glm), [zlib](https://zlib.net), [doctest](https://github.com/doctest/doctest), [TinySoundFont](https://github.com/schellingb/TinySoundFont), [dr_libs](https://github.com/mackron/dr_libs), [stb](https://github.com/nothings/stb), [bcdec](https://github.com/iOrange/bcdec), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [Recast Navigation](https://github.com/recastnavigation/recastnavigation), [cgltf](https://github.com/jkuhlmann/cgltf), [Monocypher](https://monocypher.org), [AMD FidelityFX](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), [CEF](https://bitbucket.org/chromiumembedded/cef), [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
- [PBRify Remix](https://github.com/Kim2091/PBRify_Remix) (Kim2091): models for the optional HD texture tool
