#include "replay_shake_model.h"
#include "replay_shake_settings.h"
#include "replay_shake_view.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <array>
#include <format>
#include <string>

// The REPLAY SHAKE page (an addon under EXTRAS): Replay Shake's switch, its default shake, each keyframe's shake, the export and
// setting it up. Presentation thread: reads Replay Shake's snapshot (replay_shake_view.h) and sends
// `replayshake ...` console commands; it never touches the game.
namespace dingosdk::overlay::menu {
namespace {
namespace shake = dingosdk::replay_shake;
struct Page {
    int editing = -1;          // the row whose shake is open (its number), -1 none
    bool follow_editor = true; // open the keyframe the replay editor has selected
    std::string active;        // the slider being dragged, and its value: the snapshot
    float active_value{};      // lags a frame or two behind it
};
Page& page() {
    static auto* value = new Page;
    return *value;
}
void send(SkateMenu& menu, const CallbacksV3& callbacks, const std::string& command) {
    send_console(menu, callbacks, "replayshake " + command);
}

// A slider that sends `<prefix> <setting> <value>` as it moves; `scale` turns the stored value into
// the one shown (and typed in commands), such as a fraction into percent.
void slider(SkateMenu& menu, const CallbacksV3& callbacks, Page& p, const std::string& prefix, const char* label,
    const char* setting, float value, float low, float high, const char* format, float scale = 1, bool logarithmic = false,
    const char* tooltip = nullptr) {
    field(menu, label, tooltip);
    const auto id = prefix + " " + setting;
    float shown = p.active == id ? p.active_value : value * scale;
    ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
    if (logarithmic) flags |= ImGuiSliderFlags_Logarithmic;
    ImGui::PushID(id.c_str());
    const bool moved = ImGui::SliderFloat("##value", &shown, low, high, format, flags);
    if (ImGui::IsItemActive()) {
        p.active = id;
        p.active_value = shown;
    } else if (p.active == id) {
        p.active.clear();
    }
    if (moved) send(menu, callbacks, std::format("{} {} {}", prefix, setting, shown));
    ImGui::PopID();
}

void shake_editor(SkateMenu& menu, const CallbacksV3& callbacks, Page& p, const std::string& prefix, const shake::Shake& value,
    const shake::Blend* blend) {
    ImGui::PushID(prefix.c_str());
    field(menu, "Preset", "Where to start. Changing a single amount below makes it Custom.");
    int preset = static_cast<int>(value.preset);
    if (choice(menu, "preset", preset, {"Handheld", "Walking", "Running", "Vehicle", "Quake", "Drift", "Custom"}))
        send(menu, callbacks, std::format("{} preset {}", prefix, shake::name(static_cast<shake::Preset>(preset))));
    slider(menu, callbacks, p, prefix, "Intensity", "intensity", value.intensity, 0, shake::max_intensity * 100, "%.0f%%", 100, false,
        "Scales every amount: 0% is no shake, 100% the amounts as they are.");
    slider(menu, callbacks, p, prefix, "Speed", "frequency", value.frequency, shake::min_frequency, shake::max_frequency, "%.2f Hz",
        1, true, "How fast it moves: about how many sways a second.");
    slider(menu, callbacks, p, prefix, "Roughness", "roughness", value.roughness, 0, 100, "%.0f%%", 100, false,
        "0% sways smoothly; 100% adds fast jitter on top.");
    if (blend) {
        field(menu, "Into the next", "How this keyframe's shake turns into the next keyframe's.");
        int chosen = static_cast<int>(*blend);
        if (choice(menu, "blend", chosen, {"Smooth", "Linear", "Hold"}))
            send(menu, callbacks, std::format("{} blend {}", prefix, shake::name(static_cast<shake::Blend>(chosen))));
    }
    if (ImGui::TreeNode("Fine tuning")) {
        namespace channel = shake::channel;
        const auto& a = value.amounts;
        slider(menu, callbacks, p, prefix, "Pitch (nod)", "pitch", a[channel::pitch], 0, shake::max_amounts[channel::pitch], "%.2f deg");
        slider(menu, callbacks, p, prefix, "Yaw (pan)", "yaw", a[channel::yaw], 0, shake::max_amounts[channel::yaw], "%.2f deg");
        slider(menu, callbacks, p, prefix, "Roll (tilt)", "roll", a[channel::roll], 0, shake::max_amounts[channel::roll], "%.2f deg");
        slider(menu, callbacks, p, prefix, "Sideways", "sideways", a[channel::sideways], 0, shake::max_amounts[channel::sideways] * 100,
            "%.1f cm", 100);
        slider(menu, callbacks, p, prefix, "Up and down", "up", a[channel::up], 0, shake::max_amounts[channel::up] * 100, "%.1f cm", 100);
        slider(menu, callbacks, p, prefix, "Forward and back", "forward", a[channel::forward], 0,
            shake::max_amounts[channel::forward] * 100, "%.1f cm", 100);
        slider(menu, callbacks, p, prefix, "Zoom", "zoom", a[channel::zoom], 0, shake::max_amounts[channel::zoom], "%.1f%%", 1, false,
            "Breathing of the field of view. Off in every preset.");
        note("The amounts are at 100% intensity: the furthest each motion goes.");
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void keyframes_card(SkateMenu& menu, const CallbacksV3& callbacks, Page& p, const shake::View& view) {
    begin_card(menu, "replay-keys", view.keyframe_mode ? "KEYFRAMES" : "SHAKE KEYS",
        view.keyframe_mode ? "the replay editor's, each with a shake" : "points on the timeline, each with a shake");
    if (view.keyframe_mode) {
        if (view.rows.empty()) note("The replay has no keyframes yet: the default shake runs throughout.");
        toggle_row(menu, "Follow the editor's selection", "Open the keyframe selected in the replay editor.", p.follow_editor);
        if (p.follow_editor)
            for (const auto& row : view.rows)
                if (row.selected) p.editing = row.number;
    } else {
        note(view.rows.empty() ? "No shake keys: the default shake runs throughout. Add one at the playhead to change the shake "
                                 "from there on."
                               : "The shake changes at each key, blending into the next.");
        if (primary_button(menu, "ADD A KEY AT THE PLAYHEAD", view.playhead.has_value())) send(menu, callbacks, "add");
        if (!view.playhead) note("The playhead is not readable yet (README.md, step 3); `replayshake add <seconds>` adds one by time.");
    }
    if (std::none_of(view.rows.begin(), view.rows.end(), [&](const shake::Row& row) { return row.number == p.editing; }))
        p.editing = -1;
    for (const auto& row : view.rows) {
        ImGui::PushID(row.number);
        const auto label = std::format("{}{:>3}   {:>9}   {}{}{}", row.current ? "> " : "  ", row.number, shake::seconds(row.time),
            row.own ? shake::describe(row.shake) : std::string("default"), row.own ? ", " + std::string(shake::name(row.blend)) : "",
            row.selected ? "   (selected)" : "");
        if (ImGui::Selectable(label.c_str(), p.editing == row.number)) {
            p.editing = p.editing == row.number ? -1 : row.number;
            p.follow_editor = false;
        }
        ImGui::PopID();
    }
    for (const auto& row : view.rows) {
        if (row.number != p.editing) continue;
        ImGui::Dummy(ImVec2(0, px(6)));
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted(std::format("{} {} at {}", view.keyframe_mode ? "Keyframe" : "Key", row.number,
            shake::seconds(row.time)).c_str());
        ImGui::PopFont();
        if (view.keyframe_mode && !row.own) note("Uses the default shake: change anything to give it its own.");
        shake_editor(menu, callbacks, p, std::format("key {}", row.number), row.shake, &row.blend);
        if (row.own && ImGui::Button(view.keyframe_mode ? "Back to the default shake" : "Remove this key"))
            send(menu, callbacks, std::format("key {} reset", row.number));
    }
    end_card();
}

void setup_card(SkateMenu& menu, const CallbacksV3& callbacks, const shake::View& view) {
    begin_card(menu, "replay-setup", "SETTING UP", "README.md has the three steps");
    bool test = view.test;
    if (toggle_row(menu, "Test anywhere", "Step 1: shake every view in the level with the default shake, to see the shake works. "
            "Turn the default's intensity up first.", test, view.available))
        send(menu, callbacks, test ? "test 1" : "test 0");
    bool probe = view.probe;
    if (toggle_row(menu, "Probe", "Step 2: list the cameras on show here (and in ReSkate.log), with what moves each (its "
            "director). Open the replay editor: the director that turns up there, and not while skating, is its own.", probe,
            view.available))
        send(menu, callbacks, probe ? "probe 1" : "probe 0");
    std::string cameras;
    for (const auto camera : view.cameras) cameras += (cameras.empty() ? "" : ", ") + shake::hex(camera);
    info(menu, "Replay cameras", cameras.empty() ? std::string("none yet") : cameras);
    if (view.probe) {
        if (!view.menu_state.empty()) info(menu, "Game menus", view.menu_state);
        if (view.cameras_seen.empty()) note("No camera on show (not in a level?).");
        int id = 0;
        const auto offer = [&](const char* what, std::uintptr_t value) {
            ImGui::PushID(++id);
            const bool known = std::find(view.cameras.begin(), view.cameras.end(), value) != view.cameras.end();
            ImGui::AlignTextToFramePadding();
            const bool gameplays = value == view.every_camera ||
                std::find(view.gameplay_directors.begin(), view.gameplay_directors.end(), value) != view.gameplay_directors.end();
            ImGui::TextUnformatted(std::format("On show: {} {}{}", what, shake::hex(value),
                value == view.every_camera ? " (every camera's, gameplay's too)" : gameplays ? " (gameplay's camera has it too)" : "")
                    .c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(known ? "Forget" : "The replay editor's"))
                send(menu, callbacks, std::format("camera {} {}", known ? "remove" : "add", shake::hex(value)));
            ImGui::PopID();
        };
        for (const auto& camera : view.cameras_seen) {
            if (camera.director) offer("director", camera.director);
            if (camera.vtable) offer("camera", camera.vtable);
        }
    }
    info(menu, "Editor mapping", view.mapping);
    if (!view.editor_problem.empty()) warn(view.editor_problem.c_str());
    note("Step 3 (optional): map where the replay editor keeps its playhead and keyframes, with `ui dump` and "
         "`replayshake map`, for a shake on each keyframe.");
    end_card();
}
} // namespace

void replay_page(SkateMenu& menu, const Model&, const CallbacksV3& callbacks) {
    auto& p = page();
    const auto view = shake::view();
    ImGui::BeginDisabled(!callbacks.queue_console_command);

    begin_card(menu, "replay-shake", "CAMERA SHAKE", "for the replay editor");
    bool enabled = view->enabled;
    if (toggle_row(menu, "Replay Shake", "Shake the replay editor's camera: one shake throughout, or one per keyframe.", enabled,
            view->available))
        send(menu, callbacks, enabled ? "on" : "off");
    bool gameplay = view->gameplay;
    if (toggle_row(menu, "Shake gameplay too", "Off: a replay camera known by what gameplay's camera has too (its vtable, or "
            "a director of gameplay's) shakes only while a game menu is up or the game hides its UI, as in the replay editor. "
            "On: whenever it is on show.", gameplay, view->available))
        send(menu, callbacks, gameplay ? "gameplay 1" : "gameplay 0");
    info(menu, "Now", view->available ? view->status : std::string("Unavailable on this game build."));
    if (view->playhead) info(menu, "Playhead", shake::seconds(*view->playhead));
    if (!view->next_step.empty()) warn(view->next_step.c_str());
    end_card();

    begin_card(menu, "replay-default", "DEFAULT SHAKE",
        view->keyframe_mode ? "keyframes without their own" : "the whole replay, until a key says otherwise");
    shake_editor(menu, callbacks, p, "default", view->fallback, nullptr);
    if (ImGui::Button("Reset the default")) send(menu, callbacks, "default reset");
    end_card();

    keyframes_card(menu, callbacks, p, *view);

    begin_card(menu, "replay-export", "EXPORT");
    field(menu, "Frame rate", "The export's frame rate, as picked in the replay editor: each frame of the video shakes "
        "one step of it on, so the video shakes exactly as the editor showed.");
    constexpr std::array<int, 3> rates{24, 30, 60};
    int rate = static_cast<int>(std::find(rates.begin(), rates.end(), view->export_fps) - rates.begin());
    if (rate == static_cast<int>(rates.size())) rate = -1; // a rate set in the console
    if (choice(menu, "replay-fps", rate, {"24 fps", "30 fps", "60 fps"}) && rate >= 0)
        send(menu, callbacks, std::format("fps {}", rates[static_cast<std::size_t>(rate)]));
    info(menu, "Seed", std::to_string(view->seed));
    if (ImGui::Button("Shake another way")) send(menu, callbacks, std::format("seed {}", view->seed + 1));
    end_card();

    setup_card(menu, callbacks, *view);

    ImGui::EndDisabled();
}
} // namespace dingosdk::overlay::menu
