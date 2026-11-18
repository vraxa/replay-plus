#pragma once
#include "replay_shake_model.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What the menu shows of Replay Shake: a snapshot the client tick publishes after each change. The
// menu reads it on its own thread and changes things only through `replayshake` console commands.
// Kept apart from replay_shake.cpp so the overlay library links without the game-facing half.
namespace dingosdk::replay_shake {
// A keyframe of the replay in the editor (or, while those cannot be read, a free key).
struct Row {
    int number{};         // as `replayshake key <number>` takes it
    double time{};        // seconds
    bool own{};           // has a shake of its own; otherwise the default
    Shake shake;          // its shake (or the default's)
    Blend blend = Blend::smooth;
    bool current{};       // the playhead is between it and the next
    bool selected{};      // the replay editor's selected keyframe
};
// A camera on show, while probing: its vtable and its director's (offsets in Skate.exe, 0 unknown).
struct SeenCamera {
    std::uintptr_t vtable{}, director{};
    bool operator==(const SeenCamera&) const = default;
};
struct View {
    std::uint64_t revision{};
    bool available{};       // the renderer's view hook is there
    bool enabled{}, test{}, probe{};
    bool gameplay{};        // the setting: gameplay shakes too
    bool gameplay_held{};   // a replay camera is on show, but no game menu is: taken for gameplay, kept still
    bool editor_open{};     // a replay camera is on show (in the replay editor)
    bool shaking{};         // the shake is applied to the views being drawn
    bool exporting{};
    bool keyframe_mode{};   // rows are the editor's keyframes (else free keys)
    std::optional<double> playhead;
    std::string status;     // one line: what Replay Shake is doing
    std::string next_step;  // one line: what setting up is missing, if anything
    std::string mapping;    // the editor mapping, as `replayshake map show` says it
    std::string editor_problem; // why the mapping read nothing, if it did not
    std::vector<std::uintptr_t> cameras;   // replay cameras, as vtable offsets (of cameras or directors)
    std::vector<SeenCamera> cameras_seen;  // the cameras on show now, while probing
    std::uintptr_t every_camera{};         // the vtable every camera of the game has, gameplay's too
    std::vector<std::uintptr_t> gameplay_directors; // directors gameplay's own camera selects
    std::string menu_state;                // what the game's menu state says, while probing
    Shake fallback;
    std::uint32_t seed{};
    int export_fps{};
    std::vector<Row> rows;
};
// Never null.
std::shared_ptr<const View> view() noexcept;
void publish(std::shared_ptr<const View> next) noexcept;
} // namespace dingosdk::replay_shake
