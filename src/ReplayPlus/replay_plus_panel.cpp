#include "replay_plus_overlay.h"
#include "replay_plus_depth.h"
#include "replay_plus_link.h"
#include "replay_plus_native.h"
#include "replay_plus_settings.h"
#include "Extension/ReplayShake/replay_shake_settings.h"
#include "Extension/UI/Overlay/input_capture.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Extension/UI/Overlay/skate_style.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The replay editor's panel: Replay Plus in the replay itself, worked with the controller, drawn as the
// replay editor draws its own (Advanced Edit's tabs and lists, in the game's faces and button glyphs, read
// from its data: replay_plus_native.h). LB / RB change the tab, the
// D-pad picks a row and changes its value (held, faster), A switches or chooses, X puts a value back,
// Y hides the panel while held (to see the picture), B closes. The keyboard works it too: the arrow
// keys or WASD, Enter or Space, Backspace or Esc, Q / E or Tab, R, Home / End, + / -, Shift for bigger steps.
// And the mouse: from the keyboard (F10), or once the mouse moves or clicks, the panel has the pointer
// (ReSkate's overlay gives it, replay_plus_link.h panel_pointer): what is under it lights up, a click works
// it, sliders drag, the wheel scrolls. While it is open the game gets none of the input (replay_plus.cpp
// hold_input), so nothing reaches the replay editor behind it.
namespace dingosdk::replay_plus::overlay {
namespace {
namespace d = dingosdk::overlay::detail;
namespace theme = dingosdk::overlay::theme;
using Clock = std::chrono::steady_clock;

double seconds_now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// XInput's buttons.
constexpr unsigned dpad_up = 0x0001, dpad_down = 0x0002, dpad_left = 0x0004, dpad_right = 0x0008, pad_a = 0x1000, pad_b = 0x2000, pad_x = 0x4000, pad_y = 0x8000,
                   lb = 0x0100, rb = 0x0200;

// One row of a tab: one of Replay Plus's values, one of Replay Shake's, or an action.
enum class RowKind { field, shake_on, shake_preset, shake_number, shake_seed, hood_file, action, preset, naming, video_folder, bind };
struct Row {
    RowKind kind{RowKind::field};
    const Field* field{};
    std::string label, value, hint;
    std::string key;     // shake: its `replayshake default` name; action: what it does; preset: its name
    float low{}, high{}, step{}, now{}; // shake numbers, as shown
    bool on_off{};
    // Under the value, as the game's own panels draw it: a slider at `fraction` (0..1), or `segments`
    // marks with `segment` lit; neither for an action or a preset.
    float fraction{-1};
    int segments{}, segment{};
    bool locked{}; // does not apply now (scribbled out, as the game's own locked rows are); the focus skips it
};

// What the mouse can work, where the panel drew it last frame (later ones are over earlier ones).
enum class HitKind { tab, tab_back, tab_next, row, left, right, slider, segment, plate, scroll, prompt };
struct Hit {
    HitKind kind{};
    ImVec2 a{}, b{};
    int index{};        // the tab, the row, the segment, or the prompt (0 select, 1 reset or delete, 2 back)
    int row{-1};        // the row a part of a row belongs to
    float from{}, to{}; // a slider's track (x), or the scroll bar's (y)
};

struct Panel {
    bool open{};
    int tab{1}; // LENS
    std::array<int, static_cast<std::size_t>(Tab::count)> focus{};
    std::array<float, static_cast<std::size_t>(Tab::count)> scroll{};
    unsigned previous{};      // the buttons last frame
    bool armed{};             // everything let go since it opened (the combo that opened it is not a press)
    unsigned held_dir{};      // the D-pad direction held, and since when, for its repeat
    double held_since{}, next_repeat{};
    std::uint64_t view_revision{};
    Settings local;           // what the rows show: the view's, with this panel's own changes on top
    double changed_at{-10};
    replay_shake::Shake shake;
    bool shake_on{};
    std::uint32_t shake_seed{1};
    double shake_changed_at{-10};
    std::string feedback;
    double feedback_at{-10};
    std::uint64_t reply_serial{};
    std::string confirm;      // a preset waiting for X again to be deleted (or the focus keys for A again)
    double confirm_at{-10};
    // The keyboard (or a pad the game reads itself) has worked the panel since it opened: the game's
    // input is paused whole until it closes, not only the pad's buttons kept from it.
    bool whole_input{};
    std::array<bool, 8> keys_before{};
    // A preset being named (Save as new, or Rename: the preset it was), the name so far and the cursor in it.
    bool naming{};
    std::string name_text, name_for;
    std::size_t name_cursor{};
    // The PRESETS row the focus was on when naming began (it goes back there after), and a preset just renamed:
    // the focus follows it to its place in the list once the list has it (for a few seconds).
    int name_row{};
    std::string follow_preset;
    double follow_until{};
    std::array<bool, 256> keys_was{}; // the keyboard's keys the frame before, for what is typed while naming
    // A button or key being bound (CONTROLS): the row's ("layer", a shortcut's name, "preset:<name>"); everything let go
    // first (the A, Enter or click that began it), then the first button or key pressed is it.
    bool binding{};
    std::string bind_for;
    int bind_column{};  // the column picked on CONTROLS' rows: 0 the controller's, 1 the keyboard's
    int bind_device{};  // the one waited on: only its button (0) or its key (1) counts
    bool bind_armed{};
    unsigned bind_pad_before{};
    std::array<bool, 256> bind_keys_before{};
    bool bind_click_before{};
    double bind_since{};
    double erase_next{};
    // What A does on a preset's row (left / right choose): 0 load, 1 rename, 2 export, 3 delete; for this one.
    int preset_action{};
    std::string preset_action_for;
    // The mouse: the panel has the pointer (opened from the keyboard, or the mouse moved or clicked since it opened),
    // hidden while a pad works it until the mouse moves again; and the keyboard's button prompts shown (the keyboard
    // or the mouse worked it last).
    bool pointer{}, pointer_shown{true}, keyboard_style{};
    POINT cursor_before{LONG_MIN, LONG_MIN};
    unsigned pad_before{};
    std::vector<Hit> hits;
    float row_h{1}, inner_h{1}, content_h{}, track_top{}, track_h{1}, thumb_h{};
    ImVec2 list_a{}, list_b{};
    // The rows scroll to keep the focus in view while keys or a pad move it; the wheel or the scroll bar let it go.
    bool follow_focus{true};
    // Held on an arrow (it repeats), a slider dragged (by its row's label), or the scroll bar.
    bool holding{}, sliding{}, scrolling{};
    HitKind held{HitKind::left};
    int held_row{-1};
    double held_next{};
    std::string slide_label;
};
Panel& panel() {
    static auto* value = new Panel;
    return *value;
}

void post(const std::string& line) { link::post(line); }

std::string shake_number(float v, float shown, int places, std::string_view unit) {
    return std::format("{:.{}f}{}", v * shown, places, unit);
}

// The rows of a tab, from what the view and the panel's own changes say now.
// A time on the replay's clock as the editor's timeline shows it: seconds from its start (through its chunks).
double on_timeline(const link::View& view, double clock) {
    if (!view.timeline_ok) return clock;
    const float share = link::timeline_share(view.chunks, view.timeline_start, view.timeline_length, clock, view.bar_from, view.bar_to);
    return share >= 0 ? share * view.timeline_length : clock - view.timeline_start;
}
std::vector<Row> rows_for(Panel& p, const link::View& view) {
    std::vector<Row> rows;
    const auto tab = static_cast<Tab>(p.tab);
    if (tab == Tab::shake) {
        Row on;
        on.kind = RowKind::shake_on;
        on.label = "Shake";
        on.value = p.shake_on ? "On" : "Off";
        on.hint = "The replay camera's shake (Replay Shake): one shake through the whole replay";
        on.on_off = true;
        on.segments = 2;
        on.segment = p.shake_on ? 1 : 0;
        rows.push_back(on);
        Row style;
        style.kind = RowKind::shake_preset;
        style.label = "Style";
        style.value = std::string(replay_shake::name(p.shake.preset));
        style.value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(style.value[0])));
        style.hint = "Handheld, walking, running, vehicle, earthquake or a slow drift";
        style.segments = static_cast<int>(replay_shake::Preset::count);
        style.segment = static_cast<int>(p.shake.preset);
        style.locked = !p.shake_on;
        rows.push_back(style);
        const auto number = [&](const char* label, const char* key, float now, float low, float high, float step, float shown, int places,
                                std::string_view unit, const char* hint) {
            Row r;
            r.kind = RowKind::shake_number;
            r.label = label;
            r.key = key;
            r.now = now;
            r.low = low;
            r.high = high;
            r.step = step;
            r.value = shake_number(now, shown, places, unit);
            r.hint = hint;
            r.fraction = high > low ? (now - low) / (high - low) : 0;
            r.locked = !p.shake_on;
            rows.push_back(r);
        };
        const auto& s = p.shake;
        number("Intensity", "intensity", s.intensity, 0, 3, 0.05f, 100, 0, "%", "How strongly the camera shakes");
        number("Speed", "frequency", s.frequency, 0.05f, 30, 0.05f, 1, 2, " Hz", "How fast it moves");
        number("Roughness", "roughness", s.roughness, 0, 1, 0.05f, 100, 0, "%", "Smooth to jittery");
        number("Pitch", "pitch", s.amounts[0], 0, 15, 0.1f, 1, 1, "°", "How far it tips up and down, at full intensity");
        number("Yaw", "yaw", s.amounts[1], 0, 15, 0.1f, 1, 1, "°", "How far it turns side to side");
        number("Roll", "roll", s.amounts[2], 0, 15, 0.1f, 1, 1, "°", "How far it rolls");
        number("Sideways", "sideways", s.amounts[3], 0, 0.5f, 0.005f, 100, 1, " cm", "How far it moves sideways");
        number("Up and down", "up", s.amounts[4], 0, 0.5f, 0.005f, 100, 1, " cm", "How far it moves up and down");
        number("Forward", "forward", s.amounts[5], 0, 0.5f, 0.005f, 100, 1, " cm", "How far it moves in and out");
        number("Zoom", "zoom", s.amounts[6], 0, 20, 0.5f, 1, 1, "%", "How much the field of view breathes");
        Row seed;
        seed.kind = RowKind::shake_seed;
        seed.label = "Variation";
        seed.value = std::to_string(p.shake_seed);
        seed.hint = "Another shake with the same settings";
        seed.locked = !p.shake_on;
        rows.push_back(seed);
        return rows;
    }
    if (tab == Tab::presets) {
        if (p.naming) {
            // The name being typed, the cursor in it.
            Row r;
            r.kind = RowKind::naming;
            r.label = p.name_for.empty() ? "New preset" : "Rename";
            r.key = "naming";
            r.value = p.name_text.substr(0, p.name_cursor) + "|" + p.name_text.substr(p.name_cursor);
            r.hint = "Its name: letters, digits, spaces, - and _. On the keyboard type it, Enter keeps it, Esc goes back. On a pad, the D-pad: "
                     "up / down change the letter before the cursor, right adds one, left goes back a letter; X deletes, A keeps it, B goes back.";
            rows.push_back(r);
            return rows;
        }
        const auto action = [&](std::string label, const char* key, std::string value, std::string hint) {
            Row r;
            r.kind = RowKind::action;
            r.label = std::move(label);
            r.key = key;
            r.value = std::move(value);
            r.hint = std::move(hint);
            rows.push_back(r);
        };
        action("Save as new", "save_new", "SAVE", "Keeps everything as it is now, shake included, as a new preset: you name it next");
        if (!p.local.preset.empty())
            action("Save over " + p.local.preset, "save_over", "SAVE", "Saves everything as it is now over " + p.local.preset);
        for (const auto& name : view.presets) {
            Row r;
            r.kind = RowKind::preset;
            r.label = name;
            r.key = name;
            r.value = name == p.local.preset ? "Loaded" : "";
            r.hint = p.confirm == name ? "A (or X) again deletes it: it goes to the Recycle Bin"
                                       : "A loads it. Left / right: Rename it, Export it (a file to send), or Delete it, then A. X deletes it too";
            rows.push_back(r);
        }
        action("Open import folder", "open_import", "OPEN",
               "Preset files you are sent go in this folder: each is added to your presets within a few seconds (the file itself is kept in "
               "its Imported folder). It opens in File Explorer, over the game: " + view.import_folder);
        action("Open export folder", "open_export", "OPEN",
               "Export (on a preset, left / right) puts a copy of it here: the file to send. It opens in File Explorer, over the game: " +
                   view.export_folder);
        return rows;
    }
    for (const auto& f : fields()) {
        if (f.tab != tab) continue;
        Row r;
        r.field = &f;
        r.label = std::string(f.label);
        r.value = shown(f, p.local);
        r.hint = std::string(f.hint);
        r.on_off = f.kind == Kind::toggle;
        r.locked = f.relevant && !f.relevant(p.local);
        const float v = f.get(p.local);
        if (f.kind == Kind::number && f.logarithmic && f.low > 0) r.fraction = std::log(std::max(v, f.low) / f.low) / std::log(f.high / f.low);
        else if (f.kind == Kind::number) r.fraction = f.high > f.low ? (v - f.low) / (f.high - f.low) : 0;
        else if (f.kind == Kind::toggle) r.segments = 2, r.segment = v >= 0.5f ? 1 : 0;
        else r.segments = static_cast<int>(f.choices.size()), r.segment = static_cast<int>(std::lround(v));
        // A setting the lens's choice does not use is of no help: only the chosen lens's own rows stay listed.
        if (r.locked && f.key.starts_with("lens.")) continue;
        if (r.locked && f.key.starts_with("grade.camera_")) continue; // the camera curve's, while it is off
        if (r.locked && f.tab == Tab::focus) continue; // likewise what the focus's mode does not use
        rows.push_back(r);
        if (f.key == "frame.hood" && p.local.hood == Hood::picture) {
            Row file;
            file.kind = RowKind::hood_file;
            file.label = "Picture";
            file.value = p.local.hood_file.empty() ? std::string("(none)") : p.local.hood_file;
            file.hint = view.hood_files.empty() ? "Put pictures (PNG, JPG, BMP or WebP, see-through where the picture shows) in " + view.overlays_folder
                                                : "Pictures in " + view.overlays_folder + ". Each keeps its own shape, never stretched";
            rows.push_back(file);
        }
    }
    if (tab == Tab::lens && lens_on(p.local)) {
        // How much the fisheye enlarges the game's picture in the middle, at the field of view now.
        Row zoom;
        zoom.kind = RowKind::action;
        zoom.key = "none";
        zoom.label = "Middle enlarged";
        const float fov = view.fov_degrees > 0 ? view.fov_degrees : p.local.fov_mode == FovMode::automatic ? view.fov_auto : p.local.fov;
        // The game renders finer than the screen (Sharp middle, VIDEO): what it enlarges of the game's detail.
        const float scale = view.resolution_now > 1.01f ? view.resolution_now : 1.0f;
        zoom.value = scale > 1.0f ? std::format("{:.1f}x at {:.0f}° ({:.1f}x drawn at {:.0f}%)", view.centre_zoom, fov, view.centre_zoom / scale, scale * 100)
                                  : std::format("{:.1f}x at {:.0f}°", view.centre_zoom, fov);
        zoom.hint = "How much the fisheye enlarges the middle of the game's picture: past about 2.5x it softens. Sharp middle (VIDEO) renders "
                    "finer to make up for it; a narrower field of view helps too";
        rows.push_back(zoom);
    }
    if (tab == Tab::focus && p.local.focus == Focus::keys) {
        // The keys: SET puts one at the playhead (or updates the one there) from the distance and blur
        // above; DELETE takes it away; then every key, in time order.
        const bool here = view.playhead_ok && focus_key_at(p.local, view.playhead);
        Row key;
        key.kind = RowKind::action;
        key.key = "focus_key";
        key.label = view.playhead_ok ? std::format("Key at {:.2f} s", on_timeline(view, view.playhead)) : std::string("Key at the playhead");
        key.value = here ? "UPDATE" : "SET";
        key.hint = view.playhead_ok ? "A focus key here, as the distance and blur above are. The triggers still scrub the replay while this is open"
                                    : "Open a replay to put focus keys on it";
        key.locked = !view.playhead_ok;
        rows.push_back(key);
        if (here) {
            Row remove;
            remove.kind = RowKind::action;
            remove.key = "focus_delete";
            remove.label = "Delete this key";
            remove.value = "DELETE";
            remove.hint = "Takes the focus key at the playhead away";
            rows.push_back(remove);
        }
        if (!p.local.focus_keys.empty()) {
            Row clear;
            clear.kind = RowKind::action;
            clear.key = "focus_clear";
            clear.label = std::format("Clear {} key{}", p.local.focus_keys.size(), p.local.focus_keys.size() == 1 ? "" : "s");
            clear.value = p.confirm == "focus_clear" ? "SURE?" : "CLEAR";
            clear.hint = p.confirm == "focus_clear" ? "A again takes every focus key away" : "Takes every focus key away (A, then A again)";
            rows.push_back(clear);
        }
        for (const auto& k : p.local.focus_keys) {
            Row info;
            info.kind = RowKind::action;
            info.key = "none";
            const bool at = view.playhead_ok && std::abs(k.time - view.playhead) <= 1.0 / 30.0;
            info.label = std::format("{:.2f} s{}", on_timeline(view, k.time), at ? "  (here)" : "");
            info.value = std::format("{:.2f} m, {:.0f}%", k.distance, k.blur * 100);
            info.hint = "A focus key. Scrub to it to change it: between keys the focus turns smoothly from one to the next";
            rows.push_back(info);
        }
    }
    if (tab == Tab::light && p.local.light && p.local.light_keyed) {
        // The light's keys: SET puts one at the playhead (or updates the one there), on, as the values above are; OFF one
        // that switches it off from there; DELETE takes it away; then every key, in time order.
        const auto* here = view.playhead_ok ? light_key_at(p.local, view.playhead) : nullptr;
        Row key;
        key.kind = RowKind::action;
        key.key = "light_key";
        key.label = view.playhead_ok ? std::format("Key at {:.2f} s", on_timeline(view, view.playhead)) : std::string("Key at the playhead");
        key.value = here ? "UPDATE" : "SET";
        key.hint = view.playhead_ok ? "A light key here: on, as bright, wide and warm as above. Between keys the light eases from one to the next; it "
                                      "switches on or off at a key"
                                    : "Open a replay to put light keys on it";
        key.locked = !view.playhead_ok;
        rows.push_back(key);
        Row off;
        off.kind = RowKind::action;
        off.key = "light_off";
        off.label = "Off from here";
        off.value = "OFF";
        off.hint = "A light key here that switches the light off (SET on a later key switches it on again)";
        off.locked = !view.playhead_ok;
        rows.push_back(off);
        if (here) {
            Row remove;
            remove.kind = RowKind::action;
            remove.key = "light_delete";
            remove.label = "Delete this key";
            remove.value = "DELETE";
            remove.hint = "Takes the light key at the playhead away";
            rows.push_back(remove);
        }
        if (!p.local.light_keys.empty()) {
            Row clear;
            clear.kind = RowKind::action;
            clear.key = "light_clear";
            clear.label = std::format("Clear {} key{}", p.local.light_keys.size(), p.local.light_keys.size() == 1 ? "" : "s");
            clear.value = p.confirm == "light_clear" ? "SURE?" : "CLEAR";
            clear.hint = p.confirm == "light_clear" ? "A again takes every light key away" : "Takes every light key away (A, then A again)";
            rows.push_back(clear);
        }
        for (const auto& k : p.local.light_keys) {
            Row info;
            info.kind = RowKind::action;
            info.key = "none";
            const bool at = view.playhead_ok && std::abs(k.time - view.playhead) <= 1.0 / 30.0;
            info.label = std::format("{:.2f} s{}", on_timeline(view, k.time), at ? "  (here)" : "");
            info.value = k.on ? std::format("On, {:.0f}%, {:.0f}°", k.power * 100, k.cone) : std::string("Off");
            info.hint = "A light key. Scrub to it to change it: between keys the light eases from one to the next";
            rows.push_back(info);
        }
    }
    if (tab == Tab::focus && p.local.focus == Focus::autofocus) {
        Row now;
        now.kind = RowKind::action;
        now.key = "none";
        now.label = "Focused at";
        now.value = view.focus_distance_now > 0 ? std::format("{:.2f} m", view.focus_distance_now) : std::string("-");
        if (!p.local.focus_skater) now.hint = "How far away the middle of the picture is, as the focus follows it";
        else if (!view.skater_focus_available)
            now.hint = "This game build's replay is not the one Replay Plus knows: the focus follows what is in the middle instead";
        else if (view.skater_distance > 0) now.hint = std::format("Your skater is {:.2f} m away: the focus follows them", view.skater_distance);
        else now.hint = "Play or scrub the replay: the focus follows your skater as soon as the replay has them";
        rows.push_back(now);
    }
    if (tab == Tab::lens) {
        Row now;
        now.kind = RowKind::action;
        now.key = "none";
        now.label = "Field of view now";
        now.value = view.fov_degrees > 0 ? std::format("{:.0f}°", view.fov_degrees) : std::string("-");
        now.hint = "What the replay camera shows now";
        rows.push_back(now);
    }
    if (tab == Tab::controls) {
        // The columns' titles (no row of its own to work: the focus passes it).
        Row titles;
        titles.kind = RowKind::bind;
        titles.key = "titles";
        titles.locked = true;
        rows.push_back(titles);
        Row layer;
        layer.kind = RowKind::bind;
        layer.key = "layer";
        layer.label = "Shortcut button";
        layer.hint = "Hold it in the replay editor and press the buttons below. A tap of it alone stays the game's (R3: Photo). "
                     "A (Enter, a click): press another; X: back to R3";
        rows.push_back(layer);
        for (std::size_t a = 0; a < action_count; ++a) {
            Row r;
            r.kind = RowKind::bind;
            r.key = std::string(action_names[a]);
            r.label = std::string(action_labels[a]);
            r.hint = "Left / right: the controller's or the keyboard's. A (Enter, a click on it): press the new button (held with the "
                     "shortcut button) or key. X: that one back to how it came. One another has already: the two swap";
            rows.push_back(r);
        }
        for (const auto& name : view.presets) {
            Row r;
            r.kind = RowKind::bind;
            r.key = "preset:" + name;
            r.label = "Preset: " + name;
            r.hint = "Loads this look in the replay editor. Left / right: the controller's or the keyboard's. A (Enter, a click on it): "
                     "press a button (held with the shortcut button) or a key for it. X: none there";
            rows.push_back(r);
        }
        Row reset;
        reset.kind = RowKind::action;
        reset.key = "binds_reset";
        reset.label = "Reset controls";
        reset.value = "RESET";
        reset.hint = "Every control back to how it came: R3 with the buttons above, F10 the menu and F11 the shake; no preset shortcuts";
        rows.push_back(reset);
    }
    if (tab == Tab::video) {
        Row now;
        now.kind = RowKind::action;
        now.key = "none";
        now.label = "Render resolution now";
        now.value = view.resolution_now > 0 ? std::format("{:.0f}%", view.resolution_now * 100) : std::string("-");
        if (view.resolution_wanted > 1.0f && view.resolution_limit > 0 && view.resolution_limit + 0.01f < view.resolution_wanted)
            now.value += std::format(" ({:.0f}% would not fit the card's video memory here)", view.resolution_wanted * 100);
        now.hint = "What the game renders at now. Above 100% only as far as the card's video memory allows (a big map leaves less): "
                   "past it Windows moves the game's memory out and the game stutters";
        rows.push_back(now);
        // Where exported videos go: the game's own folder, or one of the player's (each moved there once exported).
        Row folder;
        folder.kind = RowKind::video_folder;
        folder.key = "video_folder";
        folder.label = "Videos go to";
        if (view.video_folder_custom) {
            const auto cut = view.video_folder.find_last_of("\\/");
            folder.value = cut == std::string::npos || cut + 1 >= view.video_folder.size() ? view.video_folder : view.video_folder.substr(cut + 1);
            folder.hint = "Exported videos are moved here once each is done: " + view.video_folder +
                          ". A (Enter, a click) picks another folder; X (R) gives them back to the game's folder";
        } else {
            folder.value = "The game's folder";
            folder.hint = "Exported videos stay where the game puts them: " + view.video_folder +
                          ". A (Enter, a click) picks a folder of your own: the Windows folder picker opens over the game";
        }
        rows.push_back(folder);
        Row open;
        open.kind = RowKind::action;
        open.key = "open_videos";
        open.label = "Open video folder";
        open.value = "OPEN";
        open.hint = "Where your videos go, in File Explorer (over the game): " + view.video_folder;
        rows.push_back(open);
    }
    return rows;
}

