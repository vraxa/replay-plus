#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Replay Plus: Replay Shake and more for skate.'s replay editor (README.md). A fisheye lens (an adapter's
// or a modern one), its hood and 4:3 bars,
// a colour grade, the game's own effects held, a light on the camera, a fixed field of view, render
// resolutions of its own for the replay editor and for skating, presets to share, and a panel in
// the replay editor itself, worked with the controller (RB + R3 held, or F10).
//
// The runtime half (this file, the game thread) owns the settings and keeps the game's settings as
// they ask; the overlay half draws the picture's pass and the panel (replay_plus_overlay.h); the
// lens finishes exported frames too, on the CPU (the game films its frame before the overlay draws).
// Replay Shake (Extension/ReplayShake) calls in here from its own start, tick, view hooks and export.
namespace dingosdk::replay_plus {
// Once, after Replay Shake has started.
bool start(std::uintptr_t image_base) noexcept;
// Game thread, every tick, after Replay Shake's own: whether the client is in a level (13 or 21) and
// whether Replay Shake sees the replay editor open.
void tick(std::uintptr_t image_base, std::uint32_t client_state, bool editor_open) noexcept;
// Game thread: one `replayplus` verb and the words after it.
std::string run_command(std::string_view verb, const std::vector<std::string>& words);
// Replay Shake leaves its switch to Replay Plus (tapped it switches the shake, held it opens the panel).

// From Replay Shake's hooks on the game's building of its views (the game's thread that builds
// them). Whether Replay Plus wants that hook on (in the replay editor: a fixed field of view, or the
// lens and the light reading it).
bool wants_views(bool editor_open) noexcept;
// The picture's view, once the game has filled it in (`replayed` false) and once the replay camera
// has had its say (true): its field of view is fixed here when asked, and noted for the lens.
void view_built(std::uintptr_t description, bool replayed) noexcept;

// From Replay Shake's stand-in for the encoder's frame submit (an export, the render thread): the
// frame the game is about to encode, finished as the screen shows it. Never throws.
void export_frame(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch) noexcept;
} // namespace dingosdk::replay_plus
