// Replay Plus with no game: every value's table entry, the `replayplus` verbs, the saved settings (and those saved
// before the lens was settings only), presets and their names, the lens (against its reference
// numbers) and its edge compression, the overlay's picture, the grade, and an exported frame
// finished on the CPU.
#include "Extension/ReplayPlus/replay_plus_lens.h"
#include "Extension/ReplayPlus/replay_plus_settings.h"
#include "Extension/ReplayShake/replay_shake_model.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <set>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::replay_plus;
void migration_tests();
void camera_curve_tests();
namespace {
void fit_is_tight(const lens::Frame& f, const std::string& what);
int checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }
using Words = std::vector<std::string>;
Reply run(Settings& settings, const std::string& verb, const Words& words) { return command(settings, verb, words); }
float radians(float degrees) { return degrees * lens::pi / 180.0f; }

// A lens hood picture of the tests' own: 4:3, black outside an ellipse (an adapter's image circle, 1.54
// half-heights, stretched with its overlay across the window: 1.1294 by 1.54 in the picture's own units), so its
// black shows in the corners only; measured as Replay Plus measures a picture.
lens::Hole hood_hole() {
    static const lens::Hole hole = [] {
        const std::uint32_t w = 1440, h = 1080;
        std::vector<std::uint8_t> rgba(std::size_t{w} * h * 4, 0);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2 - 1, v = 1 - (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2;
                rgba[(std::size_t{y} * w + x) * 4 + 3] = (u / 1.1294f) * (u / 1.1294f) + (v / 1.54f) * (v / 1.54f) >= 1 ? 255 : 0;
            }
        return lens::measure_hole(rgba.data(), w, h);
    }();
    return hole;
}
// The angle off-axis (radians) a point of the game's picture looks along.
double angle_in_game(const lens::Frame& f, std::array<float, 2> uv) {
    const auto tg = lens::tangents(f);
    const double x = (uv[0] - 0.5) * 2.0 * tg[0], y = (0.5 - uv[1]) * 2.0 * tg[1];
    return std::atan(std::sqrt(x * x + y * y));
}
// The direction (unit) a point of a game's picture looks along, at its field of view and aspect.
std::array<double, 3> direction_in_game(double u, double v, double fov, double aspect) {
    const double ty = std::tan(fov * 0.5), tx = ty * aspect;
    const double x = (u * 2 - 1) * tx, y = (1 - v * 2) * ty, n = std::sqrt(x * x + y * y + 1);
    return {x / n, y / n, 1 / n};
}
double between_directions(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}
const Preset& built_in(const std::string& name) {
    static const auto list = built_in_presets();
    for (const auto& p : list)
        if (p.name == name) return p;
    throw std::runtime_error("no built-in preset " + name);
}

void field_tests() {
    const Settings defaults;
    std::set<std::string_view> keys;
    for (const auto& f : fields()) {
        check(keys.insert(f.key).second, std::string("one field per key: ") + std::string(f.key));
        check(f.get && f.set, std::string(f.key) + " reads and writes");
        const float v = f.get(defaults);
        if (f.kind == Kind::number) check(v >= f.low && v <= f.high, std::string(f.key) + "'s default is within its range");
        if (f.kind == Kind::choice) check(f.choices.size() == f.labels.size() && v >= 0 && v < static_cast<float>(f.choices.size()),
                                          std::string(f.key) + "'s choices have labels, and its default is one");
        // Shown and read back, every step of the way: what the panel shows, `set` takes.
        Settings s;
        const int steps = f.kind == Kind::number ? static_cast<int>(std::round((f.high - f.low) / f.step)) : static_cast<int>(f.choices.size()) + 1;
        for (int i = 0; i <= std::min(steps, 400); ++i) {
            if (f.kind == Kind::number) f.set(s, std::min(f.high, f.low + static_cast<float>(i) * f.step));
            else step(f, s, 1);
            const float before = f.get(s);
            Settings t = defaults;
            std::string why;
            check(assign(f, t, shown(f, s), why), std::string(f.key) + " takes back what it shows: " + shown(f, s) + " " + why);
            float tolerance = f.kind == Kind::number ? f.step * 0.51f + 1e-4f : 0.0f;
            if (f.logarithmic) tolerance = (before < 1 ? 0.005f : before < 10 ? 0.05f : 0.5f) + 1e-4f; // two figures shown
            check(std::abs(f.get(t) - before) <= tolerance, std::string(f.key) + " reads back as it was shown: " + shown(f, s));
        }
        // Stepping stays within the range; choices wrap.
        Settings u;
        step(f, u, 10000);
        check(f.kind != Kind::number || f.get(u) <= f.high, std::string(f.key) + " steps up to its top");
        step(f, u, -20000);
        check(f.kind != Kind::number || f.get(u) >= f.low, std::string(f.key) + " steps down to its bottom");
    }
    check(field("lens.zoom") && field("LENS.ZOOM") && !field("lens.nothing"), "fields are found by key, in any case");
    // Numbers in the panel's units or kept ones.
    Settings s;
    std::string why;
    check(assign(*field("video.replay"), s, "150%", why) && near(s.replay_scale, 1.5), "150% is 1.5");
    check(assign(*field("video.replay"), s, "175", why) && near(s.replay_scale, 1.75), "a number in the shown units is taken as shown");
    check(assign(*field("video.replay"), s, "1.25", why) && near(s.replay_scale, 1.25), "a kept number is taken as kept");
    check(assign(*field("video.replay"), s, "9", why) && near(s.replay_scale, 2.0), "past the top is the top");
    check(!assign(*field("video.replay"), s, "lots", why), "words are not numbers");
    check(assign(*field("frame.hood"), s, "Picture", why) && s.hood == Hood::picture, "a choice by its label");
    check(assign(*field("frame.hood"), s, "circle", why) && s.hood == Hood::circle, "a choice by its name");
    check(!assign(*field("frame.hood"), s, "zoom lens", why), "a choice that is not one");
    check(!field("lens.mode") && !field("lens.true") && !field("grade.camcorder"), "no switch between looks: the lens is its settings");
    check(assign(*field("light.on"), s, "on", why) && s.light && assign(*field("light.on"), s, "toggle", why) && !s.light, "toggles");
    check(assign(*field("fov.degrees"), s, "150°", why) && near(s.fov, 150), "a unit after the number");
    // Relevance: a value that does nothing as things are is shown locked.
    Settings r;
    check(!field("lens.circle")->relevant(r), "no distortion: no image circle");
    r.curve = 1;
    check(field("lens.circle")->relevant(r) && !field("lens.zoom")->relevant, "a fisheye's circle; the zoom always counts");
    check(!field("frame.opacity")->relevant(r), "no overlay: no opacity");
    r.hood = Hood::picture;
    check(field("frame.opacity")->relevant(r), "an overlay's opacity");
    check(!field("grade.camera_gamma")->relevant(r), "the camera curve's values only with it on");
    r.camera = true;
    check(field("grade.camera_gamma")->relevant(r) && field("grade.camera_knee")->relevant(r), "the camera curve's values");
    check(!field("game.barrel")->relevant(r), "the game's barrel is one of the game's own settings");
    r.game_look = true;
    check(field("game.barrel")->relevant(r), "the game's barrel with the game's look on");
    // Not finite comes back as the default.
    Settings bad;
    bad.zoom = std::nanf("");
    bad.fov = 1e9f;
    const auto fixed = clamped(bad);
    check(fixed.zoom == 1.0f && fixed.fov == 170.0f, "clamped: not finite is the default, too big is the top");
}

void command_tests() {
    Settings s;
    auto reply = run(s, "set", {"lens.curve", "100%"});
    check(reply.changed && s.curve == 1.0f && lens_on(s), "set changes a value: " + reply.text);
    reply = run(s, "set", {"lens.curve", "1"});
    check(!reply.changed, "the same again changes nothing");
    reply = run(s, "get", {"lens.curve"});
    check(reply.text == "lens.curve = 100%", "get says the value as the panel does: " + reply.text);
    reply = run(s, "step", {"lens.zoom", "5"});
    check(reply.changed && near(s.zoom, 1.05f), "step: five steps of 0.01");
    reply = run(s, "step", {"lens.zoom", "-50"});
    check(near(s.zoom, 1.0f), "step stops at the bottom");
    reply = run(s, "set", {"nothing.here", "1"});
    check(!reply.changed && reply.text.find("No value") != std::string::npos, "an unknown key says so");
    reply = run(s, "set", {"frame.file", "My", "hood.png"});
    check(s.hood_file == "My hood.png" && s.hood == Hood::picture, "a picture's name of more than one word");
    reply = run(s, "reset", {"lens"});
    check(s.curve == 0 && !lens_on(s) && s.hood_file == "My hood.png", "reset a tab: only its values");
    reply = run(s, "reset", {"frame"});
    check(s.hood == Hood::none && s.hood_file.empty(), "reset frame takes the picture away too");
    s.panel_bind = 0x300;
    s.curve = 1;
    reply = run(s, "reset", {"all"});
    check(s.curve == 0 && s.panel_bind == 0x300, "reset all keeps the panel's bind");
    reply = run(s, "off", {});
    check(!s.enabled && reply.changed, "off");
    reply = run(s, "on", {});
    check(s.enabled && reply.changed, "on");
    reply = run(s, "bind", {"0x4040"});
    check(s.panel_bind == 0x4040 && reply.changed, "bind takes button bits");
    reply = run(s, "bind", {"default"});
    check(s.panel_bind == 0, "bind default is RB + R3");
    reply = run(s, "list", {});
    check(reply.text.find("LENS:") != std::string::npos && reply.text.find("video.replay=") != std::string::npos, "list names every tab and key");
    reply = run(s, "nonsense", {});
    check(!reply.changed && reply.text.find("no such verb") != std::string::npos, "an unknown verb says so");
}