void say(Panel& p, std::string text) {
    p.feedback = std::move(text);
    p.feedback_at = seconds_now();
}

// What left / right (`steps`), A (`press`) and X (`reset`) do on a row.
// ---- Naming a preset: the keyboard types it; on a pad the D-pad picks its letters -----------------------
constexpr std::size_t name_most = 40;
constexpr std::string_view name_letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_()";
void start_naming(Panel& p, std::string text, std::string for_preset) {
    p.naming = true;
    p.name_text = std::move(text);
    if (p.name_text.size() > name_most) p.name_text.resize(name_most);
    p.name_cursor = p.name_text.size();
    p.name_for = std::move(for_preset);
    p.name_row = p.focus[static_cast<std::size_t>(Tab::presets)];
    p.confirm.clear();
    // Keys already down (the Enter that chose this) are not typed.
    for (int vk = 1; vk < 256; ++vk) p.keys_was[static_cast<std::size_t>(vk)] = dingosdk::overlay::key_down(vk);
}
// A key pressed since the frame before.
bool key_pressed(Panel& p, int vk) {
    const bool down = dingosdk::overlay::key_down(vk);
    auto& was = p.keys_was[static_cast<std::size_t>(vk)];
    const bool pressed = down && !was;
    was = down;
    return pressed;
}
void insert_letter(Panel& p, char c) {
    if (p.name_text.size() >= name_most) return;
    p.name_text.insert(p.name_cursor, 1, c);
    ++p.name_cursor;
}
// The keyboard, this frame: letters (capitals with Shift, or Caps Lock), digits, a space, '-' ('_' with Shift),
// '(' and ')' (Shift with 9 and 0).
void type_letters(Panel& p) {
    const bool shift = dingosdk::overlay::key_down(VK_SHIFT), caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    for (int vk = 'A'; vk <= 'Z'; ++vk)
        if (key_pressed(p, vk)) insert_letter(p, static_cast<char>(shift != caps ? vk : vk + ('a' - 'A')));
    for (int vk = '0'; vk <= '9'; ++vk)
        if (key_pressed(p, vk)) {
            if (!shift) insert_letter(p, static_cast<char>(vk));
            else if (vk == '9') insert_letter(p, '(');
            else if (vk == '0') insert_letter(p, ')');
        }
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)
        if (key_pressed(p, vk)) insert_letter(p, static_cast<char>('0' + (vk - VK_NUMPAD0)));
    if (key_pressed(p, VK_SPACE)) insert_letter(p, ' ');
    if (key_pressed(p, VK_OEM_MINUS)) insert_letter(p, shift ? '_' : '-');
    if (key_pressed(p, VK_SUBTRACT)) insert_letter(p, '-');
}
void finish_naming(Panel& p, const link::View& view) {
    const auto name = clean_preset_name(p.name_text);
    if (name.empty()) {
        say(p, "A name, please: letters, digits, spaces, - and _.");
        return;
    }
    const bool renaming = !p.name_for.empty();
    const bool taken = std::any_of(view.presets.begin(), view.presets.end(), [&](const std::string& t) { return same_preset_file(t, name); });
    if (taken && !(renaming && same_preset_file(name, p.name_for))) {
        say(p, std::format("There is a preset called {} already: another name, please.", name));
        return;
    }
    p.naming = false;
    if (!renaming) post("replayplus preset new " + name);
    else if (name != p.name_for) {
        post("replayplus preset rename " + p.name_for + "|" + name);
        p.follow_preset = name;
        p.follow_until = seconds_now() + 3;
    }
    p.changed_at = seconds_now() - 1;
}
// A frame while naming: the keyboard's keys, and the pad's buttons pressed and D-pad moves.
unsigned keyboard_buttons(); // (the keys the panel reads as buttons, below)
// The panel waiting for a button or key to bind (CONTROLS): once everything is let go, the first button pressed (B or Esc
// cancel, as does a click) or key (with Ctrl, Shift or Alt held: with them) is the row's, given to the game thread as a bind
// line (`replayplus bind ...`, which swaps it with another that had it).
void bind_input(Panel& p, const ControllerInput& input) {
    const double now = seconds_now();
    std::array<bool, 256> keys{};
    bool any_key = false;
    for (int vk = 8; vk < 256; ++vk) {
        keys[static_cast<std::size_t>(vk)] = dingosdk::overlay::key_down(vk);
        any_key = any_key || keys[static_cast<std::size_t>(vk)];
    }
    const unsigned pad = input.available ? input.buttons : 0u;
    const bool click = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const auto end = [&](std::string_view said) {
        p.binding = false;
        p.bind_armed = false;
        link::panel_capturing().store(false, std::memory_order_release);
        // What is still down is no press of the panel's.
        p.previous = (pad & 0xffffu) | keyboard_buttons();
        p.keys_was = keys;
        if (!said.empty()) say(p, std::string(said));
    };
    if (!p.bind_armed) {
        if (!pad && !any_key && !click) p.bind_armed = true;
        p.bind_pad_before = pad;
        p.bind_keys_before = keys;
        p.bind_click_before = click;
        if (now - p.bind_since > 10) end("Not changed.");
        return;
    }
    const unsigned pressed = pad & ~p.bind_pad_before;
    const auto key_pressed = [&](int vk) { return keys[static_cast<std::size_t>(vk)] && !p.bind_keys_before[static_cast<std::size_t>(vk)]; };
    if ((pressed & pad_b) || key_pressed(VK_ESCAPE) || (click && !p.bind_click_before) || now - p.bind_since > 20) {
        end("Not changed.");
        return;
    }
    const std::string target = p.bind_for == "layer" ? std::string("layer")
                               : p.bind_for.starts_with("preset:") ? "preset " + p.bind_for.substr(7)
                                                                    : p.bind_for;
    if (pressed && p.bind_device == 0) {
        const unsigned bit = pressed & (~pressed + 1u); // the first newly pressed (the lowest)
        post(p.bind_for == "layer" ? "replayplus bind layer " + pad_words(bit) : "replayplus bind " + target + " pad " + pad_words(bit));
        p.changed_at = now - 1; // the view brings it
        end({});
        return;
    }
    if (p.bind_for != "layer" && p.bind_device == 1) {
        for (int vk = 8; vk < 256; ++vk) {
            if (!key_pressed(vk) || vk == VK_ESCAPE) continue;
            // (The modifiers alone are no key: they go with the key pressed while they are held.)
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) continue;
            std::uint32_t key = static_cast<std::uint32_t>(vk);
            if (keys[VK_CONTROL]) key |= key_ctrl;
            if (keys[VK_SHIFT]) key |= key_shift;
            if (keys[VK_MENU]) key |= key_alt;
            post("replayplus bind " + target + " key " + key_name(key));
            p.changed_at = now - 1;
            end({});
            return;
        }
    }
    p.bind_pad_before = pad;
    p.bind_keys_before = keys;
    p.bind_click_before = click;
}
void naming_input(Panel& p, const link::View& view, unsigned pressed, int vertical, int horizontal) {
    type_letters(p);
    const double now = seconds_now();
    // Backspace, held: again and again after a moment.
    auto& back_was = p.keys_was[static_cast<std::size_t>(VK_BACK)];
    const bool back_down = dingosdk::overlay::key_down(VK_BACK), back_pressed = back_down && !back_was;
    back_was = back_down;
    bool erase = back_pressed || (pressed & pad_x) != 0;
    if (back_pressed) p.erase_next = now + 0.45;
    else if (back_down && now >= p.erase_next) {
        erase = true;
        p.erase_next = now + 0.05;
    }
    if (erase && p.name_cursor > 0) p.name_text.erase(--p.name_cursor, 1);
    if (key_pressed(p, VK_DELETE) && p.name_cursor < p.name_text.size()) p.name_text.erase(p.name_cursor, 1);
    if (key_pressed(p, VK_LEFT) && p.name_cursor > 0) --p.name_cursor;
    if (key_pressed(p, VK_RIGHT) && p.name_cursor < p.name_text.size()) ++p.name_cursor;
    if (key_pressed(p, VK_HOME)) p.name_cursor = 0;
    if (key_pressed(p, VK_END)) p.name_cursor = p.name_text.size();
    // The pad: up / down the letter before the cursor (one put in at the start), right on (at the end: a new
    // letter), left back.
    if (vertical) {
        if (p.name_cursor == 0) insert_letter(p, 'A');
        else {
            char& c = p.name_text[p.name_cursor - 1];
            const auto count = name_letters.size();
            auto at = name_letters.find(c);
            if (at == std::string_view::npos) at = 0;
            c = name_letters[(at + (vertical < 0 ? 1 : count - 1)) % count];
        }
    }
    if (horizontal < 0 && p.name_cursor > 0) --p.name_cursor;
    if (horizontal > 0) {
        if (p.name_cursor < p.name_text.size()) ++p.name_cursor;
        else insert_letter(p, 'a');
    }
    const bool keep = key_pressed(p, VK_RETURN) || (pressed & pad_a) != 0;
    const bool back = key_pressed(p, VK_ESCAPE) || (pressed & pad_b) != 0;
    if (back) {
        p.naming = false;
        say(p, p.name_for.empty() ? "Not saved." : "Not renamed.");
        return;
    }
    if (keep) finish_naming(p, view);
}

