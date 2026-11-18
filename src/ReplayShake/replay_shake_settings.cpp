#include "replay_shake_settings.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace dingosdk::replay_shake {
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}
// A number as typed, with an optional unit after it ("80%", "2.5hz", "3cm", "1.5s", "2deg").
std::optional<double> number(std::string_view text) {
    auto value = lower(text);
    for (const std::string_view unit : {"%", "hz", "cm", "deg", "s"})
        if (value.size() > unit.size() && value.ends_with(unit)) {
            value.resize(value.size() - unit.size());
            break;
        }
    double result{};
    const auto* end = value.data() + value.size();
    const auto [at, error] = std::from_chars(value.data(), end, result);
    if (error != std::errc{} || at != end || !std::isfinite(result)) return std::nullopt;
    return result;
}
std::optional<std::int64_t> whole(std::string_view text) {
    std::int64_t result{};
    const auto* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, result);
    if (error != std::errc{} || at != end) return std::nullopt;
    return result;
}
void sort_keys(std::vector<Key>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}
std::optional<std::size_t> key_at(const std::vector<Key>& keys, double time) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const double distance = std::abs(keys[i].time - time);
        if (distance <= key_tolerance && (!best || distance < std::abs(keys[*best].time - time))) best = i;
    }
    return best;
}
bool keyframe_mode(const Editor& editor) { return editor.keyframes.has_value(); }

// The settings of one shake a word changes: `default <setting> <value>` and `key <n> <setting> <value>`.
constexpr std::string_view shake_settings =
    "preset, intensity (%), frequency (Hz), roughness (%), pitch, yaw, roll (degrees), sideways, up, forward (cm), zoom (%)";
// Changes `shake` (and `blend`, for a key) as the words say. Empty on success, else what was wrong.
std::string edit(Shake& shake, Blend* blend, std::span<const std::string> words) {
    if (words.size() < 2) return std::format("Give a setting and a value: {}{}.", shake_settings, blend ? ", blend" : "");
    const auto setting = lower(words[0]);
    if (setting == "preset") {
        const auto preset = preset_named(words[1]);
        if (!preset) return std::format("Unknown preset '{}'. Presets: handheld, walking, running, vehicle, earthquake, drift, custom.", words[1]);
        apply_preset(shake, *preset);
        return {};
    }
    if (setting == "blend") {
        if (!blend) return "Only a keyframe has a blend: it says how its shake turns into the next keyframe's.";
        const auto value = blend_named(words[1]);
        if (!value) return "Blend is smooth, linear or hold.";
        *blend = *value;
        return {};
    }
    const auto value = number(words[1]);
    if (!value) return std::format("'{}' is not a number.", words[1]);
    const auto v = static_cast<float>(*value);
    if (setting == "intensity") shake.intensity = v / 100.0f;
    else if (setting == "frequency" || setting == "speed") shake.frequency = v;
    else if (setting == "roughness") shake.roughness = v / 100.0f;
    else {
        std::optional<std::size_t> which;
        for (std::size_t c = 0; c < channel_count; ++c)
            if (setting == channel_names[c]) which = c;
        if (!which) return std::format("Unknown setting '{}'. Settings: {}{}.", words[0], shake_settings, blend ? ", blend" : "");
        // Moves are typed in centimetres and kept in metres.
        const bool move = *which == channel::sideways || *which == channel::up || *which == channel::forward;
        shake.amounts[*which] = move ? v / 100.0f : v;
        shake.preset = Preset::custom;
    }
    shake = clamped(shake);
    return {};
}

