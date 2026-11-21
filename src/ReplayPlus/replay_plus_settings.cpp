#include "replay_plus_settings.h"
#include "replay_plus_lens.h"
#include "Extension/ReplayShake/replay_shake_settings.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace dingosdk::replay_plus {
namespace {
// ---- The fields --------------------------------------------------------------------------------
constexpr std::array<std::string_view, 3> fov_mode_labels{"The editor's", "Fixed", "Auto"};
constexpr std::array<std::string_view, 3> bars_labels{"Off", "4:3", "2.39:1"};
constexpr std::array<std::string_view, 3> hood_labels{"Off", "Picture", "Circle"};
constexpr std::array<std::string_view, 4> focus_labels{"Off", "Fixed", "Keyframed", "Auto"};
constexpr std::array<std::string_view, 7> aspect_labels{"The screen's", "4:3", "3:2", "1:1", "4:5 (portrait)", "9:16 (portrait)", "2.39:1"};
constexpr std::array<std::string_view, 0> no_choices{};

template <class E> float index_of(E value) { return static_cast<float>(static_cast<int>(value)); }
template <class E> E enum_from(float value) {
    const int count = static_cast<int>(E::count);
    const int index = std::clamp(static_cast<int>(std::lround(value)), 0, count - 1);
    return static_cast<E>(index);
}
bool bent(const Settings& s) { return s.curve > 0; }
bool hood_on(const Settings& s) { return s.hood != Hood::none; }
bool camera_on(const Settings& s) { return s.camera; }

constexpr Tab t_lens = Tab::lens, t_frame = Tab::frame, t_grade = Tab::grade, t_focus = Tab::focus, t_light = Tab::light, t_game = Tab::game,
              t_video = Tab::video, t_experimental = Tab::experimental;
bool game_on(const Settings& s) { return s.game_look; }
constexpr Kind k_number = Kind::number, k_toggle = Kind::toggle, k_choice = Kind::choice;
// The order here is the order on each of the panel's tabs.
const Field all_fields[]{
    // LENS: one lens, every part of it here (the looks are presets of these settings)
    {"lens.curve", "Distortion", "The projection: 0% none (the game's own picture), 100% equidistant (a fisheye adapter), 150% equisolid (a modern "
     "fisheye), 200% orthographic", t_lens, k_number, 0, 2, 0.02f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.curve; }, +[](Settings& s, float v) { s.curve = v; }, nullptr},
    {"lens.circle", "Image circle", "Where the lens sees 90 degrees off-axis, in half the picture's height: 1.67 a 4:3 picture's corners, 2.04 a "
     "16:9 one's (a full-frame fisheye); less, a round picture inside the frame. Smaller sees more", t_lens, k_number, 0.8f, 3.0f, 0.001f, 1, "",
     no_choices,
     no_choices, +[](const Settings& s) { return s.circle; }, +[](Settings& s, float v) { s.circle = v; }, &bent},
    {"lens.compress", "Edge compression", "The lens's angles pulled in towards its edge, so a lens wider than the game can draw fits in its picture: "
     "it sees this much less at 90 degrees off-axis, the middle as it is. 0% the lens's own angles", t_lens, k_number, 0, 0.5f, 0.001f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.compress; }, +[](Settings& s, float v) { s.compress = v; }, nullptr},
    {"lens.zoom", "Zoom", "Crops in, as a longer focal length: 1.00x the lens as set (cropped in just enough where the game's picture cannot feed "
     "all of it)", t_lens, k_number, 1.0f, 2.0f, 0.01f, 1, "x",
     no_choices, no_choices, +[](const Settings& s) { return s.zoom; }, +[](Settings& s, float v) { s.zoom = v; }, nullptr},
    {"lens.squeeze", "Squeeze", "How much more the lens takes in across than up and down: 1.00x none, 1.33x an anamorphic adapter's", t_lens,
     k_number, 0.67f, 1.5f, 0.01f, 1, "x", no_choices, no_choices, +[](const Settings& s) { return s.squeeze; },
     +[](Settings& s, float v) { s.squeeze = v; }, nullptr},
    {"lens.fringe", "Colour fringe", "Red and blue edges towards the corners, as cheap glass has", t_lens, k_number, 0, 4, 0.05f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.fringe; }, +[](Settings& s, float v) { s.fringe = v; }, nullptr},
    {"lens.shade", "Corner shade", "The corners darker, as a wide lens has them (stops at the corner)", t_lens, k_number, 0, 3, 0.05f, 1, " EV",
     no_choices, no_choices, +[](const Settings& s) { return s.shade; }, +[](Settings& s, float v) { s.shade = v; }, nullptr},
    {"lens.glare", "Glare", "A little of the picture's light spread over all of it, as cheap glass does: lifts the blacks", t_lens, k_number, 0, 3,
     0.05f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.glare; }, +[](Settings& s, float v) { s.glare = v; }, nullptr},
    {"lens.soft", "Edge softness", "Softer towards the edges, as a cheap adapter is", t_lens, k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.soft; }, +[](Settings& s, float v) { s.soft = v; }, nullptr},
    {"fov.mode", "Field of view", "The editor's own (its keyframes), one fixed for the whole replay, or Auto: the narrowest that shows all the lens "
     "sees", t_lens, k_choice, 0, 2, 1, 1, "", fov_mode_names, fov_mode_labels, +[](const Settings& s) { return index_of(s.fov_mode); },
     +[](Settings& s, float v) { s.fov_mode = enum_from<FovMode>(v); }, nullptr},
    {"fov.degrees", "Degrees", "The replay camera's field of view, top to bottom (skate. measures it that way)", t_lens, k_number, 20, 170, 0.1f, 1,
     "°", no_choices, no_choices, +[](const Settings& s) { return s.fov; }, +[](Settings& s, float v) { s.fov = v; },
     +[](const Settings& s) { return s.fov_mode == FovMode::fixed; }},
    {"fov.widest", "Widest", "Auto goes no wider than this: wider shows more of the lens, and enlarges (softens) its middle more", t_lens,
     k_number, 90, 170, 1, 1, "°", no_choices, no_choices, +[](const Settings& s) { return s.fov_widest; },
     +[](Settings& s, float v) { s.fov_widest = v; }, +[](const Settings& s) { return s.fov_mode == FovMode::automatic; }},
    // FRAME
    {"frame.bars", "Bars", "Black bars: 4:3 at the sides, or a 2.39:1 letterbox", t_frame, k_choice, 0, 2, 1, 1, "", bars_names,
     bars_labels, +[](const Settings& s) { return index_of(s.bars); }, +[](Settings& s, float v) { s.bars = enum_from<Bars>(v); }, nullptr},
    {"frame.hood", "Overlay", "Laid over the picture: a picture of your own (a PNG in the overlays folder, see-through where the picture "
     "shows: a lens hood's black corners, say), or a round black edge at the lens's image circle", t_frame, k_choice, 0, 2, 1, 1, "", hood_names,
     hood_labels,
     +[](const Settings& s) { return index_of(s.hood); }, +[](Settings& s, float v) { s.hood = enum_from<Hood>(v); }, nullptr},
    {"frame.scale", "Overlay size", "How big the overlay is, about the middle: 100% as drawn, fitted whole in the picture and never stretched",
     t_frame, k_number, 0.25f, 4, 0.01f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.hood_size; },
     +[](Settings& s, float v) { s.hood_size = v; }, &hood_on},
    {"frame.opacity", "Overlay strength", "How dark the overlay is drawn", t_frame, k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.hood_opacity; }, +[](Settings& s, float v) { s.hood_opacity = v; }, &hood_on},
    {"frame.fit", "Fit the lens to it", "The lens is kept to what the overlay leaves showing: the game's picture need only reach that far", t_frame,
     k_toggle, 0, 1, 1, 1, "", no_choices, no_choices, +[](const Settings& s) { return s.hood_fit ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.hood_fit = v >= 0.5f; }, +[](const Settings& s) { return lens_on(s) && s.hood != Hood::none; }},
    // COLOUR: Replay Plus's own grade (in the picture and in exports)
    {"grade.camera", "Camera curve", "A video camera's curve in place of the game's: the game's tone curve undone, then the camera's, set below. "
     "Exposure, contrast and saturation then work on the scene's light", t_grade, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.camera ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.camera = v >= 0.5f; }, nullptr},
    {"grade.camera_gamma", "Camera gamma", "Its power curve: 0.45 is Rec.709, a camcorder's; lower is flatter", t_grade, k_number, 0.3f, 0.7f, 0.005f,
     1, "", no_choices, no_choices, +[](const Settings& s) { return s.camera_gamma; }, +[](Settings& s, float v) { s.camera_gamma = v; },
     &camera_on},
    {"grade.camera_knee", "Knee", "Where the camera starts to hold the highlights back", t_grade, k_number, 0.5f, 1.0f, 0.01f, 100, "%", no_choices,
     no_choices, +[](const Settings& s) { return s.camera_knee; }, +[](Settings& s, float v) { s.camera_knee = v; }, &camera_on},
    {"grade.camera_knee_slope", "Knee slope", "How much the highlights still rise above the knee: 100% not held back at all", t_grade, k_number, 0, 1,
     0.01f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.camera_knee_slope; },
     +[](Settings& s, float v) { s.camera_knee_slope = v; }, &camera_on},
    {"grade.camera_black", "Black level", "The camera's pedestal: below 0% the blacks are crushed, above they are lifted", t_grade, k_number, -0.1f,
     0.1f, 0.005f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.camera_black; },
     +[](Settings& s, float v) { s.camera_black = v; }, &camera_on},
    {"grade.camera_clip", "White clip", "Where its whites clip: 100% at white; above, the brightest highlights reach white", t_grade, k_number, 0.9f,
     1.1f, 0.005f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.camera_clip; },
     +[](Settings& s, float v) { s.camera_clip = v; }, &camera_on},
    {"grade.camera_colour", "Colour level", "The camera's colour, about grey: 100% as it is", t_grade, k_number, 0.5f, 1.5f, 0.01f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.camera_colour; }, +[](Settings& s, float v) { s.camera_colour = v; }, &camera_on},
    {"grade.exposure", "Exposure", "Brighter or darker, in stops", t_grade, k_number, -3, 3, 0.05f, 1, " EV", no_choices, no_choices,
     +[](const Settings& s) { return s.exposure; }, +[](Settings& s, float v) { s.exposure = v; }, nullptr},
    {"grade.contrast", "Contrast", "", t_grade, k_number, 0.5f, 1.5f, 0.01f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.contrast; }, +[](Settings& s, float v) { s.contrast = v; }, nullptr},
    {"grade.saturation", "Saturation", "0% black and white", t_grade, k_number, 0, 2, 0.01f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.saturation; }, +[](Settings& s, float v) { s.saturation = v; }, nullptr},
    {"grade.warmth", "Warmth", "Cooler (blue) or warmer (amber)", t_grade, k_number, -1, 1, 0.02f, 100, "", no_choices, no_choices,
     +[](const Settings& s) { return s.warmth; }, +[](Settings& s, float v) { s.warmth = v; }, nullptr},
    {"grade.tint", "Tint", "Greener or more magenta", t_grade, k_number, -1, 1, 0.02f, 100, "", no_choices, no_choices,
     +[](const Settings& s) { return s.tint; }, +[](Settings& s, float v) { s.tint = v; }, nullptr},
    {"grade.black", "Faded blacks", "Lifts the blacks for a matte, washed look", t_grade, k_number, 0, 0.25f, 0.005f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.black; }, +[](Settings& s, float v) { s.black = v; }, nullptr},
    {"grade.vignette", "Vignette", "Darkens softly towards the corners", t_grade, k_number, 0, 1, 0.02f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.vignette; }, +[](Settings& s, float v) { s.vignette = v; }, nullptr},
    {"grade.vignette_soft", "Vignette softness", "How gradually it darkens: low, only the very corners; high, from near the middle", t_grade,
     k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.vignette_soft; },
     +[](Settings& s, float v) { s.vignette_soft = v; }, +[](const Settings& s) { return s.vignette > 0; }},
    {"grade.vignette_round", "Vignette roundness", "100% a circle, 0% the picture's own shape", t_grade, k_number, 0, 1, 0.05f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.vignette_round; }, +[](Settings& s, float v) { s.vignette_round = v; },
     +[](const Settings& s) { return s.vignette > 0; }},
    {"grade.hue", "Hue shift", "Every colour turned round the colour wheel", t_grade, k_number, -180, 180, 2, 1, "°", no_choices, no_choices,
     +[](const Settings& s) { return s.hue; }, +[](Settings& s, float v) { s.hue = v; }, nullptr},
    {"grade.filter", "Colour filter", "A coloured filter over the lens (its colour below), brightness kept", t_grade, k_number, 0, 1, 0.02f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.filter; }, +[](Settings& s, float v) { s.filter = v; }, nullptr},
    {"grade.filter_hue", "Filter colour", "Its hue: 0° red, 30° amber, 60° yellow, 120° green, 190° cyan, 220° blue, 300° magenta", t_grade,
     k_number, 0, 358, 2, 1, "°", no_choices, no_choices, +[](const Settings& s) { return s.filter_hue; },
     +[](Settings& s, float v) { s.filter_hue = v; }, +[](const Settings& s) { return s.filter > 0; }},
    {"grade.lift", "Shadows", "Lift: the darkest parts raised (above 0) or crushed (below)", t_grade, k_number, -0.3f, 0.3f, 0.01f, 100, "",
     no_choices, no_choices, +[](const Settings& s) { return s.lift; }, +[](Settings& s, float v) { s.lift = v; }, nullptr},
    {"grade.gamma", "Midtones", "Gamma: the middle tones brighter (above 100%) or darker", t_grade, k_number, 0.5f, 2.0f, 0.02f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.gamma; }, +[](Settings& s, float v) { s.gamma = v; }, nullptr},
    {"grade.gain", "Highlights", "Gain: the bright parts brighter (above 100%) or darker", t_grade, k_number, 0.5f, 1.5f, 0.02f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.gain; }, +[](Settings& s, float v) { s.gain = v; }, nullptr},
    {"grade.split_shadows", "Shadow tint", "Split toning: the shadows tinted (their colour below)", t_grade, k_number, 0, 1, 0.02f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.split_shadows; }, +[](Settings& s, float v) { s.split_shadows = v; }, nullptr},
    {"grade.split_shadows_hue", "Shadow colour", "The shadows' tint: 220° a cool blue, 180° teal, 30° amber", t_grade, k_number, 0, 358, 2, 1, "°",
     no_choices, no_choices, +[](const Settings& s) { return s.split_shadows_hue; }, +[](Settings& s, float v) { s.split_shadows_hue = v; },
     +[](const Settings& s) { return s.split_shadows > 0; }},
    {"grade.split_highlights", "Highlight tint", "Split toning: the highlights tinted (their colour below)", t_grade, k_number, 0, 1, 0.02f, 100,
     "%", no_choices, no_choices, +[](const Settings& s) { return s.split_highlights; }, +[](Settings& s, float v) { s.split_highlights = v; },
     nullptr},
    {"grade.split_highlights_hue", "Highlight colour", "The highlights' tint: 40° a warm amber, 60° yellow, 330° pink", t_grade, k_number, 0, 358,
     2, 1, "°", no_choices, no_choices, +[](const Settings& s) { return s.split_highlights_hue; },
     +[](Settings& s, float v) { s.split_highlights_hue = v; }, +[](const Settings& s) { return s.split_highlights > 0; }},
    {"grade.split_balance", "Tint balance", "Where the shadows' tint gives way to the highlights': below 0 more of the picture is shadow", t_grade,
     k_number, -1, 1, 0.05f, 100, "", no_choices, no_choices, +[](const Settings& s) { return s.split_balance; },
     +[](Settings& s, float v) { s.split_balance = v; }, +[](const Settings& s) { return s.split_shadows > 0 || s.split_highlights > 0; }},
    {"grade.grain", "Grain", "Film or tape grain of Replay Plus's own, moving, in the picture and in videos", t_grade, k_number, 0, 1, 0.02f, 100,
     "%", no_choices, no_choices, +[](const Settings& s) { return s.grain; }, +[](Settings& s, float v) { s.grain = v; }, nullptr},
    {"grade.grain_size", "Grain size", "How coarse the grain is, in pixels of a 1080-line picture", t_grade, k_number, 0.5f, 4.0f, 0.1f, 1, " px",
     no_choices, no_choices, +[](const Settings& s) { return s.grain_size; }, +[](Settings& s, float v) { s.grain_size = v; },
     +[](const Settings& s) { return s.grain > 0; }},
    // GAME: the game's own effects (its settings), as XLGraphics has them for Skater XL
    {"game.look", "Game effects", "Holds the game's own effects below while the replay editor is open (they are the game's settings: videos "
     "have them too)", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices, +[](const Settings& s) { return s.game_look ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.game_look = v >= 0.5f; }, nullptr},
    {"game.skating", "While skating too", "Holds them while skating as well, not only in the replay editor", t_game, k_toggle, 0, 1, 1, 1, "",
     no_choices, no_choices, +[](const Settings& s) { return s.game_skating ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.game_skating = v >= 0.5f; }, &game_on},
    {"game.motion", "Motion blur", "The game's own motion blur", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_motion ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.game_motion = v >= 0.5f; }, &game_on},
    {"game.shutter", "Shutter speed", "The camera's shutter for the motion blur, as 1/x of a second: 60 a camcorder's own (long smears), 250 the "
     "game's, 1000 and up crisp", t_game, k_number, 30, 4000, 0.12f, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_shutter; }, +[](Settings& s, float v) { s.game_shutter = v; },
     +[](const Settings& s) { return s.game_look && s.game_motion; }, true},
    {"game.motion_amount", "Blur strength", "How strong the motion blur is (100% the game's)", t_game, k_number, 0, 3, 0.05f, 100, "%", no_choices,
     no_choices, +[](const Settings& s) { return s.game_motion_amount; }, +[](Settings& s, float v) { s.game_motion_amount = v; },
     +[](const Settings& s) { return s.game_look && s.game_motion; }},
    {"game.bloom", "Bloom", "The glow around bright lights: 0% none, 100% the game's own, more for a stronger glow", t_game, k_number, 0, 4, 0.05f,
     100, "%", no_choices, no_choices, +[](const Settings& s) { return s.game_bloom; }, +[](Settings& s, float v) { s.game_bloom = v; }, &game_on},
    {"game.flares", "Lens flares", "The game's lens flares from bright lights", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_flares ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.game_flares = v >= 0.5f; }, &game_on},
    {"game.dirt", "Lens dirt", "Dirt on the lens, lit up by bright lights", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_dirt ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.game_dirt = v >= 0.5f; }, &game_on},
    {"game.ao", "Ambient occlusion", "The soft shadow in corners and under things", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_ao ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.game_ao = v >= 0.5f; }, &game_on},
    {"game.contact_shadows", "Contact shadows", "Fine shadows where things meet", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_contact_shadows ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.game_contact_shadows = v >= 0.5f; },
     &game_on},
    {"game.fog", "Fog", "The sky's haze over distance and height", t_game, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_fog ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.game_fog = v >= 0.5f; }, &game_on},
    {"game.vignette", "Game vignette", "The game's own darkening of the corners (Replay Plus has its own on COLOUR)", t_game, k_toggle, 0, 1, 1, 1,
     "", no_choices, no_choices, +[](const Settings& s) { return s.game_vignette ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.game_vignette = v >= 0.5f; }, &game_on},
    {"game.local_exposure", "Local exposure", "The game evening out bright and dark areas", t_game, k_toggle, 0, 1, 1, 1, "", no_choices,
     no_choices, +[](const Settings& s) { return s.game_local_exposure ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.game_local_exposure = v >= 0.5f; }, &game_on},
    {"game.exposure", "Game exposure", "The game's own exposure, in stops", t_game, k_number, -4, 4, 0.1f, 1, " EV", no_choices, no_choices,
     +[](const Settings& s) { return s.game_exposure; }, +[](Settings& s, float v) { s.game_exposure = v; }, &game_on},
    {"game.grain", "Film grain", "The game's own film grain", t_game, k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.game_grain; }, +[](Settings& s, float v) { s.game_grain = v; }, &game_on},
    {"game.fringe", "Colour fringe", "The game's own chromatic aberration", t_game, k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.game_fringe; }, +[](Settings& s, float v) { s.game_fringe = v; }, &game_on},
    {"game.detail", "Detail distance", "How far out things keep their detailed models (100% the game's)", t_game, k_number, 0.5f, 4, 0.05f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.game_detail; }, +[](Settings& s, float v) { s.game_detail = v; }, &game_on},
    {"game.shadow_distance", "Shadow distance", "How far out shadows are drawn (175% the game's)", t_game, k_number, 0.5f, 6, 0.05f, 100, "%",
     no_choices, no_choices, +[](const Settings& s) { return s.game_shadow_distance; }, +[](Settings& s, float v) { s.game_shadow_distance = v; },
     &game_on},
    {"game.view_distance", "View distance", "How far the game draws (20000 m the game's)", t_game, k_number, 1000, 40000, 500, 1, " m", no_choices,
     no_choices, +[](const Settings& s) { return s.game_view_distance; }, +[](Settings& s, float v) { s.game_view_distance = v; }, &game_on},
    {"game.barrel", "Barrel distortion", "The game's own barrel distortion (its replay editor's): how strongly the picture bows out. Replay Plus's "
     "lens (LENS) bends it again on top", t_game, k_number, -1.0f, 1.0f, 0.01f, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.game_barrel; }, +[](Settings& s, float v) { s.game_barrel = v; }, &game_on},
    {"game.barrel_edge", "Barrel edge", "The game's barrel distortion's outer curve: more bends the corners harder", t_game, k_number, -1.0f, 1.0f,
     0.01f, 1, "", no_choices, no_choices, +[](const Settings& s) { return s.game_barrel_edge; },
     +[](Settings& s, float v) { s.game_barrel_edge = v; }, &game_on},
    {"game.barrel_stretch", "Barrel stretch", "The game's barrel distortion stretched across the picture", t_game, k_number, -1.0f, 1.0f, 0.01f, 1, "",
     no_choices, no_choices, +[](const Settings& s) { return s.game_barrel_stretch; }, +[](Settings& s, float v) { s.game_barrel_stretch = v; },
     &game_on},
    // FOCUS
    {"focus.mode", "Depth of field", "Off, one focus for the replay, focus keyed along the replay, or Auto: what is in the middle, or your skater", t_focus,
     k_choice, 0, 3, 1, 1, "", focus_names, focus_labels, +[](const Settings& s) { return index_of(s.focus); },
     +[](Settings& s, float v) { s.focus = enum_from<Focus>(v); }, nullptr},
    {"focus.skater", "Skater autofocus", "Experimental. On: the depth of field follows your skater wherever they are in the picture (it turns FOCUS "
     "to Auto). Off: Auto focuses on what is in the middle", t_experimental, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.focus_skater && s.focus == Focus::autofocus ? 1.0f : 0.0f; },
     +[](Settings& s, float v) {
         s.focus_skater = v >= 0.5f;
         if (s.focus_skater) s.focus = Focus::autofocus;
     },
     nullptr},
    {"focus.distance", "Focus distance", "How far away is sharp. Keyframed: a change makes (or moves) a key at the playhead", t_focus, k_number, 0.2f,
     200, 0.04f, 1, " m", no_choices, no_choices, +[](const Settings& s) { return s.focus_distance; },
     +[](Settings& s, float v) { s.focus_distance = v; }, +[](const Settings& s) { return s.focus == Focus::fixed || s.focus == Focus::keys; }, true},
    {"focus.blur", "Blur", "How far out of focus the rest goes", t_focus, k_number, 0, 4, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.focus_blur; }, +[](Settings& s, float v) { s.focus_blur = v; },
     +[](const Settings& s) { return s.focus != Focus::off; }},
    {"focus.depth", "Sharp zone", "How deep the sharp zone around the focus is: small for a shallow look, large for most of the picture sharp",
     t_focus, k_number, 0.02f, 0.6f, 0.01f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.focus_depth; },
     +[](Settings& s, float v) { s.focus_depth = v; }, +[](const Settings& s) { return s.focus != Focus::off; }},
    {"focus.speed", "Follow speed", "Auto: how quickly the focus follows (the skater, or what is in the middle)", t_focus, k_number, 0, 1, 0.05f, 100, "%", no_choices,
     no_choices, +[](const Settings& s) { return s.focus_speed; }, +[](Settings& s, float v) { s.focus_speed = v; },
     +[](const Settings& s) { return s.focus == Focus::autofocus; }},
    {"controls.r3", "R3 shortcuts", "Hold R3 in the replay editor: RB this panel, the D-pad focus and shake keys. A tap of R3 is still the game's Photo",
     t_focus, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices, +[](const Settings& s) { return s.pad_shortcuts ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.pad_shortcuts = v >= 0.5f; }, nullptr},
    {"controls.keep_keys", "Keep keys", "Experimental. On: your camera keyframes and the focus and shake keys are kept when you leave the replay "
     "editor, and come back when you open it again (on the same footage, until the game closes). Off: every visit starts with none", t_experimental,
     k_toggle, 0, 1, 1, 1, "",
     no_choices, no_choices, +[](const Settings& s) { return s.keep_keys ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.keep_keys = v >= 0.5f; },
     nullptr},
    // LIGHT
    {"light.on", "Camera light", "A real light on the camera, in the game's world: it lights every surface it points at as the game's own lights "
     "do, with shadows, in videos too", t_light, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.light ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.light = v >= 0.5f; }, nullptr},
    {"light.shadows", "Shadows", "The light casts shadows, from just above the lens: behind and under what it lights", t_light, k_toggle, 0, 1, 1, 1, "",
     no_choices, no_choices, +[](const Settings& s) { return s.light_shadows ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.light_shadows = v >= 0.5f; },
     +[](const Settings& s) { return s.light; }},
    {"light.keyed", "Light keys", "The light follows its keys along the replay: switched on and off, brighter or dimmer, wider or narrower, set at "
     "the playhead. R3 + D-pad right switches it there", t_light, k_toggle, 0, 1, 1, 1, "", no_choices, no_choices,
     +[](const Settings& s) { return s.light_keyed ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.light_keyed = v >= 0.5f; },
     +[](const Settings& s) { return s.light; }},
    {"light.power", "Brightness", "100% lights a skater a few metres away clearly. Like any real light it is brighter close up and dimmer further away",
     t_light, k_number, 0, 8, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.light_power; }, +[](Settings& s, float v) { s.light_power = v; }, +[](const Settings& s) { return s.light; }},
    {"light.range", "Reach", "How far it lights", t_light, k_number, 1, 50, 0.5f, 1, " m", no_choices, no_choices,
     +[](const Settings& s) { return s.light_range; }, +[](Settings& s, float v) { s.light_range = v; }, +[](const Settings& s) { return s.light; }},
    {"light.cone", "Beam", "How wide the beam is, degrees across (180 lights all round)", t_light, k_number, 10, 180, 1, 1, "°", no_choices, no_choices,
     +[](const Settings& s) { return s.light_cone; }, +[](Settings& s, float v) { s.light_cone = v; }, +[](const Settings& s) { return s.light; }},
    {"light.soft", "Beam edge", "Hard to soft", t_light, k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.light_soft; }, +[](Settings& s, float v) { s.light_soft = v; }, +[](const Settings& s) { return s.light; }},
    {"light.warmth", "Colour", "A cold LED to a warm tungsten bulb", t_light, k_number, -1, 1, 0.05f, 100, "", no_choices, no_choices,
     +[](const Settings& s) { return s.light_warmth; }, +[](Settings& s, float v) { s.light_warmth = v; }, +[](const Settings& s) { return s.light; }},
    // VIDEO
    {"video.aspect", "Picture shape", "The replay editor's own picture shape: the game renders it that shape, in a window of it in the middle of "
     "the screen (black around), never stretched. Its exports and recordings come out that shape", t_video, k_choice, 0, 6, 1, 1, "",
     aspect_names, aspect_labels, +[](const Settings& s) { return index_of(s.replay_aspect); },
     +[](Settings& s, float v) { s.replay_aspect = enum_from<Aspect>(v); }, nullptr},
    {"video.sharp", "Sharp middle", "With a fisheye, the replay editor renders finer by as much as the lens enlarges the middle (up to 200%), so it "
     "keeps its detail at a wide field of view. Costs frame rate in the editor, and time in exports", t_video, k_toggle, 0, 1, 1, 1, "",
     no_choices, no_choices, +[](const Settings& s) { return s.sharp_middle ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.sharp_middle = v >= 0.5f; }, nullptr},
    {"video.sharpen", "Sharpen middle", "With a fisheye, sharpens what the lens enlarges (the middle) so textures stay crisp. Costs no frame rate. "
     "Sharp middle adds real detail; this brings back the crispness", t_video, k_number, 0, 1, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.sharpen; }, +[](Settings& s, float v) { s.sharpen = v; }, nullptr},
    {"video.textures", "Bigger texture pool", "Room for more of the game's texture detail, sized from the card's memory (2.5 GB on a 16 GB card; "
     "the game's own is 1.4 GB): a big map's textures stay sharp where they turned to mush. From the next map loaded", t_video, k_toggle, 0, 1, 1,
     1, "", no_choices, no_choices, +[](const Settings& s) { return s.bigger_textures ? 1.0f : 0.0f; },
     +[](Settings& s, float v) { s.bigger_textures = v >= 0.5f; }, nullptr},
    {"video.on", "Own resolutions", "A render resolution for the replay editor and another for skating", t_video, k_toggle, 0, 1, 1, 1, "", no_choices,
     no_choices, +[](const Settings& s) { return s.resolution ? 1.0f : 0.0f; }, +[](Settings& s, float v) { s.resolution = v >= 0.5f; }, nullptr},
    {"video.replay", "Replay editor", "Render resolution while the replay editor is open (above 100% renders finer and scales down)", t_video, k_number,
     0.5f, 2.0f, 0.05f, 100, "%", no_choices, no_choices, +[](const Settings& s) { return s.replay_scale; },
     +[](Settings& s, float v) { s.replay_scale = v; }, +[](const Settings& s) { return s.resolution; }},
    {"video.gameplay", "Skating", "Render resolution while skating", t_video, k_number, 0.5f, 2.0f, 0.05f, 100, "%", no_choices, no_choices,
     +[](const Settings& s) { return s.gameplay_scale; }, +[](Settings& s, float v) { s.gameplay_scale = v; },
     +[](const Settings& s) { return s.resolution; }},
};

