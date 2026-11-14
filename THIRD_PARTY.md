# Third-party code and references

Hailstone itself is GPL-2.0-or-later, its plugin SDK (`sdk/`) MIT (NOTICE.md). Everything listed here keeps its own
licence; all of it is compatible with GPL version 2 except MoltenVK (Apache-2.0), which is not compiled in: it is a
separate library shipped beside the macOS binary, with its licence.

## Libraries

| Library | Licence | Use |
|---|---|---|
| SDL3 | zlib | window, input, audio device, Vulkan surface |
| Vulkan-Headers | Apache-2.0 / MIT | Vulkan API |
| volk | MIT | Vulkan loader |
| MoltenVK (github.com/KhronosGroup/MoltenVK, release v1.4.2, the prebuilt `MoltenVK-macos.tar`) | Apache-2.0 | Vulkan on Metal, macOS only: shipped beside the client as `libMoltenVK.dylib`, with `MoltenVK-LICENSE.txt` |
| Vulkan Memory Allocator | MIT | GPU memory |
| glm | MIT (Happy Bunny) | maths |
| zlib | zlib | PFS inflate; PNG writer in tools |
| doctest | MIT | tests only |
| dr_wav, dr_mp3 (github.com/mackron/dr_libs, tag wav-0.14.5) | public domain or MIT No Attribution | WAV and MP3 decoding (`src/audio/pcm.cpp`) |
| TinySoundFont (github.com/schellingb/TinySoundFont, commit 853a0a1) | MIT | SoundFont synthesis of the XMI music (`src/audio/xmi_player.cpp`) with a General MIDI SoundFont |
| stb_image (github.com/nothings/stb, commit 2c980bb) | public domain / MIT | JPEG pictures on the pre-game and loading screens (`src/assets/jpeg_read.cpp`) |
| Lua 5.4.7 (`third_party/lua`) | MIT | the Lua UI's scripting (`src/ui/lua`) |
| Recast Navigation (github.com/recastnavigation/recastnavigation, tag v1.6.0) | zlib | navigation meshes and path queries (`src/nav`) |
| cgltf (github.com/jkuhlmann/cgltf, v1.15) | MIT | binary glTF reading for replacement models (`src/assets/gltf_mesh.cpp`) |
| bcdec (github.com/iOrange/bcdec, commit 80859ed) | MIT or Unlicense | BC7 block decoding for texture packs on devices without BC formats (`src/assets/texture.cpp`) |
| AMD FidelityFX FSR 1 (`third_party/fsr1`: `ffx_a.h`, `ffx_fsr1.h` from github.com/GPUOpen-Effects/FidelityFX-FSR, commit a21ffb8) | MIT | the render scale's upscale and sharpening (`shaders/fsr_easu.frag`, `fsr_rcas.frag`) |
| CEF, the Chromium Embedded Framework (cef-builds.spotifycdn.com, 156.0.14+g4fa4ab5+chromium-156.0.8078.48, minimal binary builds) | BSD-3-Clause; Chromium's own components under their licences, listed in the shipped `browser/CREDITS.html` | the in-game browser's helper (`tools/hailstone-web`, docs/browser.md), a program of its own: the client never links it. `browser/CEF-LICENSE.txt` and `browser/CREDITS.html` are installed beside it |
| Monocypher (github.com/LoupVaillant/Monocypher, tag 4.0.2) | BSD-2-Clause or CC0-1.0 | Ed25519 signatures of the update manifest (`src/core/ed25519.cpp`, docs/updater.md) |
| libcurl (the system's own: `libcurl.so.4`, `/usr/lib/libcurl.4.dylib`) | curl (MIT/X-style) | the updater's HTTPS on Linux and macOS (`src/platform/http_get.cpp`), loaded at run time; not shipped, and only its API's names and constant values (from `curl/curl.h`) are in the source |
| WinHTTP (part of Windows) | system library | the updater's HTTPS on Windows (`src/platform/http_get.cpp`) |
| bc7enc_rdo (github.com/richgel999/bc7enc_rdo, commit b943862: `bc7enc`, `rgbcx`) | MIT or public domain | BC7/BC5/BC4 encoding in `src/texpack` (`tools/texpack`, and Hailstone Texture Studio, which builds against this repository), not the client |

`tools/texpack/upscale.py` runs outside the build on the player's machine. It uses PyTorch (BSD-3-Clause) and spandrel (MIT)
from PyPI, and the PBRify models (github.com/Kim2091/PBRify_Remix, CC0-1.0), which the player downloads. None of
these is shipped, and neither are the packs they build.

## Project art

| File | Made by | Use |
|---|---|---|
| `icons/hailstone.png`, `icons/hailstone-crest.png` | gennro, with Google Gemini (cut out and resized for the icon) | the window and taskbar icon |
| `pictures/hailstone.jpg` | gennro, with Google Gemini | the default picture behind the pre-game and loading screens |

Both are the project's own, made for Hailstone, and ship under the repository's licence. Neither is taken from the
original game.

## References

- Fellowship transport (`src/net/fellowship_packets.*`, `game_connection_fellowship.cpp`) uses EQEmu PR #5134 "TOB: Fellowships" (github.com/EQEmu/Server, open and unmerged, GPL-3.0): its `common/fellowship.h`, `zone/fellowship.cpp` and RoF2 profile encoder, as protocol references only. No server code is copied. Stock EQEmu does not send RoF2 fellowship packets. Native request fields and UI behavior were independently checked against the original client; the 64-member array format and the 1080-byte invitation are documented extensions beyond RoF2.
- **OpenEQ** (github.com/daeken/OpenEQ, © daeken), declared `MIT OR Apache-2.0` in its `Cargo.toml`. Its research docs are used as a spec for asset formats, rendering, the sky and audio. Any code here written after studying its implementation must say so in a comment and be listed below.
- **EQEmu** (github.com/EQEmu/Server, GPL-3.0). Its RoF2 packet structs and opcodes are used as a protocol reference, and its session layer (`common/net/reliable_stream_connection.cpp`, `crc32.cpp`) and login server (`loginserver/client.cpp`, `login_types.h`, `world_server.cpp`, `login_opcodes_sod.conf`) as the spec for `src/net/session*.cpp` and `src/net/login.cpp` (`docs/protocol/`). No EQEmu code is copied. EQEmu zone-utilities (GPL-2.0) and the server's `zone/map.cpp` were read only for the WLD placement rotation order and signs (`docs/formats/wld.md`).
- **EQEmu PEQ database** (github.com/ProjectEQ/peqdatabase): `src/game/shield_models.inc` lists item model numbers (`items.idfile`) of its shield items (`itemtype` 8), counted by `tools/gen_shield_models.py`, and of the ornaments (`itemtype` 54) whose model in the player's game folder is shaped like a shield (measured when the table is made; nothing from the game files is kept). Only the model numbers are kept; no other item data.
- **Housing and marketplace research headers** (`Housing.h`, `HousingNpc.h`, `Market.h` of NMS-Release-Extras, MacroQuest-derived, GPL-2.0): the real estate opcodes and layouts, used as a spec and checked against the original client (`docs/protocol/rof2-packets.md` "Real estate"). No code is copied.
- **libeq** (github.com/cjab/libeq, MIT) and **LanternExtractor** (github.com/LanternEQ/LanternExtractor, MIT): WLD fragment layouts and the render-method table, used as a spec (`docs/formats/wld.md`). No code is copied.
- **LanternExtractor** data: `tools/data/lantern_animationsources.txt` is an unchanged copy of `LanternExtractor/ClientData/animationsources.txt` (MIT, © 2024 LanternEQ), the table of which character models borrow animations; `tools/gen_race_models.py` turns it into `src/game/race_models.inc`.
- **Sky**: the sky draw state (no fog, no culling, no depth write) follows OpenEQ `docs/SKY_DOME_DRAW_STATE.md`, used as a spec only; no OpenEQ sky code is ported. See `docs/formats/sky.md`.
- **OpenEQ audio notes** (`docs/AUDIO_PLAN.md`, `AUDIO_RUNTIME.md`, `CLASSIC_AMBIENT_LEVELS.md`, `CLASSIC_AMBIENT_COOLDOWNS.md`, `CLASSIC_AMBIENT_CLOCK_LIFECYCLE.md`, `XMI_NATIVE_SELECTION.md`): read as a second source for the EFF/EMT layouts, the classic emitter levels and cooldowns and the Miles XMI sequencer (note slots, loop controllers). The audio code was written independently (`docs/formats/audio.md`); no OpenEQ audio code was studied or ported.


### Code derived from OpenEQ

- `src/assets/character.cpp`: the classic character clip assembly (clip codes found by bone-track name, donor clips used only when a model has no track of its own for a clip, rest-pose fallback per bone, 100 ms default frame time), rigid one-bone-per-vertex skinning and the bottom-up character BMP rows were written after studying `crates/openeq-assets/src/character.rs` and `docs/CHARACTER_RENDERING.md`.
- `src/assets/wld.cpp`, `src/assets/wld_zone.cpp`: the WLD fragment parser and the skeletal rest-pose assembly were written after studying `crates/openeq-assets/src/wld.rs` and `docs/WLD_*.md` (bounded fragment readers, the 0x30 texture-reference position, positional material slots, the unsigned packed track scale, rest-pose composition).
- `src/assets/regions.cpp`, `src/assets/wld.cpp` (0x21/0x29): the BSP region fragment layouts (0-based 0x29 indices vs 1-based BSP leaves, the tag in the payload or the name) and the walk were written after studying `crates/openeq-assets/src/{bsp_regions.rs,zone_lines.rs,liquid_regions.rs}` and `docs/ZONE_TRAVEL_PROTOCOL.md`.
- `src/net/login.cpp`, `src/net/session.cpp`: the login packet order and sequence numbers (play request sequence 5, an 80-byte encrypted login reply with the key at offset 12) follow `crates/openeq-net/src/login.rs`; adopting a SessionResponse that arrives from another port follows `stream.rs`. The session layer itself was written from EQEmu's server code.
- `src/net/world_packets.cpp`, `src/net/zone_packets.cpp`: written from EQEmu's `rof2.cpp`/`rof2_structs.h`; `crates/openeq-net/src/world.rs` and `zone.rs` were studied to cross-check the 464-byte login record, the 274-byte character-select tail and the `OP_ClientUpdate` field offsets.
- `src/assets/eqg.cpp`, `src/assets/eqg_zone.cpp`, `src/assets/regions.cpp` (`from_boxes`): the EQG parsers (TER/MOD/ZON v1–2, the EQTZP heightmap DAT/ECO/TOG/water readers) and the zone assembly (terrain drawn untransformed, the first-same-name material rule, triangle flag bit 0 as passable and material -1 as invisible but solid, the light position swap, the SHORT2 terrain UV rule, the quad-diagonal flag, terrain-relative object Z, the tile texture bake by ecosystem layer and mask, the region box matrix) were written after studying `crates/openeq-assets/src/{zone.rs,terrain.rs,terrain/bake.rs,terrain/regions.rs,loader.rs,loader/eqg_collision.rs,loader/ter_uv.rs}` and `docs/{EQGZ_NATIVE_REGIONS,EQG_TER_UV_PACKING,EQG_MATERIAL_SURVEY,HEIGHTMAP_FORMAT}.md`. See `docs/formats/eqg.md`.
- `src/net/item_packets.cpp`: written from EQEmu's `rof2.cpp` `SerializeItem` and `rof2_structs.h`; `crates/openeq-net/src/inventory.rs` was studied to cross-check the item block sizes (77-byte header, 255/74/76-byte bodies, 30-byte spell blocks, 171-byte tail) and the bag-content index rule. The drag-item icon grid in `src/ui/item_windows.cpp` (icon 500 = first cell of dragitem1.dds, vertical grid) agrees with `crates/openeq/src/gameplay_ui.rs` and `crates/openeq-ui/src/document.rs`.
- `src/assets/character_dress.cpp`, `src/assets/eqg_character.cpp`: character appearance and EQG character models were written after studying `crates/openeq-assets/src/character/{appearance.rs,luclin.rs,modern.rs,equipment.rs}` and `docs/CHARACTER_RENDERING.md`: the classic texture naming (variant digits, the face as the head piece's tens digit, robes 10–16 → `<code>01` body mesh and CLK04–CLK10, helm meshes `<code>HE<nn>`), item models on R_POINT/L_POINT/SHIELD_POINT (shield chosen by texture name), the Luclin layered materials (`<code><part><selector><piece>_MDF`, layer compositing with clothing-only tint) and modular parts (part numbers, anchors and bone remapping), the EQGS/EQGM/EQGA layouts (conjugated quaternions, four bone weights, inverse bind matrices) and the EQG body/head piece choice.).
- `src/game/combat.cpp` `animation_code()`: the OP_Animation action → clip code table is ported from `crates/openeq-render/src/actors.rs` (`animation_code`). The respawn-window layout in `src/net/zone_packets.cpp` (including EQEmu's zero padding) follows `docs/DEATH_RECOVERY_PLAN.md`, checked against the server's `SendRespawnBinds`; held-P02 sitting in `src/render/actor_renderer.cpp` follows `docs/CHARACTER_RENDERING.md`.
- `src/net/game_connection_commerce.cpp`, `src/net/merchant_packets.cpp`, `src/net/trade_packets.cpp`: written from EQEmu's `rof2_structs.h`/`client_packet.cpp`; OpenEQ `docs/COMMERCE_PROTOCOL.md`, `docs/TRADE_PROTOCOL.md` and `docs/QUEST_HANDIN_PLAN.md` were read to cross-check the packet sizes, the buy flow without OP_MoneyUpdate, the trade-view slot numbering and the cursor queue. No OpenEQ code was studied or ported.

### Code copied from MacroQuest

MacroQuest (github.com/macroquest/macroquest), Copyright (C) 2002-present MacroQuest Authors, GNU General Public
License version 2.

- `src/game/spa_ids.h`: the `eEQSPA` spell effect ids (0..526) and the `eEQSPELLCAT` spell categories (1..176) are
  copied from `src/eqlib/include/eqlib/game/Spells.h` (without its deprecated and alias names), and `SPA_NAMES` 0..520
  from `src/eqlib/include/eqdata/spelleffects.h`. The ids 527 and 528 and the names 521..528 were added in Hailstone.
- `src/game/world_state.h`: the race ids (`Race`) and the player race constants (`EQR_*`), the con colour, stand
  state and spawn kind macros, the body type names (`szBodyType`), `eSpawnType`, `CharacterProperty`, `PlayerClass`,
  and `Spawn::IsNamed`, `GetSpawnType`, `GetTypeDesc` and `GetRaceIDName` follow MacroQuest's eqlib constants and
  `src/main/MQ2Utilities.cpp` almost line for line. `IsNamed` was changed in Hailstone and mixes MacroQuest's older
  and newer tests.

### MacroQuest used as a specification

- `src/mq/spawn_search.cpp`: the spawn search words and matching rules follow MacroQuest's `ParseSearchSpawn`,
  `ParseSearchSpawnArgs`, `SpawnMatchesSearch`, `IsNamed`, `IsInGroup`, `IsInRaid`, `IsPCNear`, `CleanupName` and
  `NthNearestSpawn` (`src/main/MQ2Utilities.cpp`) and its light names (`szLights`). The code was written for
  Hailstone; none is copied.
- `src/mq/calc.cpp` (Calculate, FastCalculate, EvaluateRPN, GetArg, GetIntFromString in `MQ2Utilities.cpp` and
  `include/mq/base/String.h`), and the `Math`, `If` and `Range` TLOs and the int, float and string members in
  `src/mq/tlo.cpp` and `src/mq/tlo_game.cpp` (`MQ2MathType.cpp`, `MQ2Data.cpp`, `MQ2BasicTypes.cpp`): written for
  Hailstone from MQ's behaviour; none is copied.