void act(Panel& p, const link::View& view, const Row& row, int steps, bool press, bool reset) {
    const double now = seconds_now();
    switch (row.kind) {
    case RowKind::field: {
        const auto& f = *row.field;
        if (reset) {
            Settings defaults;
            f.set(p.local, f.get(defaults));
        } else if (press && f.kind == Kind::toggle) {
            step(f, p.local, 1);
        } else if (press && f.kind == Kind::choice) {
            step(f, p.local, 1);
        } else if (steps) {
            step(f, p.local, steps);
        } else {
            return;
        }
        p.changed_at = now;
        // As the panel shows it ("1.20", "mk1"): what `set` reads back the same.
        const float v = f.get(p.local);
        std::string value = f.kind == Kind::choice ? std::string(f.choices[static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(v)), 0,
                                                                                                                   static_cast<int>(f.choices.size()) - 1))])
                          : f.kind == Kind::toggle ? (v >= 0.5f ? "on" : "off")
                                                   : std::format("{}", v);
        post(std::format("replayplus set {} {}", f.key, value));
        return;
    }
    case RowKind::shake_on:
        if (!(press || steps || reset)) return;
        p.shake_on = reset ? true : !p.shake_on;
        p.shake_changed_at = now;
        post(p.shake_on ? "replayshake on" : "replayshake off");
        return;
    case RowKind::shake_preset: {
        if (!(press || steps || reset)) return;
        const int count = static_cast<int>(replay_shake::Preset::count) - 1; // custom is where a change leads, not a choice
        int index = static_cast<int>(p.shake.preset);
        if (index >= count) index = 0;
        index = reset ? 0 : ((index + (steps ? steps : 1)) % count + count) % count;
        const auto preset = static_cast<replay_shake::Preset>(index);
        replay_shake::apply_preset(p.shake, preset);
        p.shake.preset = preset;
        p.shake_changed_at = now;
        post(std::format("replayshake default preset {}", replay_shake::name(preset)));
        return;
    }
    case RowKind::shake_number: {
        if (!(steps || reset)) return;
        const auto fallback = replay_shake::preset_shake(p.shake.preset == replay_shake::Preset::custom ? replay_shake::Preset::handheld : p.shake.preset);
        float next = row.now + static_cast<float>(steps) * row.step;
        if (reset) {
            next = row.key == "intensity" ? 1.0f : row.key == "frequency" ? fallback.frequency : row.key == "roughness" ? fallback.roughness : row.now;
            for (std::size_t c = 0; c < replay_shake::channel_count; ++c)
                if (row.key == replay_shake::channel_names[c]) next = fallback.amounts[c];
        }
        next = std::clamp(std::round(next / row.step) * row.step, row.low, row.high);
        if (row.key == "intensity") p.shake.intensity = next;
        else if (row.key == "frequency") p.shake.frequency = next;
        else if (row.key == "roughness") p.shake.roughness = next;
        for (std::size_t c = 0; c < replay_shake::channel_count; ++c)
            if (row.key == replay_shake::channel_names[c]) {
                p.shake.amounts[c] = next;
                p.shake.preset = replay_shake::Preset::custom;
            }
        p.shake_changed_at = now;
        // Replay Shake takes intensity and roughness in percent, turns in degrees and moves in centimetres.
        const bool percent = row.key == "intensity" || row.key == "roughness";
        const bool centimetres = row.key == "sideways" || row.key == "up" || row.key == "forward";
        const float sent = percent ? next * 100.0f : centimetres ? next * 100.0f : next;
        post(std::format("replayshake default {} {}", row.key, sent));
        return;
    }
    case RowKind::shake_seed: {
        if (!(steps || press || reset)) return;
        const long next = reset ? 1L : std::clamp(static_cast<long>(p.shake_seed) + (steps ? steps : 1), 1L, 99999L);
        p.shake_seed = static_cast<std::uint32_t>(next);
        p.shake_changed_at = now;
        post(std::format("replayshake seed {}", next));
        return;
    }
    case RowKind::hood_file:
        if (!(steps || press)) return;
        if (view.hood_files.empty()) {
            say(p, "No pictures in " + view.overlays_folder + " yet.");
            return;
        }
        post(steps < 0 ? "replayplus hood previous" : "replayplus hood next");
        p.changed_at = now - 1; // the view brings the file's name
        return;
    case RowKind::action:
        if (!press) return;
        if (row.key == "save_new") {
            start_naming(p, unused_preset_name("My look", view.presets), {});
        } else if (row.key == "open_import") {
            post("replayplus preset open import");
        } else if (row.key == "open_export") {
            post("replayplus preset open export");
        } else if (row.key == "open_videos") {
            post("replayplus videofolder open");
        } else if (row.key == "binds_reset") {
            post("replayplus bind reset");
            p.changed_at = now - 1;
        } else if (row.key == "save_over") {
            post("replayplus preset save " + p.local.preset);
        } else if (row.key == "focus_key") {
            post("replayplus focus key");
            p.changed_at = now - 1; // the view brings the key
        } else if (row.key == "focus_delete") {
            post("replayplus focus delete");
            p.changed_at = now - 1;
        } else if (row.key == "light_key") {
            post("replayplus light key");
            p.changed_at = now - 1;
        } else if (row.key == "light_off") {
            post("replayplus light off");
            p.changed_at = now - 1;
        } else if (row.key == "light_delete") {
            post("replayplus light delete");
            p.changed_at = now - 1;
        } else if (row.key == "light_clear") {
            if (p.confirm == "light_clear" && now - p.confirm_at < 4) {
                post("replayplus light clear");
                p.confirm.clear();
                p.changed_at = now - 1;
            } else {
                p.confirm = "light_clear";
                p.confirm_at = now;
            }
        } else if (row.key == "focus_clear") {
            if (p.confirm == "focus_clear" && now - p.confirm_at < 4) {
                post("replayplus focus clear");
                p.confirm.clear();
                p.changed_at = now - 1;
            } else {
                p.confirm = "focus_clear";
                p.confirm_at = now;
            }
        }
        return;
    case RowKind::preset: {
        if (p.preset_action_for != row.key) {
            p.preset_action_for = row.key;
            p.preset_action = 0;
        }
        if (steps) {
            p.preset_action = ((p.preset_action + (steps > 0 ? 1 : -1)) % 4 + 4) % 4;
            p.confirm.clear();
            return;
        }
        const auto remove = [&] {
            if (p.confirm == row.key && now - p.confirm_at < 4) {
                post("replayplus preset delete " + row.key);
                p.confirm.clear();
                p.preset_action = 0;
            } else {
                p.confirm = row.key;
                p.confirm_at = now;
            }
        };
        if (reset) {
            remove();
            return;
        }
        if (!press) return;
        if (p.preset_action == 1) {
            start_naming(p, row.key, row.key);
        } else if (p.preset_action == 2) {
            post("replayplus preset export " + row.key);
        } else if (p.preset_action == 3) {
            remove();
        } else {
            p.confirm.clear();
            post("replayplus preset load " + row.key);
            p.changed_at = now - 1;
        }
        return;
    }
    case RowKind::naming:
        return;
    case RowKind::bind: {
        if (row.locked) return;
        // Left / right: the controller's column or the keyboard's (the shortcut button has only the controller's).
        const bool layer_row = row.key == "layer";
        if (steps) {
            p.bind_column = layer_row || steps < 0 ? 0 : 1;
            return;
        }
        const int column = layer_row ? 0 : p.bind_column;
        const char* which = column ? "key" : "pad";
        if (reset) {
            if (layer_row) post("replayplus bind layer default");
            else if (row.key.starts_with("preset:")) post("replayplus bind preset " + row.key.substr(7) + " clear " + which);
            else post("replayplus bind " + row.key + " default " + which);
            p.changed_at = now - 1; // the view brings it
            return;
        }
        if (!press) return;
        p.binding = true;
        p.bind_for = row.key;
        p.bind_device = column;
        p.bind_armed = false;
        p.bind_since = now;
        link::panel_capturing().store(true, std::memory_order_release);
        return;
    }
    case RowKind::video_folder:
        if (reset) post("replayplus videofolder default");
        else if (press) post("replayplus videofolder pick");
        else return;
        p.changed_at = now - 1; // the view brings the folder
        return;
    }
}
// A slider set where the mouse holds it (0..1 along it), stepped as the panel's steps go.
void slide(Panel& p, const link::View& view, const Row& row, float fraction) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (row.locked) return;
    if (row.kind == RowKind::field && row.field && row.field->kind == Kind::number) {
        const auto& f = *row.field;
        float v = f.logarithmic && f.low > 0 ? f.low * std::pow(f.high / f.low, fraction) : f.low + (f.high - f.low) * fraction;
        if (f.logarithmic) {
            const float digits = std::pow(10.0f, std::floor(std::log10(std::max(v, 1e-3f))) - 1.0f);
            v = std::round(v / digits) * digits;
        } else if (f.step > 0) {
            v = f.low + std::round((v - f.low) / f.step) * f.step;
        }
        v = std::clamp(v, f.low, f.high);
        if (std::abs(v - f.get(p.local)) < 1e-6f) return;
        f.set(p.local, v);
        p.changed_at = seconds_now();
        post(std::format("replayplus set {} {}", f.key, f.get(p.local)));
        return;
    }
    if (row.kind == RowKind::shake_number && row.step > 0) {
        const float next = row.low + (row.high - row.low) * fraction;
        const int steps = static_cast<int>(std::lround((next - row.now) / row.step));
        if (steps) act(p, view, row, steps, false, false);
    }
}
// One of a row's marks clicked: the switch or the choice set to it.
void choose(Panel& p, const link::View& view, const Row& row, int segment) {
    if (row.kind == RowKind::bind) {
        // A click on one of its columns: that one, waiting for its button or key.
        if (row.locked) return;
        p.bind_column = row.key == "layer" ? 0 : std::clamp(segment, 0, 1);
        act(p, view, row, 0, true, false);
        return;
    }
    if (row.locked || segment == row.segment) return;
    if (row.kind == RowKind::field && row.field) {
        if (row.field->kind == Kind::toggle) act(p, view, row, 0, true, false);
        else act(p, view, row, segment - row.segment, false, false);
    } else if (row.kind == RowKind::shake_on) {
        act(p, view, row, 0, true, false);
    } else if (row.kind == RowKind::shake_preset) {
        act(p, view, row, segment - row.segment, false, false);
    }
}