bool parse_float(std::string_view text, float& value) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '+')) text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}
int decimals(float step, float shown) {
    const float s = step * shown;
    if (s >= 1.0f - 1e-4f) return 0;
    if (s >= 0.1f - 1e-5f) return 1;
    if (s >= 0.01f - 1e-6f) return 2;
    return 3;
}
std::string trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

const Settings defaults{};
} // namespace

std::span<const Field> fields() { return all_fields; }
const Field* field(std::string_view key) {
    const auto wanted = lower(key);
    for (const auto& f : all_fields)
        if (f.key == wanted) return &f;
    return nullptr;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string shown(const Field& f, const Settings& settings) {
    const float value = f.get(settings);
    switch (f.kind) {
    case Kind::toggle: return value >= 0.5f ? "On" : "Off";
    case Kind::choice: {
        const auto index = static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(value)), 0, static_cast<int>(f.labels.size()) - 1));
        return f.labels.empty() ? std::string() : std::string(f.labels[index]);
    }
    case Kind::number: break;
    }
    const float v = value * f.shown;
    int places = decimals(f.step, f.shown);
    if (f.logarithmic) places = v < 1.0f ? 2 : v < 10.0f ? 1 : 0;
    std::string text = std::format("{:.{}f}", v, places);
    if (text == "-0" || text == "-0.0" || text == "-0.00" || text == "-0.000") text.erase(0, 1);
    // Signed values (warmth, tint) show their sign, so 0 reads as the middle.
    if (f.low < 0 && v > 0 && f.unit != " EV") text.insert(0, "+");
    if (f.unit == " EV" && v > 0) text.insert(0, "+");
    return text + std::string(f.unit);
}

