#pragma once
#include "Engine/Core/Json/json.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Replay Plus's settings, with no game access (README.md): every value the replay editor's panel,
// the `replayplus` console verbs, the saved settings and the presets know, described once in a table
// (fields()), so the four can never disagree. The runtime (replay_plus.h) owns the one live copy on
// the game thread; the panel and the menu change it only through `replayplus` commands.
namespace dingosdk::replay_plus {

// ---- What the values are ----------------------------------------------------------------------

// The lens is one lens, every part of it a setting (its projection, image circle, edge compression, zoom,
// squeeze and glass): the game's plain picture traced back through it. At no distortion, zoom and squeeze
// 1x and no glass there is none, and the picture is the game's own. Every look (a fisheye adapter on a
// camcorder, a modern circular fisheye) is a preset of these settings, nothing more, so anyone can make,
// change and send a lens as a preset's file. (Settings saved before 2026-11-17 had a lens choice, "off",
// "game" or "fisheye", "true angles" and a camcorder switch: they are read as these settings, as they drew.)
// What the field of view of the replay camera is: the editor's own (its keyframes), fixed here, or what
// the fisheye needs to show all it sees (no wider than `fov_widest`): wider than that shows no more, it
// only enlarges the middle more.
enum class FovMode : std::uint8_t { game, fixed, automatic, count };
// Black bars: none, 4:3 at the sides (a 4:3 camera's frame on a wide screen), or 2.39:1 at top and bottom.
enum class Bars : std::uint8_t { none, four_three, scope, count };
// What is laid over the picture: nothing, a picture (a PNG of the player's own in the overlays folder,
// spanning the window: a lens hood's black corners, say), or a round black edge at the
// lens's image circle (a circular fisheye's).
enum class Hood : std::uint8_t { none, picture, circle, count };
inline constexpr std::string_view mk1_hood_file = "Mk1 hood.png";
// The replay editor's own picture shape: the screen's, or one of these, rendered by the game at that
// shape in a window of it in the middle of the screen (replay_plus_window.h), so exports and recordings
// come out that shape, never stretched.
enum class Aspect : std::uint8_t { screen, four_three, three_two, one_one, four_five, nine_sixteen, scope, count };
inline constexpr std::array<std::string_view, 7> aspect_names{"screen", "4:3", "3:2", "1:1", "4:5", "9:16", "2.39:1"};
inline constexpr std::array<float, 7> aspect_ratios{0.0f, 4.0f / 3.0f, 1.5f, 1.0f, 0.8f, 9.0f / 16.0f, 2.39f};
// Depth of field (the game's own, PostProcess.ForceDof*): none, one focus for the whole replay, focus
// keyed along the replay's time, or focus that follows whatever is in the middle of the picture.
enum class Focus : std::uint8_t { off, fixed, keys, autofocus, count };

inline constexpr std::array<std::string_view, 3> fov_mode_names{"game", "fixed", "auto"};
// Image circles, in half-heights of the 4:3 picture (where a lens sees 90 degrees off-axis): 1.54 for a
// fisheye adapter on a camcorder with a 1/3" sensor, where the black corners of its picture begin; 5/3 puts
// 90 degrees at the 4:3 corner instead; a modern circular fisheye's round picture is 1.54 too.
inline constexpr float mk1_circle = 1.54f, mk1_corner_circle = 5.0f / 3.0f, modern_circle = 1.54f;
inline constexpr std::array<std::string_view, 3> bars_names{"none", "4:3", "2.39:1"};
inline constexpr std::array<std::string_view, 3> hood_names{"none", "picture", "circle"};
inline constexpr std::array<std::string_view, 4> focus_names{"off", "fixed", "keys", "auto"};

// A focus key: from this replay time (seconds, as the game's replay clock counts them) the focus
// turns towards this distance and blur, easing in and out as the game's own keyframes do.
struct FocusKey {
    double time{};
    float distance{3.0f};
    float blur{1.0f};
    bool operator==(const FocusKey&) const = default;
};
inline constexpr std::size_t max_focus_keys = 256;
inline constexpr float sharp_zoom = 2.3f, sharp_most = 2.0f;
// A shake key: from this replay time the shake is on (or off), fading in (or out) over a fifth of a
// second of the replay. Before the first key the shake is the other way (each key is a change).
struct ShakeKey {
    double time{};
    bool on{true};
    bool operator==(const ShakeKey&) const = default;
};
inline constexpr std::size_t max_shake_keys = 256;

struct Settings {
    bool enabled = true; // Replay Plus as a whole; off, nothing of it shows and the game is as it was