// ---- Drawing: as the game's replay editor draws its own ------------------------------------------
// Measured in Advanced Edit's own lists (its Effects, its video settings) on a 1920 x 1440 window, 2026-11-19. Sizes are
// on a 1920 x 1080 screen, scaled as the game scales its UI (native::ui_scale). Its faces (Gilroy SemiBold for labels and
// prompts, ExtraBold for values and the row in focus), rows on black at a third over the picture, the row in focus a
// shade darker with a blue edge, a value over a blue bar with a white knob (a choice over blue marks), its own button
// glyphs, and a screen's title in its marker face: read from the game's data (replay_plus_native.h), else ReSkate's
// faces with glyphs drawn here.
namespace look {
constexpr ImU32 white = IM_COL32(255, 255, 255, 255);
constexpr ImU32 blue = IM_COL32(0, 131, 255, 255);      // the game's blue (#0083FF)
constexpr ImU32 plate = IM_COL32(0, 0, 0, 84);          // a list's black, a third over the picture
constexpr ImU32 focus = IM_COL32(0, 0, 0, 76);          // over it, the row in focus
constexpr ImU32 track = IM_COL32(255, 255, 255, 34);    // what a slider or the marks run along
constexpr ImU32 dim = IM_COL32(255, 255, 255, 110);     // a row that does not apply now
constexpr ImU32 soft = IM_COL32(255, 255, 255, 190);
constexpr ImU32 shadow = IM_COL32(0, 0, 0, 150);        // under the prompts' words
constexpr ImU32 gold = IM_COL32(227, 204, 114, 255);    // the game's CONFIRM plate
constexpr ImU32 ink = IM_COL32(14, 14, 14, 255);
constexpr ImU32 danger = IM_COL32(255, 110, 92, 255);
} // namespace look

struct Fonts {
    ImFont *label, *value, *title;
};
Fonts fonts() {
    const auto& m = d::state().menu;
    Fonts f{native::face(native::Face::label), native::face(native::Face::value), native::face(native::Face::title)};
    if (!f.label) f.label = m.body ? m.body : ImGui::GetFont();
    if (!f.value) f.value = m.bold ? m.bold : ImGui::GetFont();
    if (!f.title) f.title = m.title_large ? m.title_large : m.title ? m.title : f.value;
    return f;
}
// Sizes here are a face's em, as the game sets its text (Gilroy 26 is an em of 26 pixels); ImGui draws a face by its
// line, taller by its ascent and descent: the line that makes that em, set so the em sits where it was asked.
float line_size(ImFont* font, float size) {
    for (const auto which : {native::Face::label, native::Face::value, native::Face::title})
        if (font == native::face(which)) return size * native::line_per_em(which);
    return size;
}
float width_of(ImFont* font, float size, std::string_view text) {
    return font->CalcTextSizeA(line_size(font, size), FLT_MAX, 0, text.data(), text.data() + text.size()).x;
}
void text_at(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 colour, std::string_view text, float right = FLT_MAX) {
    const float line = line_size(font, size);
    at.y -= (line - size) * 0.5f;
    if (right < FLT_MAX) draw->PushClipRect(ImVec2(at.x, at.y - line), ImVec2(right, at.y + line * 2), true);
    draw->AddText(font, line, at, colour, text.data(), text.data() + text.size());
    if (right < FLT_MAX) draw->PopClipRect();
}
// Words with the soft shadow the game's prompts and titles have.
void shadowed(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 colour, std::string_view text, float s) {
    const ImU32 under = (look::shadow & ~IM_COL32_A_MASK) | (((look::shadow >> IM_COL32_A_SHIFT) * ((colour >> IM_COL32_A_SHIFT) & 0xff) / 255) << IM_COL32_A_SHIFT);
    text_at(draw, font, size, ImVec2(at.x + 1.5f * s, at.y + 2.0f * s), under, text);
    text_at(draw, font, size, at, colour, text);
}
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
// A tab's name as the game names its own under its tile: "Lens".
std::string tab_title(std::size_t tab) {
    std::string name(tab_names[tab]);
    for (std::size_t i = 1; i < name.size(); ++i) name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    return name;
}
// A label as the game's lists set theirs: on one line, or broken onto two at a space when it is too long.
std::pair<std::string, std::string> label_lines(ImFont* font, float size, std::string_view text, float width) {
    if (width_of(font, size, text) <= width) return {std::string(text), {}};
    std::size_t best = std::string_view::npos;
    for (auto at = text.find(' '); at != std::string_view::npos; at = text.find(' ', at + 1)) {
        if (width_of(font, size, text.substr(0, at)) > width) break;
        best = at;
    }
    if (best == std::string_view::npos) return {std::string(text), {}};
    return {std::string(text.substr(0, best)), std::string(text.substr(best + 1))};
}
// The pad as ReSkate reads it for itself (the game's reads are held meanwhile); in test builds with
// ReSkate's scripted pad's buttons too (the test inbox's `replayplus pad`).
void read_pad(ControllerInput& input) {
    DingoSDKOverlayReadControllerInput(&input, true);
#if defined(REPLAY_PLUS_TEST_PAD)
    if (WORD scripted{}; dingosdk::overlay::scripted_buttons(scripted) && scripted) {
        input.available = true;
        input.buttons |= scripted;
    }
#endif
}

using P = native::Picture;
// The game's picture of a pad's button, for the pad in use (an Xbox pad's, a DualShock 4's or a DualSense's).
std::optional<P> pad_picture(std::string_view button, ControllerStyle style) {
    const bool ps = style != ControllerStyle::xbox, five = style == ControllerStyle::dualsense;
    if (button == "A") return ps ? P::ps_cross : P::xbox_a;
    if (button == "B") return ps ? P::ps_circle : P::xbox_b;
    if (button == "X") return ps ? P::ps_square : P::xbox_x;
    if (button == "Y") return ps ? P::ps_triangle : P::xbox_y;
    if (button == "LB") return ps ? P::ps_l1 : P::xbox_lb;
    if (button == "RB") return ps ? P::ps_r1 : P::xbox_rb;
    if (button == "LT") return ps ? P::ps_l2 : P::xbox_lt;
    if (button == "RT") return ps ? P::ps_r2 : P::xbox_rt;
    if (button == "L3") return ps ? P::ps_l3 : P::xbox_l3;
    if (button == "R3") return ps ? P::ps_r3 : P::xbox_r3;
    if (button == "VIEW") return ps ? (five ? P::ps5_touchpad : P::ps4_touchpad) : P::xbox_view;
    if (button == "MENU") return ps ? (five ? P::ps5_options : P::ps4_options) : P::xbox_menu;
    return std::nullopt;
}
// The game's picture of a key it draws whole (else it writes the key's name on a blank key).
std::optional<P> key_picture(std::string_view key) {
    if (key == "Enter") return P::key_enter;
    if (key == "Esc") return P::key_esc;
    if (key == "Tab") return P::key_tab;
    if (key == "Space") return P::key_space;
    if (key == "Up") return P::key_up;
    if (key == "Down") return P::key_down;
    if (key == "Left") return P::key_left;
    if (key == "Right") return P::key_right;
    return std::nullopt;
}
// A button drawn here, when the game's pictures are not loaded: Xbox's coloured letters (PlayStation's shapes), a
// shoulder button on a grey plate, or a key on a keycap. Returns the width drawn.
float drawn_glyph(ImDrawList* draw, const Fonts& f, float side, ImVec2 at, std::string_view button, ControllerStyle style, bool keyboard) {
    if (keyboard || button.size() > 1) {
        std::string label(button);
        if (!keyboard && style != ControllerStyle::xbox) label = label == "LB" ? "L1" : label == "RB" ? "R1" : label == "LT" ? "L2" : label == "RT" ? "R2" : label;
        const float size = side * 0.52f, inset = side * 0.26f, cap = std::max(side, width_of(f.value, size, label) + inset * 2);
        if (draw) {
            draw->AddRectFilled(ImVec2(at.x, at.y + side * 0.05f), ImVec2(at.x + cap, at.y + side * 0.95f), keyboard ? look::white : IM_COL32(70, 71, 75, 255),
                                side * 0.14f);
            draw->AddRect(ImVec2(at.x, at.y + side * 0.05f), ImVec2(at.x + cap, at.y + side * 0.95f), look::ink, side * 0.14f, 0, side * 0.06f);
            text_at(draw, f.value, size, ImVec2(at.x + (cap - width_of(f.value, size, label)) * .5f, at.y + (side - size) * .5f), keyboard ? look::ink : look::white,
                    label);
        }
        return cap;
    }
    if (draw) {
        const ImVec2 centre(at.x + side * .5f, at.y + side * .5f);
        const char letter = button.empty() ? 'A' : button[0];
        draw->AddCircleFilled(centre, side * .5f, look::ink);
        draw->AddCircleFilled(centre, side * .44f, look::white);
        if (style == ControllerStyle::xbox) {
            const char text[2]{letter, 0};
            const float size = side * .55f;
            text_at(draw, f.value, size, ImVec2(centre.x - width_of(f.value, size, text) * .5f, centre.y - size * .5f), look::ink, text);
        } else {
            const float r = side * .2f, stroke = side * .08f;
            if (letter == 'A') {
                draw->AddLine(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), look::ink, stroke);
                draw->AddLine(ImVec2(centre.x - r, centre.y + r), ImVec2(centre.x + r, centre.y - r), look::ink, stroke);
            } else if (letter == 'B') {
                draw->AddCircle(centre, r, look::ink, 0, stroke);
            } else if (letter == 'X') {
                draw->AddRect(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), look::ink, 0, 0, stroke);
            } else {
                draw->AddTriangle(ImVec2(centre.x, centre.y - r * 1.1f), ImVec2(centre.x + r, centre.y + r * .75f), ImVec2(centre.x - r, centre.y + r * .75f), look::ink,
                                  stroke);
            }
        }
    }
    return side;
}
// A button as the game's prompts draw it, `side` tall with its top left at `at`: the pad's own glyph, or a key (its
// name on a key, wider for a longer name). Returns the width drawn; with no draw list it only measures.
float glyph(ImDrawList* draw, const Fonts& f, float side, ImVec2 at, std::string_view button, ControllerStyle style, bool keyboard) {
    if (keyboard) {
        if (const auto picture = key_picture(button); picture && native::loaded(*picture)) {
            if (draw) native::draw(draw, *picture, at, ImVec2(at.x + side, at.y + side));
            return side;
        }
        if (native::loaded(P::key)) {
            const float size = side * (button.size() > 2 ? 0.46f : 0.58f);
            const float w = std::max(side, width_of(f.value, size, button) + side * 0.5f);
            if (draw) {
                native::draw_wide_key(draw, at, ImVec2(at.x + w, at.y + side));
                text_at(draw, f.value, size, ImVec2(at.x + (w - width_of(f.value, size, button)) * 0.5f, at.y + (side - size) * 0.5f - side * 0.03f), look::ink,
                        button);
            }
            return w;
        }
    } else if (const auto picture = pad_picture(button, style); picture && native::loaded(*picture)) {
        const float w = side * native::aspect(*picture);
        if (draw) native::draw(draw, *picture, at, ImVec2(at.x + w, at.y + side));
        return w;
    }
    return drawn_glyph(draw, f, side, at, button, style, keyboard);
}
// The "+" the game puts between two buttons held together. Returns the width.
float plus_sign(ImDrawList* draw, const Fonts& f, float side, ImVec2 at, float s) {
    if (native::loaded(P::plus)) {
        const float h = side * 0.5f, w = h * native::aspect(P::plus);
        if (draw) native::draw(draw, P::plus, ImVec2(at.x, at.y + (side - h) * 0.5f), ImVec2(at.x + w, at.y + (side + h) * 0.5f));
        return w;
    }
    const float size = side * 0.62f;
    if (draw) shadowed(draw, f.value, size, ImVec2(at.x, at.y + (side - size) * 0.5f), look::white, "+", s);
    return width_of(f.value, size, "+");
}
// The D-pad as the game draws it, the arms that do something marked (1 up, 2 down, 4 left, 8 right).
float dpad_glyph(ImDrawList* draw, float side, ImVec2 at, unsigned arms, ControllerStyle style) {
    const bool ps = style != ControllerStyle::xbox;
    P picture = ps ? P::ps_dpad : P::xbox_dpad;
    if (arms == (1u | 2u)) picture = ps ? P::ps_dpad_vertical : P::xbox_dpad_vertical;
    else if (arms == (4u | 8u)) picture = ps ? P::ps_dpad_horizontal : P::xbox_dpad_horizontal;
    else if (arms == 1u) picture = ps ? P::ps_dpad_up : P::xbox_dpad_up;
    else if (arms == 2u) picture = ps ? P::ps_dpad_down : P::xbox_dpad_down;
    else if (arms == 4u) picture = ps ? P::ps_dpad_left : P::xbox_dpad_left;
    else if (arms == 8u) picture = ps ? P::ps_dpad_right : P::xbox_dpad_right;
    if (native::loaded(picture)) {
        if (draw) native::draw(draw, picture, at, ImVec2(at.x + side, at.y + side));
        return side;
    }
    if (draw) {
        const ImVec2 c(at.x + side * .5f, at.y + side * .5f);
        const float a = side * .15f, b = side * .47f, r = side * 0.04f;
        draw->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x + a, c.y + b), look::white, r);
        draw->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x + b, c.y + a), look::white, r);
        const ImU32 lit = look::ink;
        const float t = side * 0.09f;
        if (arms & 1) draw->AddTriangleFilled(ImVec2(c.x, c.y - b + t * 0.6f), ImVec2(c.x - t, c.y - b + t * 2.0f), ImVec2(c.x + t, c.y - b + t * 2.0f), lit);
        if (arms & 2) draw->AddTriangleFilled(ImVec2(c.x, c.y + b - t * 0.6f), ImVec2(c.x - t, c.y + b - t * 2.0f), ImVec2(c.x + t, c.y + b - t * 2.0f), lit);
        if (arms & 4) draw->AddTriangleFilled(ImVec2(c.x - b + t * 0.6f, c.y), ImVec2(c.x - b + t * 2.0f, c.y - t), ImVec2(c.x - b + t * 2.0f, c.y + t), lit);
        if (arms & 8) draw->AddTriangleFilled(ImVec2(c.x + b - t * 0.6f, c.y), ImVec2(c.x + b - t * 2.0f, c.y - t), ImVec2(c.x + b - t * 2.0f, c.y + t), lit);
    }
    return side;
}
// A single button's glyph name, as glyph() takes them (the D-pad's arms are drawn by dpad_glyph).
std::string_view button_glyph(std::uint32_t bit) {
    switch (bit) {
    case 0x100: return "LB";
    case 0x200: return "RB";
    case 0x10000: return "LT";
    case 0x20000: return "RT";
    case 0x40: return "L3";
    case 0x80: return "R3";
    case 0x1000: return "A";
    case 0x2000: return "B";
    case 0x4000: return "X";
    case 0x8000: return "Y";
    case 0x10: return "MENU";
    case 0x20: return "VIEW";
    default: return "";
    }
}
// Buttons as the game's prompts draw them: each its glyph, "+" between, the D-pad's arms on one D-pad. Nothing drawn
// without a list; returns the width.
float buttons_glyphs(ImDrawList* draw, const Fonts& f, float side, ImVec2 at, std::uint32_t bits, ControllerStyle style, float s) {
    float x = at.x;
    bool first = true;
    const auto plus = [&] {
        if (first) return;
        x += 6 * s;
        x += plus_sign(draw, f, side, ImVec2(x, at.y), s) + 6 * s;
    };
    for (std::uint32_t bit = 0x10; bit <= 0x20000; bit <<= 1) {
        if (!(bits & bit) || button_glyph(bit).empty()) continue;
        plus();
        x += glyph(draw, f, side, ImVec2(x, at.y), button_glyph(bit), style, false);
        first = false;
    }
    if (bits & 0xf) {
        plus();
        x += dpad_glyph(draw, side, ImVec2(x, at.y), bits & 0xf, style);
    }
    return x - at.x;
}
// A row's bind (CONTROLS): its buttons and its key, as the panel's settings have them.
std::pair<std::uint32_t, std::uint32_t> bind_of_row(const Settings& v, const std::string& key) {
    if (key == "layer") return {v.pad_layer, 0};
    if (key.starts_with("preset:")) {
        for (const auto& p : v.preset_binds)
            if (p.preset == key.substr(7)) return {p.bind.pad, p.bind.key};
        return {0, 0};
    }
    for (std::size_t a = 0; a < action_count; ++a)
        if (action_names[a] == key) return {v.binds[a].pad, v.binds[a].key};
    return {0, 0};
}
// A prompt as the game's: its button, then what it does, white with a soft shadow. Returns the width.
float prompt(ImDrawList* draw, const Fonts& f, float s, ImVec2 at, std::string_view button, std::string_view what, ControllerStyle style, bool keyboard) {
    const float side = 36 * s, size = 26 * s, gap = 11 * s;
    const float w = glyph(draw, f, side, at, button, style, keyboard);
    if (draw) shadowed(draw, f.label, size, ImVec2(at.x + w + gap, at.y + (side - size) * 0.5f - 1 * s), look::white, what, s);
    return w + gap + width_of(f.label, size, what);
}
// The chevrons either side of the value in focus, as the game's: a thick white "<" (way -1) or ">" (way 1).
void chevron(ImDrawList* draw, ImVec2 at, int way, float s) {
    const float w = 7 * s * static_cast<float>(way), h = 10 * s;
    const ImVec2 points[3]{ImVec2(at.x - w, at.y - h), ImVec2(at.x + w, at.y), ImVec2(at.x - w, at.y + h)};
    draw->AddPolyline(points, 3, look::white, ImDrawFlags_None, 4.5f * s);
}
// Halftone dots in a tile's corner, as on the game's tile in use.
void corner_dots(ImDrawList* draw, ImVec2 a, ImVec2 b, float s) {
    const float step = 4.6f * s;
    int row = 0;
    for (float y = a.y + step; y < b.y - step * .4f; y += step, ++row)
        for (float x = a.x + step + static_cast<float>(row % 2) * step * .5f; x < b.x - step * .4f; x += step) {
            const float t = (x - a.x) / (b.x - a.x) + (y - a.y) / (b.y - a.y) - 1.15f; // past the diagonal, towards the corner
            if (t <= 0) continue;
            draw->AddCircleFilled(ImVec2(x, y), s * (0.35f + 1.6f * t * t), IM_COL32(70, 70, 70, 200), 6);
        }
}
// A tab's icon in its tile: the game's own, else its name's first letter.
void tab_icon(ImDrawList* draw, const Fonts& f, std::size_t tab, ImVec2 a, ImVec2 b, ImU32 tint) {
    // (CONTROLS has none of the game's own: its D-pad, as its button prompts draw it.)
    const auto picture = tab == static_cast<std::size_t>(Tab::controls) ? P::xbox_dpad : static_cast<P>(static_cast<std::size_t>(P::tab_shake) + tab);
    const float side = (b.x - a.x) * 0.6f;
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    if (native::loaded(picture)) {
        const float aspect = native::aspect(picture);
        const float w = aspect >= 1 ? side : side * aspect, h = aspect >= 1 ? side / aspect : side;
        native::draw(draw, picture, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), tint);
        return;
    }
    const std::string letter(1, tab_names[tab].empty() ? '?' : tab_names[tab][0]);
    const float size = side * 0.9f;
    text_at(draw, f.value, size, ImVec2(c.x - width_of(f.value, size, letter) * 0.5f, c.y - size * 0.55f), tint, letter);
}

