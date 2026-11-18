#pragma once
#include <array>
#include <cstdint>

// The game's views, as it builds them each frame from its cameras (`replayshake source 3`, the
// default where this is known): the earliest place the picture's camera is the picture's, upstream of
// everything the renderer works out from it. Replay Shake shakes the transform the game builds the main
// view from, and nothing else: the camera the replay editor moves, its keyframes and gameplay's camera
// never see the shake, while the picture, its shadows (worked out from the view), its culling, fog and
// the renderer's record of the frame before (the view as built the frame before) all come from the
// shaken view, as from a camera that really shook.
//
// Each frame (Skate.exe 0x3eb88f0 on game build 25414733) the game goes through its camera views
// (each with a local player's index at +0xbc: 0 the first), makes each one's transform from an
// entity its camera view names (0x3ebf3a0), and fills a view description in from it (0x3ef19c0, hooked
// here): the description's first view (the render view, a 0x5c0-byte view whose camera is at +0x40)
// gets the transform, and the description keeps a second transform beside it (at +0x1160). Right after
// each fill, the replay's camera has its say (0x4b9550, hooked here too, which the game calls through a
// function it registers at start): while its replay camera is on (the replay editor), it sets the
// view's transform anew, from what that camera works out each frame from the replay's own camera
// entity ("ReplayedGameCameraTransform") and its own state (0x4c72d0: it trails the camera the editor
// moves, which is why the editor's picture lags that camera). The renderer then makes
// every view it draws from those descriptions (0x3f0bab0, then the world's set-up of its render, cull
// and previous views: 0x50a0880).
//
// Known for one game build (Skate.exe 25414733): each function is checked by its bytes before it is
// hooked, and on any other build the hook is unavailable.
namespace dingosdk::replay_shake::game_view {
// A transform as the game hands it to a view (0x90 bytes, 0x3ebf3a0 makes it): the camera's world
// transform before its offset is added to its position (rows right, up, back, position), the same
// with the offset added (the one the renderer draws from: a view's camera, at its +0x40), and the
// offset. A view keeps it at its +0 (the game's setter, 0x3e745a0).
struct Transform {
    std::array<float, 16> before_offset{};
    std::array<float, 16> world{};
    std::array<float, 4> offset{};
};
static_assert(sizeof(Transform) == 0x90, "the game's layout");
// In a camera view (the game's, not ReSkate's camera views): the index of the local player it is for.
inline constexpr std::uintptr_t camera_view_player = 0xbc;
// In a view description: its first view's transform (Transform, at +0: its camera, `world`, at +0x40),
// vertical field of view (radians), and whether that view has a projection of its own (then its field
// of view is left alone).
inline constexpr std::uintptr_t description_transform = 0, description_camera = 0x40, description_fov = 0x104,
                                description_own_projection = 0x134;

struct Callbacks {
    // Before the game fills a view description in (the game's thread that builds its views): the
    // description, the camera view it is for, and the two transforms it fills it in from, copies the
    // callback may change (the description gets them as changed): the one kept beside the view, and the
    // view's own. Never throws.
    void (*building)(std::uintptr_t description, std::uintptr_t camera_view, Transform& beside, Transform& view) noexcept{};
    // Once the description is filled in.
    void (*built)(std::uintptr_t description, std::uintptr_t camera_view) noexcept{};
    // Once the replay's camera has had its say on it, right after (it may have set the view's
    // transform anew: set_transform() sets it again).
    void (*overridden)(std::uintptr_t description) noexcept{};
};
// Requires the hook service. Checks the functions' bytes and prepares the detours, which patches
// nothing. True once prepared, on this call and later ones.
bool start(std::uintptr_t image_base, const Callbacks& callbacks) noexcept;
bool available() noexcept;
// Client thread: the detours on or off together (switching them suspends the game's threads; asking
// for them as they are costs nothing). False when they could not be switched (logged): those switched
// go back as they were where they can.
bool want(bool on) noexcept;
// The game's own setters of a view's transform (Skate.exe 0x3e745a0) and vertical field of view
// (radians: 0x3e743a0), which mark what they change for the renderer to work out again. Only once
// started; inside `built` or `overridden`, on the description's first view.
void set_transform(std::uintptr_t view, const Transform& transform) noexcept;
void set_fov(std::uintptr_t view, float fov) noexcept;
} // namespace dingosdk::replay_shake::game_view
