// Replay Shake's shake, timeline and commands, with no game: the noise, how keyframes blend, how a
// shake moves a camera, and what each `replayshake` verb does to the settings.
#include "Extension/ReplayShake/replay_shake_frames.h"
#include "Extension/ReplayShake/replay_shake_model.h"
#include "Extension/ReplayShake/replay_shake_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::replay_shake;
namespace {
int checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }
using Words = std::vector<std::string>;
Reply run(Settings& settings, const std::string& verb, const Words& words, const Editor& editor = {}) {
    return command(settings, verb, words, editor);
}
Shake loud(Preset preset, float intensity = 1.0f) {
    auto shake = preset_shake(preset);
    shake.intensity = intensity;
    return shake;
}
constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1};
float dot3(const Matrix& m, std::size_t a, std::size_t b) {
    return m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
}
bool orthonormal(const Matrix& m) {
    for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t b = a; b < 3; ++b)
            if (!near(dot3(m, a, b), a == b ? 1.0 : 0.0, 1e-5)) return false;
    // Right-handed: right x up = back.
    const float cx = m[1] * m[6] - m[2] * m[5], cy = m[2] * m[4] - m[0] * m[6], cz = m[0] * m[5] - m[1] * m[4];
    return near(cx, m[8], 1e-5) && near(cy, m[9], 1e-5) && near(cz, m[10], 1e-5);
}

void noise_tests() {
    double lowest = 1, highest = -1, mean = 0;
    for (int i = 0; i < 20000; ++i) {
        const double x = i * 0.0137 - 50;
        const float value = noise(7, x);
        check(value == noise(7, x), "noise is a function of its seed and x");
        lowest = std::min<double>(lowest, value);
        highest = std::max<double>(highest, value);
        mean += value;
    }
    check(lowest >= -1 && highest <= 1, "noise stays within -1 and 1");
    check(highest - lowest > 1.0, "noise uses most of its range");
    check(std::abs(mean / 20000) < 0.1, "noise centres on zero");
    check(noise(7, 3.0) == 0.0f && noise(7, -12.0) == 0.0f, "gradient noise passes through zero at whole numbers");
    check(noise(7, 0.4) != noise(8, 0.4), "another seed is another noise");
    // Smooth: a small step moves it a little.
    for (int i = 0; i < 2000; ++i) {
        const double x = i * 0.071;
        check(std::abs(noise(3, x + 0.001) - noise(3, x)) < 0.01, "noise is smooth");
    }
    check(noise(1, std::nan("")) == 0 && noise(1, 1e300) >= -1, "noise survives nonsense");
    // Roughness puts more of the fast layers in: more movement between close samples.
    double smooth = 0, rough = 0, smooth_range = 0;
    for (int i = 0; i < 4000; ++i) {
        const double x = i * 0.01;
        smooth += std::abs(layered(5, x + 0.01, 0) - layered(5, x, 0));
        rough += std::abs(layered(5, x + 0.01, 1) - layered(5, x, 1));
        smooth_range = std::max<double>(smooth_range, std::abs(layered(5, x, 0)));
    }
    check(rough > smooth * 1.3, "a rough shake moves more between close moments");
    check(smooth_range <= 1, "layered noise stays within -1 and 1");
}