void draw(Panel& p, const link::View& view, const std::vector<Row>& rows, ControllerStyle style, bool keyboard) {
    const Fonts f = fonts();
    p.hits.clear();
    const auto display = ImGui::GetIO().DisplaySize;
    const float s = native::ui_scale(display.x, display.y);
    auto* dl = ImGui::GetForegroundDrawList();
    // On the left, as the game keeps its own lists on the right (Advanced Edit has its tabs in the top middle and its
    // panels on the right; the first screen its title in the top left): a screen's title, the tabs, the rows.
    const float x0 = std::floor(100 * s), x1 = std::floor(x0 + 760 * s);
    float y = std::floor(40 * s);
    // The title, as the game's replay screens have theirs ("INSTANT REPLAY"), and the preset loaded beside it (never
    // as far as the game's own tabs in the top middle). On the replay editor's first screen the game's own title is
    // there already (as tall, a little lower): the panel goes under it instead, its name on the line with the preset.
    const float title_size = 72 * s, sub_size = 22 * s;
    const bool game_title = view.in_replay && !view.advanced_edit && depth::ui_shown() >= 0.5f;
    const float sub_y = game_title ? std::floor(170 * s) : y + title_size + 4 * s;
    if (!game_title) shadowed(dl, f.title, title_size, ImVec2(x0, y), look::white, "REPLAY PLUS", s);
    // Under it, what it is showing: the preset loaded (or, outside a replay, where it shows).
    if (!view.in_replay) {
        shadowed(dl, f.value, sub_size, ImVec2(x0 + 4 * s, sub_y), skate_theme::warning, "OPEN A REPLAY: THESE SHOW IN THE REPLAY EDITOR", s);
    } else {
        float sx = x0 + 4 * s;
        if (game_title) {
            shadowed(dl, f.value, sub_size, ImVec2(sx, sub_y), look::white, "REPLAY PLUS", s);
            sx += width_of(f.value, sub_size, "REPLAY PLUS") + 16 * s;
        }
        if (!p.local.preset.empty()) {
            dl->AddRectFilled(ImVec2(sx, sub_y + 3 * s), ImVec2(sx + 5 * s, sub_y + sub_size - 1 * s), look::blue);
            shadowed(dl, f.value, sub_size, ImVec2(sx + 14 * s, sub_y), look::white, p.local.preset, s);
        }
    }
    y = std::floor(sub_y + sub_size + 18 * s);
    // The tabs, as Advanced Edit's: LB and RB either side of a row of tiles, black with a white icon; the one in use
    // larger and white, its icon blue, halftone in its corner, and its name under it, blue on black.
    const float glyph_side = 28 * s, tile = 60 * s, big = 70 * s;
    const char* back_button = keyboard ? "Q" : "LB";
    const char* next_button = keyboard ? "E" : "RB";
    const float lb_w = glyph(nullptr, f, glyph_side, {}, back_button, style, keyboard);
    const float rb_w = glyph(nullptr, f, glyph_side, {}, next_button, style, keyboard);
    const float bar_mid = y + big * 0.5f;
    glyph(dl, f, glyph_side, ImVec2(x0, std::floor(bar_mid - glyph_side * 0.5f)), back_button, style, keyboard);
    glyph(dl, f, glyph_side, ImVec2(x1 - rb_w, std::floor(bar_mid - glyph_side * 0.5f)), next_button, style, keyboard);
    p.hits.push_back({HitKind::tab_back, ImVec2(x0, y), ImVec2(x0 + lb_w, y + big)});
    p.hits.push_back({HitKind::tab_next, ImVec2(x1 - rb_w, y), ImVec2(x1, y + big)});
    const float tabs_left = x0 + lb_w + 14 * s, tabs_right = x1 - rb_w - 14 * s;
    const std::size_t count = tab_names.size();
    const float each = (tabs_right - tabs_left) / static_cast<float>(count);
    ImVec2 chosen_a{}, chosen_b{};
    for (std::size_t t = 0; t < count; ++t) {
        const float cx = tabs_left + each * (static_cast<float>(t) + 0.5f);
        const bool on = static_cast<int>(t) == p.tab;
        const float side = on ? big : std::min(tile, each - 6 * s);
        const ImVec2 ta(std::floor(cx - side * 0.5f), std::floor(bar_mid - side * 0.5f)), tb(ta.x + side, ta.y + side);
        p.hits.push_back({HitKind::tab, ImVec2(cx - each * 0.5f, y), ImVec2(cx + each * 0.5f, y + big), static_cast<int>(t)});
        if (on) {
            chosen_a = ta;
            chosen_b = tb;
            continue; // drawn last, over its neighbours
        }
        skate_theme::rough_rect(dl, ta, tb, IM_COL32(26, 26, 26, 242), 0x5100 + static_cast<unsigned>(t), s);
        tab_icon(dl, f, t, ta, tb, look::white);
    }
    {
        const auto t = static_cast<std::size_t>(std::clamp(p.tab, 0, static_cast<int>(count) - 1));
        const float edge = 4 * s;
        dl->AddRectFilled(chosen_a, chosen_b, IM_COL32(38, 38, 38, 255));
        const ImVec2 ia(chosen_a.x + edge, chosen_a.y + edge), ib(chosen_b.x - edge, chosen_b.y - edge);
        dl->AddRectFilled(ia, ib, IM_COL32(232, 232, 230, 255));
        corner_dots(dl, ia, ib, s);
        tab_icon(dl, f, t, chosen_a, chosen_b, look::blue);
        const std::string name = tab_title(t);
        const float size = 26 * s, pad = 14 * s, h = 38 * s;
        const float w = width_of(f.label, size, name) + pad * 2;
        ImVec2 la(std::floor((chosen_a.x + chosen_b.x) * 0.5f - w * 0.5f), std::floor(chosen_b.y + 6 * s));
        la.x = std::clamp(la.x, x0, x1 - w);
        dl->AddRectFilled(la, ImVec2(la.x + w, la.y + h), IM_COL32(0, 0, 0, 96));
        text_at(dl, f.label, size, ImVec2(la.x + pad, la.y + (h - size) * 0.5f), look::blue, name);
        y = la.y + h + 16 * s;
    }
    // What the row in focus does (or what the last change said), under the rows as the game's settings describe theirs;
    // then the prompts. The rows end above them and above the game's timeline.
    auto& focus = p.focus[static_cast<std::size_t>(p.tab)];
    auto& scroll = p.scroll[static_cast<std::size_t>(p.tab)];
    const std::string hint = focus >= 0 && static_cast<std::size_t>(focus) < rows.size() ? rows[static_cast<std::size_t>(focus)].hint : std::string();
    const bool fresh = seconds_now() - p.feedback_at < 5;
    const std::string under = fresh ? p.feedback : hint;
    const float hint_size = 22 * s, hint_pad = 14 * s, hint_w = x1 - x0 - hint_pad * 2;
    const float hint_line = line_size(f.label, hint_size);
    const float hint_h = under.empty() ? 0.0f : f.label->CalcTextSizeA(hint_line, FLT_MAX, hint_w, under.c_str()).y + hint_pad * 2;
    const float prompts_h = 40 * s;
    const auto bar = depth::timeline_bar();
    const float floor_y = (bar.found ? bar.y * display.y : display.y - 215 * s) - 64 * s;
    const float row_h = std::floor(74 * s), label_size = 26 * s, value_size = 27 * s;
    const float below = (hint_h > 0 ? hint_h + 10 * s : 0.0f) + 14 * s + prompts_h;
    const float most = std::max(row_h * 3, floor_y - below - y);
    const float list_h = std::min(most, static_cast<float>(std::max<std::size_t>(rows.size(), 1)) * row_h);
    const float list_top = y, list_bottom = y + list_h;
    dl->AddRectFilled(ImVec2(x0, list_top), ImVec2(x1, list_bottom), look::plate);
    const float inner_h = list_h;
    const float focus_top = static_cast<float>(focus) * row_h;
    if (p.follow_focus) {
        if (focus_top - scroll < 0) scroll = focus_top;
        if (focus_top + row_h - scroll > inner_h) scroll = focus_top + row_h - inner_h;
    }
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, static_cast<float>(rows.size()) * row_h - inner_h));
    p.list_a = ImVec2(x0, list_top);
    p.list_b = ImVec2(x1, list_bottom);
    p.row_h = row_h;
    p.inner_h = inner_h;
    p.content_h = static_cast<float>(rows.size()) * row_h;
    const float label_x = x0 + 44 * s, value_mid = x0 + 560 * s, bar_w = 260 * s;
    // CONTROLS' two columns: the controller's and the keyboard's, under their titles.
    const float pad_mid = value_mid - 72 * s, key_mid = value_mid + 108 * s, pad_half = 82 * s, key_half = 62 * s;
    const float label_w = value_mid - bar_w * 0.5f - 48 * s - label_x;
    const float bx0 = value_mid - bar_w * .5f, bx1 = value_mid + bar_w * .5f;
    dl->PushClipRect(ImVec2(x0, list_top), ImVec2(x1, list_bottom), true);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const float top = list_top + static_cast<float>(i) * row_h - scroll;
        if (top > list_bottom || top + row_h < list_top) continue;
        // (Its parts only where the list shows them.)
        const float ry0 = std::max(top, list_top), ry1 = std::min(top + row_h, list_bottom);
        const auto part = [&](HitKind kind, ImVec2 a, ImVec2 b, int index = 0, float from = 0, float to = 0) {
            a.y = std::max(a.y, ry0);
            b.y = std::min(b.y, ry1);
            if (b.y > a.y) p.hits.push_back({kind, a, b, index, static_cast<int>(i), from, to});
        };
        part(HitKind::row, ImVec2(x0, top), ImVec2(x1, top + row_h), static_cast<int>(i));
        if (row.kind == RowKind::bind && row.key == "titles") {
            const float size = 19 * s, ty = top + row_h - size - 12 * s;
            for (const auto& [mid, title] : {std::pair{pad_mid, std::string_view("CONTROLLER")}, std::pair{key_mid, std::string_view("KEYBOARD")}})
                text_at(dl, f.value, size, ImVec2(std::floor(mid - width_of(f.value, size, title) * .5f), ty), look::soft, title);
            continue;
        }
        const bool lit = static_cast<int>(i) == focus;
        const bool info = row.kind == RowKind::action && row.key == "none";
        if (lit) {
            // The focus as the game's lists show it: the row a shade darker, a blue edge on its left.
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + row_h), look::focus);
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + 15 * s, top + row_h), look::blue);
        }
        const ImU32 ink = row.locked ? look::dim : info ? look::soft : look::white;
        // Its label (in the bold face in focus, as the game's), on two lines when it is long.
        ImFont* label_face = lit && !row.locked ? f.value : f.label;
        const auto [first, second] = label_lines(label_face, label_size, row.label, label_w);
        const float line = std::floor(label_size * 1.08f);
        if (second.empty()) {
            text_at(dl, label_face, label_size, ImVec2(label_x, top + (row_h - label_size) * .5f - 1 * s), ink, first, label_x + label_w + 40 * s);
        } else {
            const float ty = top + (row_h - line * 2) * .5f;
            text_at(dl, label_face, label_size, ImVec2(label_x, ty), ink, first, label_x + label_w + 40 * s);
            text_at(dl, label_face, label_size, ImVec2(label_x, ty + line), ink, second, label_x + label_w + 40 * s);
        }
        if (row.locked) {
            // Scribbled out, as the game's own rows that do not apply.
            const float lw = std::min(label_w + 20 * s, std::max(width_of(label_face, label_size, first), width_of(label_face, label_size, second)) + 24 * s);
            skate_theme::scribble(dl, ImVec2(label_x - 10 * s, top + 12 * s), ImVec2(label_x + lw, top + row_h - 12 * s), 0x900 + static_cast<unsigned>(i), s * 1.2f);
        }
        if (row.kind == RowKind::bind) {
            // Two cells: the controller's (the shortcut button, "+", its own) and the keyboard's key, as the game's prompts draw
            // them; the one picked outlined on the row in focus; the one waited on saying what to press.
            const float side = 32 * s;
            const float gy = std::floor(top + (row_h - side) * .5f);
            const auto [bits, key] = bind_of_row(p.local, row.key);
            const bool layer_row = row.key == "layer";
            const int column = layer_row ? 0 : p.bind_column;
            const bool waiting = p.binding && p.bind_for == row.key;
            part(HitKind::segment, ImVec2(pad_mid - pad_half, top + 6 * s), ImVec2(pad_mid + pad_half, top + row_h - 6 * s), 0);
            if (!layer_row) part(HitKind::segment, ImVec2(key_mid - key_half, top + 6 * s), ImVec2(key_mid + key_half, top + row_h - 6 * s), 1);
            if (lit || waiting) {
                const float mid = column ? key_mid : pad_mid, half = column ? key_half : pad_half;
                dl->AddRect(ImVec2(mid - half, top + 9 * s), ImVec2(mid + half, top + row_h - 9 * s), waiting ? look::gold : look::blue, 6 * s, 0, 2.5f * s);
            }
            const auto centred_text = [&](float mid, std::string_view text, ImU32 colour, float size) {
                text_at(dl, f.value, size, ImVec2(std::floor(mid - width_of(f.value, size, text) * .5f), top + (row_h - size) * .5f - 1 * s), colour, text);
            };
            // The controller's.
            if (waiting && p.bind_device == 0) {
                centred_text(pad_mid, "Press a button", look::gold, 20 * s);
            } else if (bits) {
                const auto pad_width = [&](ImDrawList* to, ImVec2 at) {
                    float w = buttons_glyphs(to, f, side, at, p.local.pad_layer, style, s);
                    if (layer_row) return w;
                    w += 5 * s;
                    w += plus_sign(to, f, side, ImVec2(at.x + w, at.y), s) + 5 * s;
                    return w + buttons_glyphs(to, f, side, ImVec2(at.x + w, at.y), bits, style, s);
                };
                pad_width(dl, ImVec2(std::floor(pad_mid - pad_width(nullptr, {}) * .5f), gy));
            } else {
                centred_text(pad_mid, "-", look::soft, value_size);
            }
            // The keyboard's (the shortcut button has none).
            if (layer_row) continue;
            if (waiting && p.bind_device == 1) {
                centred_text(key_mid, "Press a key", look::gold, 20 * s);
            } else if (key) {
                const std::string key_text = key_name(key);
                const float wk = glyph(nullptr, f, side, {}, key_text, style, true);
                glyph(dl, f, side, ImVec2(std::floor(key_mid - wk * .5f), gy), key_text, style, true);
            } else {
                centred_text(key_mid, "-", look::soft, value_size);
            }
            continue;
        }
        if (row.kind == RowKind::naming) {
            // The name, the cursor in it, on a light field.
            const float size = 26 * s, left = bx0 - 40 * s, right = x1 - 28 * s;
            dl->AddRectFilled(ImVec2(left, top + 15 * s), ImVec2(right, top + row_h - 15 * s), IM_COL32(240, 240, 236, 245));
            text_at(dl, f.value, size, ImVec2(left + 14 * s, top + (row_h - size) * .5f), look::ink, row.value, right - 14 * s);
            continue;
        }
        if (row.kind == RowKind::preset && lit) {
            // What A does to it, left / right choosing, as a value is chosen: "< Load >", with A's glyph after it.
            static constexpr std::array<const char*, 4> what{"Load", "Rename", "Export", "Delete"};
            const int chosen = p.preset_action_for == row.key ? std::clamp(p.preset_action, 0, 3) : 0;
            const bool confirming = p.confirm == row.key;
            const std::string label = confirming ? std::string("Delete: again") : std::string(what[static_cast<std::size_t>(chosen)]);
            const float ty = top + (row_h - value_size) * .5f - 1 * s;
            const float tw = width_of(f.value, value_size, label);
            text_at(dl, f.value, value_size, ImVec2(value_mid - tw * .5f, ty), confirming || chosen == 3 ? look::danger : look::white, label);
            chevron(dl, ImVec2(bx0 - 22 * s, top + row_h * .5f), -1, s);
            chevron(dl, ImVec2(bx1 + 22 * s, top + row_h * .5f), 1, s);
            const float side = 30 * s;
            glyph(dl, f, side, ImVec2(bx1 + 44 * s, top + (row_h - side) * .5f), keyboard ? "Enter" : "A", style, keyboard);
            part(HitKind::left, ImVec2(bx0 - 50 * s, top), ImVec2(bx0, top + row_h));
            part(HitKind::right, ImVec2(bx1, top), ImVec2(bx1 + 40 * s, top + row_h));
            part(HitKind::plate, ImVec2(bx0, top), ImVec2(bx1, top + row_h));
            continue;
        }
        if (row.kind == RowKind::preset) {
            if (row.value == "Loaded") {
                const float size = 20 * s, h = 32 * s, pad = 12 * s, w = width_of(f.value, size, "LOADED") + pad * 2;
                const ImVec2 at(std::floor(value_mid - w * .5f), std::floor(top + (row_h - h) * .5f));
                dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), look::blue);
                text_at(dl, f.value, size, ImVec2(at.x + pad, at.y + (h - size) * .5f), look::white, "LOADED");
            }
            continue;
        }
        if (row.kind == RowKind::action && !info) {
            // An action: in focus, what A does as the game's prompts show it (its glyph, its name); else its name, dimmer.
            const float side = 32 * s, size = 25 * s;
            const char* button = keyboard ? "Enter" : "A";
            // In the game's prompts' case ("Add Keyframe"): SAVE as Save.
            std::string what = row.value;
            for (std::size_t c = 1; c < what.size(); ++c)
                if (std::isalpha(static_cast<unsigned char>(what[c])) && std::isalpha(static_cast<unsigned char>(what[c - 1])))
                    what[c] = static_cast<char>(std::tolower(static_cast<unsigned char>(what[c])));
            const float gw = glyph(nullptr, f, side, {}, button, style, keyboard), tw = width_of(f.value, size, what);
            const float w = gw + 12 * s + tw, ax = std::floor(value_mid - w * .5f), ay = std::floor(top + (row_h - side) * .5f);
            if (!row.locked) part(HitKind::plate, ImVec2(ax - 12 * s, top + 6 * s), ImVec2(ax + w + 12 * s, top + row_h - 6 * s));
            if (lit && !row.locked) {
                glyph(dl, f, side, ImVec2(ax, ay), button, style, keyboard);
                text_at(dl, f.value, size, ImVec2(ax + gw + 12 * s, ay + (side - size) * .5f - 1 * s), look::white, what);
            } else {
                text_at(dl, f.value, size, ImVec2(value_mid - tw * .5f, top + (row_h - size) * .5f - 1 * s), row.locked ? look::dim : look::soft, what);
            }
            continue;
        }
        // The value, and under it its slider or its marks; "< value >" on the row in focus.
        const std::string& value = row.value;
        const bool marked = row.fraction >= 0 || row.segments > 0;
        const float value_y = marked ? top + 11 * s : top + (row_h - value_size) * .5f - 1 * s;
        const float vw = width_of(f.value, value_size, value);
        if (row.kind == RowKind::video_folder) {
            // A folder: from its start, as much as fits.
            text_at(dl, f.value, 22 * s, ImVec2(std::max(bx0 - 40 * s, std::min(value_mid - vw * .5f, x1 - 24 * s - vw)), top + (row_h - 22 * s) * .5f), ink, value,
                    x1 - 20 * s);
            continue;
        }
        text_at(dl, f.value, value_size, ImVec2(value_mid - vw * .5f, value_y), row.locked ? look::dim : look::white, value);
        const bool workable = !row.locked && !info;
        if (lit && workable) {
            const float cy = marked ? value_y + value_size * .55f : top + row_h * .5f;
            chevron(dl, ImVec2(bx0 - 22 * s, cy), -1, s);
            chevron(dl, ImVec2(bx1 + 22 * s, cy), 1, s);
        }
        const float bar_y = top + row_h - 19 * s;
        if (workable) {
            // Either side of the value: a step down or up (held, again and again); its slider, or its marks.
            part(HitKind::left, ImVec2(bx0 - 56 * s, top), ImVec2(bx0 - 4 * s, top + row_h));
            part(HitKind::right, ImVec2(bx1 + 4 * s, top), ImVec2(bx1 + 56 * s, top + row_h));
            if (row.fraction >= 0) part(HitKind::slider, ImVec2(bx0 - 8 * s, bar_y - 22 * s), ImVec2(bx1 + 8 * s, bar_y + 16 * s), 0, bx0, bx1);
            else if (row.segments > 0) {
                const float gap = 6 * s, w = (bar_w - gap * static_cast<float>(row.segments - 1)) / static_cast<float>(row.segments);
                for (int k = 0; k < row.segments; ++k) {
                    const float sx = bx0 + static_cast<float>(k) * (w + gap);
                    part(HitKind::segment, ImVec2(sx - gap * .5f, bar_y - 22 * s), ImVec2(sx + w + gap * .5f, bar_y + 16 * s), k);
                }
            }
        }
        const ImU32 fill = row.locked ? IM_COL32(120, 130, 150, 140) : look::blue;
        if (row.fraction >= 0) {
            // The game's slider: a faint line, blue as far as the value, a white knob there.
            const float at = bx0 + bar_w * std::clamp(row.fraction, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(bx0, bar_y - 2 * s), ImVec2(bx1, bar_y + 2 * s), look::track);
            dl->AddRectFilled(ImVec2(bx0, bar_y - 4 * s), ImVec2(at, bar_y + 4 * s), fill);
            dl->AddCircleFilled(ImVec2(at, bar_y), 11 * s, row.locked ? IM_COL32(190, 190, 190, 150) : look::white);
        } else if (row.segments > 0) {
            // A choice: a mark for each, the one chosen blue.
            const float gap = 6 * s, w = (bar_w - gap * static_cast<float>(row.segments - 1)) / static_cast<float>(row.segments);
            for (int k = 0; k < row.segments; ++k) {
                const float sx = bx0 + static_cast<float>(k) * (w + gap);
                if (k == row.segment) dl->AddRectFilled(ImVec2(sx, bar_y - 4 * s), ImVec2(sx + w, bar_y + 4 * s), fill);
                else dl->AddRectFilled(ImVec2(sx, bar_y - 2 * s), ImVec2(sx + w, bar_y + 2 * s), look::track);
            }
        }
    }
    dl->PopClipRect();
    // A scroll bar beside the rows, as the game's longer lists have, while there is more than shows.
    const float content = static_cast<float>(rows.size()) * row_h;
    if (content > inner_h + 1) {
        const float track_top = list_top + 6 * s, track_h = inner_h - 12 * s;
        const float thumb_h = std::max(50 * s, track_h * inner_h / content), thumb_y = track_top + (track_h - thumb_h) * (scroll / (content - inner_h));
        p.hits.push_back({HitKind::scroll, ImVec2(x1 + 2 * s, track_top), ImVec2(x1 + 26 * s, track_top + track_h), 0, -1, track_top, track_top + track_h});
        p.track_top = track_top;
        p.track_h = track_h;
        p.thumb_h = thumb_h;
        dl->AddRectFilled(ImVec2(x1 + 11 * s, track_top), ImVec2(x1 + 14 * s, track_top + track_h), IM_COL32(255, 255, 255, 60));
        dl->AddRectFilled(ImVec2(x1 + 9 * s, thumb_y), ImVec2(x1 + 16 * s, thumb_y + thumb_h), IM_COL32(255, 255, 255, 220));
    }
    float yy = list_bottom + 10 * s;
    if (!under.empty()) {
        dl->AddRectFilled(ImVec2(x0, yy), ImVec2(x1, yy + hint_h), IM_COL32(0, 0, 0, 118));
        if (fresh) dl->AddRectFilled(ImVec2(x0, yy), ImVec2(x0 + 6 * s, yy + hint_h), look::blue);
        dl->AddText(f.label, hint_line, ImVec2(x0 + hint_pad, yy + hint_pad), fresh ? look::white : IM_COL32(236, 236, 236, 255), under.c_str(), nullptr, hint_w);
        yy += hint_h + 10 * s;
    }
    // The prompts under it all, as the game puts the ones of its lists under them.
    std::vector<std::pair<std::string, std::string>> controls;
    const bool presets_tab = p.tab == static_cast<int>(Tab::presets);
    if (p.binding) controls = {{keyboard ? "Esc" : "B", "Cancel"}};
    else if (p.naming && keyboard) controls = {{"Enter", "Keep"}, {"Bksp", "Delete"}, {"Esc", "Back"}};
    else if (p.naming) controls = {{"A", "Keep"}, {"X", "Delete"}, {"B", "Back"}};
    else if (keyboard) controls = {{"Enter", "Select"}, {"R", presets_tab ? "Delete" : "Reset To Default"}, {"Bksp", "Back"}};
    else controls = {{"A", "Select"}, {"X", presets_tab ? "Delete" : "Reset To Default"}, {"Y", "Hold To Hide"}, {"B", "Back"}};
    float x = x0;
    const float fy = std::floor(yy + 4 * s);
    for (const auto& [button, what] : controls) {
        const float w = prompt(dl, f, s, ImVec2(x, fy), button, what, style, keyboard);
        // Each prompt is its button for the mouse too.
        const int does = what == "Select" || what == "Keep" ? 0 : what == "Back" ? 2 : what == "Hold To Hide" ? -1 : 1;
        if (does >= 0) p.hits.push_back({HitKind::prompt, ImVec2(x - 6 * s, fy - 6 * s), ImVec2(x + w + 6 * s, fy + 46 * s), does});
        x += w + 28 * s;
    }
}

