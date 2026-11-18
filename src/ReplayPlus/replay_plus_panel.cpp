#include "replay_plus_overlay.h"
#include "replay_plus_depth.h"
#include "replay_plus_link.h"
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
#include <string>
#include <vector>

// The replay editor's panel: Replay Plus in the replay itself, worked with the controller, drawn in
// the look of skate.'s own menus (ReSkate's theme: hand-cut dark tiles, the blue focus tile with its
// halftone dots, the brushed title face, button prompts on grey plates). LB / RB change the tab, the
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
enum class RowKind { field, shake_on, shake_preset, shake_number, shake_seed, hood_file, action, preset, naming, video_folder };
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
    const float share = link::timeline_share(view.chunks, view.timeline_start, view.timeline_length, clock);
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
            file.hint = view.hood_files.empty() ? "Put PNG pictures in " + view.overlays_folder : "Pictures in " + view.overlays_folder;
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
        } else if (row.key == "save_over") {
            post("replayplus preset save " + p.local.preset);
        } else if (row.key == "focus_key") {
            post("replayplus focus key");
            p.changed_at = now - 1; // the view brings the key
        } else if (row.key == "focus_delete") {
            post("replayplus focus delete");
            p.changed_at = now - 1;
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

// ---- Drawing ----------------------------------------------------------------------------------
struct Fonts {
    ImFont *bold, *body, *heading, *title;
};
Fonts fonts() {
    const auto& m = d::state().menu;
    Fonts f{m.bold ? m.bold : ImGui::GetFont(), m.body ? m.body : ImGui::GetFont(), nullptr, nullptr};
    f.heading = m.heading ? m.heading : f.bold;
    f.title = m.title_large ? m.title_large : m.title ? m.title : f.heading;
    return f;
}
float width_of(ImFont* font, float size, std::string_view text) { return font->CalcTextSizeA(size, FLT_MAX, 0, text.data(), text.data() + text.size()).x; }
void text_at(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 colour, std::string_view text, float right = FLT_MAX) {
    if (right < FLT_MAX) draw->PushClipRect(ImVec2(at.x, at.y - size), ImVec2(right, at.y + size * 2), true);
    draw->AddText(font, size, at, colour, text.data(), text.data() + text.size());
    if (right < FLT_MAX) draw->PopClipRect();
}
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
// A pad button as the pad in use shows it: Xbox's coloured letters (PlayStation's shapes), a shoulder
// button on a grey plate, or a key on a keycap. Returns the width drawn.
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
float glyph(ImDrawList* draw, ImFont* bold, float u, ImVec2 at, std::string_view button, ControllerStyle style, bool keyboard) {
    const float side = 44 * u;
    if (keyboard || button.size() > 1) {
        std::string label(button);
        if (!keyboard && style != ControllerStyle::xbox) label = label == "LB" ? "L1" : label == "RB" ? "R1" : label;
        const float size = 24 * u, inset = 12 * u, cap = width_of(bold, size, label) + inset * 2;
        if (draw) {
            draw->AddRectFilled(ImVec2(at.x, at.y + 2 * u), ImVec2(at.x + cap, at.y + side - 2 * u), keyboard ? theme::paper : IM_COL32(70, 71, 75, 255),
                                6 * u);
            draw->AddText(bold, size, ImVec2(at.x + inset, at.y + (side - size) * .5f), keyboard ? skate_theme::black : theme::paper, label.c_str());
        }
        return cap;
    }
    if (draw) {
        const ImVec2 centre(at.x + side * .5f, at.y + side * .5f);
        const char letter = button.empty() ? 'A' : button[0];
        if (style == ControllerStyle::xbox) {
            const ImU32 colour = letter == 'A' ? IM_COL32(95, 211, 106, 255) : letter == 'B' ? IM_COL32(255, 107, 107, 255)
                               : letter == 'X' ? IM_COL32(64, 156, 255, 255) : IM_COL32(255, 205, 64, 255);
            draw->AddCircleFilled(centre, side * .5f, colour);
            const char text[2]{letter, 0};
            const float size = side * .58f;
            const auto extent = bold->CalcTextSizeA(size, FLT_MAX, 0, text);
            draw->AddText(bold, size, ImVec2(centre.x - extent.x * .5f, centre.y - extent.y * .5f), skate_theme::black, text);
        } else {
            draw->AddCircleFilled(centre, side * .5f, IM_COL32(32, 32, 34, 255));
            const float r = side * .24f, stroke = side * .09f;
            if (letter == 'A') {
                const ImU32 c = IM_COL32(159, 185, 255, 255);
                draw->AddLine(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), c, stroke);
                draw->AddLine(ImVec2(centre.x - r, centre.y + r), ImVec2(centre.x + r, centre.y - r), c, stroke);
            } else if (letter == 'B') {
                draw->AddCircle(centre, r, IM_COL32(255, 154, 168, 255), 0, stroke);
            } else if (letter == 'X') {
                draw->AddRect(ImVec2(centre.x - r, centre.y - r), ImVec2(centre.x + r, centre.y + r), IM_COL32(242, 155, 222, 255), 0, 0, stroke);
            } else {
                const ImU32 c = IM_COL32(64, 226, 160, 255);
                draw->AddTriangle(ImVec2(centre.x, centre.y - r * 1.1f), ImVec2(centre.x + r, centre.y + r * .75f), ImVec2(centre.x - r, centre.y + r * .75f), c,
                                  stroke);
            }
        }
    }
    return side;
}
float prompt(ImDrawList* draw, ImFont* bold, float u, ImVec2 at, std::string_view button, std::string_view what, ControllerStyle style, bool keyboard) {
    const float w = glyph(draw, bold, u, at, button, style, keyboard), text = 28 * u;
    if (draw) draw->AddText(bold, text, ImVec2(at.x + w + 12 * u, at.y + (44 * u - text) * .5f), theme::paper, what.data(), what.data() + what.size());
    return w + 12 * u + width_of(bold, text, what);
}

