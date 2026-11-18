#pragma once
#include <cstdint>

// The renderer's set-up of each view it draws (the game build's skater_render.h:
// render_view_contract), one detour shared by the features that use it: the hook service takes one
// detour per function. Before the game copies a view for its shaders a feature may change it
// (Replay Shake moves its camera). The skater x-ray's own hook (Extension/Skater/xray_render.cpp),
// which reads the camera after the copy, runs inside this detour in place of the original while the
// x-ray wants it: its file is left as ReSkate has it, and its hook calls for this function come here
// (hook_interpose.h).
namespace dingosdk::render_view {
enum class Client : unsigned { xray, replay_shake, count };
// `view` is the 320-byte view being set up: its camera at +0x40, its vertical field of view at
// +0x104, its kind 0x38 bytes before it (skater_render.h). `previous` is the view the renderer
// compares it with to tell how the picture moved since the frame before (motion blur, temporal
// anti-aliasing, the lighting's temporal filters), laid out the same (0: none). `blackboard` is
// the frame's, which the function is handed first, and `jitter` its last argument (whether the
// view is jittered for the anti-aliasing, it seems). Render thread. Never throws.
using Callback = void (*)(std::uintptr_t view, std::uintptr_t previous, std::uintptr_t blackboard, std::uint8_t jitter) noexcept;
// Requires the validated build and the hook service. Checks the function's bytes and prepares the
// detour, which patches nothing. True once prepared, on this call and later ones.
bool start(std::uintptr_t image_base) noexcept;
// Set before the client first wants the hook. A client's callbacks only run while it wants it: the
// one before the game copies the view, and the one after (to put back what was changed for it).
void set_before(Client client, Callback callback) noexcept;
void set_after(Client client, Callback callback) noexcept;
// Client thread: the detour is on while any client wants it. False when it could not be switched
// (logged; it stays as it was).
bool want(Client client, bool on) noexcept;
bool wanted(Client client) noexcept;
} // namespace dingosdk::render_view