// The way in, as the game shows its own: "R3 + RB  Replay Plus" on a pad (R3 first: RB then stays the
// panel's, not the game's next tab), "F10  Replay Plus" on the keyboard, among the replay editor's button
// prompts at the foot of the screen, and only while the game's UI shows there (replay_plus_depth.h ui_shown):
// it goes as the game hides its UI (L3 / H), or fades it after a while without input.
void draw_hint(const link::View& view) {
    // In Advanced Edit only, while the game's prompts show.
    if (!view.in_replay || !view.advanced_edit || depth::ui_shown() < 0.5f) return;
    ControllerInput input;
    read_pad(input);
    const bool keyboard = !input.available;
    const Fonts f = fonts();
    const auto display = ImGui::GetIO().DisplaySize;
    const float s = native::ui_scale(display.x, display.y);
    auto* dl = ImGui::GetForegroundDrawList();
    // As the game's own prompts there: their glyphs 38 tall, their words 27 (measured beside them, 2026-11-19).
    const float side = 38 * s, size = 27 * s, gap = 8 * s;
    constexpr std::string_view what = "Replay Plus";
    // The menu's own button and key (CONTROLS): F10, and R3 + RB, as they came.
    const auto& menu = view.settings.binds[static_cast<std::size_t>(Action::panel)];
    if (keyboard && !menu.key) return;
    const std::string menu_key = key_name(menu.key);
    const auto pad_part = [&](ImDrawList* to, ImVec2 at) {
        float x = buttons_glyphs(to, f, side, at, view.settings.pad_layer, input.style, s) + gap;
        x += plus_sign(to, f, side, ImVec2(at.x + x, at.y), s) + gap;
        return x + buttons_glyphs(to, f, side, ImVec2(at.x + x, at.y), menu.pad, input.style, s);
    };
    float w = keyboard ? glyph(nullptr, f, side, {}, menu_key, input.style, true) : pad_part(nullptr, {});
    w += 12 * s + width_of(f.label, size, what);
    // In the widest stretch of the prompts' row with none of the game's prompts in it (each tab has its
    // own: Trim's leaves the middle free, Keyframe's the left of the middle), with room either side.
    const auto free = depth::ui_gap();
    const float from = free.from * display.x, to = free.to * display.x, room = 40 * s;
    if (to - from < w + room * 2) return;
    float x = std::floor((from + to) * 0.5f - w * 0.5f);
    const float y = std::floor(display.y * 0.924f - side * 0.5f);
    if (keyboard) x += glyph(dl, f, side, ImVec2(x, y), menu_key, input.style, true);
    else x += pad_part(dl, ImVec2(x, y));
    shadowed(dl, f.label, size, ImVec2(x + 12 * s, y + (side - size) * .5f - 1 * s), look::white, what, s);
}

