#pragma once
#include "replay_shake_model.h"
#include "Engine/Core/Json/json.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Replay Shake: a camera shake for the replay editor, set per keyframe (README.md).
//
// The shake is put on the transform the game builds the picture's view from each frame, just before
// the game fills the view in (game_view_hook.h): the picture, its shadows, culling, fog and motion blur
// all come from the shaken view, while no camera of the game's (the editor's own camera and its
// keyframes) ever sees it. Where that is not known, it goes on the game's camera inside the game's
// update of it (camera_update_hook.h), or else on the main view the renderer draws
// (render_view_hook.h). It shakes while one of the replay editor's cameras is on show, found by its
// vtable or director (each player finds it for their game build with `replayshake probe`, README.md
// step 2). Where the
// replay editor keeps its playhead and keyframes is read through a mapping into the game's UI model
// (`replayshake map`): with the keyframes, each keyframe has a shake of its own; with only the
// playhead, shake keys can be put on the timeline; with neither, one shake runs throughout.
//
// The shake is a pure function of the replay's time (replay_shake_model.h): during an export, the
// time steps on with the export's own frames at its frame rate, so a video matches what the editor
// showed. Settings are saved in replay_shake.json beside the ReSkate profile.
#include <optional>

namespace dingosdk::replay_shake {
// Requires the validated build and the hook service. Prepares the renderer's view hook (patches
// nothing until the replay editor shows a replay camera). False when this build cannot have it.
bool start(std::uintptr_t image_base) noexcept;
// Client thread, every tick, with the client's state (13 or 21 in a level): reads the cameras on
// show and the replay editor, hands the renderer what to shake by, saves changed settings.
void tick(std::uintptr_t image_base, std::uint32_t client_state) noexcept;
// Game thread: one `replayshake` verb (replay_shake_settings.h, and status, test, probe here).
std::string run_command(std::string_view verb, const std::vector<std::string>& words);

// For Replay Plus (Extension/ReplayPlus): what its panel shows of the shake; the default shake and its
// seed as a preset carries them, and set from one (what it says it did); whether an export is being
// filmed. Any thread (they take the state's lock).
struct PanelState {
    bool available{}, on{};
    Shake fallback;
    std::uint32_t seed{1};
    // The game's menu state as Replay Shake last read it (replay_shake_editor.h GameUi).
    bool ui_found{}, in_menu{}, ui_hidden{}, advanced_edit{};
    // The replay editor's timeline (replay_shake_editor.h GameUi).
    bool timeline{}, playhead_ok{};
    double timeline_length{}, playhead{};
    float timeline_from{}, timeline_to{}, timeline_at{};
};
PanelState panel_state() noexcept;
// Replay Plus's shake keys (replay_shake_model.h Gate), by time on the replay's clock: the shake fades
// in and out at them, on the screen and in exports. Empty: the shake runs through the whole replay.
void set_gates(std::vector<Gate> gates) noexcept;
Json preset_shake();
std::string apply_preset_shake(const Json& json);
bool exporting() noexcept;
} // namespace dingosdk::replay_shake
