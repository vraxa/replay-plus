#pragma once
#include <array>
#include <cstdint>

// The game's camera update, which Replay Shake works inside to shake a camera itself. Each of skate.'s
// cameras (engine.h's camera_vtable) has an evaluator (at the camera's +0x180; the camera is at the
// evaluator's +0x138: camera_observer.cpp) whose update runs the camera's directors, sets the camera
// from the state they work out (its world transform and field of view), then hands a state on to what
// listens to the camera. The renderer's view takes the camera during that update (Replay Shake's probe
// found it never came with a shake put on after the update; most likely as the update hands its result
// on), so the shake goes on right after the camera is set, and on the states the update keeps and
// hands on, for everything the update hands on and everything drawn from the camera to come with it
// (the picture, its shadows, fog and motion blur alike). It comes off at the start of the camera's
// next update, so its directors never see it.
//
// skate. has cameras of other classes too, which set themselves each frame from a controller of their
// own (no evaluator) through the same two setters, from a state (Skate.exe 0x1707c60) or a transform
// (0x1707a10): every write of a camera's transform goes through one of them, and each is reported too
// (`written`), for Replay Shake to tell which camera a picture is drawn from, should it not be the one
// on show, and shake that one right after the game sets it (any thread). The replay editor sets no
// other camera: its picture comes from the blender's states (blend_state, blend_kept).
//
// Known for one game build (Skate.exe 25414733): each function is checked by its bytes before it is
// hooked, and on any other build the hook is unavailable (Replay Shake then shakes the views drawn).
namespace dingosdk::replay_shake::camera_update {
// A camera's state as its directors work it out, which the update sets the camera from (Skate.exe
// 0x17ac360 turns it into the camera's transform): float[4] each, the eye, the offset added to it for
// the camera's position, the point looked at and the way up; and the vertical field of view in
// degrees. state_size bytes from the eye are the eye, offset, point looked at and way up.
inline constexpr std::uintptr_t state_eye = 0x00, state_offset = 0x10, state_target = 0x20, state_up = 0x30;
inline constexpr std::uintptr_t state_fov = 0x78;
inline constexpr std::uintptr_t state_size = 0x40;
// The evaluator's blender (what blends its directors into its state: [evaluator+0x68]) works the state
// out in four stacks, by name BaseStack, BlendStack, GlobalStack and VisualStack (its +0x398 to
// +0x3b0, each changing the state in turn: 0x171fd00), then its effects (+0x3b8). It keeps two states
// of its own: as BaseStack and BlendStack left it (blend_state), and as GlobalStack left it
// (blend_kept, which the update hands on); VisualStack's and the effects' work is only in the state the
// camera is set from. The replay editor's picture matched them (Replay Shake's probe found its camera
// there and nowhere else unshaken). Of the class the game makes only (vtables as offsets in Skate.exe):
// 0x171cfe0 makes it (vtable 0x6255538, its stacks from +0x390 on) on 0x17296e0, the base that lays
// both states out (0x6255be0, abstract).
inline constexpr std::uintptr_t blend_state = 0x20, blend_kept = 0xc0;
inline constexpr std::array<std::uintptr_t, 2> blender_vtables{0x6255538, 0x6255be0};
// In an evaluator: pointers to the object holding its state (the state at holder_state in it), to its
// blender, and to what it hands on (a byte, 1 once filled in, then the state at holder_state); and its
// own copy of the state as the camera was set from it.
inline constexpr std::uintptr_t evaluator_state = 0x60, evaluator_blender = 0x68, evaluator_handed = 0x80;
inline constexpr std::uintptr_t evaluator_copy = 0x90, holder_state = 0x10;

// All but `written` on the engine thread, during a camera's update; none of them throws.
struct Callbacks {
    // At its start: the camera as the frame before left it.
    void (*before)(std::uintptr_t camera) noexcept{};
    // Right after the update set the camera from its directors' state (`state`, which the update
    // keeps and copies on), before anything else in the update reads the camera; `blender` (0 when not
    // read) holds the blend it was made from (blend_state, blend_kept).
    void (*applied)(std::uintptr_t camera, std::uintptr_t state, std::uintptr_t blender) noexcept{};
    // Once in each update, after what it hands on to what listens to the camera is filled in and before
    // they get it: its own copy of the state as set (`copy`), and the state it hands on (`handed`; 0
    // when it hands nothing on, nor set the camera: no director moves it).
    void (*handing)(std::uintptr_t camera, std::uintptr_t copy, std::uintptr_t handed) noexcept{};
    // At its end.
    void (*after)(std::uintptr_t camera) noexcept{};
    // Right after the game set any camera's transform (any class, any thread; inside an evaluated
    // camera's update too, just before `applied`), from a state or from a transform.
    void (*written)(std::uintptr_t camera, bool from_state) noexcept{};
};
// Requires the hook service. Checks each function's bytes and prepares the detours, which patches
// nothing. True once prepared, on this call and later ones.
bool start(std::uintptr_t image_base, const Callbacks& callbacks) noexcept;
bool available() noexcept;
// Client thread: the detours on or off together (switching them suspends the game's threads; asking
// for them as they are costs nothing). False when they could not all be switched (logged): those
// switched go back as they were where they can, and the next call finishes or undoes the switch.
bool want(bool on) noexcept;
} // namespace dingosdk::replay_shake::camera_update