void draw(Panel& p, const link::View& view, const std::vector<Row>& rows, ControllerStyle style, bool keyboard) {
    const Fonts f = fonts();
    p.hits.clear();
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f);
    auto* dl = ImGui::GetForegroundDrawList();
    // Where the game's own Effects panel is (Advanced Edit, its third tab): the right half, under the
    // top, see-through, so the replay shows beside it and through it.
    // On the left, under the replay screens' titles: Advanced Edit keeps its own panels on the right and
    // its tabs in the top middle, and the first screen its title in the top left.
    const float x0 = std::floor(display.x * 0.035f), x1 = std::floor(display.x * 0.44f);
    float y = std::floor(display.y * 0.15f);
    // The title, as the replay screens have theirs (smaller: it is a panel, not a page), and what is loaded.
    const float title_size = 92 * u;
    text_at(dl, f.title, title_size, ImVec2(x0, y), theme::paper, "REPLAY PLUS", x1);
    const std::string loaded = p.local.preset.empty() ? std::string() : upper(p.local.preset);
    if (!loaded.empty()) {
        const float size = 30 * u, w = width_of(f.bold, size, loaded) + 28 * u;
        const ImVec2 at(x1 - w, y + (title_size - 50 * u) * .5f);
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + 50 * u), IM_COL32(20, 20, 22, 240), 4 * u);
        dl->AddRectFilled(at, ImVec2(at.x + 6 * u, at.y + 50 * u), theme::blue);
        text_at(dl, f.bold, size, ImVec2(at.x + 16 * u, at.y + (50 * u - size) * .5f), theme::paper, loaded);
    }
    if (!view.in_replay) text_at(dl, f.bold, 28 * u, ImVec2(x0, y + title_size), skate_theme::warning, "OPEN A REPLAY: THESE SHOW IN THE REPLAY EDITOR");
    y += title_size + 30 * u;
    // The tabs: tiles in a row with Q / E (LB / RB) either side, as Advanced Edit's are; the chosen one
    // white with its name in the game's blue.
    const float tab_h = 66 * u;
    const float lb_w = glyph(nullptr, f.bold, u, {}, keyboard ? "Q" : "LB", style, keyboard);
    glyph(dl, f.bold, u, ImVec2(x0, y + (tab_h - 44 * u) * .5f), keyboard ? "Q" : "LB", style, keyboard);
    glyph(dl, f.bold, u, ImVec2(x1 - lb_w, y + (tab_h - 44 * u) * .5f), keyboard ? "E" : "RB", style, keyboard);
    p.hits.push_back({HitKind::tab_back, ImVec2(x0, y), ImVec2(x0 + lb_w, y + tab_h)});
    p.hits.push_back({HitKind::tab_next, ImVec2(x1 - lb_w, y), ImVec2(x1, y + tab_h)});
    const float tabs_left = x0 + lb_w + 14 * u, tabs_right = x1 - lb_w - 14 * u;
    const float each = (tabs_right - tabs_left) / static_cast<float>(tab_names.size());
    for (std::size_t t = 0; t < tab_names.size(); ++t) {
        const ImVec2 ta(tabs_left + each * static_cast<float>(t) + 4 * u, y), tb(tabs_left + each * static_cast<float>(t + 1) - 4 * u, y + tab_h);
        p.hits.push_back({HitKind::tab, ta, tb, static_cast<int>(t)});
        const bool on = static_cast<int>(t) == p.tab;
        dl->AddRectFilled(ta, tb, on ? theme::paper : IM_COL32(16, 16, 17, 225), 3 * u);
        const std::string name(tab_names[t]);
        const float size = std::min(26 * u, (tb.x - ta.x - 10 * u) / std::max(1.0f, width_of(f.bold, 1, name)));
        text_at(dl, f.bold, size, ImVec2((ta.x + tb.x - width_of(f.bold, size, name)) * .5f, ta.y + (tab_h - size) * .5f), on ? theme::blue : theme::paper, name);
    }
    y += tab_h + 18 * u;
    // The rows, on a see-through panel, scrolled to keep the focus in view.
    const float row_h = 112 * u, label_size = 38 * u, value_size = 34 * u;
    // Ending above what Advanced Edit shows over the timeline (Keyframe's Add Keyframe, on the left).
    const float most = std::max(row_h * 3, display.y * 0.655f - y);
    const float list_h = std::min(most, static_cast<float>(std::max<std::size_t>(rows.size(), 1)) * row_h + 16 * u);
    const float list_top = y, list_bottom = y + list_h;
    dl->AddRectFilled(ImVec2(x0, list_top), ImVec2(x1, list_bottom), IM_COL32(18, 18, 20, 242));
    auto& focus = p.focus[static_cast<std::size_t>(p.tab)];
    auto& scroll = p.scroll[static_cast<std::size_t>(p.tab)];
    const float inner_h = list_h - 16 * u;
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
    const float value_mid = x0 + (x1 - x0) * 0.75f, bar_w = std::min(340 * u, (x1 - x0) * 0.32f);
    dl->PushClipRect(ImVec2(x0, list_top), ImVec2(x1, list_bottom), true);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const float top = list_top + 8 * u + static_cast<float>(i) * row_h - scroll;
        if (top > list_bottom || top + row_h < list_top) continue;
        // (Its parts only where the panel shows them.)
        const float ry0 = std::max(top, list_top), ry1 = std::min(top + row_h, list_bottom);
        const auto part = [&](HitKind kind, ImVec2 a, ImVec2 b, int index = 0, float from = 0, float to = 0) {
            a.y = std::max(a.y, ry0);
            b.y = std::min(b.y, ry1);
            if (b.y > a.y) p.hits.push_back({kind, a, b, index, static_cast<int>(i), from, to});
        };
        part(HitKind::row, ImVec2(x0, top), ImVec2(x1, top + row_h), static_cast<int>(i));
        const bool lit = static_cast<int>(i) == focus;
        const bool info = row.kind == RowKind::action && row.key == "none";
        if (lit) {
            // The focus as the game's own panel shows it: a blue edge, the row a shade lighter.
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + row_h), IM_COL32(255, 255, 255, 22));
            dl->AddRectFilled(ImVec2(x0, top + 6 * u), ImVec2(x0 + 9 * u, top + row_h - 6 * u), theme::blue);
        }
        const ImU32 ink = row.locked ? IM_COL32(150, 150, 152, 150) : info ? theme::muted : theme::paper;
        const float label_y = top + (row_h - label_size) * .5f;
        text_at(dl, lit ? f.bold : f.body, label_size, ImVec2(x0 + 44 * u, label_y), ink, row.label, value_mid - bar_w * .5f - 60 * u);
        if (row.locked)
            skate_theme::scribble(dl, ImVec2(x0 + 36 * u, top + 14 * u), ImVec2(x0 + 36 * u + std::min(430 * u, width_of(f.body, label_size, row.label) + 40 * u), top + row_h - 14 * u),
                                  0x900 + static_cast<unsigned>(i), u * 1.6f);
        // The value, and under it its slider or its marks; "< value >" on the row in focus.
        std::string value = row.value;
        const bool marked = row.fraction >= 0 || row.segments > 0;
        const float value_y = marked ? top + 18 * u : top + (row_h - value_size) * .5f;
        if (row.kind == RowKind::naming) {
            // The name, the cursor in it, on a light plate as a text field.
            const float size = 36 * u, left = x0 + 300 * u, right = x1 - 40 * u;
            dl->AddRectFilled(ImVec2(left, top + 22 * u), ImVec2(right, top + row_h - 22 * u), IM_COL32(236, 236, 230, 245), 4 * u);
            text_at(dl, f.bold, size, ImVec2(left + 18 * u, top + (row_h - size) * .5f), skate_theme::black, row.value, right - 18 * u);
            continue;
        }
        if (row.kind == RowKind::preset && lit) {
            // What A does to it, left / right choosing: a plate as an action's, with arrows either side.
            static constexpr std::array<const char*, 4> what{"LOAD", "RENAME", "EXPORT", "DELETE"};
            const int chosen = p.preset_action_for == row.key ? std::clamp(p.preset_action, 0, 3) : 0;
            const std::string label = p.confirm == row.key ? std::string("DELETE: A AGAIN") : std::string(what[static_cast<std::size_t>(chosen)]);
            const float pw = std::max(150 * u, width_of(f.bold, 28 * u, label) + 40 * u);
            const ImVec2 pa(x1 - 70 * u - pw, top + row_h * .5f - 26 * u), pb(x1 - 70 * u, top + row_h * .5f + 26 * u);
            part(HitKind::left, ImVec2(pa.x - 48 * u, pa.y), ImVec2(pa.x - 6 * u, pb.y));
            part(HitKind::right, ImVec2(pb.x + 6 * u, pa.y), ImVec2(pb.x + 48 * u, pb.y));
            part(HitKind::plate, pa, pb);
            dl->AddRectFilled(pa, pb, chosen == 3 || p.confirm == row.key ? IM_COL32(214, 92, 72, 255) : IM_COL32(236, 206, 92, 255), 4 * u);
            text_at(dl, f.bold, 28 * u, ImVec2((pa.x + pb.x - width_of(f.bold, 28 * u, label)) * .5f, pa.y + 12 * u), skate_theme::black, label);
            const float cy = top + row_h * .5f, h = 13 * u;
            dl->AddTriangleFilled(ImVec2(pa.x - 34 * u, cy), ImVec2(pa.x - 18 * u, cy - h), ImVec2(pa.x - 18 * u, cy + h), theme::paper);
            dl->AddTriangleFilled(ImVec2(pb.x + 34 * u, cy), ImVec2(pb.x + 18 * u, cy - h), ImVec2(pb.x + 18 * u, cy + h), theme::paper);
            continue;
        }
        if (row.kind == RowKind::preset) {
            if (row.value == "Loaded") {
                const float pw = width_of(f.bold, 24 * u, "LOADED") + 24 * u;
                dl->AddRectFilled(ImVec2(x1 - 40 * u - pw, top + row_h * .5f - 20 * u), ImVec2(x1 - 40 * u, top + row_h * .5f + 20 * u), theme::blue, 4 * u);
                text_at(dl, f.bold, 24 * u, ImVec2(x1 - 40 * u - pw + 12 * u, top + row_h * .5f - 12 * u), theme::paper, "LOADED");
            }
            continue;
        }
        if (row.kind == RowKind::action && !info) {
            // An action: a plate with its button, as the game's CONFIRM is.
            const float pw = 150 * u;
            const ImVec2 pa(x1 - 40 * u - pw, top + row_h * .5f - 26 * u), pb(x1 - 40 * u, top + row_h * .5f + 26 * u);
            if (!row.locked) part(HitKind::plate, pa, pb);
            dl->AddRectFilled(pa, pb, row.locked ? IM_COL32(40, 40, 42, 160) : lit ? IM_COL32(236, 206, 92, 255) : IM_COL32(60, 60, 64, 220), 4 * u);
            const std::string& label = row.value;
            text_at(dl, f.bold, 28 * u, ImVec2((pa.x + pb.x - width_of(f.bold, 28 * u, label)) * .5f, pa.y + 12 * u),
                    row.locked ? IM_COL32(150, 150, 152, 150) : lit ? skate_theme::black : theme::paper, label);
            continue;
        }
        const float vw = width_of(f.bold, value_size, value);
        text_at(dl, f.bold, value_size, ImVec2(value_mid - vw * .5f, value_y), row.locked ? ink : theme::paper, value);
        if (lit && !row.locked && !info && row.kind != RowKind::video_folder) {
            const float cy = value_y + value_size * .55f, h = 13 * u;
            const float lx = value_mid - bar_w * .5f - 34 * u, rx = value_mid + bar_w * .5f + 34 * u;
            dl->AddTriangleFilled(ImVec2(lx - 10 * u, cy), ImVec2(lx + 6 * u, cy - h), ImVec2(lx + 6 * u, cy + h), theme::paper);
            dl->AddTriangleFilled(ImVec2(rx + 10 * u, cy), ImVec2(rx - 6 * u, cy - h), ImVec2(rx - 6 * u, cy + h), theme::paper);
        }
        const float bar_y = top + row_h - 30 * u, bx0 = value_mid - bar_w * .5f, bx1 = value_mid + bar_w * .5f;
        if (!row.locked && !info && row.kind != RowKind::video_folder) {
            // Either side of the value: a step down or up (held, again and again); its slider, or its marks.
            part(HitKind::left, ImVec2(bx0 - 70 * u, top), ImVec2(bx0 - 4 * u, top + row_h));
            part(HitKind::right, ImVec2(bx1 + 4 * u, top), ImVec2(bx1 + 70 * u, top + row_h));
            if (row.fraction >= 0) part(HitKind::slider, ImVec2(bx0 - 8 * u, bar_y - 26 * u), ImVec2(bx1 + 8 * u, bar_y + 20 * u), 0, bx0, bx1);
            else if (row.segments > 0) {
                const float gap = 8 * u, w = (bx1 - bx0 - gap * static_cast<float>(row.segments - 1)) / static_cast<float>(row.segments);
                for (int k = 0; k < row.segments; ++k) {
                    const float sx = bx0 + static_cast<float>(k) * (w + gap);
                    part(HitKind::segment, ImVec2(sx - gap * .5f, bar_y - 26 * u), ImVec2(sx + w + gap * .5f, bar_y + 20 * u), k);
                }
            }
        }
        const ImU32 lit_bar = row.locked ? IM_COL32(90, 110, 140, 160) : theme::blue;
        if (row.fraction >= 0) {
            const float at = bx0 + (bx1 - bx0) * std::clamp(row.fraction, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(bx0, bar_y - 3 * u), ImVec2(bx1, bar_y + 3 * u), IM_COL32(70, 70, 74, 230), 3 * u);
            dl->AddRectFilled(ImVec2(bx0, bar_y - 3 * u), ImVec2(at, bar_y + 3 * u), lit_bar, 3 * u);
            dl->AddCircleFilled(ImVec2(at, bar_y), 12 * u, row.locked ? IM_COL32(170, 170, 172, 200) : theme::paper);
        } else if (row.segments > 0) {
            const float gap = 8 * u, w = (bx1 - bx0 - gap * static_cast<float>(row.segments - 1)) / static_cast<float>(row.segments);
            for (int k = 0; k < row.segments; ++k) {
                const float sx = bx0 + static_cast<float>(k) * (w + gap);
                dl->AddRectFilled(ImVec2(sx, bar_y - 3 * u), ImVec2(sx + w, bar_y + 3 * u), k == row.segment ? lit_bar : IM_COL32(70, 70, 74, 230), 2 * u);
            }
        }
    }
    dl->PopClipRect();
    // A scroll bar, as the game's lists have one, while there is more than shows.
    const float content = static_cast<float>(rows.size()) * row_h;
    if (content > inner_h + 1) {
        const float track_top = list_top + 8 * u, track_h = inner_h;
        const float thumb_h = std::max(60 * u, track_h * inner_h / content), thumb_y = track_top + (track_h - thumb_h) * (scroll / (content - inner_h));
        p.hits.push_back({HitKind::scroll, ImVec2(x1 + 2 * u, track_top), ImVec2(x1 + 26 * u, track_top + track_h), 0, -1, track_top, track_top + track_h});
        p.track_top = track_top;
        p.track_h = track_h;
        p.thumb_h = thumb_h;
        dl->AddRectFilled(ImVec2(x1 + 10 * u, track_top), ImVec2(x1 + 14 * u, track_top + track_h), IM_COL32(255, 255, 255, 40), 2 * u);
        dl->AddRectFilled(ImVec2(x1 + 8 * u, thumb_y), ImVec2(x1 + 16 * u, thumb_y + thumb_h), IM_COL32(255, 255, 255, 180), 3 * u);
    }
    // What the row in focus does (or what the last change said), on a strip under the panel, as the
    // game's settings show their description.
    const std::string hint = focus >= 0 && static_cast<std::size_t>(focus) < rows.size() ? rows[static_cast<std::size_t>(focus)].hint : std::string();
    const bool fresh = seconds_now() - p.feedback_at < 5;
    const std::string under = fresh ? p.feedback : hint;
    if (!under.empty()) {
        const float size = 30 * u, pad = 22 * u;
        const float text_h = f.body->CalcTextSizeA(size, FLT_MAX, x1 - x0 - pad * 2, under.c_str()).y;
        const ImVec2 ha(x0, list_bottom + 14 * u), hb(x1, list_bottom + 14 * u + text_h + pad * 2);
        dl->AddRectFilled(ha, hb, IM_COL32(18, 18, 20, 210));
        if (fresh) dl->AddRectFilled(ha, ImVec2(ha.x + 6 * u, hb.y), theme::blue);
        dl->AddText(f.body, size, ImVec2(x0 + pad, ha.y + pad), fresh ? theme::paper : IM_COL32(205, 205, 208, 255), under.c_str(), nullptr, x1 - x0 - pad * 2);
    }
    // The controls, at the foot of the screen on the right, as the game puts its own.
    std::vector<std::pair<std::string, std::string>> controls;
    if (p.naming && keyboard) controls = {{"Enter", "Keep"}, {"Bksp", "Delete"}, {"Esc", "Back"}};
    else if (p.naming) controls = {{"A", "Keep"}, {"X", "Delete"}, {"B", "Back"}};
    else if (keyboard) controls = {{"Enter", "Select"}, {"R", p.tab == static_cast<int>(Tab::presets) ? "Delete" : "Reset To Default"}, {"Bksp", "Back"}};
    else controls = {{"A", "Select"}, {"X", p.tab == static_cast<int>(Tab::presets) ? "Delete" : "Reset To Default"}, {"Y", "Hold To Hide"}, {"B", "Back"}};
    float total = 0;
    for (const auto& [button, what] : controls) total += prompt(nullptr, f.bold, u, {}, button, what, style, keyboard) + 40 * u;
    float x = display.x - 72 * u - total + 40 * u;
    const float fy = display.y - 100 * u;
    for (const auto& [button, what] : controls) {
        const float w = prompt(dl, f.bold, u, ImVec2(x, fy), button, what, style, keyboard);
        // Each prompt is its button for the mouse too.
        const int does = what == "Select" || what == "Keep" ? 0 : what == "Back" ? 2 : what == "Hold To Hide" ? -1 : 1;
        if (does >= 0) p.hits.push_back({HitKind::prompt, ImVec2(x - 8 * u, fy - 8 * u), ImVec2(x + w + 8 * u, fy + 52 * u), does});
        x += w + 40 * u;
    }
}