void json_tests() {
    Settings s;
    s.curve = 1.18f;
    s.circle = 1.623f;
    s.compress = 0.087f;
    s.zoom = 1.37f;
    s.squeeze = 1.2f;
    s.glare = 0.5f;
    s.camera = true;
    s.camera_gamma = 0.5f;
    s.camera_knee = 0.8f;
    s.camera_knee_slope = 0.4f;
    s.camera_black = 0.01f;
    s.camera_clip = 1.0f;
    s.camera_colour = 1.2f;
    s.hood_size = 0.55f;
    s.game_look = true;
    s.game_barrel = 0.3f;
    s.game_barrel_edge = -0.1f;
    s.game_barrel_stretch = 0.2f;
    s.fov_widest = 140;
    s.focus_depth = 0.25f;
    s.fov_mode = FovMode::fixed;
    s.fov = 152;
    s.bars = Bars::four_three;
    s.hood = Hood::picture;
    s.hood_file = "Grain hood.png";
    s.saturation = 1.12f;
    s.warmth = -0.36f;
    s.light = true;
    s.light_range = 17.5f;
    s.resolution = true;
    s.replay_scale = 1.75f;
    s.panel_bind = 0x8020;
    s.preset = "Mine";
    const auto text = to_json(s).dump(2);
    const auto back = settings_from_json(Json::parse(text));
    check(back == s, "settings come back from their JSON as they were:\n" + text);
    check(settings_from_json(Json::parse("[1,2,3]")) == Settings{}, "not an object: the defaults");
    check(settings_from_json(Json::parse(R"({"values":{"lens.zoom":"a lot","fov.degrees":9999,"lens.mode":"mk1"}})")).fov == 170.0f,
          "out of range is clamped, and what reads is kept");
    const auto partial = settings_from_json(Json::parse(R"({"values":{"lens.curve":1,"lens.squeeze":1}})"));
    check(partial.curve == 1.0f && partial.zoom == 1.0f && partial.circle == Settings{}.circle && partial.compress == 0, "what is missing stays at its default");
    migration_tests();
}

void preset_tests() {
    Preset p;
    p.name = "Test look";
    p.author = "fleep";
    p.settings.curve = 1;
    p.settings.compress = 0.1f;
    p.settings.fov_mode = FovMode::fixed;
    p.settings.fov = 145;
    p.settings.hood = Hood::picture;
    p.settings.hood_file = std::string(mk1_hood_file);
    p.settings.hood_size = 0.5f;
    p.settings.camera = true;
    p.settings.camera_knee = 0.9f;
    p.settings.bars = Bars::four_three;
    p.settings.contrast = 1.1f;
    p.shake = Json::parse(R"({"enabled":true,"default":{"preset":"handheld","intensity":0.5}})");
    Preset back;
    std::string why;
    check(preset_from_json(preset_json(p), back, why), "a preset reads back: " + why);
    check(back.name == p.name && back.author == p.author && back.settings == p.settings && back.shake == p.shake, "every part of a preset reads back");
    Preset other;
    check(!preset_from_json(Json::parse(R"({"name":"x"})"), other, why), "JSON that is not a preset");
    // Names: as their files keep them, and one not taken.
    check(clean_preset_name("  Night: lines!  ") == "Night lines" && clean_preset_name("***").empty(), "a name as a preset keeps it");
    check(clean_preset_name(std::string(60, 'a')).size() == 40, "a name is at most 40 letters");
    check(same_preset_file("My look", "my look") && !same_preset_file("My look", "My look 2"), "names alike whatever their case are one file");
    const std::vector<std::string> taken{"My look", "my look 2", "Other"};
    check(unused_preset_name("My look", taken) == "My look 3" && unused_preset_name("New", taken) == "New" && unused_preset_name("", taken) == "Preset",
          "a name not taken: a number after it");
    const std::vector<std::string> long_one{std::string(40, 'b')};
    check(unused_preset_name(long_one[0], long_one).size() == 40, "a long name taken: its number within 40 letters");
    // Loading keeps the panel's own.
    Settings current;
    current.panel_bind = 0x300;
    current.enabled = true;
    const auto loaded = apply_preset(current, p);
    check(loaded.curve == 1 && loaded.hood_size == 0.5f && loaded.camera_knee == 0.9f && loaded.panel_bind == 0x300 && loaded.preset == "Test look",
          "loading takes everything but the panel's own");
    check(preset_file_name("Cam / Lens: *night*") == "Cam  Lens night.json", "a file name without the characters Windows refuses");
    check(preset_file_name("...") == "Preset.json", "an empty name is Preset");
    for (const auto& built : built_in_presets()) {
        Preset read;
        check(preset_from_json(preset_json(built), read, why) && read.settings == clamped(built.settings), "built-in preset " + built.name + " reads back");
        check(clamped(built.settings) == built.settings, "built-in preset " + built.name + " is within every range");
        check(preset_file_name(built.name) == built.name + ".json", "built-in preset " + built.name + ": its file has its name");
        // Every value of it on its slider's steps: anyone can set it by hand.
        for (const auto& f : fields()) {
            if (f.kind != Kind::number || f.logarithmic) continue;
            const float v = f.get(built.settings), steps = (v - f.low) / f.step;
            check(std::abs(steps - std::round(steps)) < 0.01f, std::format("built-in preset {}: {} ({}) is on its slider's steps", built.name, f.key, v));
        }
        // The lens is what its settings say: at its field of view the game's picture feeds all of it (nothing fitted in
        // for it), on the screen it is made for.
        const float screen = built.settings.replay_aspect == Aspect::four_three ? 4.0f / 3.0f : 16.0f / 9.0f;
        const auto f = lens::frame_for(built.settings, screen, radians(built.settings.fov));
        check(built.settings.fov_mode == FovMode::fixed, "built-in preset " + built.name + ": a fixed field of view");
        check(lens::fit(f) * f.zoom >= 0.9999f, std::format("built-in preset {}: the game's picture feeds all of the lens ({:.5f})", built.name,
                                                           lens::fit(f) * f.zoom));
        check(built.settings.hood == Hood::none && built.settings.hood_file.empty() && lens::glass_for(built.settings).none() && !built.settings.camera,
              "built-in preset " + built.name + ": a plain lens (no overlay, glass or camera curve)");
        fit_is_tight(f, "built-in preset " + built.name);
    }
    check(built_in_presets().size() == 2, "two built-in presets");
    const auto older = older_built_in_names(), retired = retired_built_in_names();
    for (const auto& name : retired) {
        check(std::find(older.begin(), older.end(), name) != older.end(), name + ": a retired built-in was one");
        for (const auto& built : built_in_presets()) check(built.name != name, name + ": a retired built-in is not one now");
    }
}

// The fit's promise, for any lens: every point that shows lands inside what the game drew, and the
// fit is the widest that does (some point lands on its edge), unless the lens's own limit holds it.
void fit_is_tight(const lens::Frame& f, const std::string& what) {
    const float fit = lens::fit(f);
    check(fit > 0 && std::isfinite(fit), what + ": a fit");
    // How far out in the game's picture the edge of what shows lands, along a direction.
    const auto reach = [&](float phi) {
        const float dx = std::cos(phi), dy = std::sin(phi);
        const float rb = lens::visible_radius(f, std::abs(dx), std::abs(dy));
        const auto uv = lens::to_game(f, fit, dx * rb, dy * rb);
        return std::max(std::abs(uv[0] - 0.5f) * 2, std::abs(uv[1] - 0.5f) * 2);
    };
    // Every 0.05 degrees, then finely about the furthest few (what shows has corners, where the window's edge and
    // the overlay's meet: the fit is tightest there, and a step to either side may fall well short of it).
    constexpr int steps = 7200;
    std::vector<float> coarse(steps);
    for (int i = 0; i < steps; ++i) coarse[static_cast<std::size_t>(i)] = reach(static_cast<float>(i) * 2 * lens::pi / steps);
    float furthest = *std::max_element(coarse.begin(), coarse.end());
    std::vector<int> peaks;
    for (int i = 0; i < steps; ++i) {
        const float here = coarse[static_cast<std::size_t>(i)];
        if (here >= coarse[static_cast<std::size_t>((i + steps - 1) % steps)] && here >= coarse[static_cast<std::size_t>((i + 1) % steps)]) peaks.push_back(i);
    }
    std::sort(peaks.begin(), peaks.end(), [&](int a, int b) { return coarse[static_cast<std::size_t>(a)] > coarse[static_cast<std::size_t>(b)]; });
    if (peaks.size() > 12) peaks.resize(12);
    for (const int i : peaks)
        for (int j = -200; j <= 200; ++j) furthest = std::max(furthest, reach((static_cast<float>(i) + static_cast<float>(j) / 200.0f) * 2 * lens::pi / steps));
    check(furthest <= 1.0005f, std::format("{}: everything that shows is inside the game's picture ({:.4f})", what, furthest));
    const bool held = f.curve >= 1 ? fit >= 0.9999f : fit >= 0.9999f / std::max(f.curve * f.curve, 1e-4f);
    check(held || furthest >= 0.99f, std::format("{}: the fit is the widest it can be ({:.4f} of the edge)", what, furthest));
}