bool assign(const Field& f, Settings& settings, std::string_view input, std::string& why) {
    const auto text = lower(trimmed(input));
    switch (f.kind) {
    case Kind::toggle:
        if (text == "1" || text == "on" || text == "true" || text == "yes") { f.set(settings, 1); return true; }
        if (text == "0" || text == "off" || text == "false" || text == "no") { f.set(settings, 0); return true; }
        if (text == "toggle") { f.set(settings, f.get(settings) >= 0.5f ? 0.0f : 1.0f); return true; }
        why = std::format("{} takes on or off.", f.key);
        return false;
    case Kind::choice: {
        for (std::size_t i = 0; i < f.choices.size(); ++i)
            if (text == lower(f.choices[i]) || (i < f.labels.size() && text == lower(f.labels[i]))) {
                f.set(settings, static_cast<float>(i));
                return true;
            }
        float index{};
        if (parse_float(text, index) && index >= 0 && index < static_cast<float>(f.choices.size()) && index == std::floor(index)) {
            f.set(settings, index);
            return true;
        }
        why = std::format("{} takes one of:", f.key);
        for (const auto choice : f.choices) why += std::format(" {}", choice);
        why += ".";
        return false;
    }
    case Kind::number: break;
    }
    std::string_view number = text;
    bool percent = false;
    if (!number.empty() && number.back() == '%') { percent = true; number.remove_suffix(1); }
    if (!f.unit.empty()) {
        const auto unit = lower(trimmed(f.unit));
        if (!unit.empty() && number.size() > unit.size() && number.substr(number.size() - unit.size()) == unit)
            number.remove_suffix(unit.size());
    }
    float value{};
    if (!parse_float(number, value)) {
        why = std::format("{} takes a number from {} to {}.", f.key, shown(f, [&] { Settings s = settings; f.set(s, f.low); return s; }()),
                          shown(f, [&] { Settings s = settings; f.set(s, f.high); return s; }()));
        return false;
    }
    // As the panel shows it ("150%" or "150" for a scale shown in percent), or as it is kept (1.5).
    // A field shown scaled with no unit of its own (warmth, tint, the barrel's: shown -100 to +100) is
    // typed as shown when the number is a whole one; one with a unit ("150%") by the unit.
    const bool whole = number.find('.') == std::string_view::npos && number.find('e') == std::string_view::npos;
    if (percent || (f.shown != 1 && f.unit.empty() && whole) ||
        (f.shown != 1 && (value > f.high || value < f.low) && value / f.shown >= f.low - 1e-6f && value / f.shown <= f.high + 1e-6f))
        value /= f.shown;
    f.set(settings, std::clamp(value, f.low, f.high));
    return true;
}