constexpr ImU32 focus_mark = IM_COL32(64, 196, 255, 255), shake_mark = IM_COL32(255, 150, 60, 255), light_mark = IM_COL32(227, 204, 114, 255);
// The game's timeline runs from 10.16% to 90.04% of the width, the replay's start to its end (its own
// one-second ticks sit there, at 16:9 and at 4:3 alike); its bar's height is read from the UI
// (replay_plus_depth.h timeline_bar), else where it is on a 16:9 screen.
constexpr float timeline_from = 0.10156f, timeline_span = 0.79883f, timeline_y_wide = 0.8208f;

// Replay Plus's keys on the game's timeline, as the game marks its own camera keyframes: focus keys
// blue above the bar's middle, shake keys orange below it (filled where the shake comes on, hollow where
// it goes off). Only while the game's UI shows (they go with it, L3 / H).
void draw_marks(const link::View& view) {
    if (!view.in_replay || !view.timeline_ok || view.timeline_length <= 0 || depth::ui_shown() < 0.5f) return;
    if (view.settings.focus_keys.empty() && view.settings.shake_keys.empty() && view.settings.light_keys.empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    // Only where the bar shows now (not under one of the game's dialogs).
    const auto bar = depth::timeline_bar();
    if (!bar.found) return;
    const float y = std::floor(bar.y * display.y);
    // The bar is 30 pixels tall at 1440 lines (at 16:9; the UI scales with the width below that).
    const float u = std::min(display.x / 2560.0f, display.y / 1440.0f);
    const float x0 = display.x * timeline_from, span = display.x * timeline_span, r = 9 * u;
    auto* dl = ImGui::GetForegroundDrawList();
    // (Through the replay's chunks: a key on footage the replay no longer has is not drawn.)
    const auto x_of = [&](double time) {
        const float share = link::timeline_share(view.chunks, view.timeline_start, view.timeline_length, time, view.bar_from, view.bar_to);
        return share < 0 ? -1.0e6f : x0 + share * span;
    };
    const auto diamond = [&](float x, float cy, ImU32 fill, ImU32 edge) {
        const ImVec2 p[4]{ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
        dl->AddConvexPolyFilled(p, 4, fill);
        dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, 1.5f * u);
    };
    const bool keyed = view.settings.focus == Focus::keys;
    for (const auto& key : view.settings.focus_keys) {
        const float x = std::floor(x_of(key.time)) + .5f;
        if (x < x0 - r || x > x0 + span + r) continue;
        diamond(x, y - 9 * u, keyed ? focus_mark : IM_COL32(64, 196, 255, 110), IM_COL32(10, 10, 12, 230));
    }
    for (const auto& key : view.settings.shake_keys) {
        const float x = std::floor(x_of(key.time)) + .5f;
        if (x < x0 - r || x > x0 + span + r) continue;
        diamond(x, y + 9 * u, key.on ? shake_mark : IM_COL32(24, 24, 26, 235), key.on ? IM_COL32(10, 10, 12, 230) : shake_mark);
    }
    // The light's keys above the focus's, gold (the game's CONFIRM's): filled where it comes on, hollow where it goes off;
    // faint while Light keys is off.
    const bool light_keyed = view.settings.light && view.settings.light_keyed;
    for (const auto& key : view.settings.light_keys) {
        const float x = std::floor(x_of(key.time)) + .5f;
        if (x < x0 - r || x > x0 + span + r) continue;
        const ImU32 gold = light_keyed ? light_mark : IM_COL32(227, 204, 114, 110);
        diamond(x, y - 27 * u, key.on ? gold : IM_COL32(24, 24, 26, 235), key.on ? IM_COL32(10, 10, 12, 230) : gold);
    }
}

// R3 held in the replay editor: what each button does with it, as the game's own Controls Helper lists its
// controls (a heading, then each control on a plate of its own), over the left of the picture above the
// timeline; and what the last one did, for a moment after, on the game's gold plate.
void draw_guide(const link::View& view) {
    if (!view.in_replay) return;
    const double since = static_cast<double>(link::now_ms() - view.toast_at) / 1000.0;
    const bool toast = !view.toast.empty() && view.toast_at && since < 2.5;
    if (!view.r3_held && !toast) return;
    ControllerInput input;
    read_pad(input);
    const Fonts f = fonts();
    const auto display = ImGui::GetIO().DisplaySize;
    const float s = native::ui_scale(display.x, display.y);
    auto* dl = ImGui::GetForegroundDrawList();
    const auto bar = depth::timeline_bar();
    const float bottom = std::floor((bar.found ? bar.y : timeline_y_wide) * display.y - 50 * s);
    const float x = std::floor(display.x * timeline_from);
    // As the game's helper: glyphs 30 tall on plates 50 tall, 62 apart, its words 24.
    const float side = 30 * s, size = 24 * s, row = 62 * s, pad = 10 * s, gap = 10 * s;
    // Each line: its buttons (two for a pair: further / nearer, less / more; a D-pad pair on one D-pad), what it does.
    struct Line {
        std::uint32_t bits{}, pair{};
        std::string what;
    };
    std::vector<Line> lines;
    if (view.r3_held) {
        const auto& v = view.settings;
        const auto pad_of = [&](Action a) { return v.binds[static_cast<std::size_t>(a)].pad; };
        const bool keyed = v.focus == Focus::keys;
        const auto add = [&](std::uint32_t bits, std::uint32_t pair, std::string what) {
            if (bits || pair) lines.push_back({bits, pair, std::move(what)});
        };
        add(pad_of(Action::panel), 0, "Open Panel");
        add(pad_of(Action::focus_further), pad_of(Action::focus_nearer),
            std::format("Focus {:.2f} m{}", view.focus_distance_now, keyed ? "" : " (Keys It)"));
        add(pad_of(Action::blur_less), pad_of(Action::blur_more), std::format("Blur {:.0f}%", view.focus_blur_now * 100));
        if (v.light && v.light_keyed) add(pad_of(Action::light), 0, std::format("Light Key: {} From Here", view.light_on_here ? "Off" : "On"));
        else add(pad_of(Action::light), 0, std::format("Camera Light {}", v.light ? "Off" : "On"));
        add(pad_of(Action::shake), 0, std::format("Shake {}", view.shake.on ? "Off" : "On"));
        add(pad_of(Action::shake_key), 0, std::format("Shake Key: {} From Here", view.shake_here ? "Off" : "On"));
        add(pad_of(Action::delete_keys), 0, "Delete Keys Here");
        for (const auto& p : v.preset_binds)
            if (p.bind.pad) add(p.bind.pad, 0, "Preset: " + p.preset);
    }
    // Its buttons drawn (or measured): a pair of D-pad arms on one D-pad, else each its own.
    const auto line_glyphs = [&](ImDrawList* to, ImVec2 at, const Line& line) {
        if (line.pair && ((line.bits | line.pair) & ~0xfu) == 0) return dpad_glyph(to, side, at, line.bits | line.pair, input.style);
        float w = line.bits ? buttons_glyphs(to, f, side, at, line.bits, input.style, s) : 0.0f;
        if (line.pair) {
            if (w > 0) w += 4 * s;
            w += buttons_glyphs(to, f, side, ImVec2(at.x + w, at.y), line.pair, input.style, s);
        }
        return w;
    };
    const float head_h = view.r3_held ? row : 0.0f, toast_h = toast ? row : 0.0f;
    const float h = head_h + static_cast<float>(lines.size()) * row + toast_h;
    float y = bottom - h;
    if (view.r3_held) {
        // The heading: the shortcut button (R3), "Replay Plus", in the bold face, as "Controls Helper" heads the game's.
        const float hs = 32 * s, g = buttons_glyphs(dl, f, hs, ImVec2(x, y), view.settings.pad_layer, input.style, s);
        shadowed(dl, f.value, 23 * s, ImVec2(x + g + gap, y + (hs - 23 * s) * .5f - 1 * s), look::white, "Replay Plus", s);
        y += row;
    }
    for (const auto& line : lines) {
        const float gw = line_glyphs(nullptr, {}, line);
        const float w = gw + gap + width_of(f.label, size, line.what);
        dl->AddRectFilled(ImVec2(x - pad, y - pad), ImVec2(x + w + pad * 1.4f, y + side + pad), IM_COL32(0, 0, 0, 92));
        const float gx = x + line_glyphs(dl, ImVec2(x, y), line);
        text_at(dl, f.label, size, ImVec2(gx + gap, y + (side - size) * .5f - 1 * s), look::white, line.what);
        y += row;
    }
    if (toast) {
        // What it did, on the game's gold plate (as its CONFIRM), fading at the end.
        const float fade = std::clamp(since < 2.0 ? 1.0f : static_cast<float>((2.5 - since) / 0.5), 0.0f, 1.0f);
        const std::string text = upper(view.toast);
        const float tsize = 22 * s, w = width_of(f.value, tsize, text) + 28 * s;
        const auto alpha = static_cast<int>(255 * fade);
        dl->AddRectFilled(ImVec2(x - pad, y - pad), ImVec2(x - pad + w, y + side + pad), (look::gold & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT));
        text_at(dl, f.value, tsize, ImVec2(x - pad + 14 * s, y + (side - tsize) * .5f - 1 * s), (look::ink & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT), text);
    }
}

// The keys the panel reads: Up, Down, Left, Right, Enter, Backspace (or Esc), Q / Page Up, E / Page Down,
// and R; as pad buttons, so the same code works them.
unsigned keyboard_buttons() {
    unsigned buttons{};
    const auto key = [](int vk) { return dingosdk::overlay::key_down(vk); };
    const bool shift = key(VK_SHIFT);
    if (key(VK_UP) || key('W')) buttons |= dpad_up;
    if (key(VK_DOWN) || key('S')) buttons |= dpad_down;
    if (key(VK_LEFT) || key('A') || key(VK_OEM_MINUS) || key(VK_SUBTRACT)) buttons |= dpad_left;
    if (key(VK_RIGHT) || key('D') || key(VK_OEM_PLUS) || key(VK_ADD)) buttons |= dpad_right;
    if (key(VK_RETURN) || key(VK_SPACE)) buttons |= pad_a;
    if (key(VK_BACK) || key(VK_ESCAPE)) buttons |= pad_b;
    if (key('R') || key(VK_DELETE)) buttons |= pad_x;
    if (key('Q') || key(VK_PRIOR) || (key(VK_TAB) && shift)) buttons |= lb;
    if (key('E') || key(VK_NEXT) || (key(VK_TAB) && !shift)) buttons |= rb;
    return buttons;
}
// The mouse, while the panel has the pointer (last frame's drawing tells what is under it): the row under it lights
// up as it moves, a click works whatever it is on, a slider or the scroll bar drags, an arrow held repeats, the wheel
// scrolls the rows. What stands for a button (the prompts at the foot, the tab arrows) comes back as those buttons.
unsigned mouse_input(Panel& p, const link::View& view, std::vector<Row>& rows) {
    auto& io = ImGui::GetIO();
    if (!p.pointer || !link::panel_pointer().load(std::memory_order_acquire)) return 0;
    const ImVec2 m = io.MousePos;
    const bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    if (moved || io.MouseClicked[0]) p.pointer_shown = true;
    if (!p.pointer_shown) {
        io.MouseDrawCursor = false;
        return 0;
    }
    if (m.x < -1e30f || m.y < -1e30f) return 0;
    const double now = seconds_now();
    auto& focus = p.focus[static_cast<std::size_t>(p.tab)];
    auto& scroll = p.scroll[static_cast<std::size_t>(p.tab)];
    const bool shift = dingosdk::overlay::key_down(VK_SHIFT);
    const auto inside = [&](const Hit& h) { return m.x >= h.a.x && m.x <= h.b.x && m.y >= h.a.y && m.y <= h.b.y; };
    const Hit* under{};
    for (const auto& h : p.hits)
        if (inside(h)) under = &h; // the last drawn: a row's part over its row
    const auto row_of = [&](const Hit* h) -> const Row* {
        return h && h->row >= 0 && static_cast<std::size_t>(h->row) < rows.size() ? &rows[static_cast<std::size_t>(h->row)] : nullptr;
    };
    unsigned presses{};
    // Let go: a drag or a held arrow ends.
    if (!io.MouseDown[0]) p.holding = p.sliding = p.scrolling = false;
    // Dragging: a slider follows the pointer along it; the scroll bar moves the rows.
    if (p.sliding && !p.naming) {
        for (const auto& h : p.hits)
            if (h.kind == HitKind::slider) {
                const Row* r = row_of(&h);
                if (r && r->label == p.slide_label && h.to > h.from) {
                    slide(p, view, *r, (m.x - h.from) / (h.to - h.from));
                    rows = rows_for(p, view);
                    break;
                }
            }
        return 0;
    }
    if (p.scrolling) {
        const float travel = std::max(1.0f, p.track_h - p.thumb_h);
        scroll = std::clamp((m.y - p.track_top - p.thumb_h * .5f) / travel, 0.0f, 1.0f) * std::max(0.0f, p.content_h - p.inner_h);
        p.follow_focus = false;
        return 0;
    }
    // An arrow held: again after a moment, then quickly.
    if (p.holding && under && under->kind == p.held && under->row == p.held_row && now >= p.held_next) {
        if (const Row* r = row_of(under)) {
            act(p, view, *r, (p.held == HitKind::left ? -1 : 1) * (shift ? 10 : 1), false, false);
            rows = rows_for(p, view);
        }
        p.held_next = now + 0.07;
    }
    // Moving over a row lights it up (as the D-pad would), unless it does not apply now.
    if (moved && under && under->row >= 0 && !p.naming && static_cast<std::size_t>(under->row) < rows.size() &&
        !rows[static_cast<std::size_t>(under->row)].locked) {
        focus = under->row;
    }
    // The wheel scrolls the rows.
    if (io.MouseWheel != 0 && m.x >= p.list_a.x && m.x <= p.list_b.x + 30 && m.y >= p.list_a.y && m.y <= p.list_b.y) {
        scroll = std::clamp(scroll - io.MouseWheel * p.row_h, 0.0f, std::max(0.0f, p.content_h - p.inner_h));
        p.follow_focus = false;
    }
    if (!io.MouseClicked[0] || !under) return presses;
    switch (under->kind) {
    case HitKind::tab:
        if (!p.naming && under->index != p.tab) {
            p.tab = under->index;
            p.follow_focus = true;
            rows = rows_for(p, view);
        }
        break;
    case HitKind::tab_back: presses |= lb; break;
    case HitKind::tab_next: presses |= rb; break;
    case HitKind::prompt: presses |= under->index == 0 ? pad_a : under->index == 2 ? pad_b : pad_x; break;
    case HitKind::scroll: p.scrolling = true; break;
    case HitKind::row: {
        const Row* r = row_of(under);
        if (!r || r->locked || p.naming) break;
        focus = under->row;
        // A switch, a choice or an action works at a click on it; a number waits for its arrows or slider.
        const bool works = r->kind == RowKind::action || r->kind == RowKind::shake_on || r->kind == RowKind::shake_preset ||
                           r->kind == RowKind::video_folder || r->kind == RowKind::bind ||
                           (r->kind == RowKind::field && r->field && r->field->kind != Kind::number);
        if (works) {
            act(p, view, *r, 0, true, false);
            rows = rows_for(p, view);
        }
        break;
    }
    case HitKind::plate:
        if (const Row* r = row_of(under); r && !r->locked && !p.naming) {
            focus = under->row;
            act(p, view, *r, 0, true, false);
            rows = rows_for(p, view);
        }
        break;
    case HitKind::left:
    case HitKind::right:
        if (const Row* r = row_of(under); r && !r->locked && !p.naming) {
            focus = under->row;
            act(p, view, *r, (under->kind == HitKind::left ? -1 : 1) * (shift ? 10 : 1), false, false);
            rows = rows_for(p, view);
            p.holding = true;
            p.held = under->kind;
            p.held_row = under->row;
            p.held_next = now + 0.38;
        }
        break;
    case HitKind::slider:
        if (const Row* r = row_of(under); r && !r->locked && !p.naming && under->to > under->from) {
            focus = under->row;
            p.sliding = true;
            p.slide_label = r->label;
            slide(p, view, *r, (m.x - under->from) / (under->to - under->from));
            rows = rows_for(p, view);
        }
        break;
    case HitKind::segment:
        if (const Row* r = row_of(under); r && !r->locked && !p.naming) {
            focus = under->row;
            choose(p, view, *r, under->index);
            rows = rows_for(p, view);
        }
        break;
    }
    return presses;
}
} // namespace