void curve_tests() {
    // The projection family: each curve's angle and radius are each other's inverse.
    for (const float curve : {0.0f, 0.25f, 0.5f, 0.75f, 0.9995f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f})
        for (const float theta : {0.05f, 0.3f, 0.7f, 1.0f, 1.3f, 1.5f}) {
            const float x = lens::curve_radius(curve, theta);
            check(near(lens::curve_angle(curve, x), theta, 2e-4), std::format("curve {} turns back: {} -> {} -> {}", curve, theta, x,
                                                                              lens::curve_angle(curve, x)));
        }
    check(near(lens::curve_angle(0, 1), std::atan(1.0f), 1e-6), "curve 0 is rectilinear");
    check(near(lens::curve_angle(1.5f, 1.41421356f), lens::pi / 2, 1e-4), "curve 1.5 is equisolid: 90 degrees at x = sqrt 2");
    check(near(lens::curve_angle(2, 1), lens::pi / 2, 1e-4), "curve 2 is orthographic: 90 degrees at x = 1");
    // From equidistant up, 90 degrees falls where the real lens has it, at fit 1 (with 4:3 bars, where the
    // lens never runs out of angle inside the window).
    for (const float distortion : {1.0f, 1.3f, 1.7f}) {
        Settings s;
        s.circle = mk1_corner_circle; // 90 degrees at the 4:3 corner
        s.bars = Bars::four_three;
        s.curve = distortion;
        const auto frame = lens::frame_for(s, 16.0f / 9.0f, radians(120));
        check(near(lens::theta(frame, 5.0f / 3.0f, 1.0f), lens::pi / 2, 1e-4), std::format("the Mk1 at {:.0f}%: 90 degrees at its 4:3 corner", distortion * 100));
        s.hood = Hood::circle; // what shows ends at its circle
        s.circle = 1.4f;
        const auto modern = lens::frame_for(s, 16.0f / 9.0f, radians(120));
        check(near(lens::theta(modern, 1.4f, 1.0f), lens::pi / 2, 1e-4), std::format("the modern lens at {:.0f}%: 90 degrees at its circle", distortion * 100));
    }
    const float aspect = 16.0f / 9.0f;
    // Distortion 100% is the lens as it was (the Mk1's tests above); every distortion keeps the fit's promise.
    for (const bool modern : {false, true})
        for (const auto bars : {Bars::none, Bars::four_three})
            for (const float fov : {90.0f, 120.0f, 156.0f})
                for (const float distortion : {0.0f, 0.3f, 0.7f, 1.0f, 1.2f, 1.6f, 2.0f})
                    for (const float squeeze : {1.0f, 0.8f, 1.33f}) {
                        if (distortion == 0 && squeeze == 1) continue; // no lens (below)
                        Settings s;
                        s.bars = bars;
                        s.hood = modern ? Hood::circle : Hood::picture;
                        s.hood_file = std::string(mk1_hood_file);
                        s.circle = modern ? modern_circle : mk1_circle;
                        s.curve = distortion;
                        s.squeeze = squeeze;
                        const auto hole = hood_hole();
                        fit_is_tight(lens::frame_for(s, aspect, radians(fov), &hole),
                                     std::format("{} at {:.0f}%, FOV {:.0f}{}, squeeze {:.2f}", modern ? "modern" : "Mk1", distortion * 100, fov,
                                                 bars == Bars::none ? "" : ", 4:3", squeeze));
                    }
    {
        // At 200% (orthographic) the lens would run out of angle right at the 4:3 corner: it is held a
        // little short (78 degrees there at fit 1), and still turns evenly outwards all the way.
        Settings s;
        s.circle = mk1_corner_circle;
        s.bars = Bars::four_three;
        s.curve = 2;
        const auto frame = lens::frame_for(s, 16.0f / 9.0f, radians(120));
        const float corner = lens::theta(frame, 5.0f / 3.0f, 1.0f);
        check(corner > radians(75) && corner < lens::pi / 2, std::format("the Mk1 at 200%: {:.1f} degrees at its corner", corner * 180 / lens::pi));
        float last = 0;
        for (int i = 1; i <= 100; ++i) {
            const float th = lens::theta(frame, 5.0f / 3.0f * static_cast<float>(i) / 100.0f, 1.0f);
            check(th > last, "the Mk1 at 200% turns outwards all the way to its corner");
            last = th;
        }
    }
    // Distortion 0 (and nothing else of the lens): no lens, the game's own picture, pixel for pixel.
    Settings flat;
    flat.curve = 0;
    check(!lens_on(flat), "Distortion 0%: no lens");
    auto f = lens::frame_for(flat, aspect, radians(120));
    const float fit = lens::fit(f);
    for (const float qy : {-0.8f, -0.3f, 0.0f, 0.5f, 0.9f}) {
        const float v = lens::to_game(f, fit, 0.0f, qy)[1];
        for (const float qx : {-1.6f, -0.9f, 0.4f, 1.2f, 1.7f})
            check(near(lens::to_game(f, fit, qx, qy)[1], v, 2e-4), "Distortion 0: a level line stays level");
    }
    for (const float qx : {-1.7f, -0.5f, 0.6f, 1.7f})
        for (const float qy : {-0.95f, 0.2f, 0.95f}) {
            const auto uv = lens::to_game(f, fit, qx, qy);
            check(near(uv[0], 0.5f + 0.5f * qx / aspect, 1e-6) && near(uv[1], 0.5f - 0.5f * qy, 1e-6), "Distortion 0, whole screen: the game's own picture");
        }
    // More distortion bends more: the middle grows (the fit falls), the edges squeeze.
    float before = 1e9f;
    for (const float distortion : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
        Settings s;
        s.bars = Bars::four_three;
        s.curve = distortion;
        const auto frame = lens::frame_for(s, aspect, radians(120));
        // The middle's magnification: how much of the game's picture one step out from the middle covers.
        const float middle = (lens::to_game(frame, lens::fit(frame), 0.01f, 0)[0] - 0.5f) / 0.01f;
        check(middle < before, std::format("more distortion, a bigger middle ({:.0f}%: {:.4f})", distortion * 100, middle));
        before = middle;
    }
    // The glass: nothing by default; a lens of its own, bent or not.
    Settings g;
    check(lens::glass_for(g).none() && !lens_on(g), "no glass by default");
    g.fringe = 1;
    g.shade = 1;
    check(lens_on(g) && !lens::glass_for(g).none() && lens::glass_for(g).fringe == 1, "glass with no distortion");
    g.enabled = false;
    check(!lens_on(g) && lens::glass_for(g).none(), "Replay Plus off: no glass");
}

void lens_tests() {
    // Reference fits: FOV 120, 4:3 with its hood, 0.844; FOV 140, 0.933.
    // (Its Mk1 has 90 degrees at the 4:3 corner: mk1_corner_circle. It fits to an ellipse about the hood's black
    // corners, which the tests' hood picture is: measured pixel by pixel, a fit within a little of it.)
    Settings s;
    s.curve = 1;
    s.circle = mk1_corner_circle;
    s.bars = Bars::four_three;
    s.hood = Hood::picture;
    s.hood_file = std::string(mk1_hood_file);
    const auto hole = hood_hole();
    const float aspect = 16.0f / 9.0f;
    auto f = lens::frame_for(s, aspect, radians(120), &hole);
    check(near(lens::fit(f), 0.844, 0.006), std::format("adapter fit at FOV 120, 4:3 with the hood: {:.4f} (0.844)", lens::fit(f)));
    f = lens::frame_for(s, aspect, radians(140), &hole);
    check(near(lens::fit(f), 0.933, 0.006), std::format("adapter fit at FOV 140: {:.4f} (0.933)", lens::fit(f)));
    // The real lens at last: the fit stops at 1 once the field of view feeds the whole Mk1.
    float whole = 0;
    for (float fov = 100; fov <= 170; fov += 0.5f)
        if (lens::fit(lens::frame_for(s, aspect, radians(fov), &hole)) >= 0.9999f) {
            whole = fov;
            break;
        }
    check(whole > 140 && whole < 170, std::format("the whole Mk1 needs a field of view of about {:.1f}", whole));
    std::cout << std::format("  (the whole Mk1, 4:3 with its hood, needs a vertical field of view of {:.1f} degrees)\n", whole);
    // A modern fisheye's circle (1.54 at FOV 120): fit 0.776 from 65 directions;
    // 0.771 with the point where the circle meets the 4:3 window's top, which they step over (it lands
    // 0.4 degrees outside the game's picture at 0.776).
    Settings m;
    m.curve = 1.5f;
    m.bars = Bars::four_three;
    m.hood = Hood::circle;
    m.circle = 1.54f;
    f = lens::frame_for(m, aspect, radians(120));
    check(near(lens::fit(f), 0.7713, 0.0005), std::format("modern fit at FOV 120, circle 1.54: {:.4f} (0.7713; 65 directions: 0.776)", lens::fit(f)));
    fit_is_tight(f, "modern, FOV 120, circle 1.54");
    // And its projection is equisolid: theta = fit 2 asin(r / (sqrt 2 C)).
    for (const float r : {0.2f, 0.7f, 1.2f, 1.5f})
        check(near(lens::theta(f, r, 0.776f), 0.776f * 2 * std::asin(r / (1.41421356f * 1.54f)), 1e-5), "the modern lens is equisolid");
    // Zoom divides the fit.
    const float unzoomed = lens::fit(lens::frame_for(s, aspect, radians(120), &hole));
    s.zoom = 2;
    f = lens::frame_for(s, aspect, radians(120), &hole);
    check(near(lens::fit(f), unzoomed / 2, 1e-5), "zoom 2 halves the fit");
    s.zoom = 1;
    // The middle maps to the middle; left and right, up and down mirror.
    f = lens::frame_for(s, aspect, radians(120), &hole);
    const float fit = lens::fit(f);
    auto c = lens::to_game(f, fit, 0, 0);
    check(near(c[0], 0.5) && near(c[1], 0.5), "the middle is the middle");
    const auto l = lens::to_game(f, fit, -0.7f, 0.3f), r = lens::to_game(f, fit, 0.7f, 0.3f), d = lens::to_game(f, fit, 0.7f, -0.3f);
    check(near(l[0], 1 - r[0]) && near(l[1], r[1]) && near(d[1], 1 - r[1]), "the lens mirrors");
    // Every point that shows lands inside what the game drew (the fit's promise): along the window's
    // edge and the hood's, whichever is nearer.
    for (int i = 0; i <= 360; ++i) {
        const float phi = static_cast<float>(i) * lens::pi / 180.0f;
        const float dx = std::cos(phi), dy = std::sin(phi);
        const float rb = lens::visible_radius(f, std::abs(dx), std::abs(dy));
        const auto uv = lens::to_game(f, fit, dx * rb * 0.999f, dy * rb * 0.999f);
        check(uv[0] >= -1e-3f && uv[0] <= 1.001f && uv[1] >= -1e-3f && uv[1] <= 1.001f, "every point that shows is inside the game's picture");
    }
    // Equidistant: equal steps out are equal steps of angle.
    const auto tg = lens::tangents(f);
    const auto at = [&](float q) { return std::atan((lens::to_game(f, fit, q, 0)[0] - 0.5f) * 2 * tg[0]); };
    check(near(at(0.4f) - at(0.2f), at(0.2f) - at(0.0f), 1e-4), "the Mk1 is equidistant");
    // Its window: 4:3 inside 16:9.
    const auto w = lens::window(f);
    check(near(w[0], 0.75) && near(w[1], 1), "a 4:3 window on a 16:9 screen");
    curve_tests();
}