// Which key `key <which>` means, made if `create` and it is a keyframe without one. Sets `error`
// when there is none; `label` names it for the reply.
std::optional<std::size_t> target(Settings& settings, const Editor& editor, std::string_view which, bool create,
    std::string& label, std::string& error) {
    if (keyframe_mode(editor)) {
        const auto& keyframes = *editor.keyframes;
        std::optional<std::size_t> index;
        if (lower(which) == "selected") {
            if (!editor.selected || *editor.selected < 0 || static_cast<std::size_t>(*editor.selected) >= keyframes.size()) {
                error = "No keyframe is selected in the replay editor.";
                return std::nullopt;
            }
            index = static_cast<std::size_t>(*editor.selected);
        } else if (const auto n = whole(which); n && *n >= 1 && static_cast<std::size_t>(*n) <= keyframes.size()) {
            index = static_cast<std::size_t>(*n - 1);
        } else {
            error = keyframes.empty() ? "The replay has no keyframes yet."
                                      : std::format("Keyframes are numbered 1 to {}.", keyframes.size());
            return std::nullopt;
        }
        const double time = keyframes[*index];
        label = std::format("Keyframe {} ({})", *index + 1, seconds(time));
        if (const auto found = key_at(settings.keys, time)) return found;
        if (!create) return std::nullopt; // no shake of its own: nothing to change
        if (settings.keys.size() >= max_keys) {
            error = std::format("Replay Shake keeps at most {} keyframe shakes; `replayshake clean` drops those of other replays.", max_keys);
            return std::nullopt;
        }
        settings.keys.push_back({time, new_key_shake(settings.fallback), Blend::smooth});
        sort_keys(settings.keys);
        return key_at(settings.keys, time);
    }
    if (lower(which) == "selected") {
        error = "The replay editor's keyframes are not readable yet (README.md, Finding the replay editor): use a key number.";
        return std::nullopt;
    }
    const auto n = whole(which);
    if (!n || *n < 1 || static_cast<std::size_t>(*n) > settings.keys.size()) {
        error = settings.keys.empty() ? "There are no keys yet: `replayshake add` puts one at the playhead."
                                      : std::format("Keys are numbered 1 to {}.", settings.keys.size());
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(*n - 1);
    label = std::format("Key {} ({})", index + 1, seconds(settings.keys[index].time));
    return index;
}

std::string reading_text(const Reading& reading) {
    return std::format("{} {}{}", name(reading.format), reading.path.empty() ? std::string(".") : path_text(reading.path),
        reading.scale == 1 ? std::string() : std::format(" x{}", reading.scale));
}
} // namespace
std::string describe(const Mapping& mapping) {
    if (!mapping.root) return "No replay editor mapping: `replayshake map root <schema>` starts one.";
    std::string text = "root " + hex(mapping.root);
    text += "; playhead " + (mapping.playhead.path.empty() ? std::string("unmapped") : reading_text(mapping.playhead));
    text += "; keyframes " + (mapping.keyframes.empty() ? std::string("unmapped")
        : path_text(mapping.keyframes) + ", time " + reading_text(mapping.keyframe_time));
    text += "; selected " + (mapping.selected.path.empty() ? std::string("unmapped") : reading_text(mapping.selected));
    return text;
}
namespace {
// `map playhead f32 0xaa/0xbb [scale]` and its kin: format, path, then an optional scale.
std::optional<Reading> parse_reading(std::span<const std::string> words, std::string& error) {
    if (words.size() < 2) {
        error = "Give a format (f32, f64, i32, u32, i64) and a path of field hashes, like f32 0x1a2b3c4d/0x5e6f7081.";
        return std::nullopt;
    }
    Reading reading;
    const auto format = format_named(words[0]);
    if (!format) {
        error = std::format("'{}' is not a format: f32, f64, i32, u32 or i64.", words[0]);
        return std::nullopt;
    }
    reading.format = *format;
    const auto path = parse_path(words[1]);
    if (!path) {
        error = std::format("'{}' is not a path of field hashes (0x1a2b3c4d/0x5e6f7081, or . for none).", words[1]);
        return std::nullopt;
    }
    reading.path = *path;
    if (words.size() > 2) {
        const auto scale = number(words[2]);
        if (!scale || *scale == 0) {
            error = std::format("'{}' is not a scale: seconds are the value times the scale (0.001 for milliseconds).", words[2]);
            return std::nullopt;
        }
        reading.scale = *scale;
    }
    return reading;
}

Reply map_command(Settings& settings, std::span<const std::string> words) {
    auto& mapping = settings.mapping;
    const auto part = words.empty() ? std::string("show") : lower(words[0]);
    const auto rest = words.size() > 1 ? words.subspan(1) : std::span<const std::string>{};
    std::string error;
    if (part == "show") return {describe(mapping)};
    if (part == "clear") {
        const auto what = rest.empty() ? std::string("all") : lower(rest[0]);
        if (what == "playhead") mapping.playhead = {};
        else if (what == "keyframes") {
            mapping.keyframes.clear();
            mapping.keyframe_time = {};
        }
        else if (what == "selected") mapping.selected = {};
        else if (what == "all") mapping = {};
        else return {"Clear playhead, keyframes, selected or all."};
        return {"Mapping: " + describe(mapping), true};
    }
    if (part == "root") {
        std::optional<std::uint64_t> root;
        if (!rest.empty()) root = parse_hex(rest[0]);
        if (!root || !*root || *root > 0xffffffffULL) return {"Give the root view model's schema hash, like 0x1a2b3c4d."};
        mapping.root = static_cast<std::uint32_t>(*root);
        return {"Mapping: " + describe(mapping), true};
    }
    if (part == "playhead" || part == "selected") {
        auto reading = parse_reading(rest, error);
        if (!reading) return {error};
        if (reading->path.empty()) return {"The " + part + " needs a path."};
        (part == "playhead" ? mapping.playhead : mapping.selected) = std::move(*reading);
        return {"Mapping: " + describe(mapping), true};
    }
    if (part == "keyframes") {
        // map keyframes <array path> <format> <time path> [scale]
        std::optional<std::vector<std::uint32_t>> array;
        if (!rest.empty()) array = parse_path(rest[0]);
        if (!array || array->empty())
            return {"Give the keyframes' array path, then the format and path of the time in each, like "
                    "map keyframes 0xaaaa/0xbbbb f32 0xcccc."};
        auto reading = parse_reading(rest.subspan(1), error);
        if (!reading) return {error};
        mapping.keyframes = *array;
        mapping.keyframe_time = std::move(*reading);
        return {"Mapping: " + describe(mapping), true};
    }
    return {"Map root, playhead, keyframes, selected, show or clear."};
}

Reply camera_command(Settings& settings, std::span<const std::string> words) {
    const auto action = words.empty() ? std::string("list") : lower(words[0]);
    auto& cameras = settings.cameras;
    if (action == "list") {
        if (cameras.empty()) return {"No replay cameras added. `replayshake probe 1` logs the cameras on show."};
        std::string text = "Replay cameras:";
        for (const auto camera : cameras) text += " " + hex(camera);
        return {text};
    }
    if (action == "clear") {
        cameras.clear();
        return {"Replay cameras cleared.", true};
    }
    std::optional<std::uint64_t> value;
    if (words.size() > 1) value = parse_hex(words[1]);
    if ((action != "add" && action != "remove") || !value || !*value || *value > 0x7fffffffULL)
        return {"camera add|remove <a vtable offset Probe shows, of a camera or its director: 0x...>, camera list or camera clear."};
    const auto offset = static_cast<std::uintptr_t>(*value);
    const auto found = std::find(cameras.begin(), cameras.end(), offset);
    if (action == "add") {
        if (found != cameras.end()) return {hex(offset) + " is already a replay camera."};
        if (cameras.size() >= max_cameras) return {std::format("At most {} replay cameras.", max_cameras)};
        cameras.push_back(offset);
        return {hex(offset) + " is a replay camera: the shake shows while it is on show.", true};
    }
    if (found == cameras.end()) return {hex(offset) + " is not a replay camera."};
    cameras.erase(found);
    return {hex(offset) + " removed.", true};
}

std::string list_text(const Settings& settings, const Editor& editor) {
    std::string text;
    if (keyframe_mode(editor)) {
        const auto& keyframes = *editor.keyframes;
        if (keyframes.empty()) return "The replay has no keyframes: the default shake runs throughout (" +
            describe(settings.fallback) + ").";
        for (std::size_t i = 0; i < keyframes.size(); ++i) {
            const auto found = key_at(settings.keys, keyframes[i]);
            text += std::format("{}Keyframe {} at {}: {}", i ? "\n" : "", i + 1, seconds(keyframes[i]),
                found ? describe(settings.keys[*found].shake) + ", " + std::string(name(settings.keys[*found].blend))
                      : "default (" + describe(settings.fallback) + ")");
        }
        return text;
    }
    if (settings.keys.empty()) return "No keys: the default shake runs throughout (" + describe(settings.fallback) + ").";
    for (std::size_t i = 0; i < settings.keys.size(); ++i)
        text += std::format("{}Key {} at {}: {}, {}", i ? "\n" : "", i + 1, seconds(settings.keys[i].time),
            describe(settings.keys[i].shake), name(settings.keys[i].blend));
    return text;
}

// JSON. Floats are written rounded, so 0.6 is saved as 0.6 and not 0.6000000238418579.
double tidy(float value) { return std::round(static_cast<double>(value) * 1e5) / 1e5; }
Json shake_json(const Shake& shake) {
    Json json = Json::object();
    json["preset"] = std::string(name(shake.preset));
    json["intensity"] = tidy(shake.intensity);
    json["frequency"] = tidy(shake.frequency);
    json["roughness"] = tidy(shake.roughness);
    std::array<double, channel_count> amounts{};
    for (std::size_t c = 0; c < channel_count; ++c) amounts[c] = tidy(shake.amounts[c]);
    json["amounts"] = Json(amounts);
    return json;
}
template <class T> void read_number(const Json& json, std::string_view key, T& out) {
    if (!json.is_object() || !json.contains(key) || !json.at(key).is_number()) return;
    const auto value = json.at(key).get<double>();
    if (std::isfinite(value)) out = static_cast<T>(value);
}
Shake shake_from(const Json& json, const Shake& base) {
    Shake shake = base;
    if (!json.is_object()) return shake;
    if (json.contains("preset") && json.at("preset").is_string())
        if (const auto preset = preset_named(json.at("preset").string())) shake.preset = *preset;
    read_number(json, "intensity", shake.intensity);
    read_number(json, "frequency", shake.frequency);
    read_number(json, "roughness", shake.roughness);
    if (json.contains("amounts") && json.at("amounts").is_array()) {
        const auto& amounts = json.at("amounts");
        for (std::size_t c = 0; c < channel_count && c < amounts.size(); ++c)
            if (amounts.at(c).is_number()) shake.amounts[c] = amounts.at(c).get<float>();
    }
    return clamped(shake);
}
Json hex_array(std::span<const std::uint32_t> values) {
    Json json = Json::array();
    for (const auto value : values) json.push_back(Json(hex(value)));
    return json;
}
std::optional<std::uint64_t> hex_value(const Json& json) {
    if (json.is_string()) return parse_hex(json.string());
    if (json.is_number_integer()) return json.get<std::uint64_t>();
    return std::nullopt;
}
std::vector<std::uint32_t> path_from(const Json& json) {
    std::vector<std::uint32_t> path;
    if (!json.is_array()) return path;
    for (const auto& item : json) {
        const auto value = hex_value(item);
        if (!value || *value > 0xffffffffULL || path.size() >= max_path) return {};
        path.push_back(static_cast<std::uint32_t>(*value));
    }
    return path;
}
Json reading_json(const Reading& reading) {
    Json json = Json::object();
    json["path"] = hex_array(reading.path);
    json["format"] = std::string(name(reading.format));
    json["scale"] = reading.scale;
    return json;
}
Reading reading_from(const Json& json) {
    Reading reading;
    if (!json.is_object()) return reading;
    if (json.contains("path")) reading.path = path_from(json.at("path"));
    if (json.contains("format") && json.at("format").is_string())
        if (const auto format = format_named(json.at("format").string())) reading.format = *format;
    read_number(json, "scale", reading.scale);
    if (reading.scale == 0) reading.scale = 1;
    return reading;
}
} // namespace

