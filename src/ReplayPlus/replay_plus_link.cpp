#include "replay_plus_link.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

namespace dingosdk::replay_plus::link {
std::uint64_t now_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
namespace {
struct Shared {
    std::mutex mutex;
    std::shared_ptr<const Picture> picture = std::make_shared<const Picture>();
    std::shared_ptr<const View> view = std::make_shared<const View>();
    std::shared_ptr<const HoodImage> hood;
    std::deque<std::string> commands;
    std::string shot;
    bool shot_asked{};
    RenderReport report;
    bool panel_request{}, panel_request_open{}, panel_request_keyboard{};
    bool white_asked{}, white_curve{};
    float white_used{1.0f}, white_measured{};
    std::string white_detail, white_dump;
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
std::atomic<float> fov_now{0.0f};
std::atomic<float> focus_metres{0.0f};
std::atomic<std::uint64_t> focus_when{0};
std::atomic<bool> keyboard_flag{false};
std::atomic<std::uint64_t> fov_when{0};
std::atomic<bool> open_flag{false};
std::atomic<std::uint64_t> drawn_when{0};
} // namespace

void publish_picture(std::shared_ptr<const Picture> next) noexcept {
    if (!next) return;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.picture = std::move(next);
}
std::shared_ptr<const Picture> picture() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.picture;
}

void publish_hood(std::shared_ptr<const HoodImage> image) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.hood = std::move(image);
}
std::shared_ptr<const HoodImage> hood() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.hood;
}

void note_fov(float fov) noexcept {
    fov_now.store(fov, std::memory_order_relaxed);
    fov_when.store(now_ms(), std::memory_order_release);
}
float fov() noexcept { return fov_now.load(std::memory_order_relaxed); }
void note_focus_depth(float metres) noexcept {
    focus_metres.store(metres, std::memory_order_relaxed);
    focus_when.store(now_ms(), std::memory_order_release);
}
float focus_depth() noexcept { return focus_metres.load(std::memory_order_relaxed); }
std::uint64_t focus_depth_at() noexcept { return focus_when.load(std::memory_order_acquire); }

std::atomic<bool>& panel_keyboard() noexcept { return keyboard_flag; }
std::atomic<int>& trace_frames() noexcept {
    static std::atomic<int> value{0};
    return value;
}
std::atomic<bool>& export_depth_latest() noexcept {
    static std::atomic<bool> value{false};
    return value;
}
namespace {
std::atomic<std::uint64_t> pad_hidden_until{0};
}
void hide_pad_buttons(std::uint64_t ms) noexcept {
    const auto until = now_ms() + ms;
    auto was = pad_hidden_until.load(std::memory_order_relaxed);
    while (until > was && !pad_hidden_until.compare_exchange_weak(was, until, std::memory_order_release, std::memory_order_relaxed)) {
    }
}
std::uint16_t hidden_pad_buttons() noexcept {
    // Every button of the pad (XInput's), none of its triggers or sticks.
    return now_ms() < pad_hidden_until.load(std::memory_order_acquire) ? std::uint16_t{0xf3ff} : std::uint16_t{0};
}
std::uint64_t fov_at() noexcept { return fov_when.load(std::memory_order_acquire); }

namespace {
std::atomic<std::uintptr_t> game_window_handle{0};
}
void note_game_window(std::uintptr_t window) noexcept { game_window_handle.store(window, std::memory_order_release); }
std::uintptr_t game_window() noexcept { return game_window_handle.load(std::memory_order_acquire); }

namespace {
std::atomic<std::uint64_t> frame_screen{0}, frame_render{0}, frame_told_at{0}, adapter_luid{0};
}
void note_frame_sizes(std::uint32_t screen_width, std::uint32_t screen_height, std::uint32_t render_width, std::uint32_t render_height) noexcept {
    frame_screen.store((std::uint64_t{screen_width} << 32) | screen_height, std::memory_order_relaxed);
    frame_render.store((std::uint64_t{render_width} << 32) | render_height, std::memory_order_relaxed);
    frame_told_at.store(now_ms(), std::memory_order_release);
}
FrameSizes frame_sizes() noexcept {
    FrameSizes f;
    f.at = frame_told_at.load(std::memory_order_acquire);
    const auto screen = frame_screen.load(std::memory_order_relaxed), render = frame_render.load(std::memory_order_relaxed);
    f.screen_width = static_cast<std::uint32_t>(screen >> 32);
    f.screen_height = static_cast<std::uint32_t>(screen & 0xffffffffu);
    f.render_width = static_cast<std::uint32_t>(render >> 32);
    f.render_height = static_cast<std::uint32_t>(render & 0xffffffffu);
    return f;
}
void note_adapter(std::uint64_t luid) noexcept { adapter_luid.store(luid, std::memory_order_release); }
std::uint64_t adapter() noexcept { return adapter_luid.load(std::memory_order_acquire); }