void grade_tests() {
    const lens::Grade neutral;
    check(neutral.identity(), "the default grade changes nothing");
    const std::array<float, 3> c{0.2f, 0.5f, 0.8f};
    auto out = lens::graded(neutral, c, 0.7f);
    check(near(out[0], c[0]) && near(out[1], c[1]) && near(out[2], c[2]), "graded by the default: as it was");
    lens::Grade g;
    g.exposure = 1;
    out = lens::graded(g, c, 0);
    check(near(out[0], 0.4) && near(out[1], 1.0) && near(out[2], 1.6), "a stop up doubles");
    lens::Grade grey;
    grey.saturation = 0;
    out = lens::graded(grey, c, 0);
    check(near(out[0], out[1]) && near(out[1], out[2]), "no saturation: grey");
    for (const float warmth : {-1.0f, -0.4f, 0.3f, 1.0f}) {
        const auto b = lens::balance(warmth, 0.5f);
        check(near(0.2126 * b[0] + 0.7152 * b[1] + 0.0722 * b[2], 1.0, 1e-5), "white balance keeps the brightness");
    }
    check(lens::balance(1, 0)[0] > 1 && lens::balance(1, 0)[2] < 1, "warmer is redder and less blue");
    lens::Grade contrast;
    contrast.contrast = 1.4f;
    out = lens::graded(contrast, {0.18f, 0.18f, 0.18f}, 0);
    check(near(out[0], 0.18, 1e-5), "contrast turns about mid grey");
    lens::Grade vignette;
    vignette.vignette = 1;
    check(lens::graded(vignette, c, 0)[1] == c[1] && lens::graded(vignette, c, 1)[1] < c[1] * 0.2f, "the vignette darkens the corners only");
    for (int i = 0; i <= 255; ++i) {
        const float v = static_cast<float>(i) / 255.0f;
        check(near(lens::linear_to_srgb(lens::srgb_to_linear(v)), v, 1e-5), "sRGB round trip");
    }
}

void finish_tests() {
    const std::uint32_t w = 320, h = 180, pitch = w * 4 + 64;
    std::vector<std::uint8_t> picture(std::size_t{pitch} * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = picture.data() + std::size_t{y} * pitch + x * 4;
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<std::uint8_t>((x + y) % 256);
            p[3] = 255;
        }
    std::vector<std::uint8_t> scratch;
    // Nothing asked: nothing changes.
    Settings none;
    auto finish = lens::finish_for(none, 16.0f / 9.0f, radians(120));
    check(!lens::changes(finish), "the defaults change nothing");
    auto copy = picture;
    lens::finish(finish, {copy.data(), w, h, pitch, true}, scratch, 4);
    check(copy == picture, "finishing with the defaults leaves the frame as it was");
    // 4:3 bars: black at the sides, the middle untouched.
    Settings bars;
    bars.bars = Bars::four_three;
    finish = lens::finish_for(bars, 16.0f / 9.0f, radians(120));
    copy = picture;
    lens::finish(finish, {copy.data(), w, h, pitch, true}, scratch, 4);
    const auto px = [&](const std::vector<std::uint8_t>& v, std::uint32_t x, std::uint32_t y) { return v.data() + std::size_t{y} * pitch + x * 4; };
    check(px(copy, 2, 90)[0] == 0 && px(copy, 2, 90)[1] == 0 && px(copy, 317, 90)[2] == 0, "4:3 bars are black");
    check(px(copy, 160, 90)[0] == px(picture, 160, 90)[0] && px(copy, 160, 90)[1] == px(picture, 160, 90)[1], "inside the bars, untouched");
    check(px(copy, 160, 90)[3] == 255, "alpha is left as it was");
    // The Mk1: the middle shows the middle (enlarged), and the frame stays the size it was.
    Settings mk1;
    mk1.curve = 1;
    mk1.bars = Bars::four_three;
    mk1.hood = Hood::circle;
    mk1.circle = modern_circle; // a round picture inside the 4:3 window (the Mk1's own circle reaches its corners)
    finish = lens::finish_for(mk1, 16.0f / 9.0f, radians(120));
    check(lens::changes(finish), "the Mk1 changes the frame");
    copy = picture;
    lens::finish(finish, {copy.data(), w, h, pitch, false}, scratch, 3);
    check(std::abs(px(copy, 160, 90)[0] - px(picture, 160, 90)[0]) <= 3 && std::abs(px(copy, 160, 90)[1] - px(picture, 160, 90)[1]) <= 3,
          "the middle of the Mk1's picture is the middle of the game's");
    check(px(copy, 42, 1)[0] == 0 && px(copy, 42, 1)[1] == 0, "the image circle's corners are black");
    check(px(copy, 160, 2)[1] > 0, "the top middle is inside the circle");
    // Threads: the same picture whatever their number.
    auto one = picture, many = picture;
    lens::finish(finish, {one.data(), w, h, pitch, false}, scratch, 1);
    lens::finish(finish, {many.data(), w, h, pitch, false}, scratch, 7);
    check(one == many, "one thread or seven: the same frame");
    // The grade on its own, in blue-first order: warmth reddens the red channel (the third byte).
    Settings warm;
    warm.warmth = 1;
    finish = lens::finish_for(warm, 16.0f / 9.0f, radians(120));
    copy = picture;
    lens::finish(finish, {copy.data(), w, h, pitch, true}, scratch, 2);
    const auto* before = px(picture, 200, 100);
    const auto* after = px(copy, 200, 100);
    check(after[2] >= before[2] && after[0] <= before[0], "warmer: more red and less blue, in blue-first order");
    // The glass on the Mk1: the middle as it was, the corners darker (shade), red and blue apart (fringe),
    // the edges softer (soft).
    Settings plain;
    plain.curve = 1;
    plain.bars = Bars::four_three;
    // On fine detail (a smooth ramp blurs to itself): squares of three pixels, light and dark.
    auto detail = picture;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* q = detail.data() + std::size_t{y} * pitch + x * 4;
            const bool light = ((x / 3) + (y / 3)) % 2 == 0;
            q[0] = light ? 220 : 30;
            q[1] = light ? 200 : 40;
            q[2] = light ? 240 : 20;
        }
    picture = detail;
    auto clear = picture;
    lens::finish(lens::finish_for(plain, 16.0f / 9.0f, radians(120)), {clear.data(), w, h, pitch, true}, scratch, 4);
    for (const auto& [name, set] : std::vector<std::pair<const char*, std::function<void(Settings&)>>>{
             {"shade", [](Settings& s) { s.shade = 2; }}, {"fringe", [](Settings& s) { s.fringe = 4; }}, {"soft", [](Settings& s) { s.soft = 1; }}}) {
        Settings glass = plain;
        set(glass);
        auto shot = picture;
        lens::finish(lens::finish_for(glass, 16.0f / 9.0f, radians(120)), {shot.data(), w, h, pitch, true}, scratch, 4);
        const auto* mid = px(shot, 160, 90);
        const auto* was = px(clear, 160, 90);
        check(std::abs(mid[0] - was[0]) <= 2 && std::abs(mid[1] - was[1]) <= 2 && std::abs(mid[2] - was[2]) <= 2,
              std::string(name) + ": the middle is as it was");
        int changed = 0;
        for (std::uint32_t y = 4; y < h - 4; y += 3)
            for (const std::uint32_t x : {44u, 50u, 270u, 276u})
                for (int k = 0; k < 3; ++k) changed += std::abs(px(shot, x, y)[k] - px(clear, x, y)[k]) > 2;
        check(changed > 20, std::format("{}: the edges change ({} values)", name, changed));
        if (std::string(name) == "shade") {
            int darker = 0, total = 0;
            for (std::uint32_t y = 4; y < 30; ++y)
                for (std::uint32_t x = 44; x < 70; ++x, ++total)
                    darker += px(shot, x, y)[1] <= px(clear, x, y)[1];
            check(darker == total, "shade: a corner only gets darker");
        }
        if (std::string(name) == "fringe") {
            int green = 0;
            for (std::uint32_t y = 4; y < h - 4; y += 3)
                for (const std::uint32_t x : {44u, 50u, 270u, 276u}) green += std::abs(px(shot, x, y)[1] - px(clear, x, y)[1]) > 2;
            check(green == 0, "fringe: green stays where it was");
        }
    }
}