void reserve_native(ImFontAtlas& atlas, float width, float height, std::chrono::milliseconds wait) noexcept {
    native::reserve(atlas, width, height, wait);
}
void fill_native(ImFontAtlas& atlas) noexcept { native::fill(atlas); }

void draw_panel() noexcept {
    try {
        auto& p = panel();
        bool asked{}, from_keyboard{};
        if (link::take_panel_request(asked, from_keyboard)) {
            if (asked && !p.open) {
                p.open = true;
                p.armed = false;
                p.whole_input = from_keyboard;
                p.naming = false;
                p.feedback.clear();
                // From the keyboard (F10) it opens with the pointer, ready for the mouse.
                p.pointer = from_keyboard;
                p.pointer_shown = true;
                p.keyboard_style = from_keyboard;
                p.cursor_before = {LONG_MIN, LONG_MIN};
                p.follow_focus = true;
                p.holding = p.sliding = p.scrolling = false;
            } else if (!asked) {
                p.open = false;
            }
        }
        link::panel_open().store(p.open, std::memory_order_release);
        if (!p.open) p.pointer = false;
        link::panel_pointer().store(p.open && p.pointer, std::memory_order_release);
        link::note_game_window(reinterpret_cast<std::uintptr_t>(d::state().window.load()));
        draw_marks(*link::view());
        if (!p.open) {
            draw_hint(*link::view());
            draw_guide(*link::view());
            return;
        }
        link::note_panel_drawn();
        const auto view = link::view();
        // The rows show the view's values, except for a moment after the panel changed one itself (its
        // command is on its way to the game thread).
        const double now = seconds_now();
        if (view->revision != p.view_revision || now - p.changed_at > 0.6) {
            if (now - p.changed_at > 0.6) p.local = view->settings;
            p.view_revision = view->revision;
        }
        if (now - p.shake_changed_at > 0.6) {
            p.shake = view->shake.fallback;
            p.shake_on = view->shake.on;
            p.shake_seed = view->shake.seed;
        }
        if (view->reply_serial != p.reply_serial) {
            p.reply_serial = view->reply_serial;
            if (!view->reply.empty()) say(p, view->reply);
        }

        // The controller (ReSkate's own read: the game's is held meanwhile), or the keyboard.
        ControllerInput input;
        read_pad(input);
        // (While a name is typed its letters are the keyboard's: R, Q and E are no buttons then.)
        const unsigned keys = p.naming ? 0 : keyboard_buttons();
        const unsigned pad = input.available ? input.buttons & 0xffff : 0;
        const unsigned buttons = pad | keys;
        // The mouse, before the panel has the pointer: moved or clicked, the panel takes it (and the game's input whole).
        if (!p.pointer) {
            // (The real pointer and buttons: while the game's input is paused ReSkate shows the game a still mouse.)
            d::OverlayInputAccess real;
            POINT cursor{};
            bool take = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (GetCursorPos(&cursor)) {
                if (p.cursor_before.x != LONG_MIN && (std::abs(cursor.x - p.cursor_before.x) > 12 || std::abs(cursor.y - p.cursor_before.y) > 12))
                    take = true;
                p.cursor_before = cursor;
            }
            if (take && p.armed) {
                p.pointer = true;
                p.pointer_shown = true;
                p.whole_input = true;
                p.keyboard_style = true;
                link::panel_pointer().store(true, std::memory_order_release);
            }
        }
        // Whose prompts the foot shows: the keyboard's while the keyboard or the mouse worked it last, the pad's after a
        // press of the pad (which also hides the pointer until the mouse moves).
        if (keys) p.keyboard_style = true;
        if (pad & ~p.pad_before) {
            p.keyboard_style = false;
            p.pointer_shown = false;
        }
        p.pad_before = pad;
        const bool keyboard = !input.available || p.keyboard_style || (p.naming && p.whole_input);
        if (!p.armed) {
            // The combo that opened it must be let go first.
            if (!buttons) p.armed = true;
            p.previous = buttons;
        }
        // An Xbox pad (which the game reads through XInput, where ReSkate can keep single buttons from it)
        // has only its buttons kept from the game: its triggers and sticks still scrub and frame the replay
        // while the panel is open. Once the keyboard works the panel, or with a pad the game reads itself,
        // the game's input is paused whole (replay_plus.cpp hold_input) until the panel closes.
        if (keys || !input.available || input.style != ControllerStyle::xbox || p.naming || p.pointer) p.whole_input = true;
        link::panel_keyboard().store(p.whole_input, std::memory_order_release);
        link::hide_pad_buttons(400);
        unsigned pressed = p.armed ? buttons & ~p.previous : 0;
        p.previous = buttons;
        auto rows = rows_for(p, *view);
        auto& focus = p.focus[static_cast<std::size_t>(p.tab)];
        focus = std::clamp(focus, 0, std::max(0, static_cast<int>(rows.size()) - 1));
        // The mouse works the panel too (and its clicks on the prompts and the tab arrows are those buttons).
        if (p.armed) pressed |= mouse_input(p, *view, rows);
        if (pressed & (dpad_up | dpad_down | lb | rb)) p.follow_focus = true;
        // The D-pad, held: again after a moment, then quickly (left / right go faster still after a while).
        const unsigned dirs = buttons & (dpad_up | dpad_down | dpad_left | dpad_right);
        int vertical = 0, horizontal = 0;
        if (pressed & (dpad_up | dpad_down | dpad_left | dpad_right)) {
            p.held_dir = pressed & (dpad_up | dpad_down | dpad_left | dpad_right);
            p.held_since = now;
            p.next_repeat = now + 0.38;
            vertical = (pressed & dpad_up) ? -1 : (pressed & dpad_down) ? 1 : 0;
            horizontal = (pressed & dpad_left) ? -1 : (pressed & dpad_right) ? 1 : 0;
        } else if (p.armed && dirs && (dirs & p.held_dir) && now >= p.next_repeat) {
            const bool fast = now - p.held_since > 1.6;
            p.next_repeat = now + (fast ? 0.035 : 0.07);
            vertical = (dirs & dpad_up) ? -1 : (dirs & dpad_down) ? 1 : 0;
            const int speed = fast ? 5 : 1;
            horizontal = (dirs & dpad_left) ? -speed : (dirs & dpad_right) ? speed : 0;
        }
        // Shift with the keyboard's arrows: ten steps at a time.
        if (horizontal && (keys & (dpad_left | dpad_right)) && dingosdk::overlay::key_down(VK_SHIFT)) horizontal *= 10;
        if (!dirs) p.held_dir = 0;
        if (p.binding) {
            // Waiting for a button or key to bind: they are its alone.
            bind_input(p, input);
            rows = rows_for(p, *view);
            auto& at = p.focus[static_cast<std::size_t>(p.tab)];
            at = std::clamp(at, 0, std::max(0, static_cast<int>(rows.size()) - 1));
            draw(p, *view, rows, input.style, keyboard);
            return;
        }
        if (p.naming) {
            // A name being typed has the keyboard and the pad to itself.
            p.tab = static_cast<int>(Tab::presets);
            naming_input(p, *view, pressed, vertical, horizontal);
            // Kept or gone back: the Enter or Esc that did it is still down, and is no press of the panel's (it
            // started naming again, or closed the panel, 2026-11-17). The focus goes back to the row it was on
            // (it went to the top, 2026-11-17).
            if (!p.naming) {
                p.previous |= keyboard_buttons();
                p.focus[static_cast<std::size_t>(Tab::presets)] = p.name_row;
            }
            rows = rows_for(p, *view);
            auto& at = p.focus[static_cast<std::size_t>(p.tab)];
            at = std::clamp(at, 0, std::max(0, static_cast<int>(rows.size()) - 1));
            draw(p, *view, rows, input.style, keyboard);
            return;
        }
        if (pressed & pad_b) {
            p.open = false;
            p.pointer = false;
            link::panel_open().store(false, std::memory_order_release);
            link::panel_pointer().store(false, std::memory_order_release);
            return;
        }
        if (pressed & (lb | rb)) {
            const int count = static_cast<int>(tab_names.size());
            p.tab = ((p.tab + ((pressed & rb) ? 1 : -1)) % count + count) % count;
            rows = rows_for(p, *view);
        }
        auto& now_focus = p.focus[static_cast<std::size_t>(p.tab)];
        // A preset just renamed: the focus goes where the list now has it.
        if (!p.follow_preset.empty()) {
            if (seconds_now() > p.follow_until) p.follow_preset.clear();
            else if (p.tab == static_cast<int>(Tab::presets))
                for (std::size_t k = 0; k < rows.size(); ++k)
                    if (rows[k].kind == RowKind::preset && same_preset_file(rows[k].key, p.follow_preset)) {
                        now_focus = static_cast<int>(k);
                        p.follow_preset.clear();
                        break;
                    }
        }
        // Never on a row that does not apply: the first that does, from where it was.
        if (!rows.empty() && rows[static_cast<std::size_t>(std::clamp(now_focus, 0, static_cast<int>(rows.size()) - 1))].locked) {
            for (std::size_t k = 0; k < rows.size(); ++k)
                if (!rows[k].locked) {
                    now_focus = static_cast<int>(k);
                    break;
                }
        }
        if (vertical && !rows.empty()) {
            // Up / down skip the rows that do not apply now; at either end it stays.
            int next = now_focus;
            for (int k = now_focus + vertical; k >= 0 && k < static_cast<int>(rows.size()); k += vertical)
                if (!rows[static_cast<std::size_t>(k)].locked) {
                    next = k;
                    break;
                }
            now_focus = next;
        }
        // Home and End: the first and the last row that applies.
        if (!rows.empty() && (key_pressed(p, VK_HOME) || key_pressed(p, VK_END))) {
            const bool first = dingosdk::overlay::key_down(VK_HOME);
            for (int k = first ? 0 : static_cast<int>(rows.size()) - 1; k >= 0 && k < static_cast<int>(rows.size()); k += first ? 1 : -1)
                if (!rows[static_cast<std::size_t>(k)].locked) {
                    now_focus = k;
                    break;
                }
            p.follow_focus = true;
        }
        if (!rows.empty()) {
            now_focus = std::clamp(now_focus, 0, static_cast<int>(rows.size()) - 1);
            const auto& row = rows[static_cast<std::size_t>(now_focus)];
            if (!row.locked && (horizontal || (pressed & (pad_a | pad_x)))) {
                act(p, *view, row, horizontal, (pressed & pad_a) != 0, (pressed & pad_x) != 0);
                rows = rows_for(p, *view);
            }
        }
        // Y held hides the panel, to see the picture without it.
        if ((buttons & pad_y) && !keyboard) return;
        draw(p, *view, rows, input.style, keyboard);
    } catch (...) {}
}
} // namespace dingosdk::replay_plus::overlay