std::optional<Format> format_named(std::string_view text) noexcept {
    for (std::size_t i = 0; i < format_names.size(); ++i)
        if (format_names[i] == text) return static_cast<Format>(i);
    return std::nullopt;
}
std::string_view name(Format format) noexcept {
    const auto index = static_cast<std::size_t>(format);
    return index < format_names.size() ? format_names[index] : "f32";
}
std::size_t size(Format format) noexcept {
    return format == Format::f64 || format == Format::i64 ? 8 : 4;
}
double decode(Format format, const void* bytes) noexcept {
    switch (format) {
    case Format::f64: { double v; std::memcpy(&v, bytes, 8); return v; }
    case Format::i32: { std::int32_t v; std::memcpy(&v, bytes, 4); return v; }
    case Format::u32: { std::uint32_t v; std::memcpy(&v, bytes, 4); return v; }
    case Format::i64: { std::int64_t v; std::memcpy(&v, bytes, 8); return static_cast<double>(v); }
    default: { float v; std::memcpy(&v, bytes, 4); return v; }
    }
}

Shake default_shake() noexcept {
    auto shake = preset_shake(Preset::handheld);
    shake.intensity = 0;
    return shake;
}
Shake new_key_shake(const Shake& fallback) noexcept {
    auto shake = fallback;
    if (!(shake.intensity > 0)) shake.intensity = 1;
    return clamped(shake);
}