void sample_tests() {
    const auto fallback = loud(Preset::handheld, 0.5f);
    const auto lone = sample({}, fallback, 10);
    check(near(lone.amplitude[channel::yaw], 0.5 * fallback.amounts[channel::yaw]), "no points: the default at its intensity");
    check(near(lone.phase, 10 * fallback.frequency), "no points: the noise runs at the default's speed");

    // Two keyframes: a gentle handheld to a hard running shake, eased.
    std::vector<Point> points{{1.0, loud(Preset::handheld), Blend::smooth}, {3.0, loud(Preset::running), Blend::smooth}};
    const auto handheld = loud(Preset::handheld), running = loud(Preset::running);
    check(near(sample(points, fallback, 0).amplitude[channel::roll], handheld.amounts[channel::roll]), "before the first keyframe its shake holds");
    check(near(sample(points, fallback, 9).amplitude[channel::roll], running.amounts[channel::roll]), "after the last keyframe its shake holds");
    const auto middle = sample(points, fallback, 2.0);
    check(near(middle.amplitude[channel::pitch], (handheld.amounts[channel::pitch] + running.amounts[channel::pitch]) / 2),
        "halfway between two keyframes, halfway between their shakes");
    check(sample(points, fallback, 1.5).amplitude[channel::pitch] < (handheld.amounts[channel::pitch] * 0.75 + running.amounts[channel::pitch] * 0.25),
        "smooth eases out of a keyframe");
    // The phase is continuous through keyframes and runs at the speed of the moment.
    double previous = sample(points, fallback, -1).phase;
    for (int i = 1; i <= 500; ++i) {
        const double t = -1 + i * 0.012;
        const auto now = sample(points, fallback, t);
        check(now.phase > previous, "the noise only runs forward");
        check(near((now.phase - previous) / 0.012, now.frequency, 0.05 * now.frequency + 0.02), "the phase runs at the frequency");
        previous = now.phase;
    }
    check(near(sample(points, fallback, 3.0 - 1e-9).phase, sample(points, fallback, 3.0 + 1e-9).phase, 1e-6), "no jump at a keyframe");
    check(near(sample(points, fallback, 3.0).phase, 2 * (handheld.frequency + running.frequency) / 2, 1e-6),
        "an eased change of speed averages over the gap");

    // Hold keeps the first shake until the next keyframe, then cuts; the noise still never jumps.
    points[0].blend = Blend::hold;
    check(near(sample(points, fallback, 2.9).amplitude[channel::pitch], handheld.amounts[channel::pitch]), "hold keeps its shake");
    check(near(sample(points, fallback, 3.0).amplitude[channel::pitch], running.amounts[channel::pitch]), "and cuts at the next keyframe");
    check(near(sample(points, fallback, 3.0).phase, 2 * handheld.frequency, 1e-6), "held, the speed holds too");
    points[0].blend = Blend::linear;
    check(near(sample(points, fallback, 1.5).amplitude[channel::pitch],
        handheld.amounts[channel::pitch] * 0.75 + running.amounts[channel::pitch] * 0.25), "linear blends evenly");

    // Fading to nothing keeps the speed it had: the silent keyframe's own speed does not count.
    auto quiet = loud(Preset::earthquake, 0);
    std::vector<Point> fade{{0, loud(Preset::handheld), Blend::smooth}, {2, quiet, Blend::smooth}};
    for (double t = 0; t <= 2; t += 0.25)
        check(near(sample(fade, fallback, t).frequency, handheld.frequency), "a fade out does not speed up");
    check(near(sample(fade, fallback, 2).amplitude[channel::yaw], 0), "it ends at nothing");
    // Keyframes at the same moment.
    std::vector<Point> same{{1, handheld, Blend::smooth}, {1, running, Blend::smooth}, {2, handheld, Blend::smooth}};
    for (double t = 0; t <= 3; t += 0.1) {
        const auto s = sample(same, fallback, t);
        check(std::isfinite(s.phase) && std::isfinite(s.amplitude[0]), "keyframes at one moment are fine");
    }
    check(near(sample(same, fallback, 1.0).amplitude[channel::pitch], running.amounts[channel::pitch]), "the later one takes over");
}

void camera_tests() {
    const Offsets none{};
    check(still(none) && shaken(identity, none) == identity, "no offsets, no change");
    Offsets look{};
    look[channel::pitch] = 10;
    auto m = shaken(identity, look);
    check(orthonormal(m), "a shaken camera stays orthonormal and right-handed");
    // Forward is -back: a positive pitch looks up.
    check(-m[9] > 0.17 && -m[9] < 0.18, "a positive pitch looks up by its degrees");
    check(m[12] == 3 && m[13] == 4 && m[14] == 5 && m[15] == 1 && m[3] == 0, "turns leave the position and fourth lanes");
    Offsets turn_left{};
    turn_left[channel::yaw] = 10;
    m = shaken(identity, turn_left);
    check(orthonormal(m) && -m[8] < -0.17, "a positive yaw looks left");
    Offsets tilt{};
    tilt[channel::roll] = 30;
    m = shaken(identity, tilt);
    check(orthonormal(m) && near(m[1], 0.5, 1e-5) && near(m[10], 1, 1e-6), "roll turns about the view axis");
    Offsets move{};
    move[channel::sideways] = 0.1f;
    move[channel::up] = 0.2f;
    move[channel::forward] = 0.3f;
    m = shaken(identity, move);
    check(near(m[12], 3.1) && near(m[13], 4.2) && near(m[14], 4.7), "moves go along right, up and forward (-back)");
    Offsets all{};
    for (std::size_t c = 0; c < 6; ++c) all[c] = 3;
    Matrix tilted = identity;
    // A camera turned 45 degrees about up.
    const float h = std::sqrt(0.5f);
    tilted[0] = h; tilted[2] = -h; tilted[8] = h; tilted[10] = h;
    check(orthonormal(shaken(tilted, all)), "any camera stays orthonormal");
    Offsets bad{};
    bad[channel::yaw] = std::nanf("");
    check(shaken(identity, bad) == identity, "a bad offset changes nothing");
    Offsets wider{};
    wider[channel::zoom] = 10;
    check(near(zoomed(1.0f, wider), 1.1) && zoomed(1.0f, none) == 1.0f, "zoom is a percentage of the field of view");
    wider[channel::zoom] = 20000;
    check(zoomed(1.0f, wider) <= 3.0f, "zoom stays in range");

    // Offsets stay within each channel's amplitude, follow the seed, and repeat for the same moment.
    const auto s = sample({}, loud(Preset::earthquake), 4.2);
    const auto a = offsets(s, 1), b = offsets(s, 1), c = offsets(s, 2);
    check(a == b, "the same moment shakes the same way");
    check(a != c, "another seed shakes another way");
    for (std::size_t i = 0; i < channel_count; ++i) check(std::abs(a[i]) <= s.amplitude[i] + 1e-6f, "offsets stay within amplitude");
    check(offsets(sample({}, default_shake(), 4.2), 1) == Offsets{}, "the default shake is off");
    // Channels move independently.
    double together = 0;
    for (int i = 0; i < 400; ++i) {
        const auto o = offsets(sample({}, loud(Preset::handheld), i * 0.05), 1);
        together += (o[channel::pitch] / 0.6) * (o[channel::yaw] / 0.8);
    }
    check(std::abs(together / 400) < 0.08, "pitch and yaw are not the same noise");
}

