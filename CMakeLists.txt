cmake_minimum_required(VERSION 3.25)
# The oldest macOS a build runs on (set before project()). 13.3 is the first whose libc++ formats floating-point
# numbers (std::format of a float, std::to_chars), which the client uses; the bundled MoltenVK needs 12.0.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "The oldest macOS the build runs on")
project(hailstone VERSION 0.7.0 LANGUAGES C CXX)
# The release's pre-release label (SemVer: "0.1.0-alpha"); empty for a final release. With the version it is what the
# updater compares against a release's manifest (core/version.h, docs/updater.md), so change both with each release.
set(HAILSTONE_VERSION_LABEL "beta")
if(HAILSTONE_VERSION_LABEL STREQUAL "")
  set(HAILSTONE_RELEASE_VERSION ${PROJECT_VERSION})
else()
  set(HAILSTONE_RELEASE_VERSION ${PROJECT_VERSION}-${HAILSTONE_VERSION_LABEL})
endif()

# Portable RoF2-compatible client. Plan: docs/plan.md

# A plugin is a .dylib on macOS (plugin/native_plugins.cpp), where CMake would name a MODULE library .so. Set here, it
# holds for the example, the test plugins and the server plugins built in this tree.
if(APPLE)
  set(CMAKE_SHARED_MODULE_SUFFIX ".dylib")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The build's options and settings were NMSC_* up to 0.6. One still set (on the command line, or in an older build
# folder) is taken as its HAILSTONE_* name when that is not set, so an old build folder or script keeps its settings.
get_cmake_property(_hailstone_cache CACHE_VARIABLES)
foreach(_old IN LISTS _hailstone_cache)
  if(NOT _old MATCHES "^NMSC_(.+)$")
    continue()
  endif()
  set(_new "HAILSTONE_${CMAKE_MATCH_1}")
  if(NOT DEFINED CACHE{${_new}})
    get_property(_type CACHE ${_old} PROPERTY TYPE)
    get_property(_help CACHE ${_old} PROPERTY HELPSTRING)
    if(NOT _type OR _type STREQUAL "UNINITIALIZED")
      set(_type STRING)
    endif()
    set(${_new} "$CACHE{${_old}}" CACHE ${_type} "${_help}")
    if(NOT _type STREQUAL "INTERNAL")
      message(WARNING "${_old} is now called ${_new}; the build uses its value as ${_new}")
    endif()
  elseif(NOT "$CACHE{${_old}}" STREQUAL "$CACHE{${_new}}")
    message(WARNING "${_old} is ignored: it is now called ${_new}, which is set to \"$CACHE{${_new}}\"")
  endif()
endforeach()

option(HAILSTONE_BUILD_TESTS "Build unit tests" ON)
option(HAILSTONE_MANAGED_CLIENT "Build a client with only its mandatory linked add-on and approved UI" OFF)
set(HAILSTONE_BUILTIN_ADDON_SOURCE_DIR "" CACHE PATH "SDK-only mandatory built-in add-on source directory")
if(HAILSTONE_MANAGED_CLIENT AND NOT HAILSTONE_BUILTIN_ADDON_SOURCE_DIR)
  message(FATAL_ERROR "A managed client requires HAILSTONE_BUILTIN_ADDON_SOURCE_DIR")
endif()
option(HAILSTONE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(HAILSTONE_LIBFUZZER "Build libFuzzer targets (needs Clang; turns on HAILSTONE_SANITIZE)" OFF)
if(HAILSTONE_LIBFUZZER)
  set(HAILSTONE_SANITIZE ON)
endif()

if(HAILSTONE_SANITIZE AND NOT MSVC)
  # Applies to every target, including the fetched libraries, so the runtime is consistent.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
  # Clang does not link the sanitizer runtime into shared libraries, and SDL3 links with --no-undefined: build it static.
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
endif()

# Run clang-tidy (config in .clang-tidy) on our own sources while compiling. Needs clang-tidy on PATH.
option(HAILSTONE_CLANG_TIDY "Run clang-tidy on the engine, app and tools while building" OFF)

# The in-game browser's helper (tools/hailstone-web, docs/browser.md) on CEF's binary builds, fetched at configure
# time (a few hundred MB). Off in sanitizer builds (CEF's binaries are not instrumented) and on macOS (its app bundles
# are not done yet); without it pages open in the system's browser.
if(HAILSTONE_SANITIZE OR APPLE)
  set(HAILSTONE_BROWSER_DEFAULT OFF)
else()
  set(HAILSTONE_BROWSER_DEFAULT ON)
endif()
option(HAILSTONE_BROWSER "Build the in-game browser's helper on CEF" ${HAILSTONE_BROWSER_DEFAULT})

# Lua is built as C, so a Lua error longjmps through the C++ functions it called. MSVC's longjmp unwinds like an
# exception, but under its default /EHsc the compiler assumes extern "C" functions never throw and keeps no exact
# unwind state at their call sites: a luaL_argerror after a function's checks then ran the destructor of an object
# not made yet (the Windows CI crash, 0xc0000409, in "eq API: bad arguments are Lua errors"). /EHs keeps that state.
# The clang-cl build did not crash there; it takes the same flag.
if(MSVC)
  string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  # The Visual Studio generator hands cl.exe a project's files in one call; /MP compiles them in parallel. Ninja and
  # clang-cl compile one file per call, so it changes nothing there.
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(/MP)
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Dependencies)
include(Shaders)

if(MSVC)
  set(HAILSTONE_WARNINGS /W4 /permissive- /utf-8)
else()
  set(HAILSTONE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Wno-missing-field-initializers)
endif()

if(HAILSTONE_CLANG_TIDY)
  find_program(HAILSTONE_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-19 clang-tidy-20 REQUIRED)
  # Applied per target below, so third_party and the fetched dependencies are never analysed.
  set(HAILSTONE_TIDY_COMMAND ${HAILSTONE_CLANG_TIDY_EXE})
endif()