    // LENS
    float zoom = 1.0f;     // 1 the widest true view the game's field of view can feed; more crops in
    // Where it sees 90 degrees off-axis (its image circle), in half the picture's height: 1.54 an adapter's
    // (its black corners begin there), 5/3 at the 4:3 picture's corners. Smaller sees more.
    float circle = 1.667f; // 90 degrees at a 4:3 picture's corners (mk1_circle until 2026-11-17)
    // How much more it takes in across than up and down (1 none; 1.33 an anamorphic adapter's).
    float squeeze = 1.0f;
    // The lens's character: its projection (`curve`: 0 none, the game's own rectilinear picture, straight
    // lines straight; 0.5 stereographic; 1 equidistant, the Mk1's; 1.5 equisolid, a modern fisheye's; 2
    // orthographic), its colour fringe towards the edges (lateral chromatic aberration), its corners'
    // shading (stops darker at the corner) and how soft its edges are.
    float curve = 0.0f;
    float fringe = 0.0f;
    float shade = 0.0f;
    float soft = 0.0f;
    // fisheye: veiling glare, a little of the light spread over all of the picture as cheap glass does
    // (lifting the blacks): 1 is a cheap adapter's on a typical daylight frame.
    float glare = 0.0f;
    // Edge compression: the lens's angles pulled in towards its edge, so a lens wider than the game can draw
    // fits in its picture: an angle th off-axis is seen at th (1 - compress th / 90 degrees), the same every
    // way round, the middle as it is. 0 the lens's own angles; 0.1 a tenth less at 90 degrees.
    float compress = 0.0f;
    FovMode fov_mode = FovMode::game;
    float fov = 120.0f;    // fixed: the replay camera's vertical field of view, degrees
    float fov_widest = 150.0f; // auto: the widest it goes (the middle is enlarged more the wider it is)

    // FRAME (overlays)
    Bars bars = Bars::none;
    Hood hood = Hood::none;
    std::string hood_file; // Hood::picture: the PNG's name in the overlays folder
    float hood_opacity = 1.0f;
    // How far the overlay's dark edge reaches in from the window's corners: 1 as drawn, 0.5 half as far
    // (the overlay drawn larger about the middle), 0 none; up to 2.
    float hood_size = 1.0f;
    bool hood_fit = true;  // the lens is fitted to what the overlay leaves showing

    // GRADE (colour, Replay Plus's own, in the picture and in exports)
    // A video camera's curve in place of the game's: the game's own tone curve undone (to the scene's
    // light), then the camera's: its gamma (a Rec.709 power curve with its linear toe), a knee (above it the
    // highlights rise only by the knee's slope), its black level (pedestal), white clip and colour level
    // (saturation about Y'). The defaults are a Rec.709 camcorder's: knee 85% at slope 30%, black -2%, clip 104%, colour 105%. With it on, exposure,
    // contrast and saturation work on the scene's light, and the corner shade with them (lens::graded).
    bool camera = false;
    float camera_gamma = 0.45f;
    float camera_knee = 0.85f;
    float camera_knee_slope = 0.30f;
    float camera_black = -0.02f;
    float camera_clip = 1.04f;
    float camera_colour = 1.05f;
    float exposure = 0.0f;    // stops
    float contrast = 1.0f;
    float saturation = 1.0f;
    float warmth = 0.0f;      // -1 cool (blue) .. 1 warm (amber)
    float tint = 0.0f;        // -1 green .. 1 magenta
    float black = 0.0f;       // lifts the blacks (a faded, matte look) 0..0.25
    float vignette = 0.0f;    // a soft darkening towards the corners, 0..1
    float vignette_soft = 0.5f;  // how gradually it darkens: 0 only the very corners, 1 from near the middle
    float vignette_round = 1.0f; // 1 a circle, 0 the picture's own shape
    // As XLGraphics (Skater XL) has them: every colour turned round the colour wheel; a coloured filter over
    // the lens; lift, gamma and gain (shadows, midtones, highlights, on the picture's values); split toning
    // (the shadows and the highlights tinted apart); and film grain of Replay Plus's own.
    float hue = 0.0f;            // degrees, -180..180
    float filter = 0.0f;         // 0..1 of ...
    float filter_hue = 30.0f;    // ... this hue (degrees: 0 red, 30 amber, 120 green, 220 blue)
    float lift = 0.0f;           // -0.3..0.3
    float gamma = 1.0f;          // 0.5..2 (above 1 brighter midtones)
    float gain = 1.0f;           // 0.5..1.5
    float split_shadows = 0.0f, split_shadows_hue = 220.0f;
    float split_highlights = 0.0f, split_highlights_hue = 40.0f;
    float split_balance = 0.0f;  // -1 more of the picture is shadow .. 1 more is highlight
    float grain = 0.0f;          // 0..1
    float grain_size = 1.0f;     // pixels of a 1080-line picture