void timeline_tests() {
    const auto fallback = loud(Preset::handheld, 0.25f);
    std::vector<Key> keys{{2.0, loud(Preset::running), Blend::hold}, {9.0, loud(Preset::vehicle), Blend::smooth}};
    const std::vector<double> keyframes{0.0, 2.0, 5.0};
    const auto points = timeline(keyframes, keys, fallback);
    check(points.size() == 3, "one point per keyframe");
    check(points[0].shake == fallback && points[2].shake == fallback, "keyframes without a shake take the default");
    check(points[1].shake == keys[0].shake && points[1].blend == Blend::hold, "a keyframe with a shake has it");
    const auto free = timeline({}, keys, fallback);
    check(free.size() == 2 && free[1].time == 9.0, "without keyframes, the keys themselves");
    const std::vector<double> unordered{5.0, 0.0};
    check(timeline(unordered, keys, fallback)[0].time == 0.0, "points come out sorted");

    // Keys follow their keyframes as the editor moves them.
    auto moving = keys;
    follow(moving, std::vector<double>{0.0, 2.0, 5.0}, std::vector<double>{0.0, 2.5, 5.0});
    check(moving[0].time == 2.5 && moving[1].time == 9.0, "a moved keyframe takes its key, others stay");
    // Dragged past another, a keyframe is listed elsewhere: it still takes its own key only.
    std::vector<Key> pair{{1.0, loud(Preset::running), Blend::smooth}, {2.0, loud(Preset::drift), Blend::smooth}};
    follow(pair, std::vector<double>{1.0, 2.0, 3.0}, std::vector<double>{2.0, 2.5, 3.0});
    check(pair[0].time == 2.5 && pair[1].time == 2.0, "a keyframe dragged past another takes its own key");
    // Another replay with as many keyframes: nothing moves.
    auto other = keys;
    follow(other, std::vector<double>{0.0, 2.0, 5.0}, std::vector<double>{0.0, 3.0, 4.0});
    check(other[0].time == 2.0 && other[1].time == 9.0, "several keyframes changing at once is another replay");
    auto added = keys;
    follow(added, std::vector<double>{0.0, 2.0}, std::vector<double>{0.0, 1.0, 2.0});
    check(added[0].time == 2.0, "a new keyframe moves no key");
    check(keyframe_at(keyframes, 2.0005) == 1u && !keyframe_at(keyframes, 2.01), "a key sits on a keyframe within a millisecond");
}