void step(const Field& f, Settings& settings, int steps) {
    if (!steps) return;
    switch (f.kind) {
    case Kind::toggle: f.set(settings, f.get(settings) >= 0.5f ? 0.0f : 1.0f); return;
    case Kind::choice: {
        const int count = static_cast<int>(f.choices.size());
        if (count <= 0) return;
        const int now = static_cast<int>(std::lround(f.get(settings)));
        f.set(settings, static_cast<float>(((now + steps) % count + count) % count));
        return;
    }
    case Kind::number: break;
    }
    const float now = f.get(settings);
    if (f.logarithmic) {
        // A share of itself each step, kept to two significant figures (0.84 m, 3.2 m, 15 m).
        if (!steps) return;
        float moved = std::max(now, f.low) * std::pow(1.0f + f.step, static_cast<float>(std::clamp(steps, -400, 400)));
        if (!std::isfinite(moved) || moved > f.high) moved = f.high;
        if (!(moved >= f.low)) moved = f.low;
        const float digits = std::pow(10.0f, std::floor(std::log10(std::max(moved, 1e-3f))) - 1.0f);
        moved = std::round(moved / digits) * digits;
        if (moved == now) moved = now + (steps > 0 ? digits : -digits);
        f.set(settings, std::clamp(moved, f.low, f.high));
        return;
    }
    // Snapped to the step, so a value nudged from an odd one lands on a round one.
    const float moved = std::round((now + static_cast<float>(steps) * f.step) / f.step) * f.step;
    f.set(settings, std::clamp(moved, f.low, f.high));
}

Settings clamped(Settings s) {
    for (const auto& f : all_fields) {
        float v = f.get(s);
        if (!std::isfinite(v)) v = f.get(defaults);
        if (f.kind == Kind::number) v = std::clamp(v, f.low, f.high);
        f.set(s, v);
    }
    if (s.hood_file.size() > 200) s.hood_file.clear();
    for (auto& key : s.focus_keys) {
        if (!std::isfinite(key.distance)) key.distance = 3.0f;
        if (!std::isfinite(key.blur)) key.blur = 1.0f;
        key.distance = std::clamp(key.distance, 0.2f, 200.0f);
        key.blur = std::clamp(key.blur, 0.0f, 4.0f);
    }
    if (s.focus_keys.size() > max_focus_keys) s.focus_keys.resize(max_focus_keys);
    std::erase_if(s.light_keys, [](const LightKey& key) { return !std::isfinite(key.time); });
    for (auto& key : s.light_keys) {
        key.power = std::clamp(std::isfinite(key.power) ? key.power : 1.0f, 0.0f, 8.0f);
        key.range = std::clamp(std::isfinite(key.range) ? key.range : 10.0f, 1.0f, 50.0f);
        key.cone = std::clamp(std::isfinite(key.cone) ? key.cone : 70.0f, 10.0f, 180.0f);
        key.warmth = std::clamp(std::isfinite(key.warmth) ? key.warmth : 0.2f, -1.0f, 1.0f);
    }
    std::sort(s.light_keys.begin(), s.light_keys.end(), [](const LightKey& a, const LightKey& b) { return a.time < b.time; });
    if (s.light_keys.size() > max_light_keys) s.light_keys.resize(max_light_keys);
    std::erase_if(s.shake_keys, [](const ShakeKey& key) { return !std::isfinite(key.time); });
    if (s.shake_keys.size() > max_shake_keys) s.shake_keys.resize(max_shake_keys);
    tidy_shake_keys(s);
    if (s.preset.size() > 120) s.preset.clear();
    return s;
}

// ---- Saving ----------------------------------------------------------------------------------
namespace {
Json value_json(const Field& f, const Settings& s) {
    const float v = f.get(s);
    switch (f.kind) {
    case Kind::toggle: return Json(v >= 0.5f);
    case Kind::choice: {
        const auto index = static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(v)), 0, static_cast<int>(f.choices.size()) - 1));
        return Json(std::string(f.choices[index]));
    }
    case Kind::number: break;
    }
    // Kept to the step's precision, so the file reads as the panel shows it; a value that would not come
    // back the same that way (the Mk1's circle, 5/3) is kept whole.
    const double scale = std::pow(10.0, decimals(f.step, 1) + 1);
    const double rounded = std::round(static_cast<double>(v) * scale) / scale;
    return Json(static_cast<float>(rounded) == v ? rounded : static_cast<double>(v));
}
void read_value(const Field& f, Settings& s, const Json& value) {
    try {
        std::string why;
        if (value.is_string()) (void)assign(f, s, value.string(), why);
        else if (value.is_boolean()) f.set(s, value.get<bool>() ? 1.0f : 0.0f);
        else if (value.is_number()) {
            const auto number = value.get<double>();
            if (std::isfinite(number)) f.set(s, f.kind == Kind::number ? std::clamp(static_cast<float>(number), f.low, f.high) : static_cast<float>(number));
        }
    } catch (...) { /* left as it was */ }
}
// Only the values a preset has to carry: those not at their defaults.
// `look`: a preset's values, without the FOCUS tab: the focus belongs to the shot
// in hand (as its keys do), so loading a look leaves it as it is.
Json values_json(const Settings& s, bool all, bool look = false) {
    Json values = Json::object();
    for (const auto& f : all_fields) {
        if (look && (f.tab == Tab::focus || f.tab == Tab::experimental)) continue;
        if (!all && f.get(s) == f.get(defaults)) continue;
        values[f.key] = value_json(f, s);
    }
    if (all || !s.hood_file.empty()) values["frame.file"] = s.hood_file;
    return values;
}
// Settings and presets (and the share codes of then) saved before 2026-11-17: a lens choice ("lens.mode": off, the game's barrel, or
// the fisheye; in November's first version "mk1" and "modern" lenses of their own), "true angles" (a fit and an
// ease the lens picked for itself), a camcorder switch and a Mk1 hood choice. Read as the settings they are now,
// drawing as they did.
void migrate(Settings& s, const Json& values) {
    const auto text = [&](std::string_view key) {
        return values.contains(key) && values.at(key).is_string() ? lower(values.at(key).string()) : std::string();
    };
    const auto number = [&](std::string_view key, float fallback) {
        if (!values.contains(key) || !values.at(key).is_number()) return fallback;
        const double v = values.at(key).get<double>();
        return std::isfinite(v) ? static_cast<float>(v) : fallback;
    };
    const auto flag = [&](std::string_view key) { return values.contains(key) && values.at(key).is_boolean() && values.at(key).get<bool>(); };
    // The overlay: the Mk1's hood is the picture of it that comes with Replay Plus; a picture was "file".
    const auto hood = text("frame.hood");
    if (hood == "mk1") {
        s.hood = Hood::picture;
        s.hood_file = std::string(mk1_hood_file);
    } else if (hood == "file") {
        s.hood = Hood::picture;
    }
    // The camcorder: the camera curve at its defaults.
    if (flag("grade.camcorder")) s.camera = true;
    // Overlay size before 1.3 ("frame.size": how far its dark edge reached in from the corners): as the plain scale that
    // draws it so.
    if (!values.contains("frame.scale") && values.contains("frame.size")) s.hood_size = lens::scale_for_old_size(s, number("frame.size", 1.0f));
    if (!values.contains("lens.mode")) return;
    // Saved with a lens choice: the image circle's default was then the Mk1's.
    if (!values.contains("lens.circle")) s.circle = mk1_circle;
    const auto mode = text("lens.mode");
    bool fish = mode == "fisheye";
    const bool first_mk1 = mode == "mk1" || mode == "century mk1", modern = mode == "modern" || mode == "modern fisheye";
    if (first_mk1 || modern) {
        // November's first: "mk1" and "modern" were lenses of their own, Distortion a share of each one's curve
        // (100% the Mk1's equidistant, the modern lens's equisolid); its Mk1 had 90 degrees at the 4:3 corner.
        const float own = modern ? 1.5f : 1.0f, d = std::clamp(number("lens.curve", 1.0f), 0.0f, 2.0f);
        s.curve = d <= 1.0f ? own * d : own + (d - 1.0f) * (2.0f - own);
        if (first_mk1) s.circle = mk1_corner_circle;
        fish = true;
    } else if (fish && !values.contains("lens.curve")) {
        s.curve = 1.0f; // the default then (a share code carried only what was not at its default)
    }
    if (!fish) {
        // The lens was off, or the game's barrel: none of Replay Plus's lens showed.
        s.curve = 0;
        s.zoom = s.squeeze = 1;
        s.compress = s.fringe = s.shade = s.glare = s.soft = 0;
        if (mode == "game") {
            s.game_barrel = number("lens.barrel", 0);
            s.game_barrel_edge = number("lens.cubic", 0);
            s.game_barrel_stretch = number("lens.stretch", 0);
            s.game_look = true;
        }
        return;
    }
    if (flag("lens.true") && s.curve >= 1.0f) {
        // The fit and ease the lens picked for itself then, as the zoom and edge compression they are now.
        const float screen = s.replay_aspect != Aspect::screen ? aspect_ratios[static_cast<std::size_t>(s.replay_aspect)] : 16.0f / 9.0f;
        const auto old = lens::legacy_true_angles(s, hood == "mk1", screen);
        s.zoom = old.zoom;
        s.compress = old.compress;
    }
}
void read_values(Settings& s, const Json& values) {
    if (!values.is_object()) return;
    for (const auto& f : all_fields)
        if (values.contains(f.key)) read_value(f, s, values.at(f.key));
    if (values.contains("frame.file") && values.at("frame.file").is_string()) s.hood_file = values.at("frame.file").string();
    migrate(s, values);
}
} // namespace

