# External resources for Hailstone

Public projects, specs and data sources that can help build Hailstone, checked on **2026-10-20** (GitHub repositories
through the GitHub API; web pages fetched). eqinterface.com, sourceforge.net and brewall.com block scripted access;
their entries say so.

## How to read the licence column

Hailstone is GPL-2.0-or-later (NOTICE.md), its plugin SDK (`sdk/`) MIT.

| Licence class | What we may do |
|---|---|
| **MIT / BSD / ISC / zlib / CC0 / Unlicense** | Port or vendor the code with credit in `THIRD_PARTY.md`. |
| **GPL-2.0 / GPL-2.0-or-later / LGPL-2.1** | Port the code with credit in `THIRD_PARTY.md`, keeping its notice. A file with GPL-2.0-only code (MacroQuest) says so in its SPDX line. Never into `sdk/`. |
| **Apache-2.0 (only) / GPL-3.0 / AGPL / MPL file-level** | **Reference only** (see the note below). |
| **No licence (none on GitHub)** | All rights reserved by default. We may **read** it, including its docs, for facts. Don't copy code or text. |
| **Restricted custom licence** | Same as no licence, or stricter (see EQ: Requiem). |

Reference-only licences are incompatible with GPL version 2: copying them would bind the client to version 3. Read
them for the format or behaviour (facts can't be copyrighted), then write our own code. A separate Apache-2.0 library
beside the binary, such as MoltenVK, is not copied code.

"Last activity" is the last push. Use: **High** = on a milestone's critical path; **Medium** = second source or
cross-check; **Low** = niche or historical.

---

## 1. Asset formats

### 1.1 PFS / S3D / WLD (classic zones and models)

| Name | URL | Licence | Lang | Last activity | What it covers | Use | Caveats |
|---|---|---|---|---|---|---|---|
| **libeq** (cjab) | https://github.com/cjab/libeq | MIT | Rust | 2026-03 | `libeq_pfs` (read and write S3D/PFS, CRC) and `libeq_wld`, a full WLD parser with **49 fragment-type modules** that round-trip (parse and serialise). Tools `s3d` and `wld-cli`. eq-client and eqoxide build on it. | **High (M1, M4)**. Cleanest permissive WLD implementation; best port source for fragment layouts. `wld-cli` dumps fragments for testing. | No EQG support upstream. Later-era zone WLDs fail its strict parsing (see the djhenry fork). |
| libeq fork (djhenry) | https://github.com/djhenry/libeq | MIT (fork) | Rust | 2026-10 | Adds `libeq_eqg` (zone, mesh, lighting), and a `fix/tolerant-zone-wld-parsing` branch that accepts later-era zone WLDs. | Medium (M1, M6). Shows which RoF2-era WLDs break a strict parser. | Young, narrow scope. |
| **LanternExtractor** (LanternEQ) | https://github.com/LanternEQ/LanternExtractor | MIT | C# | 2025-12 | Trilogy-era S3D/WLD extraction to a text intermediate format, OBJ and glTF (see note 1). | **High (M1, M4, M8)**. Best-tested semantics for what fragments *mean*. Its animation-source table is essential for M4. | Trilogy only: no Luclin or EQG ("post-Velious" is future work). |
| LanternUnityTools | https://github.com/LanternEQ/LanternUnityTools | MIT | C# | 2026-09 | Imports Lantern's intermediate files into Unity: shaders for EQ blend modes, sky, doors, instancing. | Medium (M1). How one port mapped EQ's material types. | Unity-specific. |
| LanternGoExtract | https://github.com/tmyhres/LanternGoExtract | MIT | Go | 2026-01 | Go port of LanternExtractor, with a `sound` package that parses `_sndbnk.eff`. | Low to medium. A second reading of Lantern. | Single maintainer. |
| **WLD by Windcatcher** (EQEmu docs) | https://docs.eqemu.dev/client/guides/file-formats/wld-by-windcatcher/ (source: https://github.com/EQEmu/eqemu-docs-v2 `docs/client/guides/file-formats/wld-by-windcatcher.md`) | No licence on the docs repo | Markdown | docs repo 2026-05 | Transcription of Windcatcher's 2004 `wlddoc.pdf`: fragments 0x03–0x37 field by field, with notes and the old vs new 0x36 formats. | **High (M1)**. The canonical spec others cite. | Old; libeq and Lantern corrected errors. Cross-check. |
| WCEmu definitions (EQEmu docs) | https://docs.eqemu.dev/client/wcemu/latest/ | None stated | Markdown | 2026-05 | Text form of every WLD fragment by tool keyword (`ACTORDEF`, `DMSPRITEDEF2`, `HIERARCHICALSPRITEDEF` …), with each field's printf format. This is quail's `.wce` format. | Medium (M1, M4). Field names for our code. | Text form, not byte layout. |
| wld-doc.github.io | https://wld-doc.github.io/ (repo https://github.com/Wld-Doc/wld-doc.github.io) | None | JS + Kaitai | 2025-12 | The True3D/`.wld` family (an early beta variant, Tanarus, Return to Krondor) as **Kaitai Struct** definitions and JS parsers. | Low. Kaitai-generated fuzz tests; origin history. | Pre-release variant; WIP. |
| OpenEQ wiki (old) | https://github.com/daeken/OpenEQ/wiki/WLD | n/a | wiki | old | Header and fragment notes from the earlier OpenEQ generation. | Low. We have OpenEQ's newer docs. | Superseded. |
| **EQEmu zone-utilities** | https://github.com/EQEmu/zone-utilities | GPL-2.0 | C++ | 2024-10 | `pfs`, `s3d_loader`, `wld_fragment`, `s3d_bsp`, `s3d_skeleton_track`, `eqg_loader`, `eqg_model_loader`, **`eqg_v4_loader`** (v4 text zones, terrain tiles), `eqg_terrain`, `eqg_water_sheet`, `eqg_invis_wall`, `eqg_region`. Tools: `azone` (writes `.map`), `awater` (`.wtr`), `map_edit` (`.nav`). | **High as a reference (M1, M6, nav)**. The only C++ code that loads S3D, EQG v1–3 *and* v4 zones. | **GPL**: read, don't copy. Collision geometry only: no materials or shaders. |
| eqsage (knervous) | https://github.com/knervous/eqsage | **None** | JS | 2025-03 | Browser converter, viewer and zone editor. `sage/lib/s3d/*`: WLD, BSP, animation, skeleton, track, particle sprite, sound, lights. `sage/lib/eqg/*`: EQG decoder and encoder, model, skeleton, weight buffer, `v4-zone.js`, `eco`, `tog`, glTF export for v3 and v4. | Medium (M4, M6). Readable cross-check for EQG v4 zones and skeletal export. | No licence: read only. |
| EQZip (Shendare) | https://github.com/Shendare/EQZip | None | C# | 2022-01 | S3D/EQG archive manager (read and write). | Low (tooling). Inspecting archives by hand. | No licence. |
| s3d / s3d_extract (kevr) | https://github.com/kevrgithub/s3d | None | C++ | 2013 | Minimal PFS reader. | Low. | Old, no licence. |
| S3DModelExtracter, ZEQ-API, ZEQClient (Zaela) | https://github.com/Zaela/S3DModelExtracter, https://github.com/Zaela/ZEQ-API, https://github.com/Zaela/ZEQClient | None | C / C++ | 2014–2016 | WLD model extraction, OpenGL drawing of EQ models, an Irrlicht client experiment. | Low. Historical. | No licence. |
| eq-js-zoneloader | https://github.com/wesparish/eq-js-zoneloader | None | JS | 2026-07 | Dependency-free WebGL2 viewer (PFS, inflate, WLD, BMP/DDS decoding, collision), written in about 25 minutes with Claude Code. | Low. Shows the M1 core is small. | No licence; a toy. |

Notes:

1. LanternExtractor covers zone meshes, collision, vertex colours, **BSP regions**, ambient and placed lights, **music and sound instances**, object vertex animation, **skeletal animation**, skins and equipment. `ClientData/animationsources.txt` lists which races borrow animations from which model (e.g. `HUM,ELM`).

### 1.2 EQG family (MOD, MDS, TER, ZON, ANI, LIT, LOD, PRT/PTS, EDD, ECO, TOG, LAY, v4 zones)

| Name | URL | Licence | Lang | Last activity | What it covers | Use | Caveats |
|---|---|---|---|---|---|---|---|
| **quail** (xackery) | https://github.com/xackery/quail | MIT | Go | 2025-07 (**archived**) | The widest EQG coverage we found (see note 1). | **High (M6, also M1, M4)**. MIT readers, portable directly. Our primary EQG source. | Archived "early preview" (its checklist marks v4 zones and parts of S3D incomplete). Active forks (e.g. `dannuic/quail`), none clearly the successor. |
| quail-addon | https://github.com/xackery/quail-addon (archived), https://github.com/dariusuknuis/quail-addon (active fork, 2026-09) | MIT | Python | 2025 / 2026 | Blender import of S3D and EQG files through quail. | Low (tooling). Viewing models when debugging. | — |
| eqgzi (xackery) | https://github.com/xackery/eqgzi (archived) | MIT | C++ | 2025-04 | Zone importer evolved from Zaela's EQGZoneImporter. Writes EQG v1–3 zones (TER, ZON, MOD). | Medium (M6). Permissive C++ that *writes* EQG. | Archived. |
| EQGZoneImporter, EQGModelImporter, EQGWeaponModelImporter (Zaela) | https://github.com/Zaela/EQGZoneImporter (and siblings) | None | C++ | 2014–2021 | The first EQG writers. | Low. | No licence. |
| **EQGZI docs: shader list** | https://docs.eqemu.dev/client/eqgzi/shader-list/ | None | md | 2026-05 | **72 EQG `.fx` shader names**, with example zones and material properties (`e_TextureDiffuse0`, `e_TextureNormal0`, `e_TextureEnvironment0`, `e_fSlide1X/Y`, `e_fFresnelPower`, `e_fWaterColor1` …). | **High (M6)**. Checklist for our EQG shader families. | Names and properties, not the shader maths. |
| ZON versions table (EQEmu docs) | https://docs.eqemu.dev/client/zon-versions/ | None | md | 2026-05 | Each zone's `.zon` version (1, 2 or 4). | Medium (M6). A ready test matrix. | Hand-made list. |
| EQ Particle Editor (Zaela) | https://github.com/Zaela/EQEmuParticleEditor | None | C | 2014 | `EDD` (spell emitter definitions) record layout: about 60 partly named fields (`density`, `emission_delay`, `fadeout_time`, tints, `image_segments`, `rotation_speed` …). | Medium (M9 spell effects). | No licence; many fields unnamed. quail's `edd_read.go` (MIT) covers the same format. |
| EQRaceInventory (Shendare) | https://github.com/Shendare/EQRaceInventory | None | C++ | 2024-04 | Program and **released HTML tables for RoF2**: each race ID's model code, source file (global, `_chr`, EQG) and the zones that load it. | **High (M4)**. Ground truth for RoF2's race-to-model table. | Code unlicensed; tables can be regenerated from a RoF2 asset set. |
| Race Files / IT model files / Model Loading (EQEmu docs) | https://docs.eqemu.dev/client/race-files/, https://docs.eqemu.dev/client/it-model-files/, https://docs.eqemu.dev/client/guides/model-loading/ | None | md | 2026-05 | About 725 race rows (ID, model codes, `_chr` file). IT item model numbers. The `<zone>_chr.txt` and global load-list format (count line, then `model,file`). | Medium (M4). | Gaps (races 35–60 incomplete); no EQG column. |

Notes:

1. quail's `raw/` reads and writes **ani, bmp/dds/png/tga/jpg, dat (fe, iw, wtr, zon), def, eco, edd, lay, lit, lod, mds, mod, obg, prt, pts, rfd, ter, tog, wld, zon (including `zon_read_v4`)**. `raw/rawfrag/` holds every WLD fragment. It also has a `.wce` text round-trip, and tests.

### 1.3 Rendering behaviour docs (EQEmu docs, mostly from LanternEQ research)

| Page | URL | What it gives | Use |
|---|---|---|---|
| Fog System and Clip Plane | https://docs.eqemu.dev/client/guides/fog-system-and-clip-plane/ | Per-level fog start and end formulas for the 21-step clip slider; fog colour scaled by an hourly blend table; the MinFog == MaxFog special case; FogClipMin ≥ 30. | **High (M1)**. |
| Sky System | https://docs.eqemu.dev/client/guides/sky-system/ | 5 sky variants: textures, layer pan speeds and directions, render order, no depth writes, camera-locked placement. | High (M1/M9). |
| Sounds Reference | https://docs.eqemu.dev/client/guides/sounds/ | WAV names by `snd*.pfs` archive (UI, player, environment, mob). | Medium (M8). |

---

## 2. Network protocol

| Name | URL | Licence | Lang | Last activity | What it covers | RoF2? | Use | Caveats |
|---|---|---|---|---|---|---|---|---|
| **EQEmu server: reliable stream** | https://github.com/EQEmu/EQEmu `common/net/reliable_stream_connection.{h,cpp}`, `reliable_stream_structs.h`, `crc32.cpp`, `eqstream.cpp`, `common/eq_stream_ident.cpp` | GPL-3.0 | C++ | 2026-10 | Server side of the reliable-UDP session protocol: session request and response, sequencing, acks, fragments, combined packets, compression, CRC keys, stream identification. | Yes (client-agnostic) | **High (M2)**. What our client must interoperate with. | GPL: reference only. |
| EQEmu RoF2 patch + loginserver | `common/patches/rof2.cpp`, `rof2_structs.h`, `rof2_ops.h`, `rof2_limits.*`, `utils/patches/patch_RoF2.conf`; `loginserver/encryption.cpp`, `login_types.h`, `client.cpp` | GPL-3.0 | C++ | 2026-10 | RoF2 wire contract; login packets and DES credential handling. | Yes | Already in our plan (listed for completeness). | GPL. |
| **eq-network** (eq-p99-tools) | https://github.com/eq-p99-tools/eq-network | **MIT** | Rust | 2026-10 (active daily) | Four crates (see note 1). | **No.** Titanium (EQEmu), P99, TAKP/EQMac. | **High (M2)**. The only MIT, maintained, complete EQ client network stack. Transport and login are version-independent and port almost directly. | Game layer is Titanium/TAKP; its structs don't apply. A capability table records what was tested live. |
| **eqoxide** (djhenry) | https://github.com/djhenry/eqoxide | **None** | Rust (wgpu) | 2026-10 (active daily) | A standalone **RoF2** EQEmu client: login, char select, zone in, movement, combat, casting, merchants, loot, trade, quests, inventory. Renders zones and skinned characters with armour textures, using libeq. Docs: see note 2. | **Yes** | **High as reading (M2–M4, M7)**. The only other RoF2 client we found. Its notes list traps (dead `#if 0` structs in `rof2_structs.h`, ENCODE vs `zone/` serialiser). | **No licence: read only.** Needs its own S3D-to-glTF asset server (`eqoxide_asset_server`, Rust, no licence). Built for AI agents, not players. |
| scry-eq (ShowEQ revival) | https://github.com/scry-eq/showeq (GPL-2.0, C), https://github.com/scry-eq/scry-cpp (GPL-2.0), https://github.com/scry-eq/scry-decoder-rs (GPL-2.0, Rust), https://github.com/scry-eq/scry-qt (MIT) | mixed | C / C++ / Rust | 2026-06 to 2026-09 | ShowEQ split into capture daemon, decoder and Qt/web front ends, using protobuf. | Live servers, not RoF2 | Low to medium. Session-layer decoding; opcode-hunting methods. | GPL core; follows the live game. |
| ShowEQ (classic) | http://www.showeq.net/, mirror https://github.com/lucianposton/showeq | GPL-2.0 | C++ | mirror 2020 | Packet analyser. Its SourceForge SVN tags hold `worldopcodes.xml` / `zoneopcodes.xml` per live patch. | Tags around 2013 are close to RoF2's era | Low. EQEmu's `patch_RoF2.conf` already covers this. | GPL. sourceforge.net blocks scripted access. |
| EQExtractor (EQEmu) | https://github.com/EQEmu/EQExtractor | None | C# | 2014 | Converts pcaps to SQL. **Per-patch decoders and opcode configs for 2010–2014 live patches** (`patch_March13-2013`, `April15-2013` … `May19-2014`), RoF2's era. | Close to RoF2 | Medium (M3). Independent decoders for spawn, door, zone-point and object packets of RoF2's time. | No licence. Patch dates don't match RoF2's exactly. |
| EQPacketParser | https://github.com/daerath/EQPacketParser | MIT | C# | 2019 | Decompresses captures; splits combined and oversized packets. | n/a | Low to medium. For a pcap debugging tool. | — |
| EQNet (Zaela) | https://github.com/Zaela/EQNet | None | C++ | 2015 | Client networking library for EQEmu: ack manager, compression, CRC, login, world, chat, combat, inventory, opcode translation tables for several clients. | Older clients | Low. | No licence. |
| Sanctuary.SoeProtocol | https://github.com/PS2Sanctuary/Sanctuary.SoeProtocol | ISC | C# / Zig | 2026-05 | **Written docs** (`docs/`) and an implementation of the related SOE protocol v3 (PlanetSide 2, H1Z1, Landmark, Free Realms). Links further implementations (LibSOE, soe-dissector, h1z1-server, swg). | Same family as EQ's, different variant | Medium (M2). Best written explanation of the session state machine and windowing. | Not EQ-specific: check CRC, encoding and opcodes against EQEmu. |
| soe-protocol (FabledRealms) | https://github.com/FabledRealmsProject/soe-protocol | MIT | Rust | 2026-07 | Pure state machine with no I/O, ported from Sanctuary. Documents a **16-bit sequence-wraparound bug** in the ack-all throttle. | Same family | Medium (M2). Good design (time passed in, buffers in and out); a bug to avoid. | AI-assisted port of a PS2 variant. |
| soe-dissector | https://github.com/misterupkeep/soe-dissector | GPL-2.0 | Lua | 2020 | Wireshark dissector for the related reliable UDP protocol. | Same family | Low to medium. Debug aid; could be adapted for EQ captures. | GPL (tooling only, so not an issue). |
| Packet and OpCode Analysis (EQEmu docs) | https://docs.eqemu.dev/developer/packet-and-opcode-analysis/ | — | md | — | Capturing and decoding packets; finding opcodes. | — | Low. | No session-layer spec. |

Notes:

1. eq-network's crates:
   - `eq-network-transport`: session framing, sequencing, fragmentation, compression, keyed CRC, retransmit, stats (`soe.rs`, `fragment.rs`, `combined.rs`, `legacy.rs`, `modern.rs`, `crc.rs`);
   - `eq-network-login`: login packets, **DES-CBC with zero key and IV**, server list;
   - `eq-network-game`: world and zone codecs;
   - `eq-network`: full session client (login → server select → char select → zone, reconnect, typed events and commands).
2. eqoxide's docs:
   - `docs/protocol-notes.md` ("This client speaks RoF2", with a provenance tag per section);
   - `docs/rof2-migration.md`: how EQEmu identifies a RoF2 stream (`OP_SendLoginInfo` 0x7a09 at 464 bytes, `OP_ZoneEntry` 0x5089 at **76** bytes);
   - `character-models.md`, `equipment-textures-findings.md`, `collision-system.md`, `zone-rendering.md`.

**Finding:** no complete written spec of the RoF2 session protocol exists. Fullest sources: EQEmu's `reliable_stream_connection` (GPL, server side), eq-network-transport (MIT client), the Sanctuary docs (generic v3).

---

## 3. Other open-source EQ clients and viewers

| Name | URL | Licence | Lang / engine | Last activity | Protocol | State | Lessons and use |
|---|---|---|---|---|---|---|---|
| **eqoxide** | see section 2 | None | Rust, wgpu | 2026-10 | **RoF2** | Plays real characters. Renders zones, skinned races with armour, weapons, doors. EQG zones not yet converted. | Closest to our target protocol. Its docs are worth reading (section 2). |
| **eq-client** (eq-p99-tools) | https://github.com/eq-p99-tools/eq-client | **MIT** | Rust, Bevy | 2026-10 (daily) | Titanium / TAKP / P99 | Native top-down client (see note 1). | **High (M1, M5)**. MIT: its SIDL reader and axis module can be ported. Reads skin data at runtime, as we do. |
| **WillEQ** | https://github.com/wcassis/willeq | GPL-3.0 | C++17, Irrlicht (software, GL 2.1, GLES2) | 2026-10 | Titanium | Login through zoning, and much more (see note 2). Runs on ARM SBCs; the author calls it "mostly vibe-coded". | **High as reference (M1, M4, M8)**. Closest C++ analogue to Hailstone. Facts in `docs/reference/eff_sound_format.md` (84-byte `_sounds.eff` records) and `eq_audio_assets_catalog.md`; its audio stack validates ours. |
| WillEQ sibling "whatever" | https://github.com/wcassis/whatever | None | — | 2025-12 | Titanium | Earlier repo of the same project. | Skip. |
| open-norrath | https://github.com/dashkto/open-norrath | GPL-3.0 | Scala | 2026-03 | EQEmu | Experimental. | Low. |
| **EQ: Requiem** (knervous) | https://github.com/knervous/eqrequiem | **Restricted custom licence** (no reuse, no AI training) | TS, Babylon.js, WebTransport, Go | 2026-08 (archived as read-only) | Its own server | EQ-style browser MMO using EQ assets, with WebGPU paths. | **Avoid.** Licence forbids reuse and ML use; listed so nobody mines it by accident. |
| requiemfiles | https://github.com/knervous/requiemfiles | GPL-2.0 | JS | 2024-12 | — | Asset manifest for Requiem. | Low. |
| OpenEQ | https://github.com/daeken/OpenEQ | MIT OR Apache-2.0 (Cargo.toml; no LICENSE file) | Rust, wgpu | 2026-10 | EQEmu | Already our main secondary source. | — |
| EQWOWConverter | https://github.com/NathanHandley/EQWOWConverter | Custom ("other") | C# | 2026-10 | n/a | Converts EQ zones, creatures and items to WoW 3.3.5 formats from LanternExtractor output. | Low to medium. Independent reading of zone semantics (regions, liquids, sounds, music). |
| eql-atlas | https://github.com/foraern/eql-atlas | None | JS | 2026-09 | — | 3D viewer for EQ map files. | Low. |

Notes:

1. eq-client has an offline viewer, login UI, char select, spellbook and inventory windows, max-FPS handling from `eqclient.ini`, and screenshot tests. `eq-client-axes` defines EQ's mirrored-handedness conversion **in one place**. `eq-client-assets` reads, at runtime from the player's files: **SIDL** (`sidl.rs`, 89 KB: `Ui2DAnimation`, `WindowDrawTemplate`, pieces), sounds, maps, regions and `eqlsstr_us.txt` strings.
2. WillEQ has S3D/WLD zones (PVS, frustum, portal culling), animated textures, sky (60+ types), weather, skeletal models with equipment and tints, inventory, merchants, trading, groups, **spatial audio** (miniaudio + TinySoundFont for XMI, minimp3, dr_wav), `.eff` emitters, creature, door and player sounds, and a Recast navmesh.

The **most complete** projects, in order: eqoxide (RoF2, gameplay), WillEQ (Titanium, broadest features, C++), eq-client plus eq-network (Titanium/TAKP, cleanest design, MIT), and OpenEQ.

---

## 4. UI (SIDL, STML, skins)

| Name | URL | Licence | What it covers | Use | Caveats |
|---|---|---|---|---|---|
| eq-client `sidl.rs` | https://github.com/eq-p99-tools/eq-client/blob/main/crates/eq-client-assets/src/sidl.rs | MIT | Runtime SIDL parser (`Ui2DAnimation`, `WindowDrawTemplate`, borders, gauges, labels, images, screen pieces), reading the player's skin. | **High (M5)**. A permissive port source. | Only the windows that client draws. |
| eqinterface.com | https://www.eqinterface.com/ (e.g. https://www.eqinterface.com/patches/live_3.14.07/EQUI_InspectWnd.html) | — | Archive of EQUI files per patch, skin downloads, forum threads on SIDL attributes such as `EQType`. | Medium (M5). Explains what an attribute is *for*. | Blocks scripted fetches (403). Mostly 2002–2010. |
| SIDL Fiddle | https://github.com/mbarbre1/Fiddle | GPL-3.0 | Java EQ UI designer (a SIDL renderer). | Low to medium. A second SIDL layout implementation. | GPL; last active 2019. |
| EQUIEditor | https://github.com/anotheregostar/EQUIEditor | GPL-3.0 | C# graphical editor for EQ UI XML. | Low. | GPL. |
| DiaWind (EQEmu docs) | https://docs.eqemu.dev/quest-api/diawind/ | — | Server-side markup for popup/dialogue windows; relevant to the STML the server sends. | Low (M7). | — |

**Finding:** beyond eq-client's and OpenEQ's partial readers there is no open, complete SIDL or STML renderer, and no written STML spec.

---

## 5. Audio

| Name | URL | Licence | Lang | Last activity | What it covers | Use | Caveats |
|---|---|---|---|---|---|---|---|
| **xmi2mid (Corsix)** | https://github.com/CorsixTH/CorsixTH `CorsixTH/Src/xmi2mid.cpp` | **MIT** (file header, © 2009 Peter Cawley) | C++ | 2026-10 | XMI (multi-sequence) to Standard MIDI in about 400 lines. | **High (M8)**. Vendor it. | — |
| xmi_to_midi (knervous) | https://github.com/knervous/xmi_to_midi | None at repo level; `xmi.cpp` is Corsix's MIT code | C++ / WASM | 2024-09 | Corsix's converter, adapted through EQEmu Tools (KLS) to **emit each sequence in an EQ `.xmi` as its own MIDI file**. EQ zone XMIs hold several tracks, selected by `xmiIndex`. Example: `qeytoqrg.xmi`. | **High (M8)**. EQ-specific multi-track handling. | Take the code from CorsixTH; this repo's changes are reference. |
| **TinySoundFont + tml.h** | https://github.com/schellingb/TinySoundFont | MIT | C (single header) | 2026-07 | SF2 synthesiser and MIDI file loader. WillEQ uses it for EQ music. | **High (M8)**. | `tml.h` has no XMI support; convert first. |
| miniaudio | https://github.com/mackron/miniaudio | Public domain / MIT-0 (dual) | C | 2026-10 | Cross-platform output, mixing, 3D spatialisation (attenuation, doppler, cones), WAV, MP3 and FLAC decoders. | **High (M8)**. One dependency for output, 3D sound and decoding on all desktops. | Simple spatialiser, which suits EQ. |
| minimp3 / dr_libs | https://github.com/lieff/minimp3 (CC0), https://github.com/mackron/dr_libs (PD/MIT-0) | CC0 / PD | C | 2026 | MP3, WAV and FLAC decoding. | Medium (M8). Only if we skip miniaudio's built-in decoders. | — |
| OpenAL Soft | https://github.com/kcat/openal-soft | LGPL-2.0+ | C++ | 2026-10 | Full 3D audio with HRTF and EFX reverb. | Medium (M8). An option for the EAX-style echo `.emt` `EchoLevel` implies. | LGPL: dynamic linking only. |
| Steam Audio | https://github.com/ValveSoftware/steam-audio | Apache-2.0 | C++ | 2026-03 | HRTF, occlusion, reverb. | Low (later improvement). | Heavy. |
| FluidSynth | https://github.com/FluidSynth/fluidsynth | LGPL-2.1 | C | 2026-10 | Reference-quality SF2 synth. | Medium (M8). Alternative to TSF. | LGPL. |
| WildMIDI | https://github.com/Mindwerks/wildmidi | LGPLv3 (library) / GPLv3 (player) | C | 2026-08 | GUS-patch MIDI synth that **loads XMI directly** (via Ryan Nunn's LGPL code). | Low. | LGPL; GUS patches, not SF2. |
| libADLMIDI | https://github.com/Wohlstand/libADLMIDI | GPL-3.0 / LGPL | C++ | 2026-10 | OPL3 FM synth that reads XMI. | Low. EQ's music targets General MIDI, not OPL. | — |
| **GeneralUser GS** | https://github.com/mrbumpy409/GeneralUser-GS | Custom permissive (v2.0): "use without restriction … feel free to use it in your software projects" | SF2 (about 30 MB) | 2026-02 | General MIDI soundfont with a small memory footprint. | **High (M8)**. Good default soundfont to download on first run. | Author can't vouch for every sample's origin; asks sites not to hot-link. |
| FluidR3_GM | Debian `fluid-soundfont` (https://metadata.ftp-master.debian.org/changelogs/main/f/fluid-soundfont/unstable_copyright) | MIT | SF2 (about 140 MB) | — | General MIDI soundfont by Frank Wen. | Medium (M8). Cleanest licence, but large. | — |
| **Eff2Emt** (Shendare) | https://github.com/Shendare/Eff2Emt (also Eff2EmtGUI) | **CC0** (stated in README) | C++ | 2015 | Converts classic `_sounds.eff` (binary) plus `_sndbnk.eff` (text) into `.emt`; fully decodes the binary records. | **High (M8)**. CC0: port freely. | — |
| `.emt` field list (EQGZI sound how-to) | https://docs.eqemu.dev/client/eqgzi/sound-howto/ (source: `eqemu-docs-v2/docs/client/eqgzi/sound-howto.md`) | — | md | 2026-05 | The CSV columns, with examples: `type, SoundFile, unk, WhenActive (0 always, 1 day, 2 night), Volume, FadeInMS, FadeOutMS, WavLoopType, X, Y, Z, FullVolRadius, MaxAudibleDist, RandomizeLocation, ActivationRange, MinRepeatDelay, MaxRepeatDelay, xmiIndex, EchoLevel, IsEnvSound`. | **High (M8)**. | Community-derived; check against the server's `.emt` files. |
| WillEQ `eff_sound_format.md` | https://github.com/wcassis/willeq/blob/main/docs/reference/eff_sound_format.md | GPL-3.0 (repo) | md | 2026 | `_sndbnk.eff` sections (`EMIT`, `LOOP`, `RAND`) and the **84-byte `_sounds.eff` record**. | Medium (M8). | Facts only; don't copy the text. |
| Eff→Emt forum thread | https://www.eqemulator.org/forums/showthread.php?t=39747 | — | forum | 2015 | Shendare's format notes. | Medium. | — |

**3D sound model:** miniaudio's spatialiser (per-sound min/max distance, simple rolloff) fits the `.emt` fields `FullVolRadius` and `MaxAudibleDist` closely.

---

## 6. Data files

| Item | URL | Licence | What it covers | Use | Caveats |
|---|---|---|---|---|---|
| **EQEmu docs: spells** | https://docs.eqemu.dev/server/spells/ with sub-pages `spell-effect-fields/`, `spell-effect-ids/`, `client-spell-id-limitations/`; schema https://docs.eqemu.dev/schema/spells/spells_new/ | — | Spell effect (SPA) IDs and each one's base/limit/max semantics. `spells_new` has **237 columns** (`id` … `field236`), many marked "Unknown". | **High (M7)**. | Column order matching `spells_us.txt` is implied by the exporter (next row), not stated; confirm there. |
| EQEmu client file exporter | https://github.com/EQEmu/EQEmu `client_files/export/main.cpp` (and `client_files/import/main.cpp`) | GPL-3.0 | Writes `spells_us.txt` (from `spells_new`, `^`-separated), `SkillCaps.txt`, `BaseData.txt` and `dbstr_us.txt` in the exact layouts RoF2 reads. | **High (M7)**. The exact line format server and client must agree on. | GPL, but formats are facts. |
| DBStr types | https://docs.eqemu.dev/server/operation/dbstr-types/ | — | `dbstr_us.txt` type IDs 0–20 (AA title, AA description, spell description, race, class, deity, start city, augment, currency, expansion …) plus RoF's 21–26 and 71. | **High (M5, M7)**. | The line format (`id^type^text^…`) isn't on that page; read the exporter. |
| base_data / skill_caps / db_str schema | https://docs.eqemu.dev/schema/client-files/base_data/ (and `skill_caps/`, `db_str/`) | — | Column meanings for BaseData and SkillCaps. | Medium (M7). | — |
| EQEmu common constants | `common/races.{h,cpp}`, `classes.*`, `eq_constants.h`, `emu_constants.*`, `textures.*`, `light_source.*`, `say_link.*` (item link body format), `skill_caps.*`, `spdat.{h,cpp}` | GPL-3.0 | Race and class tables, equipment texture slots, the **RoF2 item-link body layout** (`say_link.cpp`), SPA enums. | High (M4, M7). | GPL: facts and tables only. |
| Race List / Client Race Inventory / Client Version Bitmasks | https://docs.eqemu.dev/server/npc/race-list/, `…/client-race-inventory/`, `…/player/client-version-bitmasks/` | — | Race IDs and names; which client has which race. | Medium (M4). | — |
| eqspellparser (rumstil) | https://github.com/rumstil/eqspellparser | Apache-2.0 | C# parser for `spells_us.txt` plus `spells_us_str.txt`; SPA-to-English descriptions. | Medium (M7). Its spell-description generator is a useful reference (Apache-2.0 only: read, don't copy). | Follows the **live** layout (strings split into a second file), not RoF2's. Map fields with care. |
| Lucy / spells.eqresource.com / raidloot | https://lucy.allakhazam.com/, https://spells.eqresource.com/, https://www.raidloot.com/spells | — | Browsable spell data with named fields, across history. | Low to medium. Cross-checking field meanings. | Live data, not RoF2. |
| EQEmu Spire | https://github.com/EQEmu/spire | None | TS/Go server editor: `frontend/src/app/constants/eq-race*-constants.ts` and other constants; spell, item and NPC editors with field help. | Medium (M7). Field descriptions in its editors. | No licence. |

---

## 7. Maps and navigation

| Name | URL | Licence | What it covers | Use | Caveats |
|---|---|---|---|---|---|
| In-game map format (`maps/*.txt`, `_1`, `_2` layers) | the `maps/` folder of the asset set; reader: https://github.com/martinlindhe/eqformat_map | reader is MIT (Rust, 2021) | Lines `L x0, y0, z0, x1, y1, z1, r, g, b` and points `P x, y, z, r, g, b, size, label`. X and Y are sign-flipped relative to `/loc`. | **High (M5 map window)**. The MIT reader is a test oracle. | — |
| Brewall's maps | https://github.com/RedGuides/brewall-maps (also the RedGuides resource page) | No licence (map data) | The most-used third-party map pack, more accurate than the defaults. | Low to medium. Players install it; we only read the folder. | Don't bundle it. |
| EQEmu `.map` / `.wtr` / `.nav` | See note 1 | GPL (code); data unlicensed | Server-side collision, line of sight, water regions, navmesh. Documented at https://docs.eqemu.dev/server/maps/. | Medium (M3, M7). Optional client collision or pathing cache; cross-check for our BSP-region water. | GPL loaders: format facts only. |
| Recast/Detour | https://github.com/recastnavigation/recastnavigation (also the EQEmu fork https://github.com/EQEmu/recastnavigation) | zlib | Navmesh build and query. EQEmu's `.nav` files are Detour data. | Low to medium (auto-run and follow features). | — |
| eqmap (kevr), Cartographer, ZlizEQMap, EqTool | https://github.com/kevrgithub/eqmap (none, 2013), https://github.com/Yoshiyuka/Cartographer (MPL-2.0), https://github.com/almx/ZlizEQMap (GPL-3.0), https://github.com/smasherprog/EqTool (MIT, C#, active) | mixed | External map viewers and overlays. | Low. | — |

Notes:

1. EQEmu `.map` / `.wtr` / `.nav` sources:
   - loaders in EQEmu `zone/map.cpp` (V1 and zlib-compressed V2), `zone/water_map{,_v1,_v2}.cpp`, `zone/pathfinder_nav_mesh.cpp` (Detour navmesh);
   - writers in zone-utilities `azone`, `awater`, `map_edit`;
   - data: https://github.com/EQEmu/maps (Perl tooling, 2026-01), https://github.com/Valorith/eqemu-maps, https://github.com/darkhue/EQEmuMaps.

---

## 8. Communities, wikis and other notable items

| Name | URL | Notes |
|---|---|---|
| EQEmu docs | https://docs.eqemu.dev/ (source https://github.com/EQEmu/eqemu-docs-v2, Go/MkDocs, no licence) | The most useful doc site. `docs/client/` holds the WLD spec, fog, sky, sounds, model loading, race files, zon versions, the EQGZI shader list and WCEmu. The old `docs.eqemu.io` didn't respond. |
| EQEmulator forums | https://www.eqemulator.org/forums/ | Format threads ("WLD file format" t=11066, Eff→Emt t=39747, "Anyone interested in client development?" t=33432, RoF2 tracking threads). |
| EQ Resource Center | https://www.eqresource.com/ | Player-facing data (spells, zones, races). Cross-checking only. |
| RedGuides | https://www.redguides.com/ | MacroQuest community; hosts Brewall's maps. Many RoF2/EQEmu hook projects. |
| MacroQuest / eqlib | https://github.com/macroquest/macroquest (GPL-2.0), https://github.com/macroquest/eqlib (no licence) | Structure and offset reference for the RoF2 client. |
| Rof2 client hook projects | https://github.com/CerveloFellow/rof2client-dinput8, https://github.com/knervous/eqlib (RoF2 hooks) | Small RoF2 client patches that record known client bugs. Low. |
| eqaainfo (kauffman12) | https://github.com/kauffman12/eqaainfo | Python, no licence. Decodes AA data from network captures. Low (M9). |
| EQLogParser (kauffman12) | https://github.com/kauffman12/EQLogParser | Apache-2.0, C#. Large catalogue of log-line formats. Low; keeps our chat log compatible with existing parsers. |

---

## Top 10 to use first, by milestone

Ranked by expected payoff. ✅ portable (permissive); 📖 reading only (GPL or no licence).

| # | Resource | Milestone | Why first |
|---|---|---|---|
| 1 | **cjab/libeq** (✅ MIT) with **LanternExtractor** (✅ MIT) and its `animationsources.txt` | **M1**, M4 | PFS and the 49 WLD fragment layouts from libeq; fragment meaning (BSP regions, vertex colours, lights, sound and music instances, skeletal tracks) from Lantern. All of M1 and most of M4's file side. |
| 2 | **EQEmu docs, client section**: WLD by Windcatcher, fog and clip plane, sky system (📖 facts) | **M1** | The spec plus measured rendering behaviour (21-step clip table, fog blend, sky layer speeds). Cross-check both. |
| 3 | **eq-network** transport and login crates (✅ MIT) | **M2** | The only permissive, maintained client session stack (framing, keyed CRC, fragments, combined packets, compression, retransmit, DES-zero login, server list). Port it with RoF2 opcodes and structs; check against EQEmu `reliable_stream_connection` (📖 GPL). |
| 4 | **eqoxide docs** (📖 no licence): `protocol-notes.md`, `rof2-migration.md`, `character-models.md`, `equipment-textures-findings.md`, `collision-system.md` | **M2–M4**, M7 | The only other RoF2 client: how EQEmu recognises a RoF2 stream (`OP_SendLoginInfo` 0x7a09 at 464 bytes, `OP_ZoneEntry` 0x5089 at 76 bytes), traps in `rof2_structs.h`. Read; don't copy. |
| 5 | **Shendare EQRaceInventory RoF2 tables**, EQEmu race-files and model-loading docs, EQEmu `races` and `textures` tables (📖) | **M4** | The RoF2 race-to-model-to-file table and the `_chr.txt` load-list format. Regenerate from a RoF2 asset set to validate our loader. |
| 6 | **eq-client `sidl.rs`** (✅ MIT) | **M5** | Permissive runtime SIDL reader (templates, `Ui2DAnimation`, draw templates) to compare with ours. Its `eq-client-axes` module suits EQ's mirrored axes (M1). |
| 7 | **xackery/quail** (✅ MIT, archived) | **M6** (and M4 for MDS/ANI) | MIT readers for MOD, MDS, TER, ZON v1–4, ANI, LIT, LOD, PRT, PTS, EDD, ECO, TOG and LAY. Pair with the **EQGZI shader list** (72 `.fx` names and properties) and the **zon-versions** table. Check v4 zones against zone-utilities `eqg_v4_loader` (📖 GPL). |
| 8 | **EQEmu client-file exporter, spells docs, DBStr types, `say_link.cpp`** (📖 facts) | **M7** | Exact `spells_us.txt`, `dbstr_us.txt`, `SkillCaps.txt` and `BaseData.txt` layouts, SPA semantics, the RoF2 item-link body. rumstil/eqspellparser (📖 Apache-2.0) for spell-description text. |
| 9 | **Corsix xmi2mid** (✅ MIT) + **TinySoundFont** (✅ MIT) + **miniaudio** (✅ PD/MIT-0) + **GeneralUser GS** soundfont | **M8** | Complete permissive music path: split each EQ XMI into sequences (as knervous/xmi_to_midi does), synthesise with TSF, play and spatialise with miniaudio. WillEQ ships the same stack. |
| 10 | **Shendare Eff2Emt** (✅ CC0) + EQGZI `.emt` field list + WillEQ `eff_sound_format.md` (📖) | **M8** | Classic `_sounds.eff`/`_sndbnk.eff` decoding (CC0) and the `.emt` columns: all ambient emitters need. |

Runner-up: **WillEQ** (📖 GPL, C++), the best whole-client reference for how these pieces fit in one C++ codebase (sky, weather, culling, audio, navmesh). Read; don't copy.