    // GAME (the game's own effects, as its settings: held while the replay editor is open, and while skating
    // too with game_skating; in videos as on the screen). Each default is the game's own value (read on build
    // 25414733, 2026-11-17), and only what differs from it is held.
    bool game_look = false;
    bool game_skating = false;
    float game_exposure = 0.0f; // stops: PostProcess.ForceEVCompensation
    float game_grain = 0.0f;    // 0..1: DingoVideoFilterSettings.FilmGrain*
    float game_fringe = 0.0f;   // 0..1: DingoVideoFilterSettings.ChromaticAberration*
    bool game_motion = true;            // WorldRender.MotionBlurEnable
    float game_shutter = 250.0f;        // 1 / WorldRender.MotionBlurFixedShutterTime (the game's 0.004 s)
    float game_motion_amount = 1.0f;    // WorldRender.MotionBlurScale
    float game_bloom = 1.0f;            // PostProcess.ForceBloomScale (1: the game's own, unforced; 0: none)
    bool game_flares = true;            // WorldRender.LensFlaresEnable
    bool game_dirt = false;             // WorldRender.LensDirtEnable
    bool game_ao = true;                // DingoShaderSettings.AOMode (1 on, 0 off)
    bool game_contact_shadows = true;   // WorldRender.ScreenSpaceContactShadowsEnable
    bool game_fog = true;               // WorldRender.SkyHeightFogEnable, SkyDepthFogEnable
    bool game_vignette = true;          // PostProcess.VignetteEnable (the game's own vignette)
    bool game_local_exposure = true;    // PostProcess.LocalExposureEnable
    float game_detail = 1.0f;           // Mesh.GlobalLodScale
    float game_shadow_distance = 1.75f; // Mesh.ShadowDistanceScale
    float game_view_distance = 20000.0f; // Render.ViewDistance (m)
    // The game's own barrel distortion (its replay editor's Effects), its three settings (the editor shows
    // one): DingoVideoFilterSettings.BarrelDistortionGain, ...CubicGain (its outer curve), ...Stretch.
    float game_barrel = 0.0f, game_barrel_edge = 0.0f, game_barrel_stretch = 0.0f;

    // FOCUS (depth of field, the game's own: PostProcess.ForceDof*, held while the replay editor is open)
    Focus focus = Focus::off;
    float focus_distance = 3.0f; // metres: fixed, and where a new key starts
    float focus_blur = 1.0f;     // how far out of focus the rest goes
    float focus_speed = 0.5f;    // auto: how quickly the focus follows (0 slow, 1 at once)
    // How deep the sharp zone around the focus is, as a share of the focus distance's nearness: the game
    // blurs what is nearer than distance / (1 + depth) and further than distance / (1 - depth), fully
    // a little way beyond (replay_plus.cpp focus_ranges).
    float focus_depth = 0.15f;
    std::vector<FocusKey> focus_keys; // keys: by time
    // (The FOCUS tab is not part of presets: it belongs to the replay in hand.)
    // The shake switched on and off along the replay (by time; none: Replay Shake's switch holds for all
    // of it). Like the focus keys, they belong to the replay in hand, not to presets.
    std::vector<ShakeKey> shake_keys;
    // R3 is Replay Plus's button in the replay editor: held, RB opens the panel and the D-pad and
    // triggers set focus and shake keys; a tap of it alone is still the game's own (Photo).
    bool pad_shortcuts = true;

    // LIGHT (a light on the camera)
    bool light = false;
    float light_power = 1.0f;  // 0..8
    float light_range = 10.0f; // metres it reaches
    float light_cone = 70.0f;  // degrees across (180: all round, a bare bulb)
    float light_soft = 0.5f;   // how soft the cone's edge is
    float light_warmth = 0.2f; // -1 cold LED .. 1 tungsten