void command_tests() {
    Settings s;
    check(!run(s, "default", {"intensity", "80%"}).text.empty() && near(s.fallback.intensity, 0.8), "default intensity in percent");
    check(run(s, "default", {"preset", "running"}).changed && s.fallback.preset == Preset::running &&
        near(s.fallback.intensity, 0.8) && s.fallback.frequency == preset_shake(Preset::running).frequency,
        "a preset keeps the intensity");
    run(s, "default", {"pitch", "2.5"});
    check(s.fallback.preset == Preset::custom && near(s.fallback.amounts[channel::pitch], 2.5), "changing an amount makes it custom");
    run(s, "default", {"sideways", "5cm"});
    check(near(s.fallback.amounts[channel::sideways], 0.05), "moves are typed in centimetres");
    run(s, "default", {"frequency", "1000"});
    check(s.fallback.frequency == max_frequency, "values are clamped");
    check(!run(s, "default", {"wobble", "3"}).changed, "an unknown setting changes nothing");
    check(!run(s, "default", {"intensity", "lots"}).changed, "a word that is not a number changes nothing");
    check(run(s, "default", {"reset"}).changed && s.fallback == default_shake(), "default reset");
    check(!run(s, "default", {"blend", "hold"}).changed, "the default has no blend");

    // With the replay editor's keyframes readable.
    Editor editor;
    editor.open = true;
    editor.keyframes = std::vector<double>{0.0, 1.5, 4.0};
    editor.selected = 1;
    editor.playhead = 2.0;
    auto reply = run(s, "key", {"2", "preset", "earthquake"}, editor);
    check(reply.changed && s.keys.size() == 1 && s.keys[0].time == 1.5, "a keyframe given a shake gets a key at its time");
    check(s.keys[0].shake.preset == Preset::earthquake && near(s.keys[0].shake.intensity, 1), "it starts at full intensity");
    run(s, "key", {"selected", "intensity", "40"}, editor);
    check(s.keys.size() == 1 && near(s.keys[0].shake.intensity, 0.4), "`selected` is the editor's selected keyframe");
    run(s, "key", {"2", "blend", "linear"}, editor);
    check(s.keys[0].blend == Blend::linear, "a keyframe's blend");
    check(!run(s, "key", {"3", "bogus", "1"}, editor).changed && s.keys.size() == 1, "a failed edit leaves no key behind");
    check(!run(s, "key", {"9", "intensity", "1"}, editor).changed, "keyframes are numbered from 1");
    check(run(s, "key", {"1"}, editor).text.find("default") != std::string::npos, "a keyframe without a shake says so");
    check(!run(s, "add", {}, editor).changed, "free keys are not added while keyframes are readable");
    const auto points_now = points(s, editor);
    check(points_now.size() == 3 && points_now[1].shake.preset == Preset::earthquake, "the timeline has the keyframe's shake");
    Editor empty = editor;
    empty.keyframes = std::vector<double>{};
    check(points(s, empty).empty(), "no keyframes: the default throughout");
    Editor blind = editor;
    blind.playhead.reset();
    check(points(s, blind).empty(), "keyframes without the playhead: the default throughout");
    check(run(s, "key", {"2", "reset"}, editor).changed && s.keys.empty(), "key reset gives the keyframe back to the default");
    run(s, "key", {"3", "intensity", "100"}, editor);
    editor.keyframes = std::vector<double>{0.0, 1.5};
    check(run(s, "clean", {}, editor).changed && s.keys.empty(), "clean drops shakes on no keyframe");

    // Without readable keyframes: free keys at the playhead.
    Editor free;
    free.open = true;
    free.playhead = 2.5;
    check(run(s, "add", {}, free).changed && s.keys.size() == 1 && s.keys[0].time == 2.5, "add puts a key at the playhead");
    check(!run(s, "add", {}, free).changed, "one key per moment");
    check(run(s, "add", {"1.25s"}, free).changed && s.keys[0].time == 1.25, "add at a time; keys stay sorted");
    run(s, "key", {"2", "preset", "drift"}, free);
    check(s.keys[1].shake.preset == Preset::drift, "free keys are numbered in time order");
    check(!run(s, "key", {"selected", "intensity", "1"}, free).changed, "no selection without readable keyframes");
    check(run(s, "remove", {"1"}, free).changed && s.keys.size() == 1 && s.keys[0].time == 2.5, "remove a key");
    check(!run(s, "add", {}, Editor{}).changed, "add needs a playhead or a time");
    check(points(s, free).size() == 1 && points(s, Editor{}).empty(), "free keys need the playhead to mean anything");
    check(run(s, "list", {}, free).text.find("Key 1 at 2.50 s") != std::string::npos, "list names the keys");
    check(run(s, "clear", {}, free).changed && s.keys.empty(), "clear");

    check(run(s, "off", {}).changed && !s.enabled && run(s, "on", {}).changed && s.enabled, "on and off");
    check(run(s, "seed", {"42"}).changed && s.seed == 42 && !run(s, "seed", {"-1"}).changed, "seed");
    check(run(s, "fps", {"30"}).changed && s.export_fps == 30 && !run(s, "fps", {"1000"}).changed &&
        !run(s, "fps", {"0"}).changed, "export frame rate");
    check(run(s, "presets", {}).text.find("earthquake") != std::string::npos, "presets are listed");
    check(run(s, "nonsense", {}).text.find("Unknown verb") == 0, "an unknown verb says so");

    // Cameras and the editor mapping.
    check(run(s, "camera", {"add", "0x63fc220"}).changed && s.cameras == std::vector<std::uintptr_t>{0x63fc220}, "camera add");
    check(!run(s, "camera", {"add", "63fc220"}).changed, "a camera is added once");
    check(!run(s, "camera", {"add", "zzz"}).changed, "a camera is a hexadecimal offset");
    check(run(s, "camera", {"remove", "0x63fc220"}).changed && s.cameras.empty(), "camera remove");
    check(run(s, "map", {"root", "0x1a2b3c4d"}).changed && s.mapping.root == 0x1a2b3c4d, "map root");
    check(run(s, "map", {"playhead", "f32", "0x11/0x22", "0.001"}).changed &&
        s.mapping.playhead.path == std::vector<std::uint32_t>{0x11, 0x22} && s.mapping.playhead.scale == 0.001, "map playhead");
    check(run(s, "map", {"keyframes", "0x33", "f64", "0x44"}).changed && s.mapping.keyframes == std::vector<std::uint32_t>{0x33} &&
        s.mapping.keyframe_time.format == Format::f64 && s.mapping.keyframe_time.path == std::vector<std::uint32_t>{0x44},
        "map keyframes");
    check(run(s, "map", {"keyframes", "0x33", "f32", "."}).changed && s.mapping.keyframe_time.path.empty(), "an array of plain times");
    check(run(s, "map", {"selected", "i32", "0x55"}).changed && s.mapping.selected.format == Format::i32, "map selected");
    check(!run(s, "map", {"playhead", "f16", "0x11"}).changed && !run(s, "map", {"playhead", "f32", "0x11/"}).changed,
        "a bad format or path changes nothing");
    check(run(s, "map", {"show"}).text.find("root 0x1a2b3c4d") != std::string::npos, "map show");

    // Saved and read back.
    run(s, "default", {"preset", "walking"});
    run(s, "default", {"intensity", "65"});
    run(s, "add", {"3"}, free);
    run(s, "key", {"1", "blend", "hold"}, free);
    run(s, "key", {"1", "zoom", "4"}, free);
    run(s, "camera", {"add", "0x6000010"});
    s.cameras_build = "25414733";
    check(!s.gameplay && run(s, "gameplay", {}).text.find("stays still") != std::string::npos, "gameplay stays still by default");
    check(run(s, "gameplay", {"1"}).changed && s.gameplay && !run(s, "gameplay", {"on"}).changed, "gameplay on");
    check(!run(s, "gameplay", {"maybe"}).changed && s.gameplay, "gameplay takes 0 or 1");
    const auto text = to_json(s).dump();
    const auto back = settings_from_json(Json::parse(text));
    check(back.enabled == s.enabled && back.seed == s.seed && back.export_fps == s.export_fps, "settings round trip");
    check(back.fallback == s.fallback, "the default shake round trips");
    check(back.keys.size() == 1 && back.keys[0].time == 3 && back.keys[0].blend == Blend::hold &&
        back.keys[0].shake == s.keys[0].shake, "keys round trip");
    check(back.cameras == s.cameras && back.cameras_build == "25414733" && back.mapping == s.mapping && back.gameplay,
        "cameras, their game build, gameplay and the mapping round trip");
    check(!settings_from_json(Json::parse(R"({"gameplay":"yes"})")).gameplay, "gameplay must be true or false");
    check(run(s, "gameplay", {"0"}).changed && !s.gameplay, "gameplay off");
    // Nonsense in the saved text is skipped, not trusted.
    const auto odd = settings_from_json(Json::parse(R"({"enabled":"yes","seed":-4,"export_fps":9999,
        "default":{"preset":"tornado","intensity":50,"frequency":-3,"amounts":[1,2,"x"]},
        "keys":[{"time":"soon"},{"time":1,"blend":"wobbly","shake":{"intensity":0.5}}],
        "cameras":["0x0","nope","0x123"],"mapping":{"root":"0xfffffffff","playhead":{"path":["0x1","zz"],"format":"f99"}}})"));
    check(odd.enabled && odd.seed == 1 && odd.export_fps == 60, "wrong types keep their defaults");
    check(odd.fallback.intensity == max_intensity && odd.fallback.frequency == min_frequency && odd.fallback.amounts[0] == 1,
        "out of range values are clamped");
    check(odd.keys.size() == 1 && odd.keys[0].blend == Blend::smooth && near(odd.keys[0].shake.intensity, 0.5), "a bad key is skipped");
    check(odd.cameras == std::vector<std::uintptr_t>{0x123} && odd.mapping.root == 0 && odd.mapping.playhead.path.empty(),
        "bad cameras and paths are skipped");
    check(settings_from_json(Json::parse("[1,2]")).fallback == default_shake(), "not an object: defaults");
    // The controller toggle: a combo by its buttons' names (or as ReSkate keeps it), saved as a number.
    check(parse_binding("LB+RB") == 0x300u && parse_binding("l1 + r1") == 0x300u && parse_binding("d-pad up+Y") == 0x8001u &&
        parse_binding("Back+Triangle") == 0x8020u && parse_binding("lt+rt") == 0x30000u, "combos by their buttons' names");
    check(parse_binding("768") == 0x300u && parse_binding("0x300") == 0x300u && parse_binding("0") == 0u, "combos by number");
    check(!parse_binding("LB+LB") && !parse_binding("LB+wobble") && !parse_binding("") && !parse_binding("+") &&
        !parse_binding("0x80000000") && !parse_binding("12a"), "nonsense is no combo");
    Settings toggled;
    check(run(toggled, "toggle", {}).changed && !toggled.enabled && run(toggled, "toggle", {}).changed && toggled.enabled,
        "toggle switches it off and on");
    // No controller combo of its own (Replay Plus's panel switches it): bind changes nothing, says where it went,
    // and a combo saved by an older build is let be.
    check(!run(toggled, "bind", {"LB+RB"}).changed && run(toggled, "bind", {}).text.find("Replay Plus") != std::string::npos,
        "bind is gone: Replay Plus's panel switches it");
    check(settings_from_json(Json::parse(R"({"toggle":"0x300","enabled":false})")).enabled == false &&
        to_json(settings_from_json(Json::parse(R"({"toggle":"0x300"})"))).dump().find("toggle") == std::string::npos,
        "an old saved combo is read past and not written again");
    // A full book of keyframe shakes round trips, written with tidy numbers.
    Settings full;
    for (std::size_t i = 0; i < max_keys; ++i)
        full.keys.push_back({0.25 * static_cast<double>(i), loud(static_cast<Preset>(i % 6), 0.6f), Blend::linear});
    const auto big = to_json(full).dump(2);
    check(big.find("0.6000000") == std::string::npos && big.find("\"intensity\": 0.6") != std::string::npos,
        "floats are written rounded");
    const auto again = settings_from_json(Json::parse(big));
    check(again.keys.size() == max_keys && again.keys.back().time == 0.25 * (max_keys - 1) &&
        again.keys[7].shake.preset == Preset::walking && again.keys[7].blend == Blend::linear, "256 keyframe shakes round trip");
}