Json to_json(const Settings& settings) {
    Json json = Json::object();
    json["version"] = 1;
    json["enabled"] = settings.enabled;
    json["values"] = values_json(settings, true);
    json["panel_bind"] = std::format("0x{:x}", settings.panel_bind);
    {
        // The controls, by name (readable by hand): {"layer": "R3", "panel": {"pad": "RB", "key": "F10"}, ..., "presets": [...]}.
        Json controls = Json::object();
        controls["layer"] = pad_words(settings.pad_layer);
        for (std::size_t a = 0; a < action_count; ++a) {
            Json one = Json::object();
            one["pad"] = pad_words(settings.binds[a].pad);
            one["key"] = key_name(settings.binds[a].key);
            controls[std::string(action_names[a])] = std::move(one);
        }
        Json presets = Json::array();
        for (const auto& p : settings.preset_binds) {
            Json one = Json::object();
            one["preset"] = p.preset;
            one["pad"] = pad_words(p.bind.pad);
            one["key"] = key_name(p.bind.key);
            presets.push_back(std::move(one));
        }
        controls["presets"] = std::move(presets);
        json["controls"] = std::move(controls);
    }
    json["preset"] = settings.preset;
    if (!settings.video_folder.empty()) json["video_folder"] = settings.video_folder;
    Json keys = Json::array();
    for (const auto& key : settings.focus_keys) {
        Json item = Json::object();
        item["time"] = std::round(key.time * 1e4) / 1e4;
        item["distance"] = std::round(static_cast<double>(key.distance) * 1e3) / 1e3;
        item["blur"] = std::round(static_cast<double>(key.blur) * 1e3) / 1e3;
        keys.push_back(std::move(item));
    }
    json["focus_keys"] = std::move(keys);
    Json shakes = Json::array();
    for (const auto& key : settings.shake_keys) {
        Json item = Json::object();
        item["time"] = std::round(key.time * 1e4) / 1e4;
        item["on"] = key.on;
        shakes.push_back(std::move(item));
    }
    json["shake_keys"] = std::move(shakes);
    Json lights = Json::array();
    for (const auto& key : settings.light_keys) {
        Json item = Json::object();
        item["time"] = std::round(key.time * 1e4) / 1e4;
        item["on"] = key.on;
        item["power"] = std::round(static_cast<double>(key.power) * 1e3) / 1e3;
        item["range"] = std::round(static_cast<double>(key.range) * 1e3) / 1e3;
        item["cone"] = std::round(static_cast<double>(key.cone) * 1e2) / 1e2;
        item["warmth"] = std::round(static_cast<double>(key.warmth) * 1e3) / 1e3;
        lights.push_back(std::move(item));
    }
    json["light_keys"] = std::move(lights);
    return json;
}

Settings settings_from_json(const Json& json) {
    Settings s;
    if (!json.is_object()) return s;
    try {
        if (json.contains("enabled") && json.at("enabled").is_boolean()) s.enabled = json.at("enabled").get<bool>();
        if (json.contains("values")) read_values(s, json.at("values"));
        if (json.contains("panel_bind") && json.at("panel_bind").is_string()) {
            const auto& text = json.at("panel_bind").string();
            std::uint32_t bind{};
            const auto begin = text.data() + (text.starts_with("0x") ? 2 : 0);
            if (std::from_chars(begin, text.data() + text.size(), bind, 16).ec == std::errc{}) s.panel_bind = bind;
        }
        if (json.contains("preset") && json.at("preset").is_string()) s.preset = json.at("preset").string();
        if (json.contains("video_folder") && json.at("video_folder").is_string()) s.video_folder = json.at("video_folder").string();
        if (json.contains("controls") && json.at("controls").is_object()) {
            const auto& controls = json.at("controls");
            const auto pad_of = [](const Json& one) -> std::optional<std::uint32_t> {
                if (!one.is_object() || !one.contains("pad") || !one.at("pad").is_string()) return std::nullopt;
                const auto& text = one.at("pad").string();
                if (lower(text) == "none" || text.empty()) return 0u;
                return replay_shake::parse_binding(text);
            };
            const auto key_of = [](const Json& one) -> std::optional<std::uint32_t> {
                if (!one.is_object() || !one.contains("key") || !one.at("key").is_string()) return std::nullopt;
                return parse_key(one.at("key").string());
            };
            if (controls.contains("layer") && controls.at("layer").is_string())
                if (const auto layer = replay_shake::parse_binding(controls.at("layer").string()); layer && layer_button(*layer)) s.pad_layer = *layer;
            for (std::size_t a = 0; a < action_count; ++a) {
                const std::string name(action_names[a]);
                if (!controls.contains(name)) continue;
                const auto& one = controls.at(name);
                if (const auto pad = pad_of(one)) s.binds[a].pad = *pad;
                if (const auto key = key_of(one)) s.binds[a].key = *key;
            }
            if (controls.contains("presets") && controls.at("presets").is_array())
                for (const auto& one : controls.at("presets")) {
                    if (!one.is_object() || !one.contains("preset") || !one.at("preset").is_string()) continue;
                    PresetBind p;
                    p.preset = one.at("preset").string();
                    p.bind.pad = pad_of(one).value_or(0);
                    p.bind.key = key_of(one).value_or(0);
                    if (!p.preset.empty() && (p.bind.pad || p.bind.key)) s.preset_binds.push_back(std::move(p));
                }
        }
        if (json.contains("focus_keys") && json.at("focus_keys").is_array())
            for (const auto& item : json.at("focus_keys")) {
                if (!item.is_object() || !item.contains("time") || !item.at("time").is_number()) continue;
                const double time = item.at("time").get<double>();
                const float distance = item.contains("distance") && item.at("distance").is_number() ? item.at("distance").get<float>() : 3.0f;
                const float blur = item.contains("blur") && item.at("blur").is_number() ? item.at("blur").get<float>() : 1.0f;
                if (std::isfinite(time) && std::isfinite(distance) && std::isfinite(blur)) set_focus_key(s, time, distance, blur);
            }
        if (json.contains("shake_keys") && json.at("shake_keys").is_array())
            for (const auto& item : json.at("shake_keys")) {
                if (!item.is_object() || !item.contains("time") || !item.at("time").is_number()) continue;
                const double time = item.at("time").get<double>();
                const bool on = !item.contains("on") || !item.at("on").is_boolean() || item.at("on").get<bool>();
                if (std::isfinite(time) && s.shake_keys.size() < max_shake_keys) s.shake_keys.push_back({time, on});
            }
        if (json.contains("light_keys") && json.at("light_keys").is_array())
            for (const auto& item : json.at("light_keys")) {
                if (!item.is_object() || !item.contains("time") || !item.at("time").is_number()) continue;
                const auto number = [&](const char* key, float otherwise) {
                    return item.contains(key) && item.at(key).is_number() ? item.at(key).get<float>() : otherwise;
                };
                LightKey key;
                key.time = item.at("time").get<double>();
                key.on = !item.contains("on") || !item.at("on").is_boolean() || item.at("on").get<bool>();
                key.power = number("power", 1.0f);
                key.range = number("range", 10.0f);
                key.cone = number("cone", 70.0f);
                key.warmth = number("warmth", 0.2f);
                if (std::isfinite(key.time) && s.light_keys.size() < max_light_keys) s.light_keys.push_back(key);
            }
    } catch (...) { /* whatever was read stays */ }
    return clamped(s);
}

// ---- Presets ---------------------------------------------------------------------------------
Json preset_json(const Preset& preset) {
    Json json = Json::object();
    json["replay_plus_preset"] = 1;
    json["name"] = preset.name;
    if (!preset.author.empty()) json["author"] = preset.author;
    if (!preset.notes.empty()) json["notes"] = preset.notes;
    // Every value, so loading it sets everything: one made from defaults still turns the rest back.
    json["values"] = values_json(preset.settings, true, true);
    if (!preset.shake.is_null()) json["shake"] = preset.shake;
    if (!preset.game.is_null()) json["game"] = preset.game;
    return json;
}

bool preset_from_json(const Json& json, Preset& preset, std::string& why) {
    try {
        if (!json.is_object() || !json.contains("replay_plus_preset")) {
            why = "it is not a Replay Plus preset";
            return false;
        }
        Preset result;
        result.name = json.value("name", std::string("Preset"));
        if (result.name.empty()) result.name = "Preset";
        if (result.name.size() > 60) result.name.resize(60);
        result.author = json.value("author", std::string());
        result.notes = json.value("notes", std::string());
        if (result.author.size() > 60) result.author.resize(60);
        if (result.notes.size() > 400) result.notes.resize(400);
        // A preset that leaves a value out leaves it at its default.
        if (json.contains("values")) read_values(result.settings, json.at("values"));
        result.settings = clamped(result.settings);
        if (json.contains("shake") && json.at("shake").is_object()) result.shake = json.at("shake");
        if (json.contains("game") && json.at("game").is_object()) result.game = json.at("game");
        preset = std::move(result);
        return true;
    } catch (const std::exception& error) {
        why = error.what();
        return false;
    }
}

std::string preset_file_name(std::string_view name) {
    std::string result;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')') result.push_back(c);
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    while (!result.empty() && result.front() == ' ') result.erase(result.begin());
    if (result.empty()) result = "Preset";
    if (result.size() > 60) result.resize(60);
    return result + ".json";
}

std::string clean_preset_name(std::string_view name) {
    std::string result;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 128 && (std::isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')')) result.push_back(c);
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    while (!result.empty() && result.front() == ' ') result.erase(result.begin());
    if (result.size() > 40) result.resize(40);
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}
bool same_preset_file(std::string_view a, std::string_view b) { return lower(preset_file_name(a)) == lower(preset_file_name(b)); }
std::string unused_preset_name(std::string_view name, std::span<const std::string> taken) {
    const auto used = [&](const std::string& candidate) {
        return std::any_of(taken.begin(), taken.end(), [&](const std::string& t) { return same_preset_file(t, candidate); });
    };
    std::string base = clean_preset_name(name);
    if (base.empty()) base = "Preset";
    if (!used(base)) return base;
    for (int n = 2; n < 1000; ++n) {
        auto head = base;
        const auto tail = std::format(" {}", n);
        if (head.size() + tail.size() > 40) head.resize(40 - tail.size());
        if (!used(head + tail)) return head + tail;
    }
    return base;
}