# Engine library: everything except main(), so tools and tests link it too.
add_library(hailstone_engine STATIC
  src/core/log.cpp
  src/core/build_info.cpp
  src/core/zip_write.cpp
  src/core/zip_read.cpp
  src/core/version.cpp
  src/core/ed25519.cpp
  src/core/update_manifest.cpp
  src/core/addon_manifest.cpp
  src/core/addon_index.cpp
  src/core/changelog.cpp
  src/core/addon_source.cpp
  src/core/binary_header.cpp
  src/core/sha1.cpp
  src/core/sha256.cpp
  src/core/distribution.cpp
  src/plugin/hooks.cpp
  src/plugin/consent.cpp
  src/plugin/policy.cpp
  src/net/settings_packets.cpp
  src/net/quest_npc_packets.cpp
  src/net/zone_link_packets.cpp
  src/net/server_files_packets.cpp
  src/net/server_files.cpp
  src/mq/tlo.cpp
  src/mq/tlo_game.cpp
  src/mq/spawn_search.cpp
  src/mq/parse.cpp
  src/mq/commands.cpp
  src/mq/nav_commands.cpp
  src/mq/event_pattern.cpp
  src/mq/lua_mq.cpp
  src/mq/lua_mq_web.cpp
  src/mq/lua_hailstone.cpp
  src/plugin/native_plugins.cpp
  src/plugin/plugin_tcp.cpp
  src/plugin/ui_bridge.cpp
  src/plugin/lua_value.cpp
  src/game/conversation_journal.cpp
  src/game/light_list.cpp
  src/render/scene_lighting.cpp
  src/render/atmosphere.cpp
  src/render/ambient_probe.cpp
  src/render/shadows.cpp
  src/render/light_grid.cpp
  src/render/cover_map.cpp
  src/render/culling.cpp
  src/render/shadow_chunks.cpp
  src/render/point_shadows.cpp
  src/render/point_shadow_pass.cpp
  src/game/weather.cpp
  src/render/weather_renderer.cpp
  src/platform/udp_socket.cpp
  src/platform/tcp_socket.cpp
  src/net/session_codec.cpp
  src/net/session.cpp
  src/net/reliable_channel.cpp
  src/net/des.cpp
  src/net/login.cpp
  src/net/game_connection.cpp
  src/net/game_connection_spells.cpp
  src/net/game_connection_ticks.cpp
  src/net/game_connection_spell_sets.cpp
  src/net/game_connection_ability_commands.cpp
  src/net/game_connection_mounts.cpp
  src/net/game_connection_cast.cpp
  src/net/game_connection_spell_book.cpp
  src/net/game_connection_vehicle.cpp
  src/net/game_connection_items.cpp
  src/net/game_connection_augments.cpp
  src/net/game_connection_item_use.cpp
  src/net/game_connection_commerce.cpp
  src/net/game_connection_bank.cpp
  src/net/game_connection_item_sort.cpp
  src/net/merchant_packets.cpp
  src/net/trade_packets.cpp
  src/net/game_connection_movement.cpp
  src/net/game_connection_abilities.cpp
  src/net/game_connection_tracking.cpp
  src/net/game_connection_tribute.cpp
  src/net/game_connection_adventure.cpp
  src/net/game_connection_dz.cpp
  src/net/game_connection_path.cpp
  src/net/game_connection_guild_bank.cpp
  src/net/game_connection_real_estate.cpp
  src/net/ability_packets.cpp
  src/net/game_connection_doors.cpp
  src/net/game_connection_death.cpp
  src/net/door_packets.cpp
  src/net/game_connection_group.cpp
  src/net/game_connection_group_search.cpp
  src/net/game_connection_marketplace.cpp
  src/net/game_connection_social.cpp
  src/net/game_connection_options.cpp
  src/net/game_connection_client.cpp
  src/net/game_connection_progress.cpp
  src/net/game_connection_player.cpp
  src/net/game_connection_game_commands.cpp
  src/net/game_connection_names.cpp
  src/net/game_connection_market.cpp
  src/net/game_connection_mail.cpp
  src/net/game_connection_merc.cpp
  src/net/player_packets.cpp
  src/net/market_packets.cpp
  src/net/marketplace_packets.cpp
  src/game/interact.cpp
  src/game/barter.cpp
  src/game/marketplace.cpp
  src/game/xtarget.cpp
  src/game/bazaar.cpp
  src/game/feature_restrictions.cpp
  src/game/mail.cpp
  src/game/mercenary.cpp
  src/net/aa_packets.cpp
  src/net/training_packets.cpp
  src/net/tribute_packets.cpp
  src/net/adventure_packets.cpp
  src/net/aggro_packets.cpp
  src/net/dz_packets.cpp
  src/net/guild_bank_packets.cpp
  src/net/real_estate_packets.cpp
  src/net/task_packets.cpp
  src/net/social_packets.cpp
  src/net/chat_server.cpp
  src/game/social.cpp
  src/game/server_ext.cpp
  src/game/server_ext_chain.cpp
  src/game/chat_text.cpp
  src/game/chat_windows.cpp
  src/game/client_options.cpp
  src/game/buff_overlays.cpp
  src/game/target_indicator.cpp
  src/game/mouse_look.cpp
  src/game/follow.cpp
  src/game/missiles.cpp
  src/game/spell_projectiles.cpp
  src/game/slash_commands.cpp
  src/net/group_packets.cpp
  src/net/pet_packets.cpp
  src/net/item_packets.cpp
  src/net/augment_packets.cpp
  src/net/spell_packets.cpp
  src/game/spell_description.cpp
  src/game/item_display.cpp
  src/game/augments.cpp
  src/game/spell_display.cpp
  src/game/targeting.cpp
  src/game/gamepad_bindings.cpp
  src/game/clicks.cpp
  src/game/picking.cpp
  src/game/zoning.cpp
  src/game/death_report.cpp
  src/game/spell_record.cpp
  src/game/spells.cpp
  src/game/passage_of_time.cpp
  src/game/spell_sets.cpp
  src/game/group_role_sets.cpp
  src/game/group_search.cpp
  src/net/opcodes.cpp
  src/net/world_packets.cpp
  src/net/zone_packets.cpp
  src/core/game_dir.cpp
  src/core/asset_folder.cpp
  src/core/asset_folder_choice.cpp
  src/core/server_profile.cpp
  src/core/character_files.cpp
  src/core/atomic_file.cpp
  src/core/config.cpp
  src/core/ini_file.cpp
  src/core/old_ui_settings.cpp
  src/web/browser_host.cpp
  src/platform/paths.cpp
  src/platform/message_box.cpp
  src/platform/crash_log.cpp
  src/platform/local_time.cpp
  src/platform/private_file.cpp
  src/platform/durable_file.cpp
  src/platform/process.cpp
  src/game/eq_names.cpp
  src/ui/font_faces.cpp
  src/ui/imgui_layer.cpp
  src/ui/imgui_vk_textures.cpp
  src/ui/web_textures.cpp
  src/ui/asset_picker.cpp
  src/ui/dev_screens.cpp
  src/ui/screen_pictures.cpp
  src/ui/spell_windows.cpp
  src/app/spell_ui.cpp
  src/app/bug_report.cpp
  src/app/startup_error.cpp
  src/app/updater.cpp
  src/app/update_install.cpp
  src/app/addon_install.cpp
  src/app/addon_manager.cpp
  src/app/addon_suggest.cpp
  src/app/window_icon.cpp
  src/app/item_preview_view.cpp
  src/app/item_ui.cpp
  src/app/perf_ui.cpp
  src/app/stats_ui.cpp
  src/app/actions_ui.cpp
  src/app/actions_ui_sets.cpp
  src/app/group_ui.cpp
  src/app/social_ui.cpp
  src/ui/social_windows.cpp
  src/app/player_movement.cpp
  src/app/player_ladder.cpp
  src/app/movement_test.cpp
  src/ui/item_windows.cpp
  src/ui/commerce_windows.cpp
  src/app/commerce_ui.cpp
  src/app/progress_ui.cpp
  src/app/market_ui.cpp
  src/ui/group_windows.cpp
  src/app/ui_demo.cpp
  src/assets/pfs.cpp
  src/assets/wld.cpp
  src/assets/wld_zone.cpp
  src/assets/wld_detail.cpp
  src/assets/eqg.cpp
  src/assets/eqg_zone.cpp
  src/assets/flora.cpp
  src/assets/zone_flora.cpp
  src/assets/character.cpp
  src/assets/character_dress.cpp
  src/assets/character_parts.cpp
  src/assets/eqg_character.cpp
  src/assets/model_points.cpp
  src/assets/regions.cpp
  src/assets/texture.cpp
  src/assets/png_read.cpp
  src/assets/jpeg_read.cpp
  src/assets/texture_overrides.cpp
  src/assets/sky.cpp
  src/assets/sky_weather.cpp
  src/assets/spell_effects.cpp
  src/assets/wld_particles.cpp
  src/assets/zone_fires.cpp
  src/platform/window.cpp
  src/platform/mouse_capture.cpp
  src/platform/open_folder.cpp
  src/platform/folder_dialog.cpp
  src/platform/http_get.cpp
  src/platform/credentials.cpp
  src/platform/child_processes.cpp
  src/platform/helper_process.cpp
  src/render/vk_context.cpp
  src/app/plate_test.cpp
  src/app/nameplate_layer.cpp
  src/render/gpu_requirements.cpp
  src/render/pipeline_cache.cpp
  src/render/gpu.cpp
  src/render/zone_renderer.cpp
  src/render/sky_renderer.cpp
  src/render/particle_renderer.cpp
  src/render/fire_renderer.cpp
  src/render/flora_renderer.cpp
  src/render/spell_fx_renderer.cpp
  src/render/target_ring_renderer.cpp
  src/game/view_distance.cpp
  src/game/fog.cpp
  src/game/scaled_ambient.cpp
  src/game/scene_fog.cpp
  src/game/sky_colours.cpp
  src/game/weather_state.cpp
  src/game/precipitation.cpp
  src/game/scene_light.cpp
  src/game/cloud_patches.cpp
  src/game/particles.cpp
  src/game/spell_effect_timers.cpp
  src/game/sine_table.cpp
  src/game/collision.cpp
  src/nav/nav_mesh.cpp
  src/nav/zone_nav.cpp
  src/nav/path_follower.cpp
  src/game/zone_lines.cpp
  src/game/neighbour_zones.cpp
  src/game/world_state.cpp
  src/game/useful_npcs.cpp
  src/game/path_trail.cpp
  src/game/boats.cpp
  src/game/npc_collision.cpp
  src/game/mounts.cpp
  src/game/map_data.cpp
  src/game/map_files.cpp
  src/game/player_path.cpp
  src/game/atlas.cpp
  src/game/zone_routes.cpp
  src/game/zone_names.cpp
  src/game/race_models.cpp
  src/game/spawn_heights.cpp
  src/game/appearance.cpp
  src/game/actor_look.cpp
  src/game/appearance_data.cpp
  src/game/player_customization.cpp
  src/game/char_create_look.cpp
  src/game/char_select_camera.cpp
  src/game/cameras.cpp
  src/game/spawn_animation.cpp
  src/game/anim_layers.cpp
  src/game/hit_landing.cpp
  src/game/eq_strings.cpp
  src/game/combat.cpp
  src/game/inventory.cpp
  src/game/bag_move.cpp
  src/game/item_preview.cpp
  src/game/item_moves.cpp
  src/game/item_sort.cpp
  src/game/kept_items.cpp
  src/game/item_links.cpp
  src/game/keymap.cpp
  src/game/hotbuttons.cpp
  src/game/lua_social.cpp
  src/game/doors.cpp
  src/game/object_collision.cpp
  src/game/zone_ladders.cpp
  src/game/swept_sphere.cpp
  src/game/sphere_sweep.cpp
  src/game/collision_callbacks.cpp
  src/game/merchant.cpp
  src/game/bank.cpp
  src/game/group.cpp
  src/game/alt_adv.cpp
  src/game/ability_book.cpp
  src/game/training.cpp
  src/game/guild_notes.cpp
  src/game/guild_ranks.cpp
  src/game/real_estate.cpp
  src/game/placement.cpp
  src/game/text_overlay.cpp
  src/game/tasks.cpp
  src/game/task_window.cpp
  src/game/mechanics/movement.cpp
  src/game/mechanics/movement_inputs.cpp
  src/game/mechanics/physics.cpp
  src/game/mechanics/abilities.cpp
  src/game/mechanics/tables.cpp
  src/game/mechanics/item_stats.cpp
  src/game/mechanics/effects.cpp
  src/game/mechanics/stats.cpp
  src/game/mechanics/regen.cpp
  src/game/mechanics/skills.cpp
  src/game/mechanics/spell_math.cpp
  src/game/mechanics/sheet_builder.cpp
  src/game/mechanics/item_sums.cpp
  src/game/mechanics/ticks.cpp
  src/game/mechanics/remote_spawns.cpp
  src/game/mechanics/controls.cpp
  src/game/mechanics/regions.cpp
  src/game/mechanics/vehicle.cpp
  src/game/mechanics/ladder.cpp
  src/game/mechanics/attack_rules.cpp
  src/game/attack_check.cpp
  src/game/spell_book.cpp
  src/game/group_rules.cpp
  src/game/quantity_dialog.cpp
  src/game/trade_rules.cpp
  src/game/tradeskill.cpp
  src/game/leadership.cpp
  src/game/chat_input.cpp
  src/game/hot_button_press.cpp
  src/game/hot_button_menu.cpp
  src/game/pet_buttons.cpp
  src/game/hot_button_sets.cpp
  src/game/icon_selection.cpp
  src/game/tracking.cpp
  src/game/compass.cpp
  src/game/combat_ability_menu.cpp
  src/game/cursor_drop.cpp
  src/game/item_options.cpp
  src/game/nameplate_overlap.cpp
  src/game/nameplate_style.cpp
  src/game/option_commands.cpp
  src/game/output_file.cpp
  src/ui/lua/lua_host.cpp
  src/ui/lua/lua_gc_live.cpp
  src/ui/lua/lua_imgui.cpp
  src/ui/lua/lua_code_editor.cpp
  src/ui/code_syntax.cpp
  src/ui/ui_textures.cpp
  src/ui/ui_packs.cpp
  src/ui/ui_ini.cpp
  src/app/lua_eq.cpp
  src/app/automation.cpp
  src/app/navigator.cpp
  src/app/guide_path.cpp
  src/app/lua_eq_values.cpp
  src/app/lua_eq_hud.cpp
  src/app/lua_eq_hotbar.cpp
  src/app/lua_eq_caps.cpp
  src/app/lua_eq_plugins.cpp
  src/app/lua_eq_scripts.cpp
  src/app/lua_eq_items.cpp
  src/app/lua_eq_itemdisplay.cpp
  src/app/lua_eq_adventure.cpp
  src/app/lua_eq_tint.cpp
  src/app/lua_eq_customization.cpp
  src/app/lua_eq_dialogs.cpp
  src/app/lua_eq_menu.cpp
  src/app/lua_eq_social.cpp
  src/app/lua_eq_realestate.cpp
  src/app/lua_eq_progress.cpp
  src/app/lua_eq_group_search.cpp
  src/app/lua_eq_marketplace.cpp
  src/app/lua_eq_map.cpp
  src/app/lua_eq_player.cpp
  src/app/lua_eq_market.cpp
  src/app/lua_eq_mail.cpp
  src/app/lua_eq_merc.cpp
  src/app/lua_eq_chat.cpp
  src/app/lua_eq_spellbook.cpp
  src/app/lua_eq_abilities.cpp
  src/app/lua_eq_abilitybook.cpp
  src/app/lua_eq_inventory_manager.cpp
  src/app/lua_eq_button_state.cpp
  src/app/lua_eq_socialedit.cpp
  src/app/lua_eq_luasocial.cpp
  src/app/lua_eq_names.cpp
  src/app/lua_eq_help.cpp
  src/app/lua_eq_layout.cpp
  src/app/lua_eq_video.cpp
  src/app/lua_eq_zoneguide.cpp
  src/app/lua_eq_find.cpp
  src/app/lua_eq_fellowship.cpp
  src/app/lua_eq_story.cpp
  src/app/lua_eq_journal.cpp
  src/app/lua_eq_login.cpp
  src/app/lua_eq_pregame.cpp
  src/app/lua_eq_stats.cpp
  src/app/lua_eq_web.cpp
  src/app/web_service.cpp
  src/app/server_links.cpp
  src/app/lua_eq_misc.cpp
  src/app/lua_eq_notes.cpp
  src/app/lua_eq_gamepad.cpp
  src/app/lua_eq_exchange.cpp
  src/app/gamepad.cpp
  src/game/mechanics/anim_select.cpp
  src/game/mechanics/hit_by_spell.cpp
  src/game/mechanics/cast_spell.cpp
  src/game/mechanics/collision_movement.cpp
  src/game/mechanics/switches.cpp
  src/game/mechanics/touching_switch.cpp
  src/game/position_tracker.cpp
  src/game/mechanics/floor_height.cpp
  src/game/mechanics/can_i_fit_here.cpp
  src/game/mechanics/melee.cpp
  src/game/mechanics/use_skill.cpp
  src/game/mechanics/combat_ability.cpp
  src/net/movement_packets.cpp
  src/net/notice_packets.cpp
  src/render/spawn_renderer.cpp
  src/render/spirv_file.cpp
  src/render/particle_textures.cpp
  src/render/actor_renderer.cpp
  src/render/actor_layers.cpp
  src/render/object_renderer.cpp
  src/app/world_objects.cpp
  src/app/neighbour_zones.cpp
  src/app/world_objects_ladder.cpp
  src/render/vma_impl.cpp
  src/assets/sound_files.cpp
  src/assets/xmi.cpp
  src/game/sound_rules.cpp
  src/game/spell_gem.cpp
  src/game/hot_button_state.cpp
  src/game/mechanics/spell_gems.cpp
  src/game/mechanics/find_affect_slot.cpp
  src/audio/pcm.cpp
  src/audio/sound_control.cpp
  src/audio/mixer.cpp
  src/audio/xmi_player.cpp
  src/audio/sound_library.cpp
  src/audio/zone_emitters.cpp
  src/audio/music_manager.cpp
  src/audio/listener.cpp
  src/audio/environment_presets.cpp
  src/audio/audio_system.cpp
  src/app/sound_events.cpp
  src/app/actor_emitters.cpp
  src/app/spell_particles.cpp
  src/app/spell_bolts.cpp
  src/app/zone_particles.cpp
  src/audio/codec_impl.cpp
  src/platform/audio_device.cpp
)
# Third-party single-header implementations: their own warnings are not ours.
if(MSVC)
  set_source_files_properties(src/audio/codec_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(src/audio/codec_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
# The character rules reproduce the original's x87 rounding step by step (src/game/mechanics/fpu.h): no fused
# multiply-adds there.
if(NOT MSVC)
  set_source_files_properties(
    src/game/mechanics/effects.cpp src/game/mechanics/stats.cpp src/game/mechanics/regen.cpp
    src/game/mechanics/skills.cpp src/game/mechanics/spell_math.cpp src/game/mechanics/item_stats.cpp
    src/game/mechanics/movement.cpp src/game/mechanics/movement_inputs.cpp src/game/mechanics/physics.cpp
    src/game/mechanics/abilities.cpp src/game/mechanics/item_sums.cpp src/game/mechanics/ticks.cpp
    src/game/mechanics/remote_spawns.cpp src/game/mechanics/controls.cpp src/game/mechanics/regions.cpp
    src/game/mechanics/anim_select.cpp src/game/mechanics/hit_by_spell.cpp src/game/mechanics/cast_spell.cpp
    src/game/mechanics/collision_movement.cpp src/game/sphere_sweep.cpp src/game/follow.cpp
    src/game/mechanics/switches.cpp src/game/mechanics/floor_height.cpp src/game/mechanics/can_i_fit_here.cpp
    src/audio/listener.cpp src/game/spell_gem.cpp
    src/game/mechanics/melee.cpp src/game/mechanics/use_skill.cpp src/game/fog.cpp src/game/spawn_heights.cpp
    src/game/sky_colours.cpp src/game/char_select_camera.cpp src/game/weather_state.cpp src/game/scaled_ambient.cpp src/game/missiles.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(src/game/mechanics/vehicle.cpp src/game/mechanics/ladder.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
target_include_directories(hailstone_engine PUBLIC src)
# The plugin SDK (sdk/hailstone_plugin.h): the host side of the C interface.
target_include_directories(hailstone_engine PRIVATE sdk)
# The build's version line (core/build_info.h), remade at each build from git.
add_custom_target(hailstone_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DVERSION=${HAILSTONE_RELEASE_VERSION}
          -DOUT=${PROJECT_BINARY_DIR}/generated/build_info.inc -P ${PROJECT_SOURCE_DIR}/cmake/BuildInfo.cmake
  BYPRODUCTS ${PROJECT_BINARY_DIR}/generated/build_info.inc
  COMMENT "Build version")
add_dependencies(hailstone_engine hailstone_build_info)
set_source_files_properties(src/core/build_info.cpp PROPERTIES OBJECT_DEPENDS ${PROJECT_BINARY_DIR}/generated/build_info.inc)
target_include_directories(hailstone_engine PRIVATE ${PROJECT_BINARY_DIR}/generated)
# Lua 5.4.7 (third_party/lua, MIT), for the Lua UI (src/ui/lua): compiled as C.
file(GLOB HAILSTONE_LUA_SOURCES CONFIGURE_DEPENDS third_party/lua/*.c)
add_library(hailstone_lua STATIC ${HAILSTONE_LUA_SOURCES})
target_include_directories(hailstone_lua PUBLIC third_party/lua)
set_target_properties(hailstone_lua PROPERTIES LINKER_LANGUAGE C)
if(NOT MSVC)
  target_compile_options(hailstone_lua PRIVATE -w)
  target_compile_definitions(hailstone_lua PRIVATE $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX> $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>)
endif()
target_link_libraries(hailstone_engine PUBLIC hailstone_lua hailstone_web_protocol)
target_compile_options(hailstone_engine PRIVATE ${HAILSTONE_WARNINGS})
if(HAILSTONE_CLANG_TIDY)
  set_target_properties(hailstone_engine PROPERTIES CXX_CLANG_TIDY "${HAILSTONE_TIDY_COMMAND}")
endif()
target_compile_definitions(hailstone_engine PUBLIC VK_NO_PROTOTYPES GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
target_link_libraries(hailstone_engine PUBLIC imgui hailstone_audio_headers hailstone_bcdec hailstone_stb hailstone_recast hailstone_monocypher)
# The updater (app/updater.h, docs/updater.md): the address of the release feed's manifest, and the release key's
# public half (64 hex digits) its signature is checked with: update-key.pub at the top of the tree, or this option.
# Without a key the build has no updates.
set(HAILSTONE_UPDATE_FEED "https://github.com/projecthailstone/hailstone-client/releases/latest/download/hailstone-update.txt"
    CACHE STRING "The update manifest's address")
# The add-on index (core/addon_index.h, docs/addons.md "Suggested add-ons"), signed with the same release key.
set(HAILSTONE_ADDON_INDEX "https://raw.githubusercontent.com/projecthailstone/hailstone-addon-index/HEAD/hailstone-addon-index.txt"
    CACHE STRING "The add-on index's address; empty: no suggested add-ons")
# Hailstone's page, which the login screen's GitHub button opens (docs/updater.md "What the player sees").
set(HAILSTONE_PROJECT_PAGE "https://github.com/projecthailstone/hailstone-client" CACHE STRING "Hailstone's web page")
set(HAILSTONE_UPDATE_KEY_DEFAULT "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/update-key.pub)
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/update-key.pub HAILSTONE_UPDATE_KEY_DEFAULT REGEX "^[0-9a-fA-F]+$" LIMIT_COUNT 1)
endif()
set(HAILSTONE_UPDATE_KEY "${HAILSTONE_UPDATE_KEY_DEFAULT}" CACHE STRING "The release key's public half (hex); empty: no updates")
set_property(SOURCE src/app/updater.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             HAILSTONE_UPDATE_FEED="${HAILSTONE_UPDATE_FEED}" HAILSTONE_UPDATE_KEY="${HAILSTONE_UPDATE_KEY}"
             HAILSTONE_ADDON_INDEX="${HAILSTONE_ADDON_INDEX}" HAILSTONE_PROJECT_PAGE="${HAILSTONE_PROJECT_PAGE}")
target_link_libraries(hailstone_engine PUBLIC SDL3::SDL3 Vulkan::Headers volk::volk_headers GPUOpen::VulkanMemoryAllocator glm::glm ZLIB::ZLIB)
if(WIN32)
  target_link_libraries(hailstone_engine PUBLIC ws2_32 advapi32 dbghelp winhttp)
elseif(APPLE)
  target_link_libraries(hailstone_engine PUBLIC "-framework Security" "-framework CoreFoundation")
else()
  target_link_libraries(hailstone_engine PUBLIC ${CMAKE_DL_LIBS}) # libsecret, opened when first used (platform/credentials.h)
endif()
if(MSVC)
  # getenv, sscanf and _wfopen are fine as used: no "use the _s form" warnings.
  target_compile_definitions(hailstone_engine PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
# Windows: the programs run with UTF-8 as their ANSI code page (Windows 10 1903 and later), so narrow file names,
# getenv and path::string() hold any name instead of '?' or an exception (cmake/utf8.manifest). MSVC-style linkers
# take a .manifest source; MinGW would need a resource file.
set(HAILSTONE_WIN_MANIFEST "")
set(HAILSTONE_WIN_CLIENT_RESOURCES "")
if(MSVC)
  set(HAILSTONE_WIN_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest)
  # The client's icon in the .exe (cmake/windows/hailstone.rc).
  set(HAILSTONE_WIN_CLIENT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/hailstone.rc)
endif()

hailstone_add_shaders(hailstone_shaders
  shaders/zone.vert
  shaders/zone.frag
  shaders/spawn.vert
  shaders/spawn.frag
  shaders/actor.vert
  shaders/actor.frag
  shaders/object.vert
  shaders/object.frag
  shaders/sky.vert
  shaders/sky.frag
  shaders/atmosphere.vert
  shaders/atmosphere.frag
  shaders/precipitation.vert
  shaders/precipitation.frag
  shaders/particle.vert
  shaders/particle.frag
  shaders/fire.vert
  shaders/flora.vert
  shaders/flora.frag
  shaders/fire.frag
  shaders/spell_fx.vert
  shaders/spell_fx.frag
  shaders/target_ring.vert
  shaders/target_ring.frag
  shaders/underwater.vert
  shaders/underwater.frag
  shaders/zone_shadow.vert
  shaders/zone_shadow.frag
  shaders/zone_point_shadow.vert
  shaders/zone_point_shadow.frag
  shaders/actor_point_shadow.vert
  shaders/actor_point_shadow.frag
  shaders/actor_shadow.vert
  shaders/actor_shadow.frag
  shaders/object_shadow.vert
  shaders/object_shadow.frag
  shaders/object_point_shadow.vert
  shaders/object_point_shadow.frag
  shaders/post.vert
  shaders/post.frag
  shaders/ao.frag
  shaders/plate_depth.vert
  shaders/plate_depth.frag
  shaders/bloom_down.frag
  shaders/bloom_up.frag
  shaders/fsr_easu.frag
  shaders/fsr_rcas.frag
)
add_dependencies(hailstone_engine hailstone_shaders)

# The client's folder (docs/layout.md): the build makes it in build/hailstone/, as an install does. The binary,
# shaders/, ui/ and the runtime DLLs; the data Hailstone writes (hailstone.ini, servers/, logs/ ...) lands beside them,
# since a dev build's folder is writable. $<0:> keeps multi-config generators from adding a Debug/Release level.
if(HAILSTONE_MANAGED_CLIENT)
  set(HAILSTONE_TREE_DIR ${PROJECT_BINARY_DIR}/hailstone-managed)
else()
  set(HAILSTONE_TREE_DIR ${PROJECT_BINARY_DIR}/hailstone)
endif()
# Builds before the folder put the binary itself at build/hailstone: that file has to make way for the folder.
if(EXISTS ${HAILSTONE_TREE_DIR} AND NOT IS_DIRECTORY ${HAILSTONE_TREE_DIR})
  file(REMOVE ${HAILSTONE_TREE_DIR})
endif()

# The example compiled plugin (plugins/example): plain C against sdk/hailstone_plugin.h, in build/hailstone/plugins.
add_library(MQ2Example MODULE plugins/example/MQ2Example.c)
target_include_directories(MQ2Example PRIVATE sdk)
if(HAILSTONE_MANAGED_CLIENT)
  set(_example_plugin_dir ${PROJECT_BINARY_DIR}/test-plugins)
else()
  set(_example_plugin_dir ${HAILSTONE_TREE_DIR}/plugins)
endif()
set_target_properties(MQ2Example PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${_example_plugin_dir}$<0:>
                                            RUNTIME_OUTPUT_DIRECTORY ${_example_plugin_dir}$<0:>)

# The server plugins are kept in repositories of their own and reach players as add-ons (docs/addons.md): the NMS
# server's (projecthailstone/hailstone-nms) and the Project Triune server's (projecthailstone/hailstone-projecttriune,
# private). Cloned into plugins/nms or plugins/projecttriune (both ignored here), each is built from its own
# CMakeLists.txt (the nms and projecttriune targets, in build/hailstone/plugins/<name>/), and its logic
# (HAILSTONE_NMS_LOGIC, HAILSTONE_TRIUNE_LOGIC) and tests (HAILSTONE_NMS_TESTS, HAILSTONE_TRIUNE_TESTS) join the tests. Without the
# folder nothing of it is built.
set(HAILSTONE_HAS_NMS OFF)
set(HAILSTONE_NMS_LOGIC)
set(HAILSTONE_NMS_TESTS)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/nms/CMakeLists.txt)
  add_subdirectory(plugins/nms)
  set(HAILSTONE_HAS_NMS ON)
endif()
set(HAILSTONE_HAS_TRIUNE OFF)
set(HAILSTONE_TRIUNE_LOGIC)
set(HAILSTONE_TRIUNE_TESTS)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/projecttriune/CMakeLists.txt)
  add_subdirectory(plugins/projecttriune)
  set(HAILSTONE_HAS_TRIUNE ON)
endif()

add_executable(hailstone src/app/main.cpp src/app/client_app.cpp src/app/client_app_ui.cpp src/app/client_app_draw.cpp src/app/target_ring.cpp src/app/client_app_input.cpp src/app/client_app_testdrive.cpp src/app/client_app_pick.cpp src/app/client_app_items.cpp src/app/client_app_player.cpp src/app/client_app_zone.cpp src/app/client_app_pregame.cpp src/app/client_app_options.cpp src/app/client_app_commands.cpp src/app/client_app_info.cpp src/app/client_app_web.cpp src/app/client_app_login.cpp src/app/client_app_gamepad.cpp src/app/client_app_bugreport.cpp src/app/client_app_update.cpp src/app/client_app_addons.cpp src/app/input_script.cpp src/app/model_viewer.cpp ${HAILSTONE_WIN_MANIFEST} ${HAILSTONE_WIN_CLIENT_RESOURCES})
target_compile_options(hailstone PRIVATE ${HAILSTONE_WARNINGS})
# Windows: a Release (or MinSizeRel) client is a GUI program, so no console window opens beside it; main() stays the
# entry point. Other builds keep the console for the log. Errors that stop a start are shown in a dialog either way
# (app/startup_error.h), and the log is in logs/hailstone.log.
if(MSVC)
  target_link_options(hailstone PRIVATE $<$<CONFIG:Release,MinSizeRel>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
endif()
if(HAILSTONE_CLANG_TIDY)
  set_target_properties(hailstone PROPERTIES CXX_CLANG_TIDY "${HAILSTONE_TIDY_COMMAND}")
endif()
target_link_libraries(hailstone PRIVATE hailstone_engine)
target_include_directories(hailstone PRIVATE sdk ${PROJECT_BINARY_DIR}/generated)
set(_managed_ui_roots "=${CMAKE_CURRENT_SOURCE_DIR}/ui")
file(GLOB_RECURSE _managed_ui_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.lua")
if(HAILSTONE_BUILTIN_ADDON_SOURCE_DIR)
  set(HAILSTONE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
  add_subdirectory("${HAILSTONE_BUILTIN_ADDON_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/builtin-addon")
  if(NOT TARGET hailstone_builtin_addon)
    message(FATAL_ERROR "The built-in add-on must provide the static hailstone_builtin_addon target")
  endif()
  get_target_property(_builtin_type hailstone_builtin_addon TYPE)
  get_target_property(_builtin_id hailstone_builtin_addon HAILSTONE_BUILTIN_ADDON_ID)
  if(NOT _builtin_type STREQUAL "STATIC_LIBRARY" OR NOT _builtin_id MATCHES "^[a-z][a-z0-9_-]*$")
    message(FATAL_ERROR "The built-in add-on requires a static target and a safe HAILSTONE_BUILTIN_ADDON_ID")
  endif()
  target_link_libraries(hailstone PRIVATE hailstone_builtin_addon)
  target_compile_definitions(hailstone PRIVATE HAILSTONE_HAS_BUILTIN_ADDON=1 HAILSTONE_BUILTIN_ADDON_ID="${_builtin_id}")
  if(EXISTS "${HAILSTONE_BUILTIN_ADDON_SOURCE_DIR}/ui")
    list(APPEND _managed_ui_roots "plugin:${_builtin_id}=${HAILSTONE_BUILTIN_ADDON_SOURCE_DIR}/ui")
    file(GLOB_RECURSE _builtin_ui_sources CONFIGURE_DEPENDS "${HAILSTONE_BUILTIN_ADDON_SOURCE_DIR}/ui/*.lua")
    list(APPEND _managed_ui_sources ${_builtin_ui_sources})
    add_custom_command(TARGET hailstone POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${HAILSTONE_BUILTIN_ADDON_SOURCE_DIR}/ui"
              "$<TARGET_FILE_DIR:hailstone>/builtin/${_builtin_id}/ui")
  endif()
endif()
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/generated/core/managed_ui_manifest.h"
  COMMAND ${CMAKE_COMMAND} "-DROOTS=${_managed_ui_roots}"
          "-DOUTPUT=${PROJECT_BINARY_DIR}/generated/core/managed_ui_manifest.h"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ManagedUiManifest.cmake"
  DEPENDS ${_managed_ui_sources} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ManagedUiManifest.cmake"
  VERBATIM)
add_custom_target(hailstone_managed_ui_manifest DEPENDS "${PROJECT_BINARY_DIR}/generated/core/managed_ui_manifest.h")
if(HAILSTONE_MANAGED_CLIENT)
  target_compile_definitions(hailstone PRIVATE HAILSTONE_MANAGED_CLIENT=1)
  add_dependencies(hailstone hailstone_managed_ui_manifest)
endif()
set_target_properties(hailstone PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${HAILSTONE_TREE_DIR}$<0:>)
# Shaders are looked up next to the executable.
add_custom_command(TARGET hailstone POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${HAILSTONE_SHADER_OUT} $<TARGET_FILE_DIR:hailstone>/shaders)
if(WIN32)
  add_custom_command(TARGET hailstone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:hailstone> $<TARGET_FILE_DIR:hailstone>
    COMMAND_EXPAND_LISTS)
endif()
# The Lua UI packs (ui/<name>) next to the executable, kept in step with the source (cmake/SyncUiPacks.cmake): a
# changed script is copied, a removed one deleted, the others left as they are (so the running client reloads only
# what changed); other packs there are left alone.
add_custom_command(TARGET hailstone POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/ui -DDST=$<TARGET_FILE_DIR:hailstone>/ui
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SyncUiPacks.cmake)
add_custom_command(TARGET hailstone POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/icons $<TARGET_FILE_DIR:hailstone>/icons
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/pictures $<TARGET_FILE_DIR:hailstone>/pictures
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/flora $<TARGET_FILE_DIR:hailstone>/flora
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md $<TARGET_FILE_DIR:hailstone>)
if(HAILSTONE_MOLTENVK_DYLIB)
  add_custom_command(TARGET hailstone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HAILSTONE_MOLTENVK_DYLIB} $<TARGET_FILE_DIR:hailstone>)
endif()

# cmake --install build --prefix <dir> gives the same folder: <dir>/hailstone(.exe), shaders/, ui/, icons/, pictures/, CHANGELOG.md and the shared
# libraries the build made (SDL3 when it was fetched), found through an rpath of the binary's own folder. Only these
# shipped parts are installed; an update replaces them and leaves the player's data beside them alone. The example
# plugin is its own component (cmake --install build --component examples), and so are the server plugins when
# checked out (--component nms, --component projecttriune). A release ships neither: they are add-ons.
if(APPLE)
  set_target_properties(hailstone PROPERTIES INSTALL_RPATH "@executable_path")
elseif(UNIX)
  set_target_properties(hailstone PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
install(TARGETS hailstone RUNTIME DESTINATION .)
install(DIRECTORY ${HAILSTONE_SHADER_OUT}/ DESTINATION shaders FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui/ DESTINATION ui)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/icons/ DESTINATION icons)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/pictures/ DESTINATION pictures)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/flora/ DESTINATION flora)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md DESTINATION .) # the login screen's What's new
if(HAILSTONE_MOLTENVK_DYLIB)
  # macOS: Vulkan itself, beside the binary (platform/paths.h bundled_vulkan_library), with its licence.
  install(FILES ${HAILSTONE_MOLTENVK_DYLIB} DESTINATION .)
  install(FILES ${HAILSTONE_MOLTENVK_LICENSE} RENAME MoltenVK-LICENSE.txt DESTINATION .)
endif()
if(WIN32)
  install(FILES $<TARGET_RUNTIME_DLLS:hailstone> DESTINATION .)
elseif(TARGET SDL3-shared)
  get_target_property(hailstone_sdl_imported SDL3-shared IMPORTED)
  if(NOT hailstone_sdl_imported)
    install(TARGETS SDL3-shared LIBRARY DESTINATION . NAMELINK_SKIP)
  endif()
endif()
install(TARGETS MQ2Example LIBRARY DESTINATION plugins COMPONENT examples EXCLUDE_FROM_ALL) # a MODULE is a LIBRARY

# The browser helper's messages and the address rules, built with the client's flags (exceptions on): the helper
# (tools/hailstone-web, CEF's flags: no exceptions, no RTTI) links them from here.
add_library(hailstone_web_protocol STATIC src/web/browser_protocol.cpp src/web/page_rules.cpp src/core/web_url.cpp src/platform/binary_stdio.cpp)
target_include_directories(hailstone_web_protocol PUBLIC src sdk)
target_compile_options(hailstone_web_protocol PRIVATE ${HAILSTONE_WARNINGS})
if(HAILSTONE_BROWSER)
  add_subdirectory(tools/hailstone-web)
endif()

# Command-line tools.
set(HAILSTONE_TOOLS pfsdump texdump texpack wlddump florascan chrdump netprobe playprobe eqgdump audiodump hailstone_check hailstone_sign)
foreach(tool ${HAILSTONE_TOOLS})
  add_executable(${tool} tools/${tool}.cpp ${HAILSTONE_WIN_MANIFEST})
  target_compile_options(${tool} PRIVATE ${HAILSTONE_WARNINGS})
  if(HAILSTONE_CLANG_TIDY)
    set_target_properties(${tool} PROPERTIES CXX_CLANG_TIDY "${HAILSTONE_TIDY_COMMAND}")
  endif()
  target_link_libraries(${tool} PRIVATE hailstone_engine)
endforeach()
# Texture packs built from the player's archives (src/texpack, docs/graphics.md "Built packs"): shared by texpack, the
# tests and Hailstone Texture Studio (projecthailstone/hailstone-texstudio, which builds against this tree). The models
# run behind texpack::Model; the studio's ncnn backend is in its own repository.
add_library(hailstone_texpack STATIC
  src/texpack/texpack.cpp
  src/texpack/upscale.cpp
  src/texpack/pack_job.cpp
  src/texpack/model_catalog.cpp)
target_compile_options(hailstone_texpack PRIVATE ${HAILSTONE_WARNINGS})
target_link_libraries(hailstone_texpack PUBLIC hailstone_engine PRIVATE hailstone_bc7enc)
target_link_libraries(texpack PRIVATE hailstone_texpack)

if(HAILSTONE_BUILD_TESTS)
  enable_testing()
  add_executable(hailstone_tests ${HAILSTONE_WIN_MANIFEST}
    tests/test_main.cpp
    tests/bag_move_test.cpp
    tests/byte_io_test.cpp
    tests/env_test.cpp
    tests/sdk_old_names_test.cpp
    tests/private_file_test.cpp
    tests/durable_file_test.cpp
    tests/server_script_test.cpp
    tests/server_tools_test.cpp
    tests/float_chars_test.cpp
    tests/file_time_test.cpp
    tests/screen_pictures_test.cpp
    tests/window_icon_test.cpp
    tests/sha256_test.cpp
    tests/zip_read_test.cpp
    tests/http_get_test.cpp
    tests/updater_test.cpp
    tests/addon_manifest_test.cpp
    tests/addon_index_test.cpp
    tests/changelog_test.cpp
    tests/addon_install_test.cpp
    tests/addon_manager_test.cpp
    tests/bug_report_test.cpp
    tests/bug_report_window_test.cpp
    tests/startup_error_test.cpp
    tests/look_eviction_test.cpp
    tests/plugin_hooks_test.cpp
    tests/plugin_policy_test.cpp
    tests/server_files_test.cpp
    tests/mq_tlo_test.cpp
    tests/mq_combat_test.cpp
    tests/automation_test.cpp
    tests/mq_lua_test.cpp
    tests/mq_event_pattern_test.cpp
    tests/lua_social_test.cpp
    tests/code_syntax_test.cpp
    tests/native_plugins_test.cpp
    tests/plugin_consent_test.cpp
    tests/plugin_ui_sandbox_test.cpp
    tests/lighting_test.cpp
    tests/shadows_test.cpp
    tests/culling_test.cpp
    tests/shadow_chunks_test.cpp
    tests/wrap_math_test.cpp
    tests/wipe_test.cpp
    tests/log_escape_test.cpp
    tests/step_clock_test.cpp
    tests/frame_rate_test.cpp
    tests/particle_view_test.cpp
    tests/zone_fires_test.cpp
    tests/texture_frame_test.cpp
    tests/point_shadows_test.cpp
    tests/caster_bounds_test.cpp
    tests/descriptor_limits_test.cpp
    tests/swapchain_size_test.cpp
    tests/texture_claims_test.cpp
    tests/zone_settle_test.cpp
    tests/work_budget_test.cpp
    tests/latest_jobs_test.cpp
    tests/character_share_test.cpp
    tests/light_grid_test.cpp
    tests/retire_queue_test.cpp
    tests/weather_test.cpp
    tests/texture_overrides_test.cpp
    tests/layout_test.cpp
    tests/layout_extras_test.cpp
    tests/settings_files_test.cpp
    tests/character_files_test.cpp
    tests/client_options_test.cpp
    tests/buff_overlays_test.cpp
    tests/target_indicator_test.cpp
    tests/item_options_test.cpp
    tests/option_commands_test.cpp
    tests/font_faces_test.cpp
    tests/process_test.cpp
    tests/flora_test.cpp
    tests/zone_flora_test.cpp
    tests/client_info_commands_test.cpp
    tests/gameplay_commands_test.cpp
    tests/mouse_look_test.cpp
    tests/mouse_capture_test.cpp
    tests/asset_picker_test.cpp
    ${HAILSTONE_NMS_TESTS}
    ${HAILSTONE_NMS_LOGIC}
    ${HAILSTONE_TRIUNE_TESTS}
    ${HAILSTONE_TRIUNE_LOGIC}
    tests/tcp_socket_test.cpp
    tests/plugin_tcp_test.cpp
    tests/merchant_buy_test.cpp
    tests/merchant_usable_test.cpp
    tests/ascii_case_test.cpp
    tests/ui_pack_windows_test.cpp
    tests/ui_pack_stale_state_test.cpp
    tests/net_session_test.cpp
    tests/reliable_channel_vectors_test.cpp
    tests/vehicle_ladder_vectors_test.cpp
    tests/boats_test.cpp
    tests/npc_collision_test.cpp
    tests/mounts_test.cpp
    tests/spawn_is_named_test.cpp
    tests/world_state_smooth_test.cpp
    tests/attack_check_test.cpp
    tests/attack_rules_vectors_test.cpp
    tests/ui_rules_vectors_test.cpp
    tests/net_login_test.cpp
    tests/net_packets_test.cpp
    tests/pfs_test.cpp
    tests/wld_test.cpp
    tests/eqg_test.cpp
    tests/regions_test.cpp
    tests/view_distance_test.cpp
    tests/fog_vectors_test.cpp
    tests/weather_vectors_test.cpp
    tests/precipitation_test.cpp
    tests/sky_colours_test.cpp
    tests/scene_light_test.cpp
    tests/collision_test.cpp
    tests/nav_test.cpp
    tests/map_data_test.cpp
    tests/map_files_test.cpp
    tests/player_path_test.cpp
    tests/atlas_test.cpp
    tests/zone_routes_test.cpp
    tests/zone_atlas_api_test.cpp
    tests/map_windows_test.cpp
    tests/map_spawns_test.cpp
    tests/ui_windows_test.cpp
    tests/ui_labels_test.cpp
    tests/ui_more_windows_test.cpp
    tests/conversation_journal_test.cpp
    tests/mail_windows_test.cpp
    tests/marketplace_test.cpp
    tests/chat_windows_test.cpp
    tests/hot_button_drag_test.cpp
    tests/ui_keyboard_capture_test.cpp
    tests/inventory_hot_button_test.cpp
    tests/pet_hot_buttons_test.cpp
    tests/lua_slot_info_test.cpp
    tests/social_editor_test.cpp
    tests/app_frame_work_test.cpp
    tests/frame_pace_test.cpp
    tests/alt_currency_ui_test.cpp
    tests/useful_npcs_test.cpp
    tests/guide_path_test.cpp
    tests/guild_tribute_page_test.cpp
    tests/guild_window_test.cpp
    tests/auto_inventory_ui_test.cpp
    tests/inventory_bags_key_test.cpp
    tests/parcel_money_ui_test.cpp
    tests/point_merchant_inspect_ui_test.cpp
    tests/trader_shop_test.cpp
    tests/blocked_buffs_ui_test.cpp
    tests/own_hp_test.cpp
    tests/stats_ui_test.cpp
    tests/spell_sets_test.cpp
    tests/hot_button_sets_test.cpp
    tests/group_commands_test.cpp
    tests/group_search_test.cpp
    tests/chat_ui_test.cpp
    tests/inventory_bandolier_ui_test.cpp
    tests/trade_ui_test.cpp
    tests/lua_scripts_test.cpp
    tests/adventure_packets_test.cpp
    tests/adventure_request_test.cpp
    tests/dz_packets_test.cpp
    tests/guild_bank_packets_test.cpp
    tests/notice_packets_test.cpp
    tests/web_url_test.cpp
    tests/browser_host_test.cpp
    tests/server_links_test.cpp
    tests/swept_sphere_test.cpp
    tests/eq_strings_test.cpp
    tests/distance_test.cpp
    tests/texture_test.cpp
    tests/dev_ui_test.cpp
    tests/character_test.cpp
    tests/rider_seat_test.cpp
    tests/appearance_test.cpp
    tests/appearance_vectors_test.cpp
    tests/combat_test.cpp
    tests/plate_test_test.cpp
    tests/nameplate_overlap_test.cpp
    tests/nameplate_style_test.cpp
    tests/spells_test.cpp
    tests/proc_cast_stage_test.cpp
    tests/passage_of_time_test.cpp
    tests/item_packets_test.cpp
    tests/augments_test.cpp
    tests/item_preview_test.cpp
    tests/doors_test.cpp
    tests/touching_switch_test.cpp
    tests/position_tracker_test.cpp
    tests/perf_ui_test.cpp
    tests/lua_gc_test.cpp
    tests/lua_ui_api_test.cpp
    tests/tradeskill_batch_test.cpp
    tests/tradeskill_combine_test.cpp
    tests/missiles_test.cpp
    tests/multiclass_test.cpp
    tests/server_ext_chain_test.cpp
    tests/native_extension_test.cpp
    tests/lua_folders_test.cpp
    tests/lua_character_settings_test.cpp
    tests/lua_plugin_test.cpp
    tests/spell_projectiles_test.cpp
    tests/commerce_test.cpp
    tests/bandolier_swap_test.cpp
    tests/bank_test.cpp
    tests/real_estate_test.cpp
    tests/lua_eq_pregame_test.cpp
    tests/lua_eq_realestate_test.cpp
    tests/mock_real_estate_test.cpp
    tests/placement_test.cpp
    tests/mock_placement_test.cpp
    tests/sky_test.cpp
    tests/particles_test.cpp
    tests/spell_effect_timers_test.cpp
    tests/wld_particles_test.cpp
    tests/wld_detail_test.cpp
    tests/movement_test.cpp
    tests/movement_vectors_test.cpp
    tests/mechanics_test.cpp
    tests/mechanics_vectors_test.cpp
    tests/abilities_test.cpp
    tests/ability_book_test.cpp
    tests/combat_ability_test.cpp
    tests/ability_commands_test.cpp
    tests/hot_button_state_test.cpp
    tests/group_packets_test.cpp
    tests/progress_test.cpp
    tests/task_window_test.cpp
    tests/social_test.cpp
    tests/options_social_test.cpp
    tests/player_market_test.cpp
    tests/barter_test.cpp
    tests/xtarget_test.cpp
    tests/player_customization_test.cpp
    tests/char_select_scene_test.cpp
    tests/lua_eq_args_test.cpp
    tests/lua_eq_test.cpp
    tests/ui_ini_test.cpp
    tests/ui_packs_test.cpp
    tests/item_display_test.cpp
    tests/game_vectors_test.cpp
    tests/key_commands_test.cpp
    tests/mouse_buttons_test.cpp
    tests/cameras_test.cpp
    tests/gamepad_bindings_test.cpp
    tests/anim_vectors_test.cpp
    tests/anim_layers_test.cpp
    tests/hit_by_spell_vectors_test.cpp
    tests/spell_gems_vectors_test.cpp
    tests/usable_gems_test.cpp
    tests/zone_change_request_test.cpp
    tests/spawn_hook_events_test.cpp
    tests/cast_spell_vectors_test.cpp
    tests/sound_files_test.cpp
    tests/spirv_file_test.cpp
    tests/environment_presets_test.cpp
    tests/particle_textures_test.cpp
    tests/xmi_test.cpp
    tests/zone_emitters_test.cpp
    tests/sound_rules_test.cpp
    tests/mixer_test.cpp
    tests/rows2_vectors_test.cpp
    tests/rows3_vectors_test.cpp
    tests/item_moves_vectors_test.cpp
    tests/item_sort_test.cpp
    tests/kept_items_test.cpp
    tests/inventory_manager_ui_test.cpp
    tests/audio_vectors_test.cpp
    tests/melee_vectors_test.cpp
    tests/spawn_heights_vectors_test.cpp
    tests/use_skill_vectors_test.cpp
    tests/picking_vectors_test.cpp
    tests/zoning_vectors_test.cpp
    tests/neighbour_zones_test.cpp
    tests/zone_liquid_test.cpp
    tests/player_placement_test.cpp
    tests/step_landing_test.cpp
    tests/jump_hang_test.cpp
    tests/stop_moving_test.cpp
    tests/zone_ladders_test.cpp
    tests/chat_trade_vectors_test.cpp
    tests/hotbutton_tracking_vectors_test.cpp
    tests/compass_test.cpp
    tests/context_tip_events_test.cpp
    tests/merc_vectors_test.cpp
    tests/fuzz_alloc_guard.cpp
    tests/fuzz_net_test.cpp
    tests/fuzz_assets_test.cpp
    tests/movement_inputs_test.cpp
    tests/mock_server/profile_builder.cpp
    tests/mock_server/soe_endpoint.cpp
    tests/mock_server/mock_eqemu.cpp
    tests/mock_session_test.cpp
    tests/mock_login_test.cpp
    tests/mock_game_test.cpp
    tests/mock_eqemu_compat_test.cpp
    tests/item_use_test.cpp
    tests/spell_book_swap_test.cpp
    tests/feign_death_spell_test.cpp
    tests/loot_test.cpp
    tests/player_menu_test.cpp
    tests/connection_state_test.cpp
    tests/texpack_test.cpp
    tests/addon_tool_test.cpp
  )
  target_include_directories(hailstone_tests PRIVATE tests sdk)
  target_compile_options(hailstone_tests PRIVATE ${HAILSTONE_WARNINGS})
  target_link_libraries(hailstone_tests PRIVATE hailstone_engine hailstone_texpack doctest::doctest)
  # The server extension test plugin (tests/plugins/ExtendTest.c), the same as an ABI 1 plugin, and the same with its
  # hook table under the export name after 0.6.
  add_library(ExtendTest MODULE tests/plugins/ExtendTest.c)
  add_library(ExtendTestV1 MODULE tests/plugins/ExtendTest.c)
  target_compile_definitions(ExtendTestV1 PRIVATE HAILSTONE_TEST_ABI1)
  add_library(ExtendTestNew MODULE tests/plugins/ExtendTest.c)
  target_compile_definitions(ExtendTestNew PRIVATE HAILSTONE_TEST_NEW_HOOKS)
  # A C++ test plugin calling the host table from where it should not (tests/plugins/HostProbe.cpp).
  add_library(HostProbe MODULE tests/plugins/HostProbe.cpp)
  add_library(HostProbeThrow MODULE tests/plugins/HostProbe.cpp)
  target_compile_definitions(HostProbeThrow PRIVATE HAILSTONE_PROBE_THROW_INIT)
  foreach(p ExtendTest ExtendTestV1 ExtendTestNew HostProbe HostProbeThrow)
    target_include_directories(${p} PRIVATE sdk)
    set_target_properties(${p} PROPERTIES PREFIX "")
  endforeach()
  # The stand-in browser helper (tests/browser_host_test.cpp), beside hailstone_tests (and its SDL3.dll on Windows).
  add_executable(hailstone_mock_web_helper tests/mock_web_helper.cpp)
  target_include_directories(hailstone_mock_web_helper PRIVATE src sdk)
  target_compile_options(hailstone_mock_web_helper PRIVATE ${HAILSTONE_WARNINGS})
  target_link_libraries(hailstone_mock_web_helper PRIVATE hailstone_web_protocol)
  set_target_properties(hailstone_mock_web_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:hailstone_tests>)
  add_dependencies(hailstone_tests MQ2Example ExtendTest ExtendTestV1 ExtendTestNew HostProbe HostProbeThrow hailstone_mock_web_helper)
  target_compile_definitions(hailstone_tests PRIVATE HAILSTONE_EXAMPLE_PLUGIN="$<TARGET_FILE:MQ2Example>"
    HAILSTONE_PROBE_PLUGIN="$<TARGET_FILE:HostProbe>" HAILSTONE_PROBE_THROW_PLUGIN="$<TARGET_FILE:HostProbeThrow>"
    HAILSTONE_EXTEND_PLUGIN="$<TARGET_FILE:ExtendTest>" HAILSTONE_EXTEND_PLUGIN_V1="$<TARGET_FILE:ExtendTestV1>"
    HAILSTONE_EXTEND_PLUGIN_NEW="$<TARGET_FILE:ExtendTestNew>"
    HAILSTONE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data" HAILSTONE_CLIENT_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
    # The recorded vectors are looked for beside the source tree (tests/vector_files.h), whatever the run's directory.
    HAILSTONE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    HAILSTONE_MOCK_WEB_HELPER="$<TARGET_FILE:hailstone_mock_web_helper>"
    # doctest otherwise forward-declares std::ostream, which MSVC's STL rejects when a CHECK prints a string.
    DOCTEST_CONFIG_USE_STD_HEADERS)
  if(HAILSTONE_HAS_NMS)
    target_include_directories(hailstone_tests PRIVATE ${HAILSTONE_NMS_DIR} ${HAILSTONE_NMS_DIR}/tests)
    add_dependencies(hailstone_tests nms)
    target_compile_definitions(hailstone_tests PRIVATE HAILSTONE_HAS_NMS=1 HAILSTONE_NMS_PLUGIN="$<TARGET_FILE:nms>"
      HAILSTONE_NMS_UI_DIR="${HAILSTONE_NMS_DIR}/ui")
  endif()
  if(HAILSTONE_HAS_TRIUNE)
    target_include_directories(hailstone_tests PRIVATE ${HAILSTONE_TRIUNE_DIR} ${HAILSTONE_TRIUNE_DIR}/tests)
    add_dependencies(hailstone_tests projecttriune)
    target_compile_definitions(hailstone_tests PRIVATE HAILSTONE_HAS_TRIUNE=1 HAILSTONE_TRIUNE_PLUGIN="$<TARGET_FILE:projecttriune>"
      HAILSTONE_TRIUNE_UI_DIR="${HAILSTONE_TRIUNE_DIR}/ui")
  endif()
  if(HAILSTONE_BROWSER)
    add_dependencies(hailstone_tests hailstone-web)
    target_compile_definitions(hailstone_tests PRIVATE HAILSTONE_WEB_HELPER="$<TARGET_FILE:hailstone-web>")
  endif()
  add_test(NAME hailstone_tests COMMAND hailstone_tests)
  add_executable(hailstone_managed_tests tests/managed_test_main.cpp tests/managed_distribution_test.cpp
    tests/managed_addons_test.cpp tests/managed_automation_test.cpp tests/managed_ui_test.cpp tests/managed_updates_test.cpp)
  add_dependencies(hailstone_managed_tests hailstone_managed_ui_manifest)
  target_include_directories(hailstone_managed_tests PRIVATE tests sdk ${PROJECT_BINARY_DIR}/generated)
  target_compile_definitions(hailstone_managed_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    HAILSTONE_CLIENT_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui")
  target_compile_options(hailstone_managed_tests PRIVATE ${HAILSTONE_WARNINGS})
  target_link_libraries(hailstone_managed_tests PRIVATE hailstone_engine doctest::doctest)
  add_test(NAME hailstone_managed_tests COMMAND hailstone_managed_tests)
  # Windows has no rpath: SDL3.dll has to sit next to the executable.
  if(WIN32)
    add_custom_command(TARGET hailstone_managed_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:hailstone_managed_tests> $<TARGET_FILE_DIR:hailstone_managed_tests>
      COMMAND_EXPAND_LISTS)
    add_custom_command(TARGET hailstone_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:hailstone_tests> $<TARGET_FILE_DIR:hailstone_tests>
      COMMAND_EXPAND_LISTS)
  endif()
endif()

# libFuzzer targets for the parsers: cmake -DHAILSTONE_LIBFUZZER=ON -DCMAKE_CXX_COMPILER=clang++ (implies HAILSTONE_SANITIZE).
if(HAILSTONE_LIBFUZZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "HAILSTONE_LIBFUZZER needs Clang")
  endif()
  foreach(target pfs wld texture soe packets decoders zip)
    string(TOUPPER ${target} upper)
    add_executable(fuzz_${target} tests/fuzz_libfuzzer.cpp)
    target_compile_definitions(fuzz_${target} PRIVATE HAILSTONE_FUZZ_${upper})
    target_compile_options(fuzz_${target} PRIVATE ${HAILSTONE_WARNINGS} -fsanitize=fuzzer-no-link)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_${target} PRIVATE hailstone_engine)
  endforeach()
  # The checked-out server plugins' decoders (tests/fuzz_decoders.inc HAILSTONE_FUZZ_PLUGIN_DECODER).
  target_include_directories(fuzz_decoders PRIVATE sdk)
  if(HAILSTONE_HAS_NMS)
    target_sources(fuzz_decoders PRIVATE ${HAILSTONE_NMS_DIR}/nms_packets.cpp ${HAILSTONE_NMS_DIR}/nms_rules.cpp)
    target_include_directories(fuzz_decoders PRIVATE ${HAILSTONE_NMS_DIR} ${HAILSTONE_NMS_DIR}/tests)
    target_compile_definitions(fuzz_decoders PRIVATE HAILSTONE_HAS_NMS=1)
  endif()
  if(HAILSTONE_HAS_TRIUNE)
    target_sources(fuzz_decoders PRIVATE ${HAILSTONE_TRIUNE_DIR}/triune_packets.cpp ${HAILSTONE_TRIUNE_DIR}/triune_rules.cpp
                   ${HAILSTONE_TRIUNE_DIR}/triune_windows.cpp)
    target_include_directories(fuzz_decoders PRIVATE ${HAILSTONE_TRIUNE_DIR} ${HAILSTONE_TRIUNE_DIR}/tests)
    target_compile_definitions(fuzz_decoders PRIVATE HAILSTONE_HAS_TRIUNE=1)
  endif()
endif()

# Windows has no rpath: the shared dependencies' DLLs (SDL3) next to each tool too (hailstone and hailstone_tests have
# their own copy steps above).
if(WIN32)
  foreach(exe ${HAILSTONE_TOOLS})
    add_custom_command(TARGET ${exe} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${exe}> $<TARGET_FILE_DIR:${exe}>
      COMMAND_EXPAND_LISTS)
  endforeach()
endif()