namespace {
std::atomic<std::uint32_t> under_ui_run{0};
}
void note_drawn_under_ui(bool drawn) noexcept {
    if (drawn) {
        const auto run = under_ui_run.load(std::memory_order_relaxed);
        under_ui_run.store(run < 0xffffffu ? run + 1 : run, std::memory_order_release);
    } else {
        under_ui_run.store(0, std::memory_order_release);
    }
}
std::uint32_t drawn_under_ui() noexcept { return under_ui_run.load(std::memory_order_acquire); }

namespace {
struct PadLayer {
    std::mutex mutex;
    std::uint64_t until{};          // on until then
    std::uint16_t kept{};           // buttons pressed with R3: kept from the game until let go
    bool kept_left{}, kept_right{}; // the triggers, likewise
    std::uint16_t inject{};         // a press given to the game until inject_until
    std::uint64_t inject_until{};
};
PadLayer& pad_layer() {
    static auto* value = new PadLayer;
    return *value;
}
} // namespace
std::atomic<std::uint16_t>& pad_layer_button() noexcept {
    static std::atomic<std::uint16_t> value{0x0080};
    return value;
}
std::atomic<bool>& panel_capturing() noexcept {
    static std::atomic<bool> value{false};
    return value;
}
void renew_pad_layer() noexcept {
    auto& p = pad_layer();
    std::lock_guard lock(p.mutex);
    p.until = now_ms() + 300;
}
void inject_pad(std::uint16_t buttons, std::uint64_t ms) noexcept {
    auto& p = pad_layer();
    std::lock_guard lock(p.mutex);
    p.inject = buttons;
    p.inject_until = now_ms() + ms;
}
bool pad_filter(std::uint16_t& buttons, std::uint8_t& left_trigger, std::uint8_t& right_trigger, std::int16_t& right_x,
                std::int16_t& right_y) noexcept {
    // (The shortcut button: R3 unless the player picked another.)
    const std::uint16_t r3 = pad_layer_button().load(std::memory_order_relaxed);
    constexpr std::uint16_t every_button = 0xf3ff;
    constexpr std::uint8_t pulled = 30;
    auto& p = pad_layer();
    std::lock_guard lock(p.mutex);
    const auto now = now_ms();
    const std::uint16_t was_buttons = buttons;
    const std::uint8_t was_left = left_trigger, was_right = right_trigger;
    const std::int16_t was_x = right_x, was_y = right_y;
    if (now < p.until) {
        if (buttons & r3) {
            p.kept |= static_cast<std::uint16_t>(buttons & ~r3 & every_button);
            p.kept_left = p.kept_left || left_trigger > pulled;
            p.kept_right = p.kept_right || right_trigger > pulled;
            buttons = 0;
            left_trigger = right_trigger = 0;
            right_x = right_y = 0;
        } else {
            p.kept &= buttons;
            if (left_trigger <= pulled) p.kept_left = false;
            if (right_trigger <= pulled) p.kept_right = false;
            buttons &= static_cast<std::uint16_t>(~p.kept);
            if (p.kept_left) left_trigger = 0;
            if (p.kept_right) right_trigger = 0;
        }
    } else {
        p.kept = 0;
        p.kept_left = p.kept_right = false;
    }
    if (now < p.inject_until) buttons |= p.inject;
    return buttons != was_buttons || left_trigger != was_left || right_trigger != was_right || right_x != was_x || right_y != was_y;
}

void publish_view(std::shared_ptr<const View> next) noexcept {
    if (!next) return;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.view = std::move(next);
}
std::shared_ptr<const View> view() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.view;
}

void post(std::string command) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (s.commands.size() < 256) s.commands.push_back(std::move(command));
}
std::vector<std::string> take() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    std::vector<std::string> out(s.commands.begin(), s.commands.end());
    s.commands.clear();
    return out;
}