void text_tests() {
    check(parse_hex("0x1F") == 0x1fu && parse_hex("ff") == 0xffu && !parse_hex("0x") && !parse_hex("g1") &&
        !parse_hex("0x11111111111111111"), "hex numbers");
    check(parse_path("0x1/0x2") == std::vector<std::uint32_t>{1, 2} && parse_path(".")->empty() &&
        !parse_path("") && !parse_path("0x1//0x2") && !parse_path("0x100000000"), "paths");
    check(path_text(std::vector<std::uint32_t>{0xab, 0xcd}) == "0xab/0xcd", "path text");
    const float f = 1.25f;
    const double d = -2.5;
    const std::int32_t i = -7;
    const std::uint32_t u = 4000000000U;
    const std::int64_t l = -9;
    check(decode(Format::f32, &f) == 1.25 && decode(Format::f64, &d) == -2.5 && decode(Format::i32, &i) == -7 &&
        decode(Format::u32, &u) == 4000000000.0 && decode(Format::i64, &l) == -9, "stored numbers");
    check(format_named("i64") == Format::i64 && !format_named("F32") && size(Format::f64) == 8, "formats");
    check(preset_named("Earthquake") == Preset::earthquake && !preset_named("quake") && blend_named("HOLD") == Blend::hold,
        "names ignore case");
    for (std::size_t p = 0; p < static_cast<std::size_t>(Preset::custom); ++p) {
        const auto shake = preset_shake(static_cast<Preset>(p));
        check(clamped(shake) == shake && !silent(shake), "every preset is within the limits and moves");
    }
    check(describe(loud(Preset::running, 0.8f)) == "running 80%, 2.6 Hz", "a shake described");
    check(silent(default_shake()) && new_key_shake(default_shake()).intensity == 1, "a new keyframe shake starts at full");
}
// A renderer's view set-ups, played against a Frames the way replay_shake.cpp does: each frame's
// first set-up starts a new shake, the rest show that frame's.
struct Rig {
    frames::Frames tracker;
    int starts = 0;
    bool silent = false; // every frame's shake moves nothing
    void setup(Matrix& camera, float& fov, std::uintptr_t view, std::uint64_t at_us) {
        const auto placed = tracker.place(camera, fov, view, at_us);
        switch (placed.place) {
        case frames::Place::shown:
            tracker.note(view);
            break;
        case frames::Place::again: {
            const auto shot = tracker.shot_of(placed);
            camera = shot.camera;
            fov = shot.fov;
            tracker.wrote(view, placed, shot);
            break;
        }
        case frames::Place::next: {
            Offsets moved{};
            if (!silent) {
                moved[channel::pitch] = 0.5f + 0.25f * static_cast<float>(starts % 7);
                moved[channel::up] = 0.01f * static_cast<float>(starts % 5);
            }
            ++starts;
            tracker.start(placed.base, placed.base_fov, moved, view, at_us);
            camera = tracker.now().shown;
            fov = tracker.now().fov;
            break;
        }
        }
    }
};
// The game's camera on a frame: moving along x, or standing still.
Matrix game_camera(int frame, bool moving) {
    auto m = identity;
    if (moving) m[12] = 3 + 0.1f * static_cast<float>(frame);
    return m;
}
constexpr std::uint64_t frame_us = 16667;
bool all_same(const std::vector<Matrix>& cameras) {
    for (const auto& camera : cameras)
        if (!frames::same_camera(camera, cameras.front())) return false;
    return true;
}

