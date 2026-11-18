#include "replay_shake_frames.h"
#include <algorithm>
#include <cstring>

namespace dingosdk::replay_shake::frames {
namespace {
bool same_fov(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
bool has(const Shot& shot, std::uintptr_t view) noexcept {
    const auto end = shot.views.begin() + static_cast<std::ptrdiff_t>(shot.view_count);
    return std::find(shot.views.begin(), end, view) != end;
}
} // namespace

bool same_camera(const Matrix& a, const Matrix& b) noexcept {
    for (std::size_t row = 0; row < 4; ++row)
        if (std::memcmp(a.data() + row * 4, b.data() + row * 4, 3 * sizeof(float)) != 0) return false;
    return true;
}
bool moves(const Shot& shot) noexcept {
    return shot.used && (!same_camera(shot.shown, shot.base) || !same_fov(shot.fov, shot.base_fov));
}
Shaken shake(const Matrix& base, float base_fov, const Offsets& moved) noexcept {
    return {still(moved) ? base : shaken(base, moved), zoomed(base_fov, moved)};
}

const Frames::Written* Frames::written(const Matrix& camera, float fov) const noexcept {
    for (std::size_t i = 1; i <= written_.size(); ++i) { // the newest first
        const auto& entry = written_[(next_written_ + written_.size() - i) % written_.size()];
        if (entry.used && same_camera(camera, entry.shown) && same_fov(fov, entry.fov)) return &entry;
    }
    return nullptr;
}
void Frames::remember(const Matrix& shown, float fov, const Matrix& base, float base_fov) noexcept {
    if (same_camera(shown, base) && same_fov(fov, base_fov)) return; // a still shake: the view shows the game's camera
    written_[next_written_] = {shown, base, fov, base_fov, frame_, true};
    next_written_ = (next_written_ + 1) % written_.size();
}

Placed Frames::place(const Matrix& camera, float fov, std::uintptr_t view, std::uint64_t now_us) const noexcept {
    const auto age = now_.used && now_us > now_.at_us ? now_us - now_.at_us : 0;
    const bool room = now_.used && !has(now_, view) && now_.view_count < now_.views.size();
    // A camera a view was given: this frame's (the view set up again), or an earlier frame's (not set
    // up since). Then what was under it is shaken, never the shaken camera on top.
    if (const auto* given = written(camera, fov)) {
        if (now_.used && given->frame == frame_ && age < stale_us) return {Place::shown, given->base, given->base_fov};
        if (given->frame != frame_ && room && age < longest_frame_us) return {Place::again, given->base, given->base_fov};
        return {Place::next, given->base, given->base_fov};
    }
    if (now_.used && same_camera(camera, now_.base) && same_fov(fov, now_.base_fov)) {
        // A shake that moves nothing leaves the view as the game set it up, as does a view given the
        // game's camera back after its copy (returned()): the same view soon after is still this frame's.
        if (!moves(now_) && has(now_, view) && age < frame_window_us) return {Place::shown, now_.base, now_.base_fov};
        if (now_.returned && has(now_, view) && age < frame_window_us) return {Place::again, now_.base, now_.base_fov};
        if (room) {
            const bool moved_in = before_.used && (!same_camera(before_.base, now_.base) || !same_fov(before_.base_fov, now_.base_fov));
            if (age < frame_window_us || (moved_in && age < longest_frame_us)) return {Place::again, now_.base, now_.base_fov};
        }
    }
    return {Place::next, camera, fov};
}

Shaken Frames::shot_of(const Placed& placed) const noexcept {
    if (same_camera(placed.base, now_.base) && same_fov(placed.base_fov, now_.base_fov)) return {now_.shown, now_.fov};
    return shake(placed.base, placed.base_fov, now_.moved);
}

void Frames::note(std::uintptr_t view) noexcept {
    if (!now_.used) return;
    ++now_.setups;
    if (!has(now_, view) && now_.view_count < now_.views.size()) now_.views[now_.view_count++] = view;
}

void Frames::wrote(std::uintptr_t view, const Placed& placed, const Shaken& given) noexcept {
    remember(given.camera, given.fov, placed.base, placed.base_fov);
    note(view);
}

void Frames::start(const Matrix& base, float base_fov, const Offsets& moved, std::uintptr_t view, std::uint64_t now_us) noexcept {
    before_ = now_;
    ++frame_;
    const auto shot = shake(base, base_fov, moved);
    now_ = Shot{};
    now_.base = base;
    now_.base_fov = base_fov;
    now_.shown = shot.camera;
    now_.fov = shot.fov;
    now_.moved = moved;
    now_.at_us = now_us;
    now_.views[0] = view;
    now_.view_count = 1;
    now_.setups = 1;
    now_.used = true;
    remember(now_.shown, now_.fov, base, base_fov);
}

void Frames::reset() noexcept {
    now_ = Shot{};
    before_ = Shot{};
    written_ = {};
    next_written_ = 0;
}
} // namespace dingosdk::replay_shake::frames