Json to_json(const Settings& settings) {
    Json json = Json::object();
    json["version"] = 1;
    json["enabled"] = settings.enabled;
    json["seed"] = settings.seed;
    json["export_fps"] = settings.export_fps;
    json["default"] = shake_json(settings.fallback);
    Json keys = Json::array();
    for (const auto& key : settings.keys) {
        Json item = Json::object();
        item["time"] = key.time;
        item["blend"] = std::string(name(key.blend));
        item["shake"] = shake_json(key.shake);
        keys.push_back(std::move(item));
    }
    json["keys"] = std::move(keys);
    Json cameras = Json::array();
    for (const auto camera : settings.cameras) cameras.push_back(Json(hex(camera)));
    json["cameras"] = std::move(cameras);
    json["cameras_build"] = settings.cameras_build;
    json["gameplay"] = settings.gameplay;
    Json mapping = Json::object();
    mapping["root"] = hex(settings.mapping.root);
    mapping["playhead"] = reading_json(settings.mapping.playhead);
    mapping["keyframes"] = hex_array(settings.mapping.keyframes);
    mapping["keyframe_time"] = reading_json(settings.mapping.keyframe_time);
    mapping["selected"] = reading_json(settings.mapping.selected);
    json["mapping"] = std::move(mapping);
    return json;
}