// ---- Controls ---------------------------------------------------------------------------------------------------------
namespace vk { // (the virtual-key codes Windows gives keys: no Windows header in the model)
inline constexpr std::uint32_t add = 0x6b, back = 0x08, capital = 0x14, decimal = 0x6e, del = 0x2e, divide = 0x6f, down = 0x28, end = 0x23, escape = 0x1b, f1 = 0x70, f10 = 0x79, f11 = 0x7a, f24 = 0x87, home = 0x24, insert = 0x2d, left = 0x25, multiply = 0x6a, next = 0x22, numlock = 0x90, numpad0 = 0x60, numpad9 = 0x69, oem_1 = 0xba, oem_102 = 0xe2, oem_2 = 0xbf, oem_3 = 0xc0, oem_4 = 0xdb, oem_5 = 0xdc, oem_6 = 0xdd, oem_7 = 0xde, oem_comma = 0xbc, oem_minus = 0xbd, oem_period = 0xbe, oem_plus = 0xbb, pause = 0x13, prior = 0x21, enter = 0x0d, right = 0x27, scroll = 0x91, snapshot = 0x2c, space = 0x20, subtract = 0x6d, tab = 0x09, up = 0x26;
} // namespace vk
Binds default_binds() noexcept {
    Binds b{};
    b[static_cast<std::size_t>(Action::panel)] = {0x0200, vk::f10};     // RB, F10
    b[static_cast<std::size_t>(Action::light)] = {0x0008, 0};          // Right
    b[static_cast<std::size_t>(Action::shake)] = {0x0004, vk::f11};     // Left, F11
    b[static_cast<std::size_t>(Action::shake_key)] = {0x0100, 0};      // LB
    b[static_cast<std::size_t>(Action::delete_keys)] = {0x0020, 0};    // View
    b[static_cast<std::size_t>(Action::focus_further)] = {0x0001, 0};  // Up
    b[static_cast<std::size_t>(Action::focus_nearer)] = {0x0002, 0};   // Down
    b[static_cast<std::size_t>(Action::blur_less)] = {0x10000, 0};     // LT
    b[static_cast<std::size_t>(Action::blur_more)] = {0x20000, 0};     // RT
    return b;
}
bool layer_button(std::uint32_t bits) noexcept {
    constexpr std::uint32_t allowed = 0xf3ff & ~0x3000u; // every button but A and B (no triggers)
    return bits && (bits & (bits - 1)) == 0 && (bits & allowed) == bits;
}
namespace {
struct PadName {
    std::uint32_t bit;
    std::string_view name;
};
// In the order a combo is written: shoulders and triggers, sticks, face buttons, the D-pad, Start and Back.
constexpr PadName pad_names[]{{0x100, "LB"}, {0x200, "RB"}, {0x10000, "LT"}, {0x20000, "RT"}, {0x40, "L3"}, {0x80, "R3"},
                              {0x1000, "A"},  {0x2000, "B"},  {0x4000, "X"},    {0x8000, "Y"},    {0x1, "Up"},  {0x2, "Down"},
                              {0x4, "Left"},  {0x8, "Right"}, {0x10, "Start"},  {0x20, "Back"}};
struct KeyName {
    std::uint32_t vk;
    std::string_view name;
};
constexpr KeyName key_names[]{
    {vk::back, "Backspace"}, {vk::tab, "Tab"}, {vk::enter, "Enter"}, {vk::pause, "Pause"}, {vk::capital, "CapsLock"}, {vk::escape, "Esc"},
    {vk::space, "Space"}, {vk::prior, "PgUp"}, {vk::next, "PgDn"}, {vk::end, "End"}, {vk::home, "Home"}, {vk::left, "Left"}, {vk::up, "Up"},
    {vk::right, "Right"}, {vk::down, "Down"}, {vk::snapshot, "PrtSc"}, {vk::insert, "Insert"}, {vk::del, "Delete"}, {vk::multiply, "Num*"},
    {vk::add, "Num+"}, {vk::subtract, "Num-"}, {vk::decimal, "Num."}, {vk::divide, "Num/"}, {vk::numlock, "NumLock"}, {vk::scroll, "ScrollLock"},
    {vk::oem_1, ";"}, {vk::oem_plus, "="}, {vk::oem_comma, ","}, {vk::oem_minus, "-"}, {vk::oem_period, "."}, {vk::oem_2, "/"},
    {vk::oem_3, "`"}, {vk::oem_4, "["}, {vk::oem_5, "\\"}, {vk::oem_6, "]"}, {vk::oem_7, "'"}, {vk::oem_102, "\\2"}};
std::string one_key_name(std::uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk >= vk::f1 && vk <= vk::f24) return std::format("F{}", vk - vk::f1 + 1);
    if (vk >= vk::numpad0 && vk <= vk::numpad9) return std::format("Num{}", vk - vk::numpad0);
    for (const auto& k : key_names)
        if (k.vk == vk) return std::string(k.name);
    return std::format("Key{}", vk);
}
std::optional<std::uint32_t> one_key(std::string_view text) {
    const auto t = lower(text);
    if (t.size() == 1 && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= '0' && t[0] <= '9')))
        return static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(t[0])));
    if (t.size() >= 2 && t[0] == 'f' && t.find_first_not_of("0123456789", 1) == std::string::npos) {
        const int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return static_cast<std::uint32_t>(vk::f1 + n - 1);
    }
    if (t.size() == 4 && t.starts_with("num") && t[3] >= '0' && t[3] <= '9') return static_cast<std::uint32_t>(vk::numpad0 + (t[3] - '0'));
    if (t.starts_with("key") && t.size() > 3 && t.find_first_not_of("0123456789", 3) == std::string::npos) {
        const int vk = std::atoi(t.c_str() + 3);
        if (vk > 0 && vk < 256) return static_cast<std::uint32_t>(vk);
    }
    for (const auto& k : key_names)
        if (lower(k.name) == t) return k.vk;
    // Other names it goes by.
    constexpr KeyName also[]{{vk::escape, "escape"}, {vk::enter, "return"}, {vk::prior, "pageup"}, {vk::next, "pagedown"}, {vk::del, "del"},
                             {vk::insert, "ins"}, {vk::back, "bksp"}};
    for (const auto& k : also)
        if (k.name == t) return k.vk;
    return std::nullopt;
}
} // namespace
std::string pad_words(std::uint32_t bits) {
    if (!bits) return "None";
    std::string text;
    for (const auto& n : pad_names)
        if (bits & n.bit) text += (text.empty() ? "" : "+") + std::string(n.name);
    return text.empty() ? std::format("0x{:x}", bits) : text;
}
std::string key_name(std::uint32_t key) {
    if (!(key & 0xff)) return "None";
    std::string text;
    if (key & key_ctrl) text += "Ctrl+";
    if (key & key_shift) text += "Shift+";
    if (key & key_alt) text += "Alt+";
    return text + one_key_name(key & 0xff);
}
std::optional<std::uint32_t> parse_key(std::string_view text) {
    std::string t(text);
    if (lower(t) == "none" || t.empty()) return 0u;
    std::uint32_t mods = 0;
    // "Ctrl+Shift+1": its modifiers, then the key (a "+" key alone is "Num+" or "=").
    while (true) {
        const auto plus = t.find('+');
        if (plus == std::string::npos || plus + 1 >= t.size()) break;
        const auto part = lower(t.substr(0, plus));
        if (part == "ctrl" || part == "control") mods |= key_ctrl;
        else if (part == "shift") mods |= key_shift;
        else if (part == "alt") mods |= key_alt;
        else break;
        t = t.substr(plus + 1);
    }
    const auto key = one_key(t);
    if (!key) return std::nullopt;
    return *key | mods;
}
namespace {
// What a bind is bound to: a shortcut, or a preset by name.
struct Target {
    bool layer{};
    std::optional<std::size_t> action;
    std::string preset;
};
Bind* bind_of(Settings& s, const Target& t) {
    if (t.action) return &s.binds[*t.action];
    for (auto& p : s.preset_binds)
        if (lower(p.preset) == lower(t.preset)) return &p.bind;
    return nullptr;
}
std::string target_text(const Target& t) {
    if (t.layer) return "The shortcut button";
    if (t.action) return std::string(action_labels[*t.action]);
    return "Preset " + t.preset;
}
}
// Binds shown as the console lists them.
std::string binds_text(const Settings& s) {
    std::string text = std::format("Shortcut button: {} (held in the replay editor; a tap of it alone stays the game's).", pad_words(s.pad_layer));
    for (std::size_t a = 0; a < action_count; ++a)
        text += std::format("\n  {}: {}{}{}", action_labels[a], s.binds[a].pad ? pad_words(s.pad_layer) + " + " + pad_words(s.binds[a].pad) : "no button",
                            s.binds[a].key ? ", " : "", s.binds[a].key ? key_name(s.binds[a].key) : "");
    for (const auto& p : s.preset_binds)
        text += std::format("\n  Preset {}: {}{}{}", p.preset, p.bind.pad ? pad_words(s.pad_layer) + " + " + pad_words(p.bind.pad) : "no button",
                            p.bind.key ? ", " : "", p.bind.key ? key_name(p.bind.key) : "");
    return text;
}
// `bind layer <button>`, `bind <shortcut> pad <buttons> | key <key> | default | clear [pad|key]`, `bind preset <name> pad ... | key
// ... | clear`, `bind reset`. A button or key another has already is swapped with it (the other gets this one's), so nothing
// is lost; the menu always keeps a button.
std::string bind_verb(Settings& s, const std::vector<std::string>& words) {
    const auto usage = std::string("bind layer <button> | bind <shortcut> pad <button> | key <key> | default | clear [pad|key] | "
                                   "bind preset <name> pad <button> | key <key> | clear | bind reset. Shortcuts: ") + [] {
        std::string list;
        for (const auto n : action_names) list += (list.empty() ? "" : ", ") + std::string(n);
        return list;
    }();
    if (words.empty()) return usage;
    const auto first = lower(words[0]);
    if (first == "reset") {
        s.pad_layer = default_pad_layer;
        s.binds = default_binds();
        s.preset_binds.clear();
        return "The controls are back at their defaults: R3 with the buttons as they came, F10 and F11.";
    }
    Target target;
    std::vector<std::string> rest;
    if (first == "layer") {
        target.layer = true;
        rest.assign(words.begin() + 1, words.end());
    } else if (first == "preset") {
        // The name is every word up to the last one or two: "bind preset My look key F5", "bind preset My look clear".
        if (words.size() < 3) return usage;
        const auto last = lower(words.back());
        const std::size_t tail = last == "clear" ? 1 : 2;
        if (words.size() < 2 + tail) return usage;
        for (std::size_t i = 1; i + tail < words.size(); ++i) target.preset += (target.preset.empty() ? "" : " ") + words[i];
        rest.assign(words.end() - static_cast<std::ptrdiff_t>(tail), words.end());
    } else {
        const auto at = std::find(action_names.begin(), action_names.end(), first);
        if (at == action_names.end()) return usage;
        target.action = static_cast<std::size_t>(at - action_names.begin());
        rest.assign(words.begin() + 1, words.end());
    }
    if (rest.empty()) return usage;
    const auto how = lower(rest[0]);
    // The shortcut button.
    if (target.layer) {
        const auto bits = how == "default" ? std::optional<std::uint32_t>(default_pad_layer) : replay_shake::parse_binding(rest[0]);
        if (!bits || !layer_button(*bits)) return "The shortcut button is one of the pad's buttons (not A, B or a trigger): R3, L3, LB, RB, X, Y, Back...";
        // A shortcut on that button gets the old shortcut button instead.
        for (auto& b : s.binds)
            if (b.pad == *bits) b.pad = s.pad_layer;
        for (auto& p : s.preset_binds)
            if (p.bind.pad == *bits) p.bind.pad = s.pad_layer;
        s.pad_layer = *bits;
        return std::format("The shortcut button is {} now: hold it in the replay editor with the others.", pad_words(*bits));
    }
    const bool is_menu = target.action == static_cast<std::size_t>(Action::panel);
    if (how == "clear") {
        const auto what = rest.size() > 1 ? lower(rest[1]) : std::string();
        if (!target.preset.empty()) {
            // Its button, its key, or both.
            if (Bind* mine = bind_of(s, target)) {
                if (what != "key") mine->pad = 0;
                if (what != "pad") mine->key = 0;
            }
            std::erase_if(s.preset_binds, [](const PresetBind& p) { return !p.bind.pad && !p.bind.key; });
            const Bind* left = bind_of(s, target);
            if (!left) return std::format("Preset {} has no button or key now.", target.preset);
            return std::format("Preset {}: {}{}.", target.preset, left->pad ? pad_words(s.pad_layer) + " + " + pad_words(left->pad) : "no button",
                               left->key ? ", " + key_name(left->key) : std::string(", no key"));
        }
        auto& b = s.binds[*target.action];
        if (what != "key" && !is_menu) b.pad = 0;
        if (what != "pad") b.key = 0;
        return std::format("{}: {}{}.", target_text(target), b.pad ? pad_words(s.pad_layer) + " + " + pad_words(b.pad) : "no button",
                           b.key ? ", " + key_name(b.key) : std::string(", no key"));
    }
    if (how == "default") {
        if (!target.action) return usage;
        const auto wanted = default_binds()[*target.action];
        // Its button, its key, or both, through the same swaps as a new one.
        const auto what = rest.size() > 1 ? lower(rest[1]) : std::string();
        std::string reply;
        if (what != "key") reply = bind_verb(s, {words[0], "pad", pad_words(wanted.pad)});
        if (what != "pad") reply = bind_verb(s, {words[0], "key", key_name(wanted.key)});
        auto& b = s.binds[*target.action];
        return std::format("{} is back to {} + {}{}.", target_text(target), pad_words(s.pad_layer), pad_words(b.pad), b.key ? ", " + key_name(b.key) : "");
    }
    if (rest.size() < 2 || (how != "pad" && how != "key")) return usage;
    const bool pad = how == "pad";
    std::uint32_t value{};
    if (pad) {
        const auto bits = lower(rest[1]) == "none" ? std::optional<std::uint32_t>(0u) : replay_shake::parse_binding(rest[1]);
        if (!bits) return "No such button: LB, RB, LT, RT, L3, R3, A, B, X, Y, Up, Down, Left, Right, Start, Back.";
        if (*bits == s.pad_layer || (*bits & s.pad_layer)) return std::format("{} is the shortcut button itself: pick another.", pad_words(s.pad_layer));
        if (!*bits && is_menu) return "The menu always keeps a button.";
        value = *bits;
    } else {
        const auto key = parse_key(rest[1]);
        if (!key) return "No such key: F1..F24, A..Z, 0..9, Num0..Num9, Insert, Delete, Home, End, PgUp, PgDn, ... (Ctrl+, Shift+, Alt+ before it).";
        value = *key;
    }
    // Its bind (a preset gets one).
    Bind* mine = bind_of(s, target);
    if (!mine) {
        s.preset_binds.push_back({target.preset, {}});
        mine = &s.preset_binds.back().bind;
    }
    const std::uint32_t old = pad ? mine->pad : mine->key;
    // Another with it already gets this one's old one (swapped): the menu never left with no button.
    std::string swapped;
    if (value) {
        const auto take = [&](Bind& other, std::string_view name) {
            auto& slot = pad ? other.pad : other.key;
            if (&other == mine || slot != value) return true;
            if (pad && &other == &s.binds[static_cast<std::size_t>(Action::panel)] && !old) return false;
            slot = old;
            swapped = std::format(" ({} had it: it has {} now.)", name, pad ? (old ? pad_words(old) : "no button") : key_name(old));
            return true;
        };
        for (std::size_t a = 0; a < action_count; ++a)
            if (!take(s.binds[a], action_labels[a])) return "That is the menu's button, and this has none to give it: pick another.";
        for (auto& p : s.preset_binds)
            if (!take(p.bind, "Preset " + p.preset)) return "That is the menu's button, and this has none to give it: pick another.";
        // (The vector may have moved: mine again.)
        mine = bind_of(s, target);
    }
    (pad ? mine->pad : mine->key) = value;
    std::erase_if(s.preset_binds, [](const PresetBind& p) { return !p.bind.pad && !p.bind.key; });
    if (pad)
        return std::format("{}: {}.{}", target_text(target), value ? pad_words(s.pad_layer) + " + " + pad_words(value) : "no button", swapped);
    return std::format("{}: {}.{}", target_text(target), value ? key_name(value) : "no key", swapped);
}