void focus_tests() {
    Settings s;
    check(s.focus == Focus::off && !wants_game_settings(s), "no focus by default");
    s.enabled = true;
    s.focus = Focus::fixed;
    check(wants_game_settings(s), "a focus holds the game's settings");
    // A distance steps by a share of itself and shows two figures.
    const auto& d = *field("focus.distance");
    check(d.logarithmic, "the focus distance steps by a share of itself");
    s.focus_distance = 3.0f;
    check(shown(d, s) == "3.0 m", "3 m shows as " + shown(d, s));
    step(d, s, 1);
    check(near(s.focus_distance, 3.1f, 1e-4), std::format("3 m a step out is 3.1 m: {}", s.focus_distance));
    s.focus_distance = 9.9f;
    step(d, s, 1);
    check(near(s.focus_distance, 10.0f, 1e-4), std::format("9.9 m a step out is 10 m: {}", s.focus_distance));
    step(d, s, 1);
    check(near(s.focus_distance, 11.0f, 1e-4) && shown(d, s) == "11 m", std::format("10 m a step out is 11 m: {}", s.focus_distance));
    s.focus_distance = 0.84f;
    check(shown(d, s) == "0.84 m", "0.84 m shows as " + shown(d, s));
    step(d, s, -1);
    check(near(s.focus_distance, 0.81f, 1e-4), std::format("0.84 m a step in is 0.81 m: {}", s.focus_distance));
    step(d, s, -500);
    check(s.focus_distance == d.low, "a step in stops at the nearest");
    step(d, s, 500);
    check(s.focus_distance == d.high, "a step out stops at the furthest");
    // Keys: in time order, one a frame; eased between, held before the first and after the last.
    Settings k;
    k.focus = Focus::keys;
    k.focus_distance = 7;
    k.focus_blur = 0.25f;
    check(focus_at(k, 12).distance == 7 && focus_at(k, 12).blur == 0.25f, "no keys: the fixed focus");
    set_focus_key(k, 3.0, 10.0f, 1.5f);
    set_focus_key(k, 1.0, 2.0f, 0.5f);
    check(k.focus_keys.size() == 2 && k.focus_keys[0].time == 1.0 && k.focus_keys[1].time == 3.0, "keys are kept in time order");
    set_focus_key(k, 1.01, 2.5f, 0.5f);
    check(k.focus_keys.size() == 2 && k.focus_keys[0].distance == 2.5f, "a key within a frame of one is that one, changed");
    set_focus_key(k, 1.0, 2.0f, 0.5f);
    check(near(focus_at(k, 0).distance, 2) && near(focus_at(k, 1).distance, 2) && near(focus_at(k, 0.5).blur, 0.5), "before the first key: the first");
    check(near(focus_at(k, 5).distance, 10) && near(focus_at(k, 3).blur, 1.5), "after the last key: the last");
    const auto middle = focus_at(k, 2);
    check(near(middle.distance, 1.0 / ((0.5 + 0.1) / 2), 1e-3) && near(middle.blur, 1.0, 1e-4),
          std::format("half way: half way in how near it looks ({} m), and in blur ({})", middle.distance, middle.blur));
    check(focus_at(k, 1.2).distance < focus_at(k, 1.4).distance && focus_at(k, 1.4).distance < focus_at(k, 2.6).distance, "it turns one way");
    check(near(focus_at(k, 1.0001).distance, 2, 1e-3) && near(focus_at(k, 2.9999).distance, 10, 1e-2), "eased: it leaves and arrives gently");
    const auto* here = focus_key_at(k, 3.02);
    check(here && here->distance == 10.0f && !focus_key_at(k, 2.5), "the key at a time, within a frame");
    Settings fixed = k;
    fixed.focus = Focus::fixed;
    check(focus_at(fixed, 2).distance == 7, "fixed: the keys wait");
    // Saved with the settings (not with presets).
    k.enabled = true;
    const auto back = settings_from_json(Json::parse(to_json(k).dump(2)));
    check(back == k && back.focus_keys.size() == 2, "focus keys come back from the settings' JSON");
    Preset p;
    p.name = "Look";
    p.settings = k;
    Preset read;
    std::string why;
    check(preset_from_json(preset_json(p), read, why) && read.settings.focus_keys.empty() && read.settings.focus == Focus::off,
          "a preset carries no focus: it belongs to a replay");
    check(preset_json(p).at("values").contains("lens.curve") && !preset_json(p).at("values").contains("focus.mode"), "a preset's values leave the focus out");
    Settings current;
    current.focus = Focus::autofocus;
    current.focus_blur = 2.5f;
    set_focus_key(current, 4.0, 5.0f, 1.0f);
    const auto loaded = apply_preset(current, p);
    check(loaded.focus_keys == current.focus_keys && loaded.focus == Focus::autofocus && loaded.focus_blur == 2.5f,
          "loading a look keeps the replay's focus and its keys");
    // Taken away: the nearest within reach.
    check(!remove_focus_key(k, 2.0, 0.25) && k.focus_keys.size() == 2, "no key in reach: none taken");
    check(remove_focus_key(k, 2.9, 0.25) && k.focus_keys.size() == 1 && k.focus_keys[0].time == 1.0, "the key in reach is taken");
    // At most so many; a key past the end of what is kept is not made.
    Settings many;
    for (int i = 0; i < 300; ++i) set_focus_key(many, i * 0.1, 3, 1);
    check(many.focus_keys.size() == max_focus_keys, "focus keys are kept to so many");
    // Hostile JSON: no time, not numbers, too many.
    const auto odd = settings_from_json(Json::parse(R"({"focus_keys":[{"distance":3},{"time":"x"},{"time":2,"distance":1e9,"blur":-5},7]})"));
    check(odd.focus_keys.size() == 1 && odd.focus_keys[0].distance == 200.0f && odd.focus_keys[0].blur == 0.0f, "odd focus keys: only what makes sense");
    // The verbs: set and step them as any other value.
    Settings v;
    auto reply = run(v, "set", {"focus.mode", "keys"});
    check(reply.changed && v.focus == Focus::keys, "set focus.mode keys: " + reply.text);
    reply = run(v, "set", {"focus.mode", "Auto"});
    check(reply.changed && v.focus == Focus::autofocus, "focus.mode by its label");
    reply = run(v, "set", {"focus.distance", "12.5", "m"});
    check(reply.changed && near(v.focus_distance, 12.5f), "set focus.distance 12.5 m: " + reply.text);
    reply = run(v, "set", {"focus.blur", "150%"});
    check(reply.changed && near(v.focus_blur, 1.5f), "set focus.blur 150%");
    reply = run(v, "set", {"lens.curve", "80%"});
    check(reply.changed && near(v.curve, 0.8f), "set lens.curve 80%");
}
} // namespace