std::atomic<bool>& panel_open() noexcept { return open_flag; }
void ask_panel(bool open, bool from_keyboard) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.panel_request = true;
    s.panel_request_open = open;
    s.panel_request_keyboard = from_keyboard;
}
bool take_panel_request(bool& open, bool& from_keyboard) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.panel_request) return false;
    s.panel_request = false;
    open = s.panel_request_open;
    from_keyboard = s.panel_request_keyboard;
    return true;
}
float timeline_share(const std::vector<Chunk>& chunks, double timeline_start, double timeline_length, double clock, float bar_from,
                     float bar_to) noexcept {
    if (chunks.empty()) return timeline_length > 0 ? static_cast<float>((clock - timeline_start) / timeline_length) : -1.0f;
    // (Within a frame of a chunk's ends counts as in it.)
    constexpr double slack = 1.0 / 60.0;
    for (const auto& c : chunks)
        if (clock >= c.clock_from - slack && clock <= c.clock_to + slack) {
            const double span = c.clock_to - c.clock_from;
            const double along = span > 1e-9 ? std::clamp((clock - c.clock_from) / span, 0.0, 1.0) : 0.0;
            const float whole = c.from + static_cast<float>(along) * (c.to - c.from);
            // On the part the timeline shows (off it, as nowhere: trimmed away).
            const float shown = bar_to - bar_from;
            if (shown <= 1e-6f) return whole;
            const float on_bar = (whole - bar_from) / shown;
            constexpr float edge = 1e-3f;
            return on_bar < -edge || on_bar > 1.0f + edge ? -1.0f : std::clamp(on_bar, 0.0f, 1.0f);
        }
    return -1.0f;
}
std::atomic<bool>& panel_pointer() noexcept {
    static std::atomic<bool> value{false};
    return value;
}
bool panel_has_pointer() noexcept {
    return panel_pointer().load(std::memory_order_acquire) && now_ms() - drawn_when.load(std::memory_order_acquire) < 1000;
}
void note_panel_drawn() noexcept { drawn_when.store(now_ms(), std::memory_order_release); }
std::uint64_t panel_drawn_at() noexcept { return drawn_when.load(std::memory_order_acquire); }

void ask_shot(std::string path) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.shot = std::move(path);
        s.shot_asked = true;
    } catch (...) {}
}
bool take_shot(std::string& path) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        if (!s.shot_asked) return false;
        s.shot_asked = false;
        path = std::move(s.shot);
        return true;
    } catch (...) { return false; }
}
bool shot_asked() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return s.shot_asked;
    } catch (...) { return false; }
}
bool panel_requested() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.panel_request;
}
void report(const RenderReport& r) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.report = r;
    } catch (...) {}
}
RenderReport render_report() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return s.report;
    } catch (...) { return {}; }
}

void ask_white() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.white_asked = true;
    } catch (...) {}
}
bool take_white() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        const bool asked = s.white_asked;
        s.white_asked = false;
        return asked;
    } catch (...) { return false; }
}
void note_white(float used, float measured, bool curve) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.white_used = used;
        s.white_measured = measured;
        s.white_curve = curve;
    } catch (...) {}
}
bool scene_white_curve() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return s.white_curve;
    } catch (...) { return false; }
}
void note_white_detail(std::string detail) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.white_detail = std::move(detail);
    } catch (...) {}
}
float scene_white() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return s.white_used;
    } catch (...) { return 1.0f; }
}
float scene_white_measured() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return s.white_measured;
    } catch (...) { return 0.0f; }
}
std::string white_detail() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return s.white_detail;
    } catch (...) { return {}; }
}
std::atomic<float>& white_fixed() noexcept {
    static std::atomic<float> value{0.0f};
    return value;
}
void ask_white_dump(std::string path) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.white_dump = std::move(path);
        s.white_asked = true;
    } catch (...) {}
}
std::atomic<float>& light_facing_metres() noexcept {
    static std::atomic<float> value{light_facing_metres_default};
    return value;
}
std::atomic<bool>& game_light() noexcept {
    static std::atomic<bool> value{false};
    return value;
}
std::atomic<bool>& light_on_now() noexcept {
    static std::atomic<bool> value{true};
    return value;
}
std::atomic<float>& light_facing_weight() noexcept {
    static std::atomic<float> value{light_facing_weight_default};
    return value;
}
std::string take_white_dump() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        return std::exchange(s.white_dump, std::string());
    } catch (...) { return {}; }
}
} // namespace dingosdk::replay_plus::link