Settings apply_preset(const Settings& current, const Preset& preset) {
    Settings next = preset.settings;
    next.enabled = current.enabled;
    next.panel_bind = current.panel_bind;
    // The focus (and its keys) belongs to the shot in hand, not to a look.
    next.focus = current.focus;
    next.focus_distance = current.focus_distance;
    next.focus_blur = current.focus_blur;
    next.focus_speed = current.focus_speed;
    next.focus_depth = current.focus_depth;
    next.focus_skater = current.focus_skater;
    next.focus_keys = current.focus_keys;
    next.shake_keys = current.shake_keys;
    // So do the light's keys (and whether it follows them).
    next.light_keyed = current.light_keyed;
    next.light_keys = current.light_keys;
    next.pad_shortcuts = current.pad_shortcuts;
    next.keep_keys = current.keep_keys;
    next.video_folder = current.video_folder;
    // The player's controls go with the player, not a look.
    next.pad_layer = current.pad_layer;
    next.binds = current.binds;
    next.preset_binds = current.preset_binds;
    next.preset = preset.name;
    return clamped(next);
}

std::vector<Preset> built_in_presets() {
    std::vector<Preset> list;
    // Two plain fisheyes to start from, every value set by hand, the field of view fixed: equidistant
    // (Distortion 100%), 90 degrees at the picture's corners (a full-frame fisheye: its image circle the
    // corner's distance, in half-heights), the edge compressed just enough that the game's picture at 150
    // degrees feeds all of it (fit 1). No overlay, no glass, no camera curve; the shake as it is.
    {
        Preset p;
        p.name = "Fisheye 16-9";
        p.notes = "A plain full-frame fisheye on the 16:9 picture: equidistant, 90 degrees at the corners, 150 degrees.";
        p.settings.curve = 1.0f;
        p.settings.circle = 2.040f;
        p.settings.compress = 0.084f;
        p.settings.fov_mode = FovMode::fixed;
        p.settings.fov = 150.0f;
        list.push_back(p);
    }
    {
        Preset p;
        p.name = "Fisheye 4-3";
        p.notes = "A plain full-frame fisheye on a 4:3 picture (4:3 bars, and the picture shape 4:3): equidistant, 90 degrees at the "
                  "corners, 150 degrees.";
        p.settings.curve = 1.0f;
        p.settings.circle = 1.667f;
        p.settings.compress = 0.102f;
        p.settings.fov_mode = FovMode::fixed;
        p.settings.fov = 150.0f;
        p.settings.bars = Bars::four_three;
        p.settings.replay_aspect = Aspect::four_three;
        list.push_back(p);
    }
    {
        // Nothing on: the game's own picture, as the replay editor shows it without Replay Plus, to start a look from or
        // to go back to. Every value as it comes (no lens, the editor's own field of view, no bars, overlay, shape, grade
        // or light, the game's own effects left to it), the video tab's sharpening off too, and the shake off. (The focus
        // and its keys are the shot's, as with every preset.)
        Preset p;
        p.name = "Plain";
        p.notes = "Nothing on: the game's own picture. No lens, frame, colour, light or shake.";
        p.settings.sharp_middle = false;
        p.settings.sharpen = 0.0f;
        p.shake = Json::parse(R"({"enabled":false})");
        list.push_back(p);
    }
    return list;
}
// Every name a released version wrote, so one deleted stays deleted. ("Plain" goes in here too once a version that
// writes it has been released: until then everyone who updates is given it once.)
std::vector<std::string> older_built_in_names() { return {"Fisheye 16-9", "Fisheye 4-3"}; }
// Built-ins earlier versions wrote that this one has not; set_up_folders moves the ones still as written into "Older
// built-ins" (one saved over, or made anew, is the player's own and stays).
std::vector<std::string> retired_built_in_names() { return {}; }

// ---- Verbs -----------------------------------------------------------------------------------
Reply command(Settings& settings, std::string_view verb, std::span<const std::string> words) {
    const auto v = lower(verb);
    if (v == "on" || v == "off") {
        const bool on = v == "on";
        const bool changed = settings.enabled != on;
        settings.enabled = on;
        return {on ? "Replay Plus is on." : "Replay Plus is off: the replay editor shows the game's own picture.", changed};
    }
    if (v == "get" || v == "set" || v == "step") {
        if (words.empty()) return {std::format("{} <key>{}: `replayplus list` shows the keys.", v, v == "get" ? "" : v == "set" ? " <value>" : " <steps>")};
        if (lower(words[0]) == "frame.file") {
            if (v == "get") return {std::format("frame.file = {}", settings.hood_file.empty() ? std::string("(none)") : settings.hood_file)};
            if (v == "set") {
                std::string name;
                for (std::size_t i = 1; i < words.size(); ++i) name += (i > 1 ? " " : "") + words[i];
                const bool changed = settings.hood_file != name;
                settings.hood_file = name;
                if (!name.empty()) settings.hood = Hood::picture;
                return {std::format("frame.file = {}", name.empty() ? std::string("(none)") : name), changed};
            }
        }
        const auto* f = field(words[0]);
        if (!f) return {std::format("No value is called {}: `replayplus list` shows them.", words[0])};
        if (v == "get") return {std::format("{} = {}", f->key, shown(*f, settings))};
        if (words.size() < 2) return {std::format("{} {} <{}>", v, f->key, v == "set" ? "value" : "steps")};
        const Settings before = settings;
        if (v == "set") {
            std::string why;
            if (!assign(*f, settings, words[1], why)) return {why};
        } else {
            int steps{};
            const auto& text = words[1];
            const auto begin = text.data() + (text.starts_with("+") ? 1 : 0);
            if (std::from_chars(begin, text.data() + text.size(), steps).ec != std::errc{}) return {"step <key> <whole number of steps>"};
            step(*f, settings, steps);
        }
        settings = clamped(settings);
        return {std::format("{} = {}", f->key, shown(*f, settings)), !(before == settings)};
    }
    if (v == "reset") {
        const auto what = words.empty() ? std::string("all") : lower(words[0]);
        const Settings before = settings;
        if (what == "all") {
            Settings fresh;
            fresh.enabled = settings.enabled;
            fresh.panel_bind = settings.panel_bind;
            fresh.video_folder = settings.video_folder; // where videos go is no look
            // Nor are the player's controls (reset controls puts those back).
            fresh.pad_layer = settings.pad_layer;
            fresh.binds = settings.binds;
            fresh.preset_binds = settings.preset_binds;
            settings = fresh;
            return {"Every Replay Plus value is back at its default.", !(before == settings)};
        }
        if (what == "controls") {
            settings.pad_layer = default_pad_layer;
            settings.binds = default_binds();
            settings.preset_binds.clear();
            return {"The controls are back at their defaults: R3 with the buttons as they came, F10 and F11.", !(before == settings)};
        }
        if (const auto* f = field(what)) {
            f->set(settings, f->get(defaults));
            return {std::format("{} = {} (its default)", f->key, shown(*f, settings)), !(before == settings)};
        }
        // A whole tab: "reset lens".
        for (std::size_t t = 0; t < tab_names.size(); ++t)
            if (lower(tab_names[t]) == what || (what == "colour" && t == static_cast<std::size_t>(Tab::grade)) ||
                (what == "color" && t == static_cast<std::size_t>(Tab::grade))) {
                for (const auto& f : all_fields)
                    if (static_cast<std::size_t>(f.tab) == t) f.set(settings, f.get(defaults));
                if (t == static_cast<std::size_t>(Tab::frame)) settings.hood_file.clear();
                return {std::format("{} is back at its defaults.", tab_names[t]), !(before == settings)};
            }
        return {"reset all, reset <tab> (lens, frame, colour, light, video) or reset <key>."};
    }
    if (v == "bind") {
        if (words.empty() || (words.size() == 1 && lower(words[0]) == "list")) return {binds_text(settings)};
        if (const auto first = lower(words[0]); first == "layer" || first == "reset" || first == "preset" ||
                                                 std::find(action_names.begin(), action_names.end(), first) != action_names.end()) {
            const Settings before = settings;
            const auto reply = bind_verb(settings, std::vector<std::string>(words.begin(), words.end()));
            return {reply, !(before == settings)};
        }
        const auto text = lower(words[0]);
        std::uint32_t bind{};
        if (text == "clear" || text == "default" || text == "0") bind = 0;
        else {
            const auto begin = text.data() + (text.starts_with("0x") ? 2 : 0);
            if (std::from_chars(begin, text.data() + text.size(), bind, 16).ec != std::errc{} || !bind)
                return {"bind <0x...> (XInput button bits), or bind default: the panel opens on RB + R3 held."};
        }
        const bool changed = settings.panel_bind != bind;
        settings.panel_bind = bind;
        return {bind ? std::format("The panel opens on 0x{:x} held.", bind) : std::string("The panel opens on RB + R3 held."), changed};
    }
    if (v == "list" || v == "values") {
        std::string wanted = words.empty() ? std::string() : lower(words[0]);
        std::string text;
        for (std::size_t t = 1; t < tab_names.size(); ++t) {
            if (t == static_cast<std::size_t>(Tab::presets)) continue;
            if (!wanted.empty() && lower(tab_names[t]) != wanted) continue;
            text += std::format("{}:", tab_names[t]);
            for (const auto& f : all_fields)
                if (static_cast<std::size_t>(f.tab) == t) text += std::format(" {}={}", f.key, shown(f, settings));
            if (t == static_cast<std::size_t>(Tab::frame)) text += std::format(" frame.file={}", settings.hood_file.empty() ? "-" : settings.hood_file);
            text += "\n";
        }
        if (!text.empty()) text.pop_back();
        return {text.empty() ? std::string("list [lens|frame|colour|light|video]") : text};
    }
    return {std::format("replayplus {}: no such verb. Try `replayplus help`.", verb)};
}