// Shake keys (on and off along the replay) and how Replay Shake fades them (replay_shake_model.h gate_at).
void shake_key_tests() {
    Settings s;
    check(shake_on_at(s, 5.0, true) && !shake_on_at(s, 5.0, false), "no keys: the switch holds");
    check(!toggle_shake_key(s, 2.0, true) && s.shake_keys.size() == 1 && !s.shake_keys[0].on, "switched off at 2 s");
    check(shake_on_at(s, 1.0, true) && !shake_on_at(s, 3.0, true), "on before the first key (the other way), off after");
    check(toggle_shake_key(s, 4.0, true) && s.shake_keys.size() == 2, "on again at 4 s");
    check(!shake_on_at(s, 3.9, true) && shake_on_at(s, 4.0, true) && shake_on_at(s, 9.0, true), "off between, on after");
    check(!toggle_shake_key(s, 4.01, true) && s.shake_keys.size() == 1, "switching at a key takes it away");
    check(!toggle_shake_key(s, 1.0, true) && s.shake_keys.size() == 1 && s.shake_keys[0].time == 1.0 && !s.shake_keys[0].on,
          "switched off earlier: the later off key changes nothing, and goes");
    s.shake_keys = {{1.0, true}, {2.0, true}, {3.0, false}, {4.0, false}, {5.0, true}};
    tidy_shake_keys(s);
    check(s.shake_keys.size() == 3 && s.shake_keys[1].time == 3.0, "keys that change nothing are dropped");
    check(remove_shake_key(s, 3.02) && s.shake_keys.size() == 1, "removing a key drops the one it made pointless");
    check(!remove_shake_key(s, 9.0), "no key there");
    Settings j;
    j.shake_keys = {{1.5, false}, {6.25, true}};
    const auto back = settings_from_json(to_json(j));
    check(back.shake_keys == j.shake_keys, "shake keys are saved and read back");
    Preset preset;
    preset.settings.shake_keys = {{9.0, true}};
    check(apply_preset(j, preset).shake_keys == j.shake_keys, "a preset leaves the replay's shake keys");

    using replay_shake::Gate;
    const std::vector<Gate> none;
    check(replay_shake::gate_at(none, 3.0) == 1.0f, "no gates: all of the shake");
    const std::vector<Gate> gates{{2.0, false}, {4.0, true}};
    check(replay_shake::gate_at(gates, 1.0) == 1.0f, "before an off key: on");
    check(replay_shake::gate_at(gates, 2.0) == 1.0f && near(replay_shake::gate_at(gates, 2.1), 0.5f), "fading out over 0.2 s");
    check(replay_shake::gate_at(gates, 2.2) == 0.0f && replay_shake::gate_at(gates, 3.9) == 0.0f, "off");
    check(replay_shake::gate_at(gates, 4.2) == 1.0f && replay_shake::gate_at(gates, 99.0) == 1.0f, "on again");
    const std::vector<Gate> on_first{{3.0, true}};
    check(replay_shake::gate_at(on_first, 0.0) == 0.0f && replay_shake::gate_at(on_first, 3.5) == 1.0f, "before an on key: off");
    const auto o = replay_shake::gated(replay_shake::Offsets{1, 2, 3}, 0.5f);
    check(near(o[0], 0.5f) && near(o[1], 1.0f) && near(o[2], 1.5f), "offsets scale with the gate");
}

// Auto's field of view, the middle's enlargement, the focus's ranges, and the camcorder curve.
void view_tests() {
    const float aspect = 16.0f / 9.0f;
    Settings s;
    s.curve = 1;
    // A lens with 90 degrees at the 4:3 corner, under the hood: a game picture can feed all of
    // it. The real one's 90 degrees at the hood's edge it never can (edge compression is for that).
    s.circle = mk1_corner_circle;
    s.bars = Bars::four_three;
    s.hood = Hood::picture;
    s.hood_file = std::string(mk1_hood_file);
    const auto hole = hood_hole();
    const float fov = lens::auto_fov(s, aspect, 170, &hole);
    const auto at = lens::frame_for(s, aspect, fov, &hole);
    check(lens::fit(at) >= 0.999f, std::format("Auto: the whole lens at {:.1f} degrees", fov * 180 / lens::pi));
    const auto narrower = lens::frame_for(s, aspect, fov - radians(1), &hole);
    check(lens::fit(narrower) < 0.999f, "Auto: the narrowest that shows it all");
    check(near(lens::auto_fov(s, aspect, 120, &hole), radians(120), 1e-5), "Auto goes no wider than its widest");
    s.zoom = 1.5f;
    check(lens::auto_fov(s, aspect, 170, &hole) < fov, "zoomed in, Auto needs less");
    s.zoom = 1;
    s.compress = 0.1f;
    check(lens::auto_fov(s, aspect, 170, &hole) < fov, "its edge compressed, Auto needs less");
    s.compress = 0;
    // The middle's enlargement: tan(fov / 2) over fit K, more the wider the field of view.
    float last = 0;
    for (const float degrees : {100.0f, 120.0f, 140.0f, 156.0f, 170.0f}) {
        const auto f = lens::frame_for(s, aspect, radians(degrees));
        const float zoom = lens::centre_zoom(f, lens::fit(f));
        // Against the mapping itself: a small step from the middle in the game's picture's pixels.
        const float step = 0.001f;
        const float game = (0.5f - lens::to_game(f, lens::fit(f), 0, step)[1]) * 2 / step; // game half-heights per picture half-height
        check(near(zoom, 1 / game, 0.002f * zoom), std::format("the middle's enlargement at {:.0f}: {:.3f} (by the mapping {:.3f})", degrees, zoom, 1 / game));
        check(zoom > last, "wider, more enlarged");
        last = zoom;
    }
    Settings none;
    check(lens::centre_zoom(lens::frame_for(none, aspect, radians(120)), 1) == 1, "no fisheye: not enlarged");
    // The focus's ranges: sharp around the distance, blurred beyond, the blur shrunk by the enlargement.
    for (const float distance : {0.5f, 3.0f, 20.0f, 180.0f})
        for (const float depth : {0.02f, 0.15f, 0.6f}) {
            const auto r = focus_ranges({distance, 1.0f}, depth, 1.0f);
            check(r.near_start < r.near_end && r.near_end < distance && distance < r.far_start && r.far_start <= r.far_end,
                  std::format("focus at {} m, sharp zone {}: {:.2f} {:.2f} {:.2f} {:.2f}", distance, depth, r.near_start, r.near_end, r.far_start,
                              r.far_end));
        }
    check(focus_ranges({3, 1}, 0.15f, 1).blur_max == 0.5f && near(focus_ranges({3, 1}, 0.15f, 4).blur_max, 0.125f), "the blur over the enlargement");
    check(focus_ranges({3, 2}, 0.6f, 1).far_end == focus_far_away, "a deep zone blurs only the far distance");
    // The camcorder curve: the game's tone curve undone (its Narkowicz ACES fit), then the camera's.
    const auto aces = [](float x) { return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f); };
    for (const float x : {0.0f, 0.01f, 0.18f, 0.5f, 1.0f, 3.0f})
        check(near(lens::inverse_filmic(aces(x)), x, 1e-3f * std::max(x, 1.0f)), std::format("the filmic curve undone at {}", x));
    const lens::Grade vx;
    const auto grey = lens::camera_curve(vx, {0.18f, 0.18f, 0.18f});
    check(near(grey[0], (1.099f * std::pow(0.18f, 0.45f) - 0.099f - 0.02f) / 0.98f, 1e-5), "mid grey through the camera's curve");
    check(lens::camera_curve(vx, {10, 10, 10})[0] == 1.0f && lens::camera_curve(vx, {0, 0, 0})[0] == 0.0f, "the camera clips white and black");
    lens::Grade off;
    const auto c = lens::graded(off, {0.2f, 0.4f, 0.6f}, 0.5f);
    check(c[0] == 0.2f && c[1] == 0.4f && c[2] == 0.6f, "the grade at its defaults changes nothing");
    lens::Grade cam;
    cam.camera = true;
    check(!cam.identity(), "the camera curve is a grade");
    const auto d = lens::graded(cam, {0.0f, 0.0f, 0.0f}, 0.0f);
    check(d[0] == 0.0f, "black stays black through the camera curve");
    camera_curve_tests();
}

