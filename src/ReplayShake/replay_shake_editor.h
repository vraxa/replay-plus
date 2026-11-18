#pragma once
#include "replay_shake_settings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// What Replay Shake reads of the game, on the client thread: which cameras the camera views have
// on show (one of them is the replay editor's while it is open), and the replay editor's playhead
// and keyframes, from the game's UI model where a mapping says they are. Reads only.
namespace dingosdk::replay_shake::scan {
struct Seen {
    std::size_t index{};                 // in the camera view manager's list
    std::uintptr_t view{};               // the view itself
    std::array<std::uint32_t, 2> ids{};  // both 0: the local player's own view
    bool enabled{};
    std::uintptr_t camera{};             // the camera on show (0: none)
    std::uintptr_t vtable{};             // its vtable, as an offset in Skate.exe (0: none, or not in it)
    // Its director: the vtable (offset in Skate.exe) of the camera entry its evaluator has selected,
    // what moves the camera (skate.'s cameras all share one vtable, so it is this that tells the
    // replay editor's from gameplay's). 0: not read. `directors`: how many entries it has.
    std::uintptr_t director{};
    std::size_t directors{};
    // A view of another class than the camera views (listed for the probe, nothing more read of it:
    // not enabled, no camera), and its vtable (offset in Skate.exe, 0 when outside it).
    bool other_view{};
    std::uintptr_t view_vtable{};
};
// Every camera view's camera on show, and the views of other classes in the same list; empty when the
// views cannot be read (no level).
std::vector<Seen> cameras_on_show(std::uintptr_t image_base) noexcept;

// The game's menu state (UI/Foundations/State/DelMar_UI_ScreenData, which ReSkate's nametags
// follow too): a game menu is up (the pause menu, the replay editor...), or the game hides its UI.
struct GameUi {
    bool found{}; // the state was read; otherwise nothing is known
    bool in_menu{}, ui_hidden{};
    // The replay editor's Advanced Edit is up (its button prompts say so: UiReader::advanced_edit).
    bool advanced_edit{};
    // The replay editor's timeline (its ReplayTimeline_Widget's model): the replay's length in seconds,
    // and where the trim's start and end and the playhead sit along it (0 to 1). Not read: `timeline`
    // false. `playhead`: the replay's clock (replay_activity) as it was when the timeline was read, so
    // the timeline's start on that clock is playhead - at * length.
    bool timeline{}, playhead_ok{};
    double timeline_length{}, playhead{};
    float timeline_from{}, timeline_to{}, timeline_at{};
    bool operator==(const GameUi&) const = default;
};
// Reads the menu state at most every 100 ms (keeping the last), and looks for it again at most
// every two seconds while it is missing. Client thread (it takes the UI model's lock).
class UiReader {
public:
    GameUi read(std::uintptr_t image_base) noexcept;

private:
    std::uint64_t handle_{};
    std::uintptr_t type_{}, manager_{};
    std::uint64_t next_read_ms_{}, next_search_ms_{};
    // The replay editor's button prompts (advanced_edit), kept as found.
    std::uint64_t controls_handle_{};
    std::uintptr_t controls_type_{};
    std::uint64_t next_controls_search_ms_{};
    // The replay editor's timeline model (read_timeline), kept as found.
    std::uint64_t timeline_handle_{};
    std::uintptr_t timeline_type_{};
    std::uint64_t next_timeline_search_ms_{};
    GameUi last_;
};

// Reads the replay editor's playhead, keyframes and selection from the UI model. Keeps the root
// view model it found, and looks for it again at most every two seconds while it is missing.
class Reader {
public:
    // Fills what `mapping` maps into `editor`. Empty when everything mapped was read; otherwise what
    // went wrong (what could be read still is).
    std::string read(std::uintptr_t image_base, const Mapping& mapping, Editor& editor) noexcept;
    // Drop the root (the mapping changed).
    void forget() noexcept;

private:
    std::uint64_t root_handle_{};
    std::uintptr_t root_type_{};
    std::uint32_t root_schema_{};
    std::uint64_t next_search_ms_{};
};
// The most keyframes read from one replay.
inline constexpr unsigned most_keyframes = 256;
} // namespace dingosdk::replay_shake::scan