    // VIDEO (the replay editor's picture shape; render resolution: the replay editor's and gameplay's)
    Aspect replay_aspect = Aspect::screen;
    // With a fisheye, the replay editor renders finer by as much as the lens enlarges the middle of the
    // game's picture past sharp_zoom (its enlargement at 120 degrees), up to sharp_most: the middle then
    // has as much detail as at 120 degrees (replay_plus.cpp keep_sharp_scale).
    bool sharp_middle = true;
    bool resolution = false;
    float replay_scale = 1.5f;   // Render.ResolutionScale while the replay editor is open
    float gameplay_scale = 1.0f; // ...and while skating
    // The game's texture streaming pool (TextureStreaming.PoolSize, made anew as each map loads) raised for the
    // card: the game's own 1.4 GB is a console's, and a big map's textures turn to mush in it.
    bool bigger_textures = true;

    // The controller combo that, held, opens the panel in the replay editor (ReSkate's binding form,
    // controller_bindings.h: XInput's button bits); 0: RB + R3.
    std::uint32_t panel_bind = 0;
    std::string preset; // the preset last loaded, by name (shown in the panel)

    bool operator==(const Settings&) const = default;
};
// Whether Replay Plus's lens does anything: it bends, crops or squeezes the picture, or has glass. (Off, the
// picture is the game's own, pixel for pixel.)
inline bool lens_on(const Settings& s) noexcept {
    return s.enabled && (s.curve != 0 || s.zoom != 1 || s.squeeze != 1 || s.compress != 0 || s.fringe != 0 || s.shade != 0 || s.glare != 0 ||
                         s.soft != 0);
}

// ---- The table every value is described in ---------------------------------------------------

enum class Kind : std::uint8_t { number, toggle, choice };
enum class Tab : std::uint8_t { shake, lens, frame, grade, focus, light, game, video, presets, count };
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Tab::count)> tab_names{
    "SHAKE", "LENS", "FRAME", "COLOUR", "FOCUS", "LIGHT", "GAME", "VIDEO", "PRESETS"};

struct Field {
    std::string_view key;   // as `replayplus set <key> <value>` and the presets take it: "lens.zoom"
    std::string_view label; // as the panel shows it: "Zoom"
    std::string_view hint;  // one line on what it does
    Tab tab;
    Kind kind;
    float low{}, high{}, step{}; // numbers: the range and the panel's step
    float shown{1};              // numbers: shown times this (100: a percentage)
    std::string_view unit;       // after the number as shown: "%", "°", " m", " EV"
    std::span<const std::string_view> choices; // choices: their names (as saved), and labels below
    std::span<const std::string_view> labels;
    float (*get)(const Settings&){};
    void (*set)(Settings&, float){};
    // Only shown (and only meaningful) when this says so; null: always.
    bool (*relevant)(const Settings&){};
    // Stepped by a share of itself rather than an amount (a distance: fine near, coarse far).
    bool logarithmic{};
};
std::span<const Field> fields();
const Field* field(std::string_view key);
// A field's value as the panel and the console show it ("1.20x", "4:3", "On").
std::string shown(const Field& field, const Settings& settings);
// Sets a field from text: a number, a choice's name or label, on/off. Within its range. False and
// why when the text means nothing for it.
bool assign(const Field& field, Settings& settings, std::string_view text, std::string& why);
// Steps a field as the panel's left / right do (`steps` of them: negative is left). Choices wrap.
void step(const Field& field, Settings& settings, int steps);
// Every value within its range; anything not finite becomes its default.
Settings clamped(Settings settings);

// ---- Saving ---------------------------------------------------------------------------------
Json to_json(const Settings& settings);
// Whatever it can read; anything missing or out of range stays at its default.
Settings settings_from_json(const Json& json);

