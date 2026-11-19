#pragma once
#include <imgui.h>
#include <chrono>
#include <cstdint>

// The game's own look for Replay Plus's panel, read from the installed game's data (nothing of the game's is
// shipped, as ReSkate's own pictures of the game's, game_images.h): the faces its replay editor writes in (Gilroy
// SemiBold and ExtraBold, and the marker face of its screen titles), its button glyphs (Xbox, PlayStation, keyboard
// and mouse, as its prompts draw them) and icons of its own for the panel's tabs. They go into the overlay's font
// atlas as it is built. Missing (another build of the game, a read that failed), the panel draws in ReSkate's faces
// and draws its glyphs itself.
namespace dingosdk::replay_plus::native {
enum class Face : std::uint8_t { label, value, title };
// The face, or null while the game's is not loaded.
ImFont* face(Face which) noexcept;
// How much taller its line is than its em (ImGui sizes a face by its line, the game sets text by its em): 1 when not
// loaded.
float line_per_em(Face which) noexcept;

enum class Picture : std::uint8_t {
    // Xbox, as the game names them (its "menu" is the View button, two windows; its "options" the Menu button).
    xbox_a, xbox_b, xbox_x, xbox_y, xbox_lb, xbox_rb, xbox_lt, xbox_rt, xbox_l3, xbox_r3, xbox_view, xbox_menu,
    xbox_dpad, xbox_dpad_up, xbox_dpad_down, xbox_dpad_left, xbox_dpad_right, xbox_dpad_vertical, xbox_dpad_horizontal,
    // PlayStation (DualShock 4 and DualSense differ only in their options and touch pad buttons).
    ps_cross, ps_circle, ps_square, ps_triangle, ps_l1, ps_r1, ps_l2, ps_r2, ps_l3, ps_r3, ps4_touchpad, ps4_options,
    ps5_touchpad, ps5_options, ps_dpad, ps_dpad_up, ps_dpad_down, ps_dpad_left, ps_dpad_right, ps_dpad_vertical,
    ps_dpad_horizontal,
    // Keyboard and mouse: a blank key (the game writes the key's name on it), and those it draws whole.
    key, key_enter, key_esc, key_tab, key_space, key_up, key_down, key_left, key_right, mouse_left, mouse_wheel,
    // The "+" between two buttons held together.
    plus,
    // The panel's tabs: SHAKE a camcorder, LENS a zoom, FRAME a frame, COLOUR the replay editor's own Effects,
    // FOCUS an eye, LIGHT a burst of light, GAME a gamepad, VIDEO the settings cog, PRESETS the replay editor's
    // Save, EXPERIMENTAL a warning sign.
    tab_shake, tab_lens, tab_frame, tab_colour, tab_focus, tab_light, tab_game, tab_video, tab_presets, tab_experimental,
    count
};
// Whether it is loaded (else draw() draws nothing).
bool loaded(Picture picture) noexcept;
// Its width over its height (1 when not loaded).
float aspect(Picture picture) noexcept;
// Drawn over a..b (stretched to it), tinted (white: its own colours).
bool draw(ImDrawList* draw, Picture picture, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) noexcept;
// A key's picture stretched across (a wide key: F10, Bksp), its ends kept square: a..b, b.x - a.x >= its height.
bool draw_wide_key(ImDrawList* draw, ImVec2 a, ImVec2 b) noexcept;

// The overlay's side (overlay_render.cpp): reading starts once (any thread); as the overlay builds its font atlas
// the faces are added and room made for the pictures (`width` x `height`: the screen's, the game's UI scaling with
// min(width / 1920, height / 1080)), waiting up to `wait` for a read still running; after it is built, the pictures
// go into their room.
void prepare() noexcept;
void reserve(ImFontAtlas& atlas, float width, float height, std::chrono::milliseconds wait) noexcept;
void fill(ImFontAtlas& atlas) noexcept;
// The scale the game's UI is drawn at on a screen this size: 1 on a 1920 x 1080 one.
inline float ui_scale(float width, float height) noexcept {
    const float s = width / 1920.0f < height / 1080.0f ? width / 1920.0f : height / 1080.0f;
    return s > 0.25f ? s : 0.25f;
}
} // namespace dingosdk::replay_plus::native