// ---- Helpers ----------------------------------------------------------------------------------
bool wants_picture(const Settings& s) noexcept {
    // (The camera light is not drawn by the pass: it is a light of the game's, replay_plus_light.h, since 1.3.2.)
    if (!s.enabled) return false;
    return lens_on(s) || !lens::grade_for(s).identity() || s.bars != Bars::none || (s.hood != Hood::none && s.hood_opacity > 0);
}
bool wants_views(const Settings& s) noexcept {
    // The field of view is read for the lens's fit and the light's reach, and set when fixed; the camera, for how far
    // away the skater is (the skater's autofocus).
    return s.enabled && (s.fov_mode != FovMode::game || lens_on(s) || s.light || (s.focus == Focus::autofocus && s.focus_skater));
}
bool wants_game_settings(const Settings& s) noexcept {
    // The lens holds the game's barrel off (it would bend the picture twice).
    return s.enabled && (lens_on(s) || s.game_look || s.focus != Focus::off);
}

// ---- Focus keys ------------------------------------------------------------------------------
FocusAt focus_at(const Settings& s, double time) {
    const auto& keys = s.focus_keys;
    if (s.focus != Focus::keys || keys.empty()) return {s.focus_distance, s.focus_blur};
    if (!std::isfinite(time) || time <= keys.front().time) return {keys.front().distance, keys.front().blur};
    if (time >= keys.back().time) return {keys.back().distance, keys.back().blur};
    const auto next = std::upper_bound(keys.begin(), keys.end(), time, [](double t, const FocusKey& k) { return t < k.time; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    const double span = b.time - a.time;
    double u = span > 1e-9 ? (time - a.time) / span : 1.0;
    u = u * u * (3.0 - 2.0 * u); // eased in and out
    // The distance moves evenly in how near it looks (1 / distance), as a focus ring turns.
    const double near_a = 1.0 / std::max(a.distance, 0.05f), near_b = 1.0 / std::max(b.distance, 0.05f);
    const double near = near_a + (near_b - near_a) * u;
    return {static_cast<float>(1.0 / near), static_cast<float>(a.blur + (b.blur - a.blur) * u)};
}
FocusRanges focus_ranges(FocusAt focus, float depth, float centre_zoom) {
    const float d = std::clamp(std::isfinite(focus.distance) ? focus.distance : 3.0f, 0.05f, focus_far_away);
    const float k = std::clamp(std::isfinite(depth) ? depth : 0.15f, 0.02f, 0.6f), ramp = 0.25f + k;
    FocusRanges r;
    r.near_end = d / (1.0f + k);
    r.near_start = d / (1.0f + k + ramp);
    r.far_start = std::min(d / (1.0f - k), focus_far_away);
    r.far_end = 1.0f - k - ramp > 0.02f ? std::min(d / (1.0f - k - ramp), focus_far_away) : focus_far_away;
    const float blur = std::clamp(std::isfinite(focus.blur) ? focus.blur : 1.0f, 0.0f, 4.0f);
    r.blur_max = blur * 0.5f / std::max(std::isfinite(centre_zoom) ? centre_zoom : 1.0f, 1.0f);
    return r;
}
void set_focus_key(Settings& s, double time, float distance, float blur) {
    if (!std::isfinite(time)) return;
    distance = std::clamp(std::isfinite(distance) ? distance : 3.0f, 0.2f, 200.0f);
    blur = std::clamp(std::isfinite(blur) ? blur : 1.0f, 0.0f, 4.0f);
    for (auto& key : s.focus_keys)
        if (std::abs(key.time - time) <= 1.0 / 30.0) {
            key.distance = distance;
            key.blur = blur;
            return;
        }
    if (s.focus_keys.size() >= max_focus_keys) return;
    s.focus_keys.push_back({time, distance, blur});
    std::sort(s.focus_keys.begin(), s.focus_keys.end(), [](const FocusKey& a, const FocusKey& b) { return a.time < b.time; });
}
bool remove_focus_key(Settings& s, double time, double tolerance) {
    auto best = s.focus_keys.end();
    double nearest = tolerance;
    for (auto it = s.focus_keys.begin(); it != s.focus_keys.end(); ++it)
        if (const double d = std::abs(it->time - time); d <= nearest) {
            nearest = d;
            best = it;
        }
    if (best == s.focus_keys.end()) return false;
    s.focus_keys.erase(best);
    return true;
}
const FocusKey* focus_key_at(const Settings& s, double time) {
    for (const auto& key : s.focus_keys)
        if (std::abs(key.time - time) <= 1.0 / 30.0) return &key;
    return nullptr;
}

// ---- Light keys ------------------------------------------------------------------------------
LightAt light_at(const Settings& s, double time) {
    const LightAt set{s.light, s.light_power, s.light_range, s.light_cone, s.light_warmth};
    const auto& keys = s.light_keys;
    if (!s.light_keyed || keys.empty()) return set;
    const auto of = [&](const LightKey& k) { return LightAt{s.light && k.on, k.power, k.range, k.cone, k.warmth}; };
    if (!std::isfinite(time) || time <= keys.front().time) return of(keys.front());
    if (time >= keys.back().time) return of(keys.back());
    const auto next = std::upper_bound(keys.begin(), keys.end(), time, [](double t, const LightKey& k) { return t < k.time; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    const double span = b.time - a.time;
    double u = span > 1e-9 ? (time - a.time) / span : 1.0;
    u = u * u * (3.0 - 2.0 * u); // eased in and out
    const auto mix = [&](float x, float y) { return static_cast<float>(x + (y - x) * u); };
    return {s.light && a.on, mix(a.power, b.power), mix(a.range, b.range), mix(a.cone, b.cone), mix(a.warmth, b.warmth)};
}
void set_light_key(Settings& s, double time, const LightAt& light) {
    if (!std::isfinite(time)) return;
    LightKey key{time, light.on, std::clamp(light.power, 0.0f, 8.0f), std::clamp(light.range, 1.0f, 50.0f), std::clamp(light.cone, 10.0f, 180.0f),
                 std::clamp(light.warmth, -1.0f, 1.0f)};
    for (auto& k : s.light_keys)
        if (std::abs(k.time - time) <= 1.0 / 30.0) {
            key.time = k.time;
            k = key;
            return;
        }
    if (s.light_keys.size() >= max_light_keys) return;
    s.light_keys.push_back(key);
    std::sort(s.light_keys.begin(), s.light_keys.end(), [](const LightKey& a, const LightKey& b) { return a.time < b.time; });
}
bool remove_light_key(Settings& s, double time, double tolerance) {
    auto best = s.light_keys.end();
    double nearest = tolerance;
    for (auto it = s.light_keys.begin(); it != s.light_keys.end(); ++it)
        if (const double d = std::abs(it->time - time); d <= nearest) {
            nearest = d;
            best = it;
        }
    if (best == s.light_keys.end()) return false;
    s.light_keys.erase(best);
    return true;
}
const LightKey* light_key_at(const Settings& s, double time) {
    for (const auto& key : s.light_keys)
        if (std::abs(key.time - time) <= 1.0 / 30.0) return &key;
    return nullptr;
}

// ---- Shake keys ------------------------------------------------------------------------------
bool shake_on_at(const Settings& s, double time, bool otherwise) {
    const auto& keys = s.shake_keys;
    if (keys.empty()) return otherwise;
    bool on = !keys.front().on;
    for (const auto& key : keys) {
        if (key.time > time) break;
        on = key.on;
    }
    return on;
}
void tidy_shake_keys(Settings& s) {
    auto& keys = s.shake_keys;
    std::stable_sort(keys.begin(), keys.end(), [](const ShakeKey& a, const ShakeKey& b) { return a.time < b.time; });
    std::vector<ShakeKey> kept;
    for (const auto& key : keys)
        if (kept.empty() || key.on != kept.back().on) kept.push_back(key);
    keys = std::move(kept);
}
bool remove_shake_key(Settings& s, double time, double tolerance) {
    auto best = s.shake_keys.end();
    double nearest = tolerance;
    for (auto it = s.shake_keys.begin(); it != s.shake_keys.end(); ++it)
        if (const double d = std::abs(it->time - time); d <= nearest) {
            nearest = d;
            best = it;
        }
    if (best == s.shake_keys.end()) return false;
    s.shake_keys.erase(best);
    tidy_shake_keys(s);
    return true;
}
bool toggle_shake_key(Settings& s, double time, bool otherwise) {
    if (!std::isfinite(time)) return shake_on_at(s, time, otherwise);
    if (!remove_shake_key(s, time)) {
        const bool now = shake_on_at(s, time, otherwise);
        if (s.shake_keys.size() < max_shake_keys) s.shake_keys.push_back({time, !now});
        tidy_shake_keys(s);
    }
    return shake_on_at(s, time, otherwise);
}
} // namespace dingosdk::replay_plus