// ---- Presets ---------------------------------------------------------------------------------
// A preset is a whole look to share: every value above but the panel's own, Replay Shake's default
// shake, and (optionally) the game's own replay editor effects. A JSON file in the presets folder: shared
// as that file (Export puts a copy in the export folder; one put in the import folder is added).
struct Preset {
    std::string name, author, notes;
    Settings settings;
    Json shake;   // Replay Shake's default shake and switch (replay_shake.h: shake_json), or null: unchanged
    Json game;    // the game's replay editor effect values (ReplayEffect_* options), or null: unchanged
};
Json preset_json(const Preset& preset);
// False and why when it is not a preset.
bool preset_from_json(const Json& json, Preset& preset, std::string& why);
// A preset's file name for its name: letters, digits, spaces, '-', '_' and brackets kept, the rest dropped.
std::string preset_file_name(std::string_view name);
// A name as a preset keeps it (what its file name keeps, at most 40 characters, no spaces at either end;
// empty: none), and `name` or with a number after it (" 2", " 3", ...) so that no preset in `taken` has its
// file (Windows' file names are the same whatever their case).
std::string clean_preset_name(std::string_view name);
std::string unused_preset_name(std::string_view name, std::span<const std::string> taken);
bool same_preset_file(std::string_view a, std::string_view b);
// What loading a preset changes: every value it has, but not the panel's own (its bind, which preset).
Settings apply_preset(const Settings& current, const Preset& preset);
// The presets Replay Plus writes into its presets folder (replay_plus.cpp set_up_folders: once, and
// again when they are newer), and the names its earlier versions wrote.
inline constexpr int built_in_presets_version = 6;
std::vector<Preset> built_in_presets();
std::vector<std::string> older_built_in_names();
std::vector<std::string> retired_built_in_names();

// ---- Focus keys ------------------------------------------------------------------------------
struct FocusAt {
    float distance{}, blur{};
};
// The focus at a replay time: between keys it eases from one to the next (as the game's keyframes
// do); before the first and after the last it holds theirs; with no keys, the fixed values.
FocusAt focus_at(const Settings& settings, double time);
// What the game's own depth of field is given for a focus (its "sprite" ranges, PostProcess.ForceSpriteDof*,
// which its circular depth of field follows; its physical one blurs only what is nearer than the focus):
// sharp from near_end to far_start, fully blurred nearer than near_start and further than far_end
// (metres), as blurred as blur_max. The sharp zone is `depth` of the focus's nearness either side, the
// blur ramping in over as much again and a quarter more. The game draws the blur in its own picture,
// which a fisheye enlarges in the middle `centre_zoom` times (lens::centre_zoom): blur_max is shrunk as
// much, so the picture's blur stays as asked at any field of view.
struct FocusRanges {
    float near_start{}, near_end{}, far_start{}, far_end{}, blur_max{};
};
inline constexpr float focus_far_away = 10000.0f;
FocusRanges focus_ranges(FocusAt focus, float depth, float centre_zoom);
// The key at `time` (within a frame, 1/30 s) set to these, or a new one there. Keys stay in time order.
void set_focus_key(Settings& settings, double time, float distance, float blur);
// The key within `tolerance` of `time` removed; false when there is none.
bool remove_focus_key(Settings& settings, double time, double tolerance = 1.0 / 30.0);
// The key at `time` (within a frame), if any.
const FocusKey* focus_key_at(const Settings& settings, double time);

// ---- Shake keys ------------------------------------------------------------------------------
// Whether the shake is on at a replay time: the last key at or before it says; before the first key,
// the other way round from it. With no keys, `otherwise` (Replay Shake's own switch).
bool shake_on_at(const Settings& settings, double time, bool otherwise);
// The shake switched at `time`: a key there (within a frame) is taken away, else one is made that turns
// the shake the other way from how it is there. Keys that change nothing are dropped. The shake's state
// from `time` on, after.
bool toggle_shake_key(Settings& settings, double time, bool otherwise);
// The key within `tolerance` of `time` removed; false when there is none.
bool remove_shake_key(Settings& settings, double time, double tolerance = 1.0 / 30.0);
// Keys that change nothing (the same as the one before) dropped, in time order.
void tidy_shake_keys(Settings& settings);

// ---- Verbs -----------------------------------------------------------------------------------
struct Reply {
    std::string text;
    bool changed{}; // the settings changed: save them
};
// One `replayplus` verb that only touches the settings: `set <key> <value>`, `get <key>`, `step <key>
// <n>`, `reset [<key>|all]`, `on`, `off`, `bind <combo>`, `list [tab]`. Others (presets, status, the
// research verbs) are the runtime's; unknown ones say so.
Reply command(Settings& settings, std::string_view verb, std::span<const std::string> words);
inline constexpr std::array<std::string_view, 9> settings_verbs{"set", "get", "step", "reset", "on", "off", "bind", "list", "values"};

// ---- Small helpers shared with the runtime and the panel --------------------------------------
std::string lower(std::string_view text);
// Whether the settings ask anything of the picture (the overlay pass) / of the game's views (the
// field of view) / of the game's own settings while the replay editor is open.
bool wants_picture(const Settings& s) noexcept;
bool wants_views(const Settings& s) noexcept;
bool wants_game_settings(const Settings& s) noexcept;
} // namespace dingosdk::replay_plus
