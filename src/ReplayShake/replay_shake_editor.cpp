#include "replay_shake_editor.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_activity.h"
#include "replay_shake_build.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <cmath>
#include <format>
#include <span>
#include <stdexcept>

namespace dingosdk::replay_shake::scan {
namespace {
namespace spawn = addr::client_source_spawn;
namespace md = multiplayer::menu_data;
// A UI model value that holds another model's handle (`ui dump` prints it as "(model) -> handle").
constexpr unsigned kind_handle = 18;
constexpr std::uint64_t search_every_ms = 2000;

// The value at `path` below `value`, following references to other models on the way.
md::Value resolve(const md::Context& context, md::Value value, std::span<const std::uint32_t> path) {
    const auto follow = [&](md::Value at) {
        if (md::kind(at.type) != kind_handle) return at;
        const auto handle = md::read<md::Handle>(context.address(at));
        const auto type = handle ? context.type_of(handle) : 0;
        md::require(handle && type, "A mapped field refers to no model.");
        return md::Value{handle, type};
    };
    value = follow(value);
    for (const auto hash : path) value = follow(context.field(value, hash));
    return value;
}
// A mapped number, in seconds (or whatever its scale makes it).
double number(const md::Context& context, md::Value value, const Reading& reading) {
    const auto bytes = md::size(value.type);
    if (bytes != size(reading.format))
        throw std::runtime_error(std::format("the value is {} bytes, not {} ({}): use f32, i32 or u32 for 4, f64 or i64 for 8",
            bytes, size(reading.format), name(reading.format)));
    std::array<std::byte, 8> raw{};
    md::require(memory::peek_bytes(context.address(value), raw.data(), bytes), "The mapped value is unreadable.");
    const double result = decode(reading.format, raw.data()) * reading.scale;
    md::require(std::isfinite(result), "The mapped value is not a number.");
    return result;
}
void note(std::string& problems, std::string_view what, const std::exception& error) {
    problems += std::format("{}{}: {}", problems.empty() ? "" : "; ", what, error.what());
}
// The camera's director, as Extension/Skater/camera_observer.cpp finds the local camera's: its
// evaluator (pointing back at it), the evaluator's entries (a list pointing back at the
// evaluator), the last of them selected. Left 0 should any of it not hold.
void director(Seen& seen, std::uintptr_t base, std::uintptr_t image_end) noexcept {
    std::uintptr_t evaluator{}, back{}, list{}, begin{}, end{}, entry{}, vtable{};
    if (!seen.camera || !memory::peek(seen.camera + layout::camera_evaluator, evaluator) || !evaluator || (evaluator & 7) ||
        !memory::peek(evaluator + layout::evaluator_camera, back) || back != seen.camera ||
        !memory::peek(evaluator + layout::evaluator_entries, list) || !list || (list & 7) || !memory::peek(list, back) ||
        back != evaluator || !memory::peek(list + layout::entries_begin, begin) || !memory::peek(list + layout::entries_end, end) ||
        !begin || end <= begin || (end - begin) % layout::entry_stride || (end - begin) / layout::entry_stride > layout::most_entries ||
        !memory::peek(end - layout::entry_stride, entry) || !entry || (entry & 7) || !memory::peek(entry, vtable) ||
        vtable < base || vtable >= image_end)
        return;
    seen.director = vtable - base;
    seen.directors = (end - begin) / layout::entry_stride;
}
} // namespace

std::vector<Seen> cameras_on_show(std::uintptr_t base) noexcept {
    std::vector<Seen> result;
    try {
        std::uintptr_t manager{}, vtable{}, begin{}, end{};
        if (!base || !memory::peek(base + spawn::view_manager, manager) || !manager || !memory::peek(manager, vtable) ||
            vtable != base + spawn::view_manager_vtable || !memory::peek(manager + layout::views_begin, begin) ||
            !memory::peek(manager + layout::views_end, end) || end < begin || (end - begin) % 8 ||
            (end - begin) / 8 > layout::most_views)
            return result;
        const auto image_end = base + supported_build::game_image_size;
        for (std::size_t i = 0; i < (end - begin) / 8; ++i) {
            std::uintptr_t view{}, view_vtable{};
            if (!memory::peek(begin + i * 8, view) || !view || !memory::peek(view, view_vtable)) continue;
            if (view_vtable != base + spawn::camera_view_vtable) {
                Seen other;
                other.index = i;
                other.view = view;
                other.other_view = true;
                if (view_vtable >= base && view_vtable < image_end) other.view_vtable = view_vtable - base;
                result.push_back(other);
                continue;
            }
            Seen seen;
            seen.index = i;
            seen.view = view;
            std::uint8_t enabled{};
            if (!memory::peek(view + layout::view_ids[0], seen.ids[0]) || !memory::peek(view + layout::view_ids[1], seen.ids[1]) ||
                !memory::peek(view + layout::view_enabled, enabled) || !memory::peek(view + layout::view_camera, seen.camera))
                continue;
            seen.enabled = enabled != 0;
            std::uintptr_t camera_vtable{};
            if (seen.camera && memory::peek(seen.camera, camera_vtable) && camera_vtable >= base && camera_vtable < image_end)
                seen.vtable = camera_vtable - base;
            director(seen, base, image_end);
            result.push_back(seen);
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

std::string Reader::read(std::uintptr_t base, const Mapping& mapping, Editor& editor) noexcept {
    if (!mapping.root) return "No replay editor mapping.";
    try {
        const auto ui = md::read<std::uintptr_t>(base + addr::engine::ui_manager);
        const auto manager = ui ? md::read<std::uintptr_t>(ui + layout::ui_models) : 0;
        if (!manager || !game::native_data().models.value) return "The game's UI model is not ready.";
        const md::Context context(base, manager);
        // The game's own writers take this lock; reads under it never see a model half changed.
        game::ModelWriteLock lock(manager);
        if (root_schema_ != mapping.root) {
            forget();
            root_schema_ = mapping.root; // so the search below waits its turn while the root is missing
        }
        md::Value root{root_handle_, root_type_};
        if (!root.handle || context.type_of(root.handle) != root.type) {
            root_handle_ = 0;
            const auto now = GetTickCount64();
            if (now < next_search_ms_) return std::format("No root view model {} is loaded.", hex(mapping.root));
            // Scanning the registry costs milliseconds: not every tick.
            next_search_ms_ = now + search_every_ms;
            const auto roots = context.roots({mapping.root});
            if (roots.empty()) return std::format("No root view model {} is loaded.", hex(mapping.root));
            root = roots.front().model;
            root_handle_ = root.handle;
            root_type_ = root.type;
        }
        std::string problems;
        if (!mapping.playhead.path.empty()) {
            try {
                editor.playhead = number(context, resolve(context, root, mapping.playhead.path), mapping.playhead);
            } catch (const std::exception& error) { note(problems, "playhead", error); }
        }
        if (!mapping.keyframes.empty()) {
            try {
                const auto array = resolve(context, root, mapping.keyframes);
                unsigned count{}, stride{};
                const auto bytes = context.array(array, most_keyframes, count, stride);
                const auto& time = mapping.keyframe_time;
                std::vector<double> times;
                times.reserve(count);
                for (unsigned i = 0; i < count; ++i) {
                    if (time.path.empty()) {
                        // An array of plain numbers: each element is its time.
                        md::require(stride == size(time.format), "The keyframes are not numbers of the mapped format.");
                        const double value = decode(time.format, bytes.data() + static_cast<std::size_t>(i) * stride) * time.scale;
                        md::require(std::isfinite(value), "A keyframe time is not a number.");
                        times.push_back(value);
                    } else {
                        times.push_back(number(context, resolve(context, context.element(array, i), time.path), time));
                    }
                }
                editor.keyframes = std::move(times);
            } catch (const std::exception& error) { note(problems, "keyframes", error); }
        }
        if (!mapping.selected.path.empty()) {
            try {
                const double index = number(context, resolve(context, root, mapping.selected.path), mapping.selected);
                if (index >= -1 && index < most_keyframes) editor.selected = static_cast<int>(std::lround(index));
            } catch (const std::exception& error) { note(problems, "selected", error); }
        }
        return problems;
    } catch (const std::exception& error) {
        root_handle_ = 0; // looked for again, but not before the next search is due
        return error.what();
    } catch (...) {
        root_handle_ = 0;
        return "The UI model could not be read.";
    }
}

// Advanced Edit, from the replay editor's button prompts (its controls helper: the model of schema
// 0xe6ab1489 holds a list, 0x5fc6738b, of buttons, each a model, 0xecfbf5f3, whose input binding,
// 0xa2b71c93, names its action, 0xb1e14cd6). In Advanced Edit they hold "Back" and
// "EditReplaySettings"; on the replay's first screen, "ConfirmExit" and the way into Advanced Edit,
// "Replay_AdvancedEdit" (`ui dump`, 2026-11-16).
static bool advanced_edit(const md::Context& context, std::uint64_t now, std::uint64_t& controls_handle_, std::uintptr_t& controls_type_,
                   std::uint64_t& next_controls_search_ms_) noexcept {
    constexpr std::uint32_t controls = 0xe6ab1489, buttons = 0x5fc6738b, button = 0xecfbf5f3, binding = 0xa2b71c93, action = 0xb1e14cd6;
    try {
        if (!controls_handle_ || context.type_of(controls_handle_) != controls_type_) {
            controls_handle_ = 0;
            if (now < next_controls_search_ms_) return false;
            next_controls_search_ms_ = now + search_every_ms;
            const auto roots = context.roots({controls});
            static bool said_none = false;
            if (roots.empty() && !said_none) {
                said_none = true;
                logging::write(logging::Level::info, logging::Channel::graphics, "Replay Shake: the replay editor's buttons are not in the UI model.");
            }
            if (roots.empty()) return false;
            controls_handle_ = roots.front().model.handle;
            controls_type_ = roots.front().model.type;
        }
        const auto list = context.field(md::Value{controls_handle_, controls_type_}, buttons);
        unsigned count{}, stride{};
        (void)context.array(list, 48, count, stride);
        bool back = false, settings = false, way_in = false;
        for (unsigned i = 0; i < std::min(count, 48u); ++i) {
            try {
                // The button's reference to its model, and the action's name in it, read where the schema
                // puts them (Context::field takes sub-models only, and refuses a reference to another one).
                const auto element = context.element(list, i);
                const auto ref = context.member(element.type, button);
                const auto handle = md::read<md::Handle>(context.address(element) + ref.offset);
                const auto type = handle ? context.type_of(handle) : 0;
                if (!type) continue;
                const auto inner = context.member(type, binding);
                const auto named = context.member(inner.type, action);
                const auto name = md::string(md::read<md::Address>(context.address(md::Value{handle, type}) + inner.offset + named.offset), 48);
                back = back || name == "Back";
                settings = settings || name == "EditReplaySettings";
                way_in = way_in || name == "Replay_AdvancedEdit";
            } catch (...) {
                // An entry that cannot be read is passed over.
            }
        }
        return back && settings && !way_in;
    } catch (const std::exception& error) {
        static std::string said;
        if (said != error.what()) {
            said = error.what();
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::string("Replay Shake: the replay editor's buttons could not be read: ") + error.what());
        }
        controls_handle_ = 0;
        return false;
    } catch (...) {
        controls_handle_ = 0;
        return false;
    }
}

// The replay editor's timeline: a root of schema 0xd71ee0a1 lists widgets (0x61742cb4), each holding
// (0x716496c8, then 0x25e4d6c8) a reference (0xecfbf5f3) to its model. The timeline's
// (UI/Features/Social/Replay/Widgets/ReplayTimeline_Widget) has the replay's length in seconds
// (0x3e6bd625, a double) and three shares of it (floats): 0xadb99775 the trim's start, 0xa2df9b3b its
// end, 0x50b950bb the playhead (`ui dump`, 2026-11-16: 6.833 s, 0, 1, 1 at the end of a whole replay).
static void read_timeline(const md::Context& context, std::uintptr_t base, std::uint64_t now, std::uint64_t& handle, std::uintptr_t& type,
                          std::uint64_t& next_search, GameUi& out) noexcept {
    constexpr std::uint32_t root_schema = 0xd71ee0a1, widgets = 0x61742cb4, slot = 0x716496c8, binding = 0x25e4d6c8, reference = 0xecfbf5f3;
    constexpr std::uint32_t length = 0x3e6bd625, trim_from = 0xadb99775, trim_to = 0xa2df9b3b, at = 0x50b950bb;
    try {
        if (!handle || context.type_of(handle) != type) {
            handle = 0;
            if (now < next_search) return;
            next_search = now + search_every_ms;
            for (const auto& root : context.roots({root_schema})) {
                const auto list = context.field(root.model, widgets);
                unsigned count{}, stride{};
                (void)context.array(list, 64, count, stride);
                for (unsigned i = 0; i < std::min(count, 64u) && !handle; ++i) {
                    try {
                        const auto inner = context.field(context.field(context.element(list, i), slot), binding);
                        const auto ref = context.member(inner.type, reference);
                        const auto found = md::read<md::Handle>(context.address(inner) + ref.offset);
                        const auto found_type = found ? context.type_of(found) : 0;
                        if (!found_type) continue;
                        // The timeline's model is the one with a length (member throws for others).
                        if (md::size(context.member(found_type, length).type) != sizeof(double)) continue;
                        handle = found;
                        type = found_type;
                    } catch (...) {
                        // Another widget: passed over.
                    }
                }
                if (handle) break;
            }
            if (!handle) return;
        }
        const auto at_model = context.address(md::Value{handle, type});
        const auto field = [&](std::uint32_t hash, std::size_t bytes) {
            const auto f = context.member(type, hash);
            md::require(md::size(f.type) == bytes, "The replay timeline's model differs.");
            return at_model + f.offset;
        };
        const double seconds = md::read<double>(field(length, sizeof(double)));
        const float from = md::read<float>(field(trim_from, sizeof(float))), to = md::read<float>(field(trim_to, sizeof(float))),
                    where = md::read<float>(field(at, sizeof(float)));
        if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600 || !std::isfinite(from) || !std::isfinite(to) || !std::isfinite(where))
            return;
        out.timeline = true;
        out.timeline_length = seconds;
        out.timeline_from = from;
        out.timeline_to = to;
        out.timeline_at = where;
        std::uint8_t lane{};
        double playhead{};
        out.playhead_ok = memory::peek(base + addr::replay_activity::playing, lane) && lane &&
                          memory::peek(base + addr::replay_activity::playhead, playhead) && std::isfinite(playhead);
        out.playhead = out.playhead_ok ? playhead : 0.0;
    } catch (...) {
        handle = 0;
    }
}

GameUi UiReader::read(std::uintptr_t base) noexcept {
    const auto now = GetTickCount64();
    if (now < next_read_ms_) return last_;
    next_read_ms_ = now + 100;
    last_ = {};
    try {
        const auto ui = base ? md::read<std::uintptr_t>(base + addr::engine::ui_manager) : 0;
        const auto manager = ui ? md::read<std::uintptr_t>(ui + layout::ui_models) : 0;
        if (!manager || !game::native_data().models.value) return last_;
        const md::Context context(base, manager);
        game::ModelWriteLock lock(manager);
        if (manager != manager_ || !handle_ || context.type_of(handle_) != type_) {
            handle_ = 0;
            manager_ = manager;
            if (now < next_search_ms_) return last_;
            next_search_ms_ = now + search_every_ms;
            const auto roots = context.roots({layout::screen_data});
            if (roots.empty()) return last_;
            handle_ = roots.front().model.handle;
            type_ = roots.front().model.type;
        }
        const auto at = context.address(md::Value{handle_, type_});
        if (!at) return last_;
        last_.in_menu = md::read<std::uint8_t>(at + layout::screen_in_menu) != 0;
        last_.ui_hidden = md::read<std::uint8_t>(at + layout::screen_hide_ui) != 0;
        last_.found = true;
        last_.advanced_edit = last_.in_menu && advanced_edit(context, now, controls_handle_, controls_type_, next_controls_search_ms_);
        if (last_.in_menu) read_timeline(context, base, now, timeline_handle_, timeline_type_, next_timeline_search_ms_, last_);
    } catch (...) {
        handle_ = 0;
        last_ = {};
    }
    return last_;
}

void Reader::forget() noexcept {
    root_handle_ = 0;
    root_type_ = 0;
    root_schema_ = 0;
    next_search_ms_ = 0;
}
} // namespace dingosdk::replay_shake::scan