// The way in, as the game shows its own: "R3 + RB  Replay Plus" on a pad (R3 first: RB then stays the
// panel's, not the game's next tab), "F10  Replay Plus" on the
// keyboard, among the replay editor's button prompts at the foot of the screen, and only while the game's
// UI shows there (replay_plus_depth.h ui_shown): it goes as the game hides its UI (L3 / H), or fades it
// after a while without input.
void draw_hint(const link::View& view) {
    // In Advanced Edit only, while the game's prompts show.
    if (!view.in_replay || !view.advanced_edit || depth::ui_shown() < 0.5f) return;
    ControllerInput input;
    read_pad(input);
    const bool keyboard = !input.available;
    const Fonts f = fonts();
    const auto display = ImGui::GetIO().DisplaySize;
    // The game's prompts are larger than the panel's own (their keys about 44 pixels, their words about 36,
    // at 1440 lines): as theirs.
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f) * 2.1f;
    auto* dl = ImGui::GetForegroundDrawList();
    constexpr std::string_view what = "Replay Plus";
    const float spacing = 10 * u, text = 28 * u;
    float w = keyboard ? glyph(nullptr, f.bold, u, {}, "F10", input.style, true)
                       : glyph(nullptr, f.bold, u, {}, "R3", input.style, false) + spacing + width_of(f.bold, text, "+") + spacing +
                             glyph(nullptr, f.bold, u, {}, "RB", input.style, false);
    w += 12 * u + width_of(f.bold, text, what);
    // In the widest stretch of the prompts' row with none of the game's prompts in it (each tab has its
    // own: Trim's leaves the middle free, Keyframe's the left of the middle), with room either side.
    const auto free = depth::ui_gap();
    const float from = free.from * display.x, to = free.to * display.x, room = 40 * u;
    if (to - from < w + room * 2) return;
    float x = std::floor((from + to) * 0.5f - w * 0.5f);
    const float y = std::floor(display.y * 0.924f - 22 * u);
    if (keyboard) {
        x += glyph(dl, f.bold, u, ImVec2(x, y), "F10", input.style, true);
    } else {
        x += glyph(dl, f.bold, u, ImVec2(x, y), "R3", input.style, false) + spacing;
        dl->AddText(f.bold, text, ImVec2(x, y + (44 * u - text) * .5f), theme::paper, "+");
        x += width_of(f.bold, text, "+") + spacing;
        x += glyph(dl, f.bold, u, ImVec2(x, y), "RB", input.style, false);
    }
    dl->AddText(f.bold, text, ImVec2(x + 12 * u, y + (44 * u - text) * .5f), theme::paper, what.data(), what.data() + what.size());
}