// What an edge compression is: the lens's own angles eased in, the same way every way round (circles stay circles),
// never turning back, never stretched past about 1.3 times the lens (a squeeze of the last band alone smeared the
// game's edge out by the hood, 2026-11-17); and the true angles of settings saved before then, as settings.
void compression_tests() {
    // Saved before 2026-11-17 with true angles: the zoom and edge compression that lens picked for itself.
    Settings old;
    old.curve = 1;
    old.circle = 1.6f;
    old.fov_mode = FovMode::automatic;
    old.fov_widest = 150;
    old.bars = Bars::four_three;
    old.replay_aspect = Aspect::four_three;
    const auto legacy = lens::legacy_true_angles(old, true, 4.0f / 3.0f);
    check(legacy.zoom >= 1.0f && legacy.zoom < 1.2f && legacy.compress > 0.0f && legacy.compress < 0.3f,
          std::format("an old true-angles lens: zoom {:.5f}, compress {:.5f}", legacy.zoom, legacy.compress));
    // A lens wider than the game's picture at 150 degrees, compressed to fit, the hood picture over it.
    Settings lens_set;
    lens_set.curve = 1;
    lens_set.circle = 1.6f;
    lens_set.compress = 0.12f;
    lens_set.fov_mode = FovMode::fixed;
    lens_set.fov = 150;
    lens_set.bars = Bars::four_three;
    lens_set.replay_aspect = Aspect::four_three;
    lens_set.hood = Hood::picture;
    lens_set.hood_file = std::string(mk1_hood_file);
    const auto hole = hood_hole();
    const auto f = lens::frame_for(lens_set, 4.0f / 3.0f, radians(lens_set.fov), &hole);
    const float fit = lens::fit(f);
    check(fit >= 0.9999f, std::format("compressed enough, the lens is all its own (fit {:.6f})", fit));
    // On a 16:9 picture with 4:3 bars: the same picture (a built-in made both ways).
    for (const float size : {1.0f, 0.5f}) {
        Settings four = built_in("Fisheye 4-3").settings, wide = four;
        wide.replay_aspect = Aspect::screen;
        four.hood = wide.hood = Hood::picture;
        four.hood_size = wide.hood_size = size;
        const auto a = lens::frame_for(four, 4.0f / 3.0f, radians(four.fov), &hole), b = lens::frame_for(wide, 16.0f / 9.0f, radians(wide.fov), &hole);
        const float fa = lens::fit(a), fb = lens::fit(b);
        double most = 0;
        for (float qy = -1; qy <= 1.001f; qy += 0.1f)
            for (float qx = -4.0f / 3.0f; qx <= 4.0f / 3.0f + 0.001f; qx += 0.1f) {
                const auto ua = lens::to_game(a, fa, qx, qy), ub = lens::to_game(b, fb, qx, qy);
                most = std::max(most, between_directions(direction_in_game(ua[0], ua[1], a.fov, 4.0 / 3.0), direction_in_game(ub[0], ub[1], b.fov, 16.0 / 9.0)));
            }
        check(fa >= 0.9999f && fb >= 0.9999f && most < radians(0.0005f),
              std::format("4:3 bars on 16:9 are the 4:3 picture (at most {:.6f} degrees apart)", most * 180 / lens::pi));
        check(lens::hood_uv(a, 0.7f, -0.4f) == lens::hood_uv(b, 0.7f, -0.4f), "and the hood where it was");
    }
    // The compression: the lens's own angles at the middle, (1 - compress) of them at 90 degrees, circles round.
    const float k = (lens::pi * 0.5f) / lens_set.circle;
    for (float qx = 0.01f; qx <= 0.05f; qx += 0.01f) {
        const double th = angle_in_game(f, lens::to_game(f, fit, qx, 0.0f));
        check(near(th, k * qx, 0.006 * k * qx + 1e-5), std::format("the lens's own angles at the middle (r {:.2f}: {:.5f} against {:.5f})", qx, th, k * qx));
    }
    {
        Settings wide = lens_set;
        wide.fov = 170;
        const auto g = lens::frame_for(wide, 4.0f / 3.0f, radians(170), &hole);
        const double at90 = angle_in_game(g, lens::to_game(g, 1, 0, lens_set.circle));
        check(near(at90, lens::pi / 2 * (1 - lens_set.compress), 2e-4),
              std::format("compressed {:.1f}%: {:.2f} degrees where the lens sees 90", lens_set.compress * 100, at90 * 180 / lens::pi));
    }
    for (float r = 0.1f; r <= 1.0f; r += 0.1f) {
        const double across = angle_in_game(f, lens::to_game(f, fit, r, 0.0f));
        for (float phi = 0.1f; phi <= lens::pi * 0.5f; phi += 0.1f) {
            const double th = angle_in_game(f, lens::to_game(f, fit, r * std::cos(phi), r * std::sin(phi)));
            check(near(th, across, 1e-3), std::format("circles stay circles (r {:.1f}, phi {:.1f}: {:.4f} against {:.4f})", r, phi, th, across));
        }
    }
    // The hood at full and at half size, each compressed just enough at 150 degrees: out to its corners never
    // turning back, never stretched past about 1.3 times the lens, what shows inside the game's picture; a smaller
    // hood needs more compression, the middle the same.
    float needed[2]{};
    for (const int which : {0, 1}) {
        const float size = which == 0 ? 1.0f : 0.5f;
        Settings v = lens_set;
        v.hood_size = size;
        for (int i = 0; i <= 500; ++i) {
            v.compress = static_cast<float>(i) * 0.001f;
            const auto t = lens::frame_for(v, 4.0f / 3.0f, radians(v.fov), &hole);
            if (lens::fit(t) * t.zoom >= 0.9999f) break;
        }
        needed[which] = v.compress;
        const auto g = lens::frame_for(v, 4.0f / 3.0f, radians(v.fov), &hole);
        const float gfit = lens::fit(g);
        float most_stretch = 0.0f;
        for (float phi = 0.0f; phi <= lens::pi * 0.5f; phi += 0.02f) {
            double last = 0;
            float last_r = 0.0f;
            const float shows_to = lens::visible_radius(g, std::cos(phi), std::sin(phi));
            for (float r = 0.02f; r <= shows_to; r += 0.02f) {
                const auto uv = lens::to_game(g, gfit, r * std::cos(phi), r * std::sin(phi));
                const double th = angle_in_game(g, uv);
                check(th >= last - 1e-4, std::format("compressed, never turning back (phi {:.2f}, r {:.2f})", phi, r));
                if (last_r > 0.0f && th < 1.5) most_stretch = std::max(most_stretch, static_cast<float>(k * (r - last_r) / std::max(th - last, 1e-6)));
                last = th;
                last_r = r;
                check(uv[0] >= -1e-3f && uv[0] <= 1.001f && uv[1] >= -1e-3f && uv[1] <= 1.001f,
                      std::format("what shows is inside the game's picture (hood {:.0f}%, phi {:.2f}, r {:.2f}: {:.3f}, {:.3f})", size * 100, phi, r, uv[0], uv[1]));
            }
        }
        check(most_stretch > 1.05f && most_stretch < 1.4f, std::format("never stretched past about 1.3 times the lens ({:.2f}, hood {:.0f}%)", most_stretch,
                                                                       size * 100));
    }
    check(needed[1] > needed[0], std::format("a smaller hood shows more, so needs more compression ({:.3f} against {:.3f})", needed[1], needed[0]));
}

// The overlay's size and its picture's see-through middle.
void hood_tests() {
    const auto hole = hood_hole();
    Settings s;
    s.curve = 1;
    s.circle = 1.6f;
    s.bars = Bars::four_three;
    s.hood = Hood::picture;
    s.hood_file = std::string(mk1_hood_file);
    const float corner = 5.0f / 3.0f;
    // How far in from the window's corner the black reaches, along the diagonal (q).
    const auto black_from_corner = [&](float size) {
        Settings v = s;
        v.hood_size = size;
        const auto f = lens::frame_for(v, 4.0f / 3.0f, radians(150), &hole);
        return corner - lens::visible_radius(f, 0.8f, 0.6f);
    };
    const float drawn = black_from_corner(1.0f);
    check(drawn > 0.1f && drawn < 0.25f, std::format("the Mk1 hood's black reaches {:.3f} in from the corner", drawn));
    check(near(black_from_corner(0.5f), drawn * 0.5f, 1e-3), "at half size, half as far");
    check(near(black_from_corner(2.0f), drawn * 2.0f, 1e-3), "at twice the size, twice as far");
    {
        Settings v = s;
        v.hood_size = 0.5f;
        const auto f = lens::frame_for(v, 4.0f / 3.0f, radians(150), &hole);
        check(f.hood_scale > 1.0f && near(lens::hood_uv(f, 4.0f / 3.0f, 1.0f)[0], 0.5f + 0.5f / f.hood_scale, 1e-6),
              "a smaller hood is its picture drawn larger about the middle");
    }
    // The circle overlay at its size: its edge moved out from where the lens sees 90 degrees.
    Settings c = s;
    c.hood = Hood::circle;
    c.circle = 1.54f;
    c.hood_size = 0.5f;
    const auto f = lens::frame_for(c, 4.0f / 3.0f, radians(150), nullptr);
    check(near(f.hood_circle, corner - 0.5f * (corner - 1.54f), 1e-5), "the circle's edge half as far in from the corner");
    // The picture's hole, measured: a transparent ellipse in an opaque frame.
    const std::uint32_t w = 400, h = 300;
    std::vector<std::uint8_t> rgba(std::size_t{w} * h * 4, 255);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w * 2 - 1, v = 1 - (y + 0.5f) / h * 2;
            if ((u / 0.8f) * (u / 0.8f) + (v / 0.6f) * (v / 0.6f) < 1) rgba[(std::size_t{y} * w + x) * 4 + 3] = 0;
        }
    const auto measured = lens::measure_hole(rgba.data(), w, h);
    check(measured.known, "a picture with a see-through middle");
    for (std::size_t k = 0; k <= lens::hole_steps; k += lens::hole_steps / 8) {
        const float psi = static_cast<float>(k) * lens::pi * 0.5f / static_cast<float>(lens::hole_steps), cu = std::cos(psi), sv = std::sin(psi);
        const float want = 1.0f / std::sqrt((cu / 0.8f) * (cu / 0.8f) + (sv / 0.6f) * (sv / 0.6f));
        check(near(measured.radius[k], want, 0.012), std::format("the hole measured at {:.0f} degrees: {:.4f} (an ellipse's {:.4f})", psi * 180 / lens::pi,
                                                                 measured.radius[k], want));
    }
    std::vector<std::uint8_t> solid(std::size_t{w} * h * 4, 255);
    check(!lens::measure_hole(solid.data(), w, h).known, "no see-through middle: none");
    std::vector<std::uint8_t> clear(std::size_t{w} * h * 4, 0);
    const auto open = lens::measure_hole(clear.data(), w, h);
    check(open.known && open.radius[0] >= 1.0f && open.radius[lens::hole_steps] >= 1.0f, "see-through to its edges");
}

