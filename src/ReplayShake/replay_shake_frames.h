#pragma once
#include "replay_shake_model.h"
#include <array>
#include <cstddef>
#include <cstdint>

// One shake per frame of the game, with no game access. The renderer sets each frame's picture up
// more than once (skate. sets the same view up four times a frame, and another renderer could set
// up several views from one camera): every set-up of a frame has to show the same shaken camera,
// and the renderer's record of the frame before (its previous view) the camera that frame showed,
// or the passes of one picture disagree and the frame-to-frame effects (motion blur, anti-aliasing,
// the lighting's temporal filters) read the difference as movement. The runtime (replay_shake.cpp)
// asks a Frames where each set-up belongs and does what it says; the tests feed it the patterns a
// renderer may have.
namespace dingosdk::replay_shake::frames {
// What one frame of the game showed.
struct Shot {
    Matrix base{}, shown{};  // its camera as the game set it up, and as shaken
    float base_fov{}, fov{}; // its vertical field of view (radians), likewise
    Offsets moved{};         // the frame's shake, for a view of it from another camera
    std::uint64_t at_us{};   // when its first set-up came
    std::array<std::uintptr_t, 8> views{}; // the views set up from it
    std::size_t view_count{};
    std::uint32_t setups{};  // its set-ups so far: the next one's number in the frame, from 0
    bool returned{};         // its views were given the game's camera back after a copy (returned())
    bool used{};
};
// Whether the first three lanes of each row are the same, bit for bit (the renderer keeps other
// values in the fourth lanes).
bool same_camera(const Matrix& a, const Matrix& b) noexcept;
// Whether a shot's shake moved its camera (or zoomed it) at all.
bool moves(const Shot& shot) noexcept;

enum class Place : std::uint8_t {
    shown, // the view shows this frame's camera already (set up again): leave it
    again, // another view of this frame: give it this frame's shake (on `base`: shot_of())
    next,  // the first set-up of the next frame: shake `base` and start() it
};
struct Placed {
    Place place{};
    Matrix base{};    // the camera under the shake
    float base_fov{};
};
// Set-ups of one frame come within frame_window_us of its first. Another view set up from the same
// camera later is the next frame's, unless the camera moved into this frame (were it moving still,
// the next frame would have come with another camera): then up to longest_frame_us. A view set up
// again as the game set it up starts the next frame at once. A view still showing this frame's
// shake stale_us after the frame began was not set up again by the game: the shake goes on.
inline constexpr std::uint64_t frame_window_us = 6000, longest_frame_us = 40000, stale_us = 100000;

// A camera (`base`, `base_fov`) shaken by `moved`: what shaken() and zoomed() make of it.
struct Shaken {
    Matrix camera{};
    float fov{};
};
Shaken shake(const Matrix& base, float base_fov, const Offsets& moved) noexcept;

class Frames {
public:
    // Where a set-up of `view` from `camera` (`fov`) at `now_us` belongs.
    Placed place(const Matrix& camera, float fov, std::uintptr_t view, std::uint64_t now_us) const noexcept;
    // This frame's camera for a view placed again: the frame's own, or its shake on another base.
    Shaken shot_of(const Placed& placed) const noexcept;
    // The view shows this frame's camera already (Place::shown). Each set-up of the frame counts
    // (Shot::setups), however placed.
    void note(std::uintptr_t view) noexcept;
    // The view was given `given`, this frame's shake on `placed.base` (Place::again).
    void wrote(std::uintptr_t view, const Placed& placed, const Shaken& given) noexcept;
    // The views of this frame are given the game's camera back once the game has copied them: set up
    // again soon after, a view of it shows the game's camera, and is still this frame's.
    void returned() noexcept { now_.returned = now_.used; }
    // The next frame: `base` shaken by `moved`, first set up as `view`. This frame becomes before().
    // What the view is to show: now().shown and now().fov.
    void start(const Matrix& base, float base_fov, const Offsets& moved, std::uintptr_t view, std::uint64_t now_us) noexcept;
    const Shot& now() const noexcept { return now_; }
    const Shot& before() const noexcept { return before_; }
    std::uint32_t frame() const noexcept { return frame_; } // frames started
    // Shaking starts again: the frames before are no one's.
    void reset() noexcept;

private:
    // A shaken camera given to a view, and what was under it: should the game not set that view up
    // again, it still shows it, and it is shaken from what was under it, never on top.
    struct Written {
        Matrix shown{}, base{};
        float fov{}, base_fov{};
        std::uint32_t frame{};
        bool used{};
    };
    const Written* written(const Matrix& camera, float fov) const noexcept;
    void remember(const Matrix& shown, float fov, const Matrix& base, float base_fov) noexcept;
    Shot now_, before_;
    std::uint32_t frame_{}; // frames started
    std::array<Written, 16> written_{};
    std::size_t next_written_{};
};
} // namespace dingosdk::replay_shake::frames
