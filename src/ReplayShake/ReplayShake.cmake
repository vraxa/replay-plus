# Replay Shake (README.md), added to ReSkate's own targets from a single line in CMakeLists.txt,
# after the runtime and the overlay are defined and before every target is configured:
#
#     include(cmake/Apps.cmake)
#     include(Extension/ReplayShake/ReplayShake.cmake OPTIONAL)
#
# Everything else it needs is set up here, so a new ReSkate rarely touches what Replay Shake
# changed (tools/update-reskate.ps1 does the update).

# Should ReSkate ever do one of the things below another way, the build carries on and says so: as
# a warning, and in replay_shake_notes.txt in the build folder, which tools/update-reskate.ps1 repeats.
set_property(GLOBAL PROPERTY REPLAY_SHAKE_NOTES "")
function(replay_shake_note text)
    message(WARNING "Replay Shake: ${text}")
    set_property(GLOBAL APPEND PROPERTY REPLAY_SHAKE_NOTES "${text}")
endfunction()

# The game build this ReSkate reads its addresses for: addresses.h points `addr` at its folder.
file(READ "${PROJECT_SOURCE_DIR}/Engine/Game/Build/addresses.h" replay_shake_addresses)
string(REGEX MATCH "namespace addr = game::build::v([0-9A-Za-z_]+);" replay_shake_found "${replay_shake_addresses}")
set(replay_shake_build "${CMAKE_MATCH_1}")
if(NOT replay_shake_build OR NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/Engine/Game/Build/${replay_shake_build}")
    message(FATAL_ERROR "Replay Shake: Engine/Game/Build/addresses.h no longer says which game build folder ReSkate "
        "reads (namespace addr = game::build::v<build>;). See Extension/ReplayShake/README.md, Staying up to date.")
endif()

# What ReSkate reads of the game outside the build folder, Replay Shake takes from ReSkate's own
# sources, so a game build that moves it needs nothing changed here either. Should ReSkate read one
# of them another way, game build 20260929's is kept.
#
# replay_shake_find(<variable> <what> <source> <regex, a group per number> <build 20260929's>
#                   [<what to do should it be read wrong>])
set(replay_shake_n "(0x[0-9A-Fa-f]+|[0-9]+)")
function(replay_shake_find variable what source pattern fallback)
    set(then "If Probe then lists no cameras, see Extension/ReplayShake/README.md, Staying up to date.")
    if(ARGC GREATER 5)
        set(then "${ARGV5}")
    endif()
    set(path "${PROJECT_SOURCE_DIR}/${source}")
    if(EXISTS "${path}")
        file(READ "${path}" text)
        string(REGEX MATCH "${pattern}" found "${text}")
        if(found)
            set(values "")
            foreach(group RANGE 1 ${CMAKE_MATCH_COUNT})
                list(APPEND values "${CMAKE_MATCH_${group}}")
            endforeach()
            set(${variable} "${values}" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${variable} "${fallback}" PARENT_SCOPE)
    replay_shake_note("${source} no longer reads ${what} as Replay Shake knows it, so Replay Shake reads it as on game build 20260929. ${then}")
endfunction()

replay_shake_find(replay_shake_views_begin "where the camera view manager's views begin" Extension/Skater/source_spawn_camera.cpp
    "begin = reader\\.pointer\\(id\\.view_manager, ${replay_shake_n}\\)" 8)
replay_shake_find(replay_shake_views_end "where the camera view manager's views end" Extension/Skater/source_spawn_camera.cpp
    "end = reader\\.pointer\\(id\\.view_manager, ${replay_shake_n}\\)" 0x10)
replay_shake_find(replay_shake_view_ids "a camera view's ids" Extension/Skater/source_spawn_camera.cpp
    "!reader\\.value<std::uint32_t>\\(view, ${replay_shake_n}\\) && !reader\\.value<std::uint32_t>\\(view, ${replay_shake_n}\\)"
    "0x148;0x14c")
replay_shake_find(replay_shake_view_enabled "whether a camera view is on" Extension/Skater/source_spawn_camera.cpp
    "view_enabled = reader\\.value<unsigned char>\\(id\\.view, ${replay_shake_n}\\)" 0x45d)
replay_shake_find(replay_shake_view_camera "a camera view's camera on show" Extension/Skater/source_spawn_camera.cpp
    "active = reader\\.pointer\\(id\\.view, ${replay_shake_n}\\)" 0x2b0)
replay_shake_find(replay_shake_ui_models "the UI manager's model manager" Extension/UI/NativeMenu/native_menu_dump.cpp
    "ui \\? read<Address>\\(ui \\+ ${replay_shake_n}\\)" 0x140)
set(replay_shake_sources "replayshake source 1 then finds no camera to shake: see Extension/ReplayShake/README.md, Staying up to date.")
replay_shake_find(replay_shake_camera_transform "a camera's world transform" Extension/Skater/source_spawn_camera.cpp
    "camera\\.active, ${replay_shake_n} \\+ row \\* 16\\)" 0x50 "${replay_shake_sources}")
replay_shake_find(replay_shake_camera_fov "a camera's field of view" Extension/Skater/source_spawn_camera.cpp
    "reader\\.value<float>\\(camera\\.active, ${replay_shake_n}\\)" 0xac "${replay_shake_sources}")
set(replay_shake_directors "If Probe then shows no camera's director, a replay camera added by its director does not count: see Extension/ReplayShake/README.md, Staying up to date.")
replay_shake_find(replay_shake_camera_evaluator "a camera's evaluator" Extension/Skater/camera_observer.cpp
    "memory::peek\\(result\\.camera \\+ ${replay_shake_n}, backlink\\) && backlink == evaluator" 0x180 "${replay_shake_directors}")
replay_shake_find(replay_shake_evaluator_camera "an evaluator's camera" Extension/Skater/camera_observer.cpp
    "memory::peek\\(evaluator \\+ ${replay_shake_n}, result\\.camera\\)" 0x138 "${replay_shake_directors}")
replay_shake_find(replay_shake_evaluator_entries "an evaluator's camera entries" Extension/Skater/camera_observer.cpp
    "memory::peek\\(evaluator \\+ ${replay_shake_n}, collection\\)" 8 "${replay_shake_directors}")
replay_shake_find(replay_shake_entries "where an evaluator's camera entries begin and end" Extension/Skater/camera_observer.cpp
    "memory::peek\\(collection \\+ ${replay_shake_n}, begin\\) && memory::peek\\(collection \\+ ${replay_shake_n}, end\\)"
    "8;0x10" "${replay_shake_directors}")
replay_shake_find(replay_shake_entry_stride "how far apart an evaluator's camera entries are" Extension/Skater/camera_observer.cpp
    "\\(end - begin\\) % ${replay_shake_n} == 0" 0x10 "${replay_shake_directors}")
set(replay_shake_menus "Replay Shake may then not tell the replay editor from gameplay. Should the shake stop showing in the editor, turn on Shake gameplay too on the REPLAY SHAKE page (Extension/ReplayShake/README.md, Staying up to date).")
replay_shake_find(replay_shake_screen_data "the game's menu state" Extension/Multiplayer/Hud/game_ui_state.cpp
    "screen_data_hash = ${replay_shake_n};" 0x2c5645a1 "${replay_shake_menus}")
replay_shake_find(replay_shake_screen_in_menu "whether a game menu is up" Extension/Multiplayer/Hud/game_ui_state.cpp
    "in_menu_offset = ${replay_shake_n}[,;]" 20 "${replay_shake_menus}")
replay_shake_find(replay_shake_screen_hide_ui "whether the game hides its UI" Extension/Multiplayer/Hud/game_ui_state.cpp
    "hide_ui_offset = ${replay_shake_n}[,;]" 80 "${replay_shake_menus}")
# The camera entries gameplay's own camera selects (the game build's camera_observer.h): as a replay
# camera's director they say no more than a camera's vtable, gameplay's too.
set(replay_shake_gameplay_directors "")
set(replay_shake_observer "Engine/Game/Build/${replay_shake_build}/camera_observer.h")
if(EXISTS "${PROJECT_SOURCE_DIR}/${replay_shake_observer}")
    file(READ "${PROJECT_SOURCE_DIR}/${replay_shake_observer}" replay_shake_text)
    string(REGEX MATCHALL "entry_vtable = 0x[0-9A-Fa-f]+" replay_shake_found "${replay_shake_text}")
    foreach(replay_shake_match IN LISTS replay_shake_found)
        string(REGEX REPLACE "^.* = " "" replay_shake_value "${replay_shake_match}")
        list(APPEND replay_shake_gameplay_directors "${replay_shake_value}")
    endforeach()
endif()
if(NOT replay_shake_gameplay_directors)
    replay_shake_note("${replay_shake_observer} no longer lists gameplay's camera entries (entry_vtable), so a replay camera added by such a director would shake gameplay too. Add the replay editor's own director, one that does not show while skating.")
endif()
list(LENGTH replay_shake_gameplay_directors replay_shake_gameplay_director_count)
list(JOIN replay_shake_gameplay_directors ", " replay_shake_gameplay_directors)
list(GET replay_shake_entries 0 replay_shake_entries_begin)
list(GET replay_shake_entries 1 replay_shake_entries_end)
list(GET replay_shake_view_ids 0 replay_shake_view_id_first)
list(GET replay_shake_view_ids 1 replay_shake_view_id_second)

# The game build's headers Replay Shake reads, and the layout above, through one generated header:
# its sources never name the build folder, so a new game build in ReSkate needs nothing changed here.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/replay_shake_build.h" @ONLY CONTENT
"#pragma once
// Generated by Extension/ReplayShake/ReplayShake.cmake for game build folder @replay_shake_build@.
#include \"Engine/Game/Build/addresses.h\"
#include \"Engine/Game/Build/@replay_shake_build@/client_source_spawn.h\"
#include \"Engine/Game/Build/@replay_shake_build@/engine.h\"
#include \"Engine/Game/Build/@replay_shake_build@/replay_export.h\"
#include \"Engine/Game/Build/@replay_shake_build@/skater_render.h\"
#include <array>
#include <cstddef>
#include <cstdint>

namespace dingosdk::replay_shake::layout {
// The camera views, as Extension/Skater/source_spawn_camera.cpp reads them: the view manager
// (client_source_spawn.h) lists its views from +views_begin to +views_end; each view has two ids
// (both 0 on the local player's own view), whether it is on, and the camera it has on show.
inline constexpr std::uintptr_t views_begin = @replay_shake_views_begin@, views_end = @replay_shake_views_end@;
inline constexpr std::array<std::uintptr_t, 2> view_ids{@replay_shake_view_id_first@, @replay_shake_view_id_second@};
inline constexpr std::uintptr_t view_enabled = @replay_shake_view_enabled@, view_camera = @replay_shake_view_camera@;
inline constexpr std::size_t most_views = 32;
// The UI model manager in the UI manager (engine.h ui_manager), as `ui dump` reads it
// (Extension/UI/NativeMenu/native_menu_dump.cpp).
inline constexpr std::uintptr_t ui_models = @replay_shake_ui_models@;
// A camera's own world transform (rows right, up, back, position, as the views take it) and vertical
// field of view in degrees, as Extension/Skater/source_spawn_camera.cpp reads the active camera's.
inline constexpr std::uintptr_t camera_transform = @replay_shake_camera_transform@, camera_fov = @replay_shake_camera_fov@;
// A camera's director, what moves it (the game's own camera, the replay editor's...), as
// Extension/Skater/camera_observer.cpp reads it: the camera's evaluator at +camera_evaluator, which
// points back at the camera from +evaluator_camera, has its camera entries listed at
// +evaluator_entries (a list that points back at the evaluator, its entries from +entries_begin to
// +entries_end, entry_stride apart); the last of them is the one selected.
inline constexpr std::uintptr_t camera_evaluator = @replay_shake_camera_evaluator@, evaluator_camera = @replay_shake_evaluator_camera@;
inline constexpr std::uintptr_t evaluator_entries = @replay_shake_evaluator_entries@;
inline constexpr std::uintptr_t entries_begin = @replay_shake_entries_begin@, entries_end = @replay_shake_entries_end@;
inline constexpr std::uintptr_t entry_stride = @replay_shake_entry_stride@;
inline constexpr std::size_t most_entries = 32;
// The entries gameplay's own camera selects (camera_observer.h): no help telling the replay editor apart.
inline constexpr std::array<std::uintptr_t, @replay_shake_gameplay_director_count@> gameplay_directors{@replay_shake_gameplay_directors@};
// The game's menu state (UI/Foundations/State/DelMar_UI_ScreenData), as
// Extension/Multiplayer/Hud/game_ui_state.cpp reads it: the schema hash of its root view model, and
// the bytes in it that say a game menu is up and that the game hides its UI.
inline constexpr std::uint32_t screen_data = @replay_shake_screen_data@;
inline constexpr std::uintptr_t screen_in_menu = @replay_shake_screen_in_menu@, screen_hide_ui = @replay_shake_screen_hide_ui@;
} // namespace dingosdk::replay_shake::layout
")

# The shake, its timeline and its commands: no game access, shared by the runtime and the menu.
add_library(dingosdk_replay_shake_model STATIC
    Extension/ReplayShake/replay_shake_frames.cpp
    Extension/ReplayShake/replay_shake_model.cpp
    Extension/ReplayShake/replay_shake_settings.cpp)
target_link_libraries(dingosdk_replay_shake_model PUBLIC dingosdk_json)

# One of ReSkate's files takes hook_interpose.h ahead of its first line, and stays exactly as ReSkate
# has it (hook_interpose.h says why): its hook calls are made in `namespace`, unqualified, and its
# hook is the one on `marker`. With `switches` ON, its enabling and disabling go through
# hook_interpose.h too. `meaning` is what it means should the file hook another way.
function(replay_shake_interpose file namespace marker switches meaning)
    set(text "")
    if(EXISTS "${PROJECT_SOURCE_DIR}/${file}")
        file(READ "${PROJECT_SOURCE_DIR}/${file}" text)
    endif()
    string(FIND "${text}" "namespace ${namespace}" at_namespace)
    string(FIND "${text}" "${marker}" at_marker)
    string(REGEX MATCH "[^:A-Za-z0-9_]hook_prepare\\(" calls "${text}")
    if(at_namespace EQUAL -1 OR at_marker EQUAL -1 OR NOT calls)
        replay_shake_note("${file} no longer hooks ${marker} the way Replay Shake knows (hook_prepare in ${namespace}): ${meaning}")
    endif()
    string(REGEX MATCH "hook_queue_enable|hook_apply_queued" queued "${text}")
    if(switches AND queued)
        replay_shake_note("${file} now switches its hooks in a queue (hook_queue_enable), which hook_interpose.h does not route: ${meaning}")
    endif()
    if(MSVC)
        set_property(SOURCE "${file}" APPEND PROPERTY COMPILE_OPTIONS "/FIExtension/ReplayShake/hook_interpose.h")
    else()
        set_property(SOURCE "${file}" APPEND PROPERTY COMPILE_OPTIONS
            "-include${PROJECT_SOURCE_DIR}/Extension/ReplayShake/hook_interpose.h")
    endif()
endfunction()

if(TARGET dingosdk_runtime)
    target_sources(dingosdk_runtime PRIVATE
        Extension/ReplayShake/camera_update_hook.cpp
        Extension/ReplayShake/game_view_hook.cpp
        Extension/ReplayShake/render_view_hook.cpp
        Extension/ReplayShake/replay_shake.cpp
        Extension/ReplayShake/replay_shake_commands.cpp
        Extension/ReplayShake/replay_shake_editor.cpp
        Extension/ReplayShake/replay_shake_export.cpp
        Extension/ReplayShake/replay_shake_watch.cpp)
    target_include_directories(dingosdk_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(dingosdk_runtime PRIVATE dingosdk_replay_shake_model)
    replay_shake_interpose(Extension/Skater/xray_render.cpp dingosdk::xray render_view_contract ON
        "should it still hook the renderer's view set-up, the skater x-ray (plugins' Hall of Meat bones) and Replay Shake cannot both (logs/ReSkate.log says which could not).")
    replay_shake_interpose(Extension/Rendering/replay_export.cpp dingosdk::replay_export submit_frame OFF
        "exports are then timed by the playhead (or the game's clock) rather than by their frames.")
    # Anything else of ReSkate's on the renderer's view set-up could not share its one detour.
    file(GLOB_RECURSE replay_shake_sources RELATIVE "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/Extension/*.cpp" "${PROJECT_SOURCE_DIR}/Runtime/*.cpp")
    foreach(source IN LISTS replay_shake_sources)
        if(NOT source MATCHES "^Extension/(ReplayShake/|Skater/xray_render\\.cpp$)")
            file(STRINGS "${PROJECT_SOURCE_DIR}/${source}" uses REGEX "render_view_contract")
            if(uses)
                replay_shake_note("${source} uses the renderer's view set-up (render_view_contract): should it hook it, it and Replay Shake cannot both, the one that starts later failing (logs/ReSkate.log says which).")
            endif()
        endif()
    endforeach()
endif()
if(TARGET dingosdk_overlay)
    target_sources(dingosdk_overlay PRIVATE
        Extension/ReplayShake/replay_shake_page.cpp
        Extension/ReplayShake/replay_shake_view.cpp)
    target_link_libraries(dingosdk_overlay PUBLIC dingosdk_replay_shake_model)
endif()

option(DINGOSDK_BUILD_REPLAY_SHAKE_TESTS "Build Replay Shake regression tests" OFF)
if(DINGOSDK_BUILD_REPLAY_SHAKE_TESTS)
    enable_testing()
    # The shake: its noise, how keyframes blend, how it moves a camera, and the `replayshake` verbs.
    # No game and no Windows: it builds wherever the JSON library does.
    add_executable(dingosdk_replay_shake_tests Extension/ReplayShake/Test/replay_shake_tests.cpp)
    target_include_directories(dingosdk_replay_shake_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(dingosdk_replay_shake_tests PRIVATE dingosdk_replay_shake_model)
    add_test(NAME replay_shake COMMAND dingosdk_replay_shake_tests)
endif()

# Replay Plus (Extension/ReplayPlus/README.md): built on Replay Shake, in the same patch.
include("${PROJECT_SOURCE_DIR}/Extension/ReplayPlus/ReplayPlus.cmake" OPTIONAL)

get_property(replay_shake_notes GLOBAL PROPERTY REPLAY_SHAKE_NOTES)
list(JOIN replay_shake_notes "\n" replay_shake_notes)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/replay_shake_notes.txt" "${replay_shake_notes}")