// The camera curve's values: at their defaults as they have always been, each one doing what it says.
void camera_curve_tests() {
    const lens::Grade vx;
    check(lens::camera_toe_slope(0.45f) == 4.5f, "Rec.709's toe at gamma 0.45");
    for (const float v : {0.0f, 0.005f, 0.018f, 0.05f, 0.18f, 0.5f, 0.9f, 1.5f, 4.0f}) {
        float old = v < 0.018f ? 4.5f * v : 1.099f * std::pow(v, 0.45f) - 0.099f;
        if (old > 0.85f) old = 0.85f + (old - 0.85f) * 0.30f;
        old = std::clamp(std::min((old - 0.02f) / 0.98f, 1.04f), 0.0f, 1.0f);
        check(near(lens::camera_curve(vx, {v, v, v})[0], old, 1e-6), std::format("the camera curve at its defaults is as before ({})", v));
    }
    const auto at = [](const lens::Grade& g, float v) { return lens::camera_curve(g, {v, v, v})[0]; };
    lens::Grade flatter = vx;
    flatter.camera_gamma = 0.6f;
    check(at(flatter, 0.18f) < at(vx, 0.18f), "a higher gamma value: darker mid grey");
    const float toe = lens::camera_toe_slope(0.6f);
    check(near(toe * 0.018f, 1.099f * std::pow(0.018f, 0.6f) - 0.099f, 1e-5), "the toe meets the power curve");
    lens::Grade knee = vx;
    knee.camera_knee = 0.7f;
    check(at(knee, 0.9f) < at(vx, 0.9f) && near(at(knee, 0.1f), at(vx, 0.1f), 1e-6), "a lower knee: highlights lower, shadows as they were");
    lens::Grade black = vx;
    black.camera_black = 0.0f;
    check(at(black, 0.05f) > at(vx, 0.05f), "no pedestal: the shadows up");
    lens::Grade clip = vx;
    clip.camera_clip = 0.9f;
    check(at(clip, 10.0f) <= 0.9f + 1e-6f && at(vx, 10.0f) == 1.0f, "the white clip");
    lens::Grade colour = vx;
    colour.camera_colour = 1.5f;
    const auto plain = lens::camera_curve(vx, {0.3f, 0.2f, 0.1f}), richer = lens::camera_curve(colour, {0.3f, 0.2f, 0.1f});
    check(richer[0] - richer[2] > plain[0] - plain[2], "more colour level, more colour");
}

// Settings saved before the lens was settings only (2026-11-17) read as they drew.
void migration_tests() {
    const auto read = [](const char* text) { return settings_from_json(Json::parse(text)); };
    // Before the fisheye was one lens: "mk1" and "modern", Distortion a share of each one's own curve.
    const auto mk1 = read(R"({"values":{"lens.mode":"mk1","lens.curve":0.5,"lens.circle":1.54}})");
    check(lens_on(mk1) && near(mk1.curve, 0.5f) && mk1.circle == mk1_corner_circle, "an old Mk1: the fisheye at its circle as that version drew it");
    const auto modern = read(R"({"values":{"lens.mode":"modern","lens.curve":1.5,"lens.circle":1.6}})");
    check(near(modern.curve, 1.75f) && near(modern.circle, 1.6f), "an old modern lens: equisolid and past it");
    check(near(read(R"({"values":{"lens.mode":"modern","lens.curve":1}})").curve, 1.5f), "an old modern lens at 100%: equisolid");
    // "fisheye" with no curve saved: the Mk1's (100%); "off": no lens (whatever its other values were).
    const auto fish = read(R"({"values":{"lens.mode":"fisheye","lens.zoom":1.2}})");
    check(fish.curve == 1.0f && near(fish.zoom, 1.2f) && fish.circle == mk1_circle, "an old fisheye: 100%, at the image circle that was its default");
    const auto off = read(R"({"values":{"lens.mode":"off","lens.curve":1,"lens.zoom":1.5,"lens.fringe":0.8,"lens.circle":1.54}})");
    check(!lens_on(off) && off.curve == 0 && off.zoom == 1 && off.fringe == 0 && off.circle == mk1_circle,
          "an old save with its lens off: no lens (its glass was off with it)");
    // The game's barrel: its own values on the GAME tab, with the game's look on.
    const auto game = read(R"({"values":{"lens.mode":"game","lens.barrel":0.6,"lens.cubic":0.1,"lens.stretch":-0.2}})");
    check(!lens_on(game) && game.game_look && near(game.game_barrel, 0.6f) && near(game.game_barrel_edge, 0.1f) && near(game.game_barrel_stretch, -0.2f),
          "an old game barrel: the game's own, on its tab");
    // The camcorder switch: the camera curve at its defaults. The Mk1 hood: its picture.
    const auto cam = read(R"({"values":{"grade.camcorder":true,"frame.hood":"mk1"}})");
    check(cam.camera && cam.camera_gamma == 0.45f && cam.hood == Hood::picture && cam.hood_file == mk1_hood_file, "an old camcorder and Mk1 hood");
    const auto file = read(R"({"values":{"frame.hood":"file","frame.file":"Mine.png"}})");
    check(file.hood == Hood::picture && file.hood_file == "Mine.png", "an old picture of one's own");
    // True angles: the zoom and edge compression the lens picked for itself then, as settings (legacy_true_angles).
    const auto old = read(R"({"values":{"lens.mode":"fisheye","lens.curve":1,"lens.circle":1.6,"lens.true":true,"fov.mode":"auto",
        "fov.widest":150,"frame.hood":"mk1","frame.fit":true,"frame.bars":"4:3","video.aspect":"4:3"}})");
    check(old.zoom >= 1.0f && old.compress > 0.0f && old.compress < 0.3f && near(old.circle, 1.6f) && old.hood == Hood::picture,
          std::format("an old true-angles lens: zoom {:.5f}, compressed {:.4f}", old.zoom, old.compress));
    const auto hole = hood_hole();
    const float fov = lens::auto_fov(old, 4.0f / 3.0f, old.fov_widest, &hole);
    const auto f = lens::frame_for(old, 4.0f / 3.0f, fov, &hole);
    check(lens::fit(f) * f.zoom >= 0.9999f, std::format("and its Auto feeds all of it ({:.2f})", fov * 180 / lens::pi));
    // As saved now it reads back the same (nothing left to migrate).
    check(settings_from_json(to_json(old)) == old, "migrated, saved and read: the same");
}

// The colour tools XLGraphics has: identities at their defaults, brightness kept where it should be.
void colour_tool_tests() {
    lens::Grade g;
    check(g.identity(), "the default grade changes nothing");
    const auto m = lens::colour_matrix(g);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) check(near(m[static_cast<std::size_t>(r * 3 + c)], r == c ? 1.0f : 0.0f, 1e-5), "no hue, no saturation: identity");
    g.hue = 120;
    const auto h = lens::colour_matrix(g);
    for (int r = 0; r < 3; ++r) check(near(h[static_cast<std::size_t>(r * 3)] + h[static_cast<std::size_t>(r * 3 + 1)] + h[static_cast<std::size_t>(r * 3 + 2)], 1.0f, 1e-4),
                                      "a hue shift leaves grey grey");
    const std::array<float, 3> red{1, 0, 0};
    const auto turned = lens::graded(g, red, 0);
    check(turned[1] > turned[0] && turned[1] > turned[2], "red turned 120 degrees comes out green");
    g.hue = 0;
    g.filter = 1;
    g.filter_hue = 30;
    const auto f = lens::filter_gains(g);
    check(near(0.2126f * f[0] + 0.7152f * f[1] + 0.0722f * f[2], 1.0f, 1e-3) && f[0] > f[2], "an amber filter keeps brightness and warms");
    g.filter = 0;
    const auto off = lens::split_offset(220, 1);
    check(near(0.2126f * off[0] + 0.7152f * off[1] + 0.0722f * off[2], 0.0f, 1e-5) && off[2] > 0, "a blue shadow tint: no brightness, more blue");
    g.lift = 0.1f;
    check(lens::graded(g, {0, 0, 0}, 0)[0] > 0.005f, "lift raises black");
    g.lift = 0;
    g.gain = 0.5f;
    check(lens::graded(g, {1, 1, 1}, 0)[0] < 0.6f, "gain lowers white");
    g.gain = 1;
    g.gamma = 2;
    check(lens::graded(g, {0.18f, 0.18f, 0.18f}, 0)[0] > 0.18f, "gamma above 1 brightens the midtones");
    check(near(lens::vignette_power(0.5f), 2.5f, 1e-5), "the vignette's default softness is the old curve");
    lens::Grade round, own;
    own.vignette_round = 0;
    check(near(lens::vignette_edge(round, 4.0f / 3.0f, 1.0f, 4.0f / 3.0f), 1.0f, 1e-5) && near(lens::vignette_edge(own, 4.0f / 3.0f, 1.0f, 4.0f / 3.0f), 1.0f, 1e-5),
          "the vignette's edge is 1 at the corner, round or not");
    float most = 0, least = 0;
    for (int i = 0; i < 4000; ++i) {
        const float n = lens::grain_noise(static_cast<float>(i % 97) * 3.1f, static_cast<float>(i / 97) * 2.3f, 1.0f, static_cast<float>(i % 7));
        most = std::max(most, n);
        least = std::min(least, n);
    }
    check(most <= 0.5f && least >= -0.5f && most - least > 0.6f, "grain spreads over its range at 1 px");
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> suites{
        {"fields", field_tests}, {"commands", command_tests}, {"json", json_tests}, {"presets", preset_tests},
        {"lens", lens_tests}, {"grade", grade_tests}, {"finish", finish_tests}, {"focus", focus_tests},
        {"shake keys", shake_key_tests}, {"view", view_tests}, {"edge compression", compression_tests}, {"overlay", hood_tests},
        {"colour tools", colour_tool_tests}};
    try {
        for (const auto& [name, suite] : suites) {
            suite();
            std::cout << "ok " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << " (after " << checks << " checks)\n";
        return 1;
    }
    std::cout << checks << " checks passed\n";
    return 0;
}