// A D-pad, as the game draws its own: a cross, the arms pressed lit (1 up, 2 down, 4 left, 8 right).
float dpad_glyph(ImDrawList* draw, float u, ImVec2 at, unsigned arms) {
    const float side = 44 * u;
    if (draw) {
        const ImVec2 c(at.x + side * .5f, at.y + side * .5f);
        const float a = side * .15f, b = side * .47f;
        const ImU32 dim = IM_COL32(120, 121, 126, 255), lit = theme::paper;
        draw->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x + a, c.y + b), dim, 2 * u);
        draw->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x + b, c.y + a), dim, 2 * u);
        if (arms & 1) draw->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x + a, c.y - a), lit, 2 * u);
        if (arms & 2) draw->AddRectFilled(ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y + b), lit, 2 * u);
        if (arms & 4) draw->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x - a, c.y + a), lit, 2 * u);
        if (arms & 8) draw->AddRectFilled(ImVec2(c.x + a, c.y - a), ImVec2(c.x + b, c.y + a), lit, 2 * u);
    }
    return side;
}
constexpr ImU32 focus_mark = IM_COL32(64, 196, 255, 255), shake_mark = IM_COL32(255, 150, 60, 255);
// The game's timeline runs from 10.16% to 90.04% of the width, the replay's start to its end (its own
// one-second ticks sit there, at 16:9 and at 4:3 alike); its bar's height is read from the UI
// (replay_plus_depth.h timeline_bar), else where it is on a 16:9 screen.
constexpr float timeline_from = 0.10156f, timeline_span = 0.79883f, timeline_y_wide = 0.8208f;