Settings settings_from_json(const Json& json) {
    Settings settings;
    if (!json.is_object()) return settings;
    try {
        if (json.contains("enabled") && json.at("enabled").is_boolean()) settings.enabled = json.at("enabled").get<bool>();
        double seed = settings.seed;
        read_number(json, "seed", seed);
        if (seed >= 0 && seed <= 0xffffffff) settings.seed = static_cast<std::uint32_t>(seed);
        double fps = settings.export_fps;
        read_number(json, "export_fps", fps);
        if (fps >= 1 && fps <= max_export_fps) settings.export_fps = static_cast<int>(fps);
        if (json.contains("default")) settings.fallback = shake_from(json.at("default"), settings.fallback);
        if (json.contains("keys") && json.at("keys").is_array())
            for (const auto& item : json.at("keys")) {
                if (!item.is_object() || settings.keys.size() >= max_keys) continue;
                Key key;
                key.time = std::numeric_limits<double>::quiet_NaN();
                read_number(item, "time", key.time);
                if (!std::isfinite(key.time)) continue;
                if (item.contains("blend") && item.at("blend").is_string())
                    if (const auto blend = blend_named(item.at("blend").string())) key.blend = *blend;
                key.shake = item.contains("shake") ? shake_from(item.at("shake"), new_key_shake(settings.fallback))
                                                   : new_key_shake(settings.fallback);
                settings.keys.push_back(key);
            }
        sort_keys(settings.keys);
        if (json.contains("cameras") && json.at("cameras").is_array())
            for (const auto& item : json.at("cameras")) {
                const auto value = hex_value(item);
                if (value && *value && *value <= 0x7fffffffULL && settings.cameras.size() < max_cameras &&
                    std::find(settings.cameras.begin(), settings.cameras.end(), *value) == settings.cameras.end())
                    settings.cameras.push_back(static_cast<std::uintptr_t>(*value));
            }
        if (json.contains("cameras_build") && json.at("cameras_build").is_string() && json.at("cameras_build").string().size() <= 32)
            settings.cameras_build = json.at("cameras_build").string();
        if (json.contains("gameplay") && json.at("gameplay").is_boolean()) settings.gameplay = json.at("gameplay").get<bool>();
        if (json.contains("mapping") && json.at("mapping").is_object()) {
            const auto& mapping = json.at("mapping");
            if (mapping.contains("root"))
                if (const auto root = hex_value(mapping.at("root")); root && *root <= 0xffffffffULL)
                    settings.mapping.root = static_cast<std::uint32_t>(*root);
            if (mapping.contains("playhead")) settings.mapping.playhead = reading_from(mapping.at("playhead"));
            if (mapping.contains("keyframes")) settings.mapping.keyframes = path_from(mapping.at("keyframes"));
            if (mapping.contains("keyframe_time")) settings.mapping.keyframe_time = reading_from(mapping.at("keyframe_time"));
            if (mapping.contains("selected")) settings.mapping.selected = reading_from(mapping.at("selected"));
        }
    } catch (const std::exception&) {
        // A value of the wrong type: the rest stays at its default.
    }
    return settings;
}