void frame_tests() {
    for (const bool moving : {true, false}) {
        const std::string how = moving ? " (moving camera)" : " (still camera)";
        // skate.: one view, set up four times a frame; the game sets it up afresh each frame.
        {
            Rig rig;
            Matrix shown_before{};
            for (int f = 0; f < 12; ++f) {
                Matrix camera = game_camera(f, moving);
                float fov = 1.0f;
                std::vector<Matrix> seen;
                for (int k = 0; k < 4; ++k) {
                    rig.setup(camera, fov, 0x1000, static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 900);
                    seen.push_back(camera);
                }
                check(rig.starts == f + 1, "one shake a frame from one view set up four times" + how);
                check(rig.tracker.now().setups == 4 && rig.tracker.frame() == static_cast<std::uint32_t>(f + 1),
                    "each set-up of a frame is counted" + how);
                check(all_same(seen) && !frames::same_camera(seen[0], game_camera(f, moving)),
                    "every set-up of a frame shows the frame's shake" + how);
                check(frames::same_camera(rig.tracker.now().base, game_camera(f, moving)), "the frame's base is the game's camera" + how);
                if (f) check(frames::same_camera(rig.tracker.before().shown, shown_before), "the frame before is kept as shown" + how);
                shown_before = seen[0];
            }
        }
        // Four views of one camera, the same four each frame.
        {
            Rig rig;
            for (int f = 0; f < 12; ++f) {
                std::vector<Matrix> seen;
                for (int k = 0; k < 4; ++k) {
                    Matrix camera = game_camera(f, moving);
                    float fov = 1.0f;
                    rig.setup(camera, fov, 0x1000 + 0x200 * static_cast<std::uintptr_t>(k),
                        static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 1500);
                    seen.push_back(camera);
                }
                check(rig.starts == f + 1 && all_same(seen), "four views of a frame share its shake" + how);
            }
        }
        // Four views of one camera, made anew each frame (other addresses).
        {
            Rig rig;
            for (int f = 0; f < 12; ++f) {
                std::vector<Matrix> seen;
                for (int k = 0; k < 4; ++k) {
                    Matrix camera = game_camera(f, moving);
                    float fov = 1.0f;
                    rig.setup(camera, fov, 0x10000 + 0x200 * static_cast<std::uintptr_t>(f * 4 + k),
                        static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 1200);
                    seen.push_back(camera);
                }
                check(rig.starts == f + 1 && all_same(seen), "new views each frame: one shake a frame" + how);
            }
        }
    }
    // Views made anew each frame, the camera coming to a stop: the shake goes on within three frames.
    {
        Rig rig;
        for (int f = 0; f < 10; ++f)
            for (int k = 0; k < 2; ++k) {
                Matrix camera = game_camera(std::min(f, 4), true);
                float fov = 1.0f;
                rig.setup(camera, fov, 0x10000 + 0x200 * static_cast<std::uintptr_t>(f * 2 + k),
                    static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 1000);
            }
        check(rig.starts >= 8, "a camera that stops holds its last shake for two frames at most");
    }
    // Views given the game's camera back after each copy (`replayshake after 0`): one frame a frame still,
    // each set-up given the frame's shake again.
    for (const bool moving : {true, false}) {
        Rig rig;
        for (int f = 0; f < 6; ++f) {
            Matrix camera = game_camera(f, moving);
            float fov = 1.0f;
            std::vector<Matrix> seen;
            for (int k = 0; k < 4; ++k) {
                rig.setup(camera, fov, 0x1000, static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 300);
                seen.push_back(camera);
                rig.tracker.returned();
                camera = rig.tracker.now().base;
            }
            check(rig.starts == f + 1 && all_same(seen) && !frames::same_camera(seen[0], game_camera(f, moving)),
                std::string("views given the game's camera back keep one shake a frame") + (moving ? " (moving)" : " (still)"));
        }
    }
    // A shake that moves nothing: the set-ups of a frame still make one frame.
    {
        Rig rig;
        rig.silent = true;
        for (int f = 0; f < 6; ++f) {
            Matrix camera = game_camera(0, false);
            float fov = 1.0f;
            for (int k = 0; k < 4; ++k)
                rig.setup(camera, fov, 0x1000, static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 900);
            check(rig.starts == f + 1 && frames::same_camera(camera, game_camera(0, false)), "a still shake: one frame a frame");
        }
    }
    // A view the game does not set up again keeps showing an earlier frame's shake: each frame it
    // takes that frame's on the camera under it (never a shake on a shake), set up once a frame or
    // four times, and starts no frame of its own.
    for (const int times : {1, 4}) {
        Rig rig;
        const auto under = game_camera(0, true);
        Matrix stale = under;
        float stale_fov = 1.0f;
        for (int f = 0; f < 8; ++f) {
            Matrix main = game_camera(f, true);
            float fov = 1.0f;
            const auto t = static_cast<std::uint64_t>(f) * frame_us;
            rig.setup(main, fov, 0x1000, t);
            for (int k = 0; k < times; ++k) rig.setup(stale, stale_fov, 0x2000, t + 500 + static_cast<std::uint64_t>(k) * 300);
            check(rig.starts == f + 1, "a view not set up again starts no frame of its own");
            check(frames::same_camera(stale, f ? shaken(under, rig.tracker.now().moved) : main),
                "a view not set up again shows this frame's shake on what was under it, never one on another");
        }
    }
    // The same, set up before the frame's main view: the main view's frames and the frame before stay right.
    {
        Rig rig;
        Matrix stale = game_camera(0, true);
        float stale_fov = 1.0f;
        for (int f = 0; f < 8; ++f) {
            const auto t = static_cast<std::uint64_t>(f) * frame_us;
            if (f) rig.setup(stale, stale_fov, 0x2000, t);
            Matrix main = game_camera(f, true);
            float fov = 1.0f;
            rig.setup(main, fov, 0x1000, t + 400);
            if (!f) rig.setup(stale, stale_fov, 0x2000, t + 700);
            check(rig.starts == f + 1, "a stale view before the main view starts no frame");
            if (f) check(frames::same_camera(rig.tracker.before().base, game_camera(f - 1, true)), "the frame before stays the main view's");
        }
    }
    // A game that stops setting the view up afresh (a still camera): the shake still goes on.
    {
        Rig rig;
        Matrix camera = game_camera(0, false);
        float fov = 1.0f;
        for (int f = 0; f < 30; ++f)
            for (int k = 0; k < 4; ++k)
                rig.setup(camera, fov, 0x1000, static_cast<std::uint64_t>(f) * frame_us + static_cast<std::uint64_t>(k) * 900);
        check(rig.starts >= 4 && rig.starts <= 6, "a view never set up again still shakes on, every tenth of a second");
    }
    // Another camera altogether is another frame; reset() forgets the frames before.
    {
        Rig rig;
        Matrix camera = game_camera(0, true), other = game_camera(30, true);
        float fov = 1.0f, other_fov = 1.0f;
        rig.setup(camera, fov, 0x1000, 0);
        rig.setup(other, other_fov, 0x2000, 300);
        check(rig.starts == 2, "a view from another camera starts its own frame");
        rig.tracker.reset();
        check(!rig.tracker.now().used && !rig.tracker.before().used, "reset forgets every frame");
        Matrix fourth{};
        fourth[3] = 1;
        Matrix plain = identity;
        plain[3] = 7; // a fourth lane the renderer keeps something else in
        check(frames::same_camera(identity, plain) && !frames::same_camera(identity, fourth), "cameras compare by their first three lanes");
    }
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> suites{
        {"noise", noise_tests}, {"sample", sample_tests}, {"camera", camera_tests},
        {"timeline", timeline_tests}, {"commands", command_tests}, {"text", text_tests}, {"frames", frame_tests}};
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
