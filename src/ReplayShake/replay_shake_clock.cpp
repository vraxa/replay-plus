#include "replay_shake_export.h"
#include <cmath>

// The export's clock, with no game access (replay_shake_export.h): the runtime's replay_time takes its numbers from here.
namespace dingosdk::replay_shake::export_frames {
double clock_at_share(const Timeline& timeline, double share) noexcept {
    if (timeline.chunks.empty()) return timeline.start + share * timeline.length;
    // Through its chunk; before the first, its start; past the last, its end.
    const Timeline::Chunk* last = &timeline.chunks.front();
    for (const auto& c : timeline.chunks) {
        if (share < c.from) return c.clock_from;
        if (share <= c.to) {
            const double span = static_cast<double>(c.to) - c.from;
            const double along = span > 1e-9 ? (share - c.from) / span : 0.0;
            return c.clock_from + along * (c.clock_to - c.clock_from);
        }
        last = &c;
    }
    return last->clock_to;
}

int next_streak(int streak, double last, double now) noexcept {
    // One frame on (a quarter speed's 1/240 s to 1x's 1/60 s, or held still), as an export plays them; a jump back or of
    // more than half a second (another clip, the editor's own playhead after the export) starts a new run.
    if (!std::isfinite(now) || !std::isfinite(last) || now < last || now - last > 0.5) return 0;
    return streak < 1000000 ? streak + 1 : streak;
}
double time_by_frames(const Timeline& e, double clock, double along) noexcept {
    if (!e.known || e.length <= 1e-3) return clock + along;
    if (!e.chunks.empty()) {
        // The whole replay's seconds per share (its chunks' footage over the shares they fill), from the trim's start.
        double footage = 0, shares = 0;
        for (const auto& c : e.chunks) {
            footage += c.clock_to - c.clock_from;
            shares += static_cast<double>(c.to) - c.from;
        }
        if (footage > 1e-3 && shares > 1e-6) return clock_at_share(e, static_cast<double>(e.trim_from) + along * shares / footage);
    }
    // One stretch: the timeline's start, the trim's start on it, and on from there.
    const double shown = static_cast<double>(e.bar_to) - e.bar_from;
    const double trim_at = shown > 1e-6 ? (static_cast<double>(e.trim_from) - e.bar_from) / shown : 0.0;
    return e.start + trim_at * e.length + along;
}
} // namespace dingosdk::replay_shake::export_frames