std::vector<Point> points(const Settings& settings, const Editor& editor) {
    if (!editor.playhead) return {};
    if (editor.keyframes) {
        if (editor.keyframes->empty()) return {};
        return timeline(*editor.keyframes, settings.keys, settings.fallback);
    }
    return timeline({}, settings.keys, settings.fallback);
}

Reply command(Settings& settings, std::string_view verb, std::span<const std::string> words, const Editor& editor) {
    const auto v = lower(verb);
    if (v == "on" || v == "off" || v == "toggle") {
        const bool on = v == "toggle" ? !settings.enabled : v == "on";
        const bool changed = settings.enabled != on;
        settings.enabled = on;
        return {on ? "Replay Shake is on." : "Replay Shake is off.", changed};
    }
    if (v == "bind")
        return {"Replay Shake has no controller combo of its own now: Replay Plus's panel switches it (its SHAKE tab: hold "
                "RB + R3 in the replay editor, or press F10)."};
    if (v == "default") {
        if (!words.empty() && lower(words[0]) == "reset") {
            settings.fallback = default_shake();
            return {"Default shake reset: " + describe(settings.fallback) + ".", true};
        }
        if (words.empty()) return {"Default shake: " + describe(settings.fallback) + "."};
        auto shake = settings.fallback;
        if (const auto error = edit(shake, nullptr, words); !error.empty()) return {error};
        const bool changed = !(shake == settings.fallback);
        settings.fallback = shake;
        return {"Default shake: " + describe(shake) + ".", changed};
    }
    if (v == "key") {
        if (words.empty()) return {"key <number|selected> <setting> <value>, or key <number|selected> reset."};
        std::string label, error;
        const bool reset = words.size() > 1 && lower(words[1]) == "reset";
        if (reset) {
            const auto index = target(settings, editor, words[0], false, label, error);
            if (!error.empty()) return {error};
            if (!index) return {label + " has no shake of its own."};
            settings.keys.erase(settings.keys.begin() + static_cast<std::ptrdiff_t>(*index));
            return {label + (keyframe_mode(editor) ? " is back to the default shake." : " removed."), true};
        }
        if (words.size() == 1) {
            const auto index = target(settings, editor, words[0], false, label, error);
            if (!error.empty()) return {error};
            return {label + ": " + (index ? describe(settings.keys[*index].shake) + ", " +
                std::string(name(settings.keys[*index].blend)) : "default (" + describe(settings.fallback) + ")")};
        }
        const auto before = settings.keys;
        const auto index = target(settings, editor, words[0], true, label, error);
        if (!index) return {error.empty() ? label + " has no shake." : error};
        auto& key = settings.keys[*index];
        if (const auto problem = edit(key.shake, &key.blend, words.subspan(1)); !problem.empty()) {
            settings.keys = before; // a keyframe given a shake only to fail to set it keeps none
            return {problem};
        }
        return {label + ": " + describe(key.shake) + ", " + std::string(name(key.blend)) + ".", true};
    }
    if (v == "add") {
        if (keyframe_mode(editor))
            return {"The replay editor's keyframes are readable: give one a shake with `replayshake key <number> ...`."};
        std::optional<double> time = editor.playhead;
        if (!words.empty()) time = number(words[0]);
        if (!time) return {words.empty() ? "The playhead is not readable yet: give a time in seconds, like `replayshake add 2.5`."
                                         : "Give the time in seconds."};
        if (key_at(settings.keys, *time)) return {"There is a key at " + seconds(*time) + " already."};
        if (settings.keys.size() >= max_keys) return {std::format("At most {} keys.", max_keys)};
        settings.keys.push_back({*time, new_key_shake(settings.fallback), Blend::smooth});
        sort_keys(settings.keys);
        const auto index = key_at(settings.keys, *time).value_or(0);
        return {std::format("Key {} at {}: {}.", index + 1, seconds(*time), describe(settings.keys[index].shake)), true};
    }
    if (v == "remove") {
        if (words.empty()) return {"remove <number>"};
        std::string label, error;
        const auto index = target(settings, editor, words[0], false, label, error);
        if (!error.empty()) return {error};
        if (!index) return {label + " has no shake of its own."};
        settings.keys.erase(settings.keys.begin() + static_cast<std::ptrdiff_t>(*index));
        return {label + (keyframe_mode(editor) ? " is back to the default shake." : " removed."), true};
    }
    if (v == "clear") {
        const bool changed = !settings.keys.empty();
        settings.keys.clear();
        return {"Every keyframe shake removed.", changed};
    }
    if (v == "clean") {
        if (!keyframe_mode(editor)) return {"Clean needs the replay editor's keyframes (it keeps the shakes on them)."};
        const auto before = settings.keys.size();
        std::erase_if(settings.keys, [&](const Key& key) { return !keyframe_at(*editor.keyframes, key.time); });
        const auto removed = before - settings.keys.size();
        return {std::format("Removed {} shake{} on no keyframe of this replay.", removed, removed == 1 ? "" : "s"), removed > 0};
    }
    if (v == "seed") {
        if (words.empty()) return {std::format("Seed {}.", settings.seed)};
        const auto seed = whole(words[0]);
        if (!seed || *seed < 0 || *seed > 0xffffffffLL) return {"The seed is a whole number from 0 to 4294967295."};
        const bool changed = settings.seed != static_cast<std::uint32_t>(*seed);
        settings.seed = static_cast<std::uint32_t>(*seed);
        return {std::format("Seed {}: the same settings now shake another way.", settings.seed), changed};
    }
    if (v == "fps") {
        if (words.empty()) return {std::format("Exports at {} frames a second.", settings.export_fps)};
        const auto fps = whole(words[0]);
        if (!fps || *fps < 1 || *fps > max_export_fps)
            return {std::format("fps is the export's frame rate: 24, 30 or 60 (at most {}).", max_export_fps)};
        const bool changed = settings.export_fps != static_cast<int>(*fps);
        settings.export_fps = static_cast<int>(*fps);
        return {std::format("Exports at {} frames a second.", settings.export_fps), changed};
    }
    if (v == "gameplay") {
        const auto word = words.empty() ? std::string() : lower(words[0]);
        if (word.empty())
            return {settings.gameplay ? "Gameplay shakes too." : "Only the replay editor shakes: gameplay stays still."};
        const bool on = word == "1" || word == "on" || word == "true";
        if (!on && word != "0" && word != "off" && word != "false") return {"gameplay 0|1"};
        const bool changed = settings.gameplay != on;
        settings.gameplay = on;
        return {on ? "Gameplay shakes too, whenever a replay camera is on show."
                   : "Only the replay editor shakes: while a game menu is up or the game hides its UI. Gameplay stays still.",
            changed};
    }
    if (v == "camera") return camera_command(settings, words);
    if (v == "map") return map_command(settings, words);
    if (v == "presets") {
        std::string text = "Presets:";
        for (std::size_t i = 0; i + 1 < preset_names.size(); ++i)
            text += std::format(" {} ({})", preset_names[i], describe(preset_shake(static_cast<Preset>(i))));
        return {text + "; custom keeps its own values."};
    }
    if (v == "list") return {list_text(settings, editor)};
    return {std::format("Unknown verb '{}'. Verbs: on, off, toggle, default, key, add, remove, clear, clean, seed, fps, "
        "camera, map, presets, list, gameplay, status, test, probe, history, copies, passes, after, source.", verb)};
}