// Replay Plus's keys on the game's timeline, as the game marks its own camera keyframes: focus keys
// blue above the bar's middle, shake keys orange below it (filled where the shake comes on, hollow where
// it goes off). Only while the game's UI shows (they go with it, L3 / H).
void draw_marks(const link::View& view) {
    if (!view.in_replay || !view.timeline_ok || view.timeline_length <= 0 || depth::ui_shown() < 0.5f) return;
    if (view.settings.focus_keys.empty() && view.settings.shake_keys.empty()) return;
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
        const float share = link::timeline_share(view.chunks, view.timeline_start, view.timeline_length, time);
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
}

// R3 held in the replay editor: what each button does with it, over the left of the picture above the
// timeline; and what the last one did, for a moment after.
void draw_guide(const link::View& view) {
    if (!view.in_replay) return;
    const double since = static_cast<double>(link::now_ms() - view.toast_at) / 1000.0;
    const bool toast = !view.toast.empty() && view.toast_at && since < 2.5;
    if (!view.r3_held && !toast) return;
    ControllerInput input;
    read_pad(input);
    const Fonts f = fonts();
    const auto display = ImGui::GetIO().DisplaySize;
    const float u = std::min(display.x / 3840.0f, display.y / 2160.0f) * 1.6f;
    auto* dl = ImGui::GetForegroundDrawList();
    const auto bar = depth::timeline_bar();
    const float bottom = std::floor((bar.found ? bar.y : timeline_y_wide) * display.y - 70 * u);
    const float x = std::floor(display.x * timeline_from), text = 28 * u, row = 56 * u, pad = 18 * u;
    struct Line {
        std::string_view button; // "RB", "LB", "LT", "RT", or "" for the D-pad
        unsigned arms{};
        std::string what;
    };
    std::vector<Line> lines;
    if (view.r3_held) {
        const auto& v = view.settings;
        const bool keyed = v.focus == Focus::keys;
        lines.push_back({"RB", 0, "Replay Plus panel"});
        lines.push_back({"", 1 | 2, std::format("Focus {:.2f} m (further / nearer){}", view.focus_distance_now, keyed ? "" : ": makes it keyed")});
        lines.push_back({"LT", 0, std::format("Blur {:.0f}% (LT less, RT more)", view.focus_blur_now * 100)});
        lines.push_back({"", 8, std::format("Camera light {}", v.light ? "off" : "on")});
        lines.push_back({"", 4, std::format("Shake {} (the whole replay)", view.shake.on ? "off" : "on")});
        lines.push_back({"LB", 0, std::format("Shake key: {} from here", view.shake_here ? "off" : "on")});
        lines.push_back({"VIEW", 0, "Delete the keys here"});
    }
    float w = 0;
    for (const auto& line : lines) w = std::max(w, 44 * u + 14 * u + width_of(f.bold, text, line.what));
    if (toast) w = std::max(w, width_of(f.bold, text, view.toast));
    w = std::min(w + pad * 2, display.x * 0.6f);
    const float title_h = view.r3_held ? row : 0, toast_h = toast ? row : 0;
    const float h = title_h + static_cast<float>(lines.size()) * row + toast_h + pad * 2;
    const float top = bottom - h;
    dl->AddRectFilled(ImVec2(x, top), ImVec2(x + w, bottom), IM_COL32(16, 16, 18, 210), 6 * u);
    float y = top + pad;
    if (view.r3_held) {
        const float g = glyph(dl, f.bold, u, ImVec2(x + pad, y), "R3", input.style, false);
        dl->AddText(f.bold, text, ImVec2(x + pad + g + 14 * u, y + (44 * u - text) * .5f), theme::paper, "Replay Plus: held, with");
        y += row;
    }
    for (const auto& line : lines) {
        float gx = x + pad;
        if (line.button.empty()) {
            gx += dpad_glyph(dl, u, ImVec2(gx, y), line.arms);
        } else if (line.button == "LT") {
            gx += glyph(dl, f.bold, u, ImVec2(gx, y), "LT", input.style, false) + 6 * u;
            gx += glyph(dl, f.bold, u, ImVec2(gx, y), "RT", input.style, false);
        } else {
            gx += glyph(dl, f.bold, u, ImVec2(gx, y), line.button, input.style, false);
        }
        dl->AddText(f.bold, text, ImVec2(gx + 14 * u, y + (44 * u - text) * .5f), theme::paper, line.what.c_str());
        y += row;
    }
    if (toast) {
        const float fade = since < 2.0 ? 1.0f : static_cast<float>((2.5 - since) / 0.5);
        const auto ink = IM_COL32(236, 206, 92, static_cast<int>(255 * std::clamp(fade, 0.0f, 1.0f)));
        dl->AddText(f.bold, text, ImVec2(x + pad, y + (44 * u - text) * .5f), ink, view.toast.c_str());
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
                           r->kind == RowKind::video_folder || (r->kind == RowKind::field && r->field && r->field->kind != Kind::number);
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