Json shake_to_json(const Shake& shake) { return shake_json(shake); }
Shake shake_from_json(const Json& json, const Shake& base) {
    Shake start = base;
    if (json.is_object() && !json.contains("amounts") && json.contains("preset") && json.at("preset").is_string())
        if (const auto preset = preset_named(json.at("preset").string())) apply_preset(start, *preset);
    return shake_from(json, start);
}

std::string describe(const Shake& shake) {
    return std::format("{} {:.0f}%, {:.2g} Hz", name(shake.preset), shake.intensity * 100.0f, shake.frequency);
}
std::string seconds(double value) { return std::format("{:.2f} s", value); }
std::string hex(std::uint64_t value) { return std::format("0x{:x}", value); }
std::optional<std::uint64_t> parse_hex(std::string_view text) noexcept {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    if (text.empty() || text.size() > 16) return std::nullopt;
    std::uint64_t value{};
    const auto* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, value, 16);
    if (error != std::errc{} || at != end) return std::nullopt;
    return value;
}
std::optional<std::uint32_t> parse_binding(std::string_view text) {
    std::string squeezed; // its names without spaces, dashes or underscores: "d-pad up" is "dpadup"
    for (const char c : lower(text))
        if (c != ' ' && c != '-' && c != '_') squeezed += c;
    if (squeezed.empty()) return std::nullopt;
    // A number, as ReSkate's own `bind` commands take it (decimal, or 0x hex).
    if (squeezed.find_first_not_of("0123456789") == std::string::npos ||
        (squeezed.starts_with("0x") && squeezed.size() > 2 && squeezed.find_first_not_of("0123456789abcdef", 2) == std::string::npos)) {
        const auto value = squeezed.starts_with("0x") ? parse_hex(squeezed) : [&]() -> std::optional<std::uint64_t> {
            const auto number_value = whole(squeezed);
            if (!number_value || *number_value < 0) return std::nullopt;
            return static_cast<std::uint64_t>(*number_value);
        }();
        if (!value || *value > 0xffffffffULL || !valid_action_binding(static_cast<std::uint32_t>(*value))) return std::nullopt;
        return static_cast<std::uint32_t>(*value);
    }
    // Buttons by name (controller_bindings.h's bits: the XInput buttons, the triggers above them).
    struct Name {
        std::string_view name;
        std::uint32_t bit;
    };
    constexpr Name names[]{{"lb", 0x100}, {"l1", 0x100}, {"rb", 0x200}, {"r1", 0x200}, {"lt", 0x10000}, {"l2", 0x10000},
        {"rt", 0x20000}, {"r2", 0x20000}, {"l3", 0x40}, {"ls", 0x40}, {"r3", 0x80}, {"rs", 0x80}, {"a", 0x1000},
        {"cross", 0x1000}, {"b", 0x2000}, {"circle", 0x2000}, {"x", 0x4000}, {"square", 0x4000}, {"y", 0x8000},
        {"triangle", 0x8000}, {"up", 1}, {"dpadup", 1}, {"down", 2}, {"dpaddown", 2}, {"left", 4}, {"dpadleft", 4},
        {"right", 8}, {"dpadright", 8}, {"start", 0x10}, {"options", 0x10}, {"menu", 0x10}, {"back", 0x20},
        {"view", 0x20}, {"share", 0x20}, {"create", 0x20}, {"select", 0x20}};
    std::uint32_t combo{};
    std::string_view rest = squeezed;
    while (true) {
        const auto plus = rest.find('+');
        const auto part = rest.substr(0, plus);
        const auto found = std::find_if(std::begin(names), std::end(names), [&](const Name& entry) { return entry.name == part; });
        if (found == std::end(names) || (combo & found->bit)) return std::nullopt;
        combo |= found->bit;
        if (plus == std::string_view::npos) break;
        rest.remove_prefix(plus + 1);
    }
    if (!valid_controller_combo(combo)) return std::nullopt;
    return combo;
}
std::optional<std::vector<std::uint32_t>> parse_path(std::string_view text) {
    std::vector<std::uint32_t> path;
    if (text == ".") return path;
    while (!text.empty()) {
        const auto slash = text.find('/');
        const auto part = text.substr(0, slash);
        const auto value = parse_hex(part);
        if (!value || *value > 0xffffffffULL || path.size() >= max_path) return std::nullopt;
        path.push_back(static_cast<std::uint32_t>(*value));
        if (slash == std::string_view::npos) break;
        text.remove_prefix(slash + 1);
        if (text.empty()) return std::nullopt; // a trailing slash
    }
    if (path.empty()) return std::nullopt;
    return path;
}
std::string path_text(std::span<const std::uint32_t> path) {
    std::string text;
    for (std::size_t i = 0; i < path.size(); ++i) text += (i ? "/" : "") + hex(path[i]);
    return text;
}
} // namespace dingosdk::replay_shake
