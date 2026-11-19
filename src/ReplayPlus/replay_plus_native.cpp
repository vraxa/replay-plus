#include "replay_plus_native.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Resource/texture.h"
#include "Engine/Vfs/game_bundles.h"
#include "Engine/Vfs/game_textures.h"
#include "Extension/Skater/xray_skeleton.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where the game keeps each (build 25414733; texture names rarely change between builds): found with a scan of
// every bundle's resources on 2026-11-19. The faces are TrueType files as they are (resources of type 0x063994c5).
namespace dingosdk::replay_plus::native {
namespace {
namespace fb = dingosdk::frostbite;

constexpr std::string_view font_toc = "Win32/ui.toc";
constexpr std::string_view font_bundle = "win32/configurations/delmarrimefontconfiguration_languageformat_english";
constexpr std::array<std::string_view, 3> font_names{
    "ui/styleguide/fonts/gilroy/gilroy-semibold", "ui/styleguide/fonts/gilroy/gilroy-extrabold",
    "ui/styleguide/fonts/skatermarker/skatemarker_bold"};

// The button glyphs and the settings cog: with the game's configuration. The replay editor's icons: with the core
// game assets. The rest: in the game's level root.
constexpr std::string_view globals_toc = "Win32/globals.toc";
constexpr std::string_view globals_bundle = "win32/configurations/gameconfigurations/delmargameconfiguration";
constexpr std::string_view root_toc = "Win32/levels/game/bam_levelroot/bam_levelroot.toc";
constexpr std::string_view core_bundle = "win32/levels/game/bam_levelroot/bam_coregameassets";
constexpr std::string_view root_bundle = "win32/levels/game/bam_levelroot/bam_levelroot";
enum class Place : std::uint8_t { globals, core, root };
struct Source {
    Place place;
    std::string_view name;
    float size; // its visible part drawn at most this tall on a 1920 x 1080 screen: made that size (no mips in the atlas)
};
constexpr float glyph_size = 40, icon_size = 44;
constexpr std::array<Source, static_cast<std::size_t>(Picture::count)> sources{{
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_a_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_b_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_x_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_y_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_lbumper_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_rbumper_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_ltrigger_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_rtrigger_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_lstick-click_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_rstick-click_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_menu_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_options_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-none_256_256_256", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-up_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-down_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-left_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-right_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-vertical_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/xbox/img_xbox_dpad-horizontal_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_cross_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_circle_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_square_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_triangle_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_lbumper_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_rbumper_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_ltrigger_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_rtrigger_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_lstick-click_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_rstick-click_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps4_touchpad_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps4_options_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps5_touchpad_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps5_options_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-none_256_256_256", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-up_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-down_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-left_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-right_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-vertical_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/ps/img_ps_dpad-horizontal_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_bg_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_enter_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_esc_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_tab_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_space_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_up_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_down_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_left_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_keyboard_right_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_mouse_lclick_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/mnk/img_input_mouse_wheel_108_108_108", glyph_size},
    {Place::globals, "ui/textures/icons/inputs/img_input_combinatorial_and_108_64_108", glyph_size},
    {Place::core, "ui/textures/icons/toolbox/icon_replay_camera_128_128_128", icon_size},
    {Place::core, "ui/textures/features/replay/img_replay_zoom_64_64_64", icon_size},
    {Place::root, "ui/textures/common/shapes/bespoke/img_icon_condition_frame_alt_128_128_128", icon_size},
    {Place::core, "ui/textures/icons/replay/img_replay_effects_128_128_128", icon_size},
    {Place::root, "ui/textures/icons/generic/img_icon_show_128_128_128", icon_size},
    {Place::root, "ui/textures/common/shapes/bespoke/img_icon_emptyspotlight_128_128_128", icon_size},
    {Place::core, "ui/textures/icons/img_icon_gamepad_192_96_96", icon_size},
    {Place::globals, "ui/textures/icons/generic/img_generic_settings_128_128_128", icon_size},
    {Place::core, "ui/textures/icons/replay/img_replay_save_128_128_128", icon_size},
    {Place::core, "ui/textures/icons/generic/img_generic_warning_128_64_64", icon_size},
}};
constexpr std::size_t picture_count = sources.size();

struct Read {
    std::array<std::vector<unsigned char>, 3> fonts;           // the TrueType files (empty: missing)
    std::array<float, 3> line_per_em{1, 1, 1};                 // each one's line over its em
    std::array<std::optional<fb::Image>, picture_count> images; // their visible parts, at the size the game keeps them
};
// A TrueType face's line (its hhea ascender less its descender, as ImGui sizes it) over its em (head's units per em).
float line_per_em_of(const std::vector<unsigned char>& font) {
    const auto u16 = [&](std::size_t at) { return at + 2 <= font.size() ? static_cast<unsigned>(font[at] << 8 | font[at + 1]) : 0u; };
    const auto u32 = [&](std::size_t at) { return at + 4 <= font.size() ? (std::uint32_t{u16(at)} << 16) | u16(at + 2) : 0u; };
    const auto i16 = [&](std::size_t at) { return static_cast<int>(static_cast<std::int16_t>(u16(at))); };
    std::size_t head = 0, hhea = 0;
    const unsigned tables = u16(4);
    for (unsigned t = 0; t < tables; ++t) {
        const std::size_t entry = 12 + std::size_t{t} * 16;
        const auto tag = u32(entry);
        if (tag == 0x68656164u) head = u32(entry + 8); // head
        if (tag == 0x68686561u) hhea = u32(entry + 8); // hhea
    }
    const unsigned em = head ? u16(head + 18) : 0u;
    const int line = hhea ? i16(hhea + 4) - i16(hhea + 6) : 0;
    const float ratio = em ? static_cast<float>(line) / static_cast<float>(em) : 1.0f;
    return ratio > 0.8f && ratio < 2.0f ? ratio : 1.0f;
}
// A picture's visible part: the clear margin round it dropped (the game's glyphs fill only the middle two thirds of
// theirs), one pixel of it kept for the filtering at the edge. Sizes are then the glyph's own.
fb::Image visible_part(const fb::Image& image) {
    if (!image.width || !image.height || image.rgba.size() != std::size_t{image.width} * image.height * 4) return image;
    std::uint32_t x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x)
            if (image.rgba[(std::size_t{y} * image.width + x) * 4 + 3] > 24) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
    if (x1 <= x0 || y1 <= y0) return image;
    x0 = x0 > 0 ? x0 - 1 : 0;
    y0 = y0 > 0 ? y0 - 1 : 0;
    x1 = std::min(image.width, x1 + 1);
    y1 = std::min(image.height, y1 + 1);
    fb::Image result{x1 - x0, y1 - y0, std::vector<std::uint8_t>(std::size_t{x1 - x0} * (y1 - y0) * 4)};
    for (std::uint32_t y = y0; y < y1; ++y)
        std::memcpy(result.rgba.data() + std::size_t{y - y0} * result.width * 4, image.rgba.data() + (std::size_t{y} * image.width + x0) * 4,
                    std::size_t{result.width} * 4);
    return result;
}
Read read_all() {
    Read result;
    const auto folder = xray::game_folder();
    try {
        vfs::GameData data(folder);
        const auto toc = data.read_toc(font_toc);
        const auto bundle = data.read_bundle(toc, font_bundle);
        if (!bundle) throw std::runtime_error("the bundle of its faces is missing");
        for (std::size_t i = 0; i < font_names.size(); ++i) {
            std::size_t index{};
            const auto* asset = bundle->find(fb::AssetKind::resource, font_names[i], &index);
            const auto* payload = asset ? bundle->payload(fb::AssetKind::resource, index) : nullptr;
            if (!payload) {
                logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the game's face {} is missing.", font_names[i]);
                continue;
            }
            const auto bytes = data.read(*payload);
            // A TrueType file (its version 1.0) or an OpenType one ("OTTO").
            if (bytes.size() < 12 || !((bytes[0] == std::byte{0} && bytes[1] == std::byte{1}) || (bytes[0] == std::byte{'O'} && bytes[1] == std::byte{'T'}))) {
                logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the game's face {} is not a font file.", font_names[i]);
                continue;
            }
            result.fonts[i].resize(bytes.size());
            std::memcpy(result.fonts[i].data(), bytes.data(), bytes.size());
            result.line_per_em[i] = line_per_em_of(result.fonts[i]);
        }
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the game's faces are unavailable: {}.", error.what());
    }
    try {
        vfs::GameTextures textures(folder);
        std::size_t missing{};
        for (std::size_t i = 0; i < picture_count; ++i) {
            const auto& source = sources[i];
            const auto toc = source.place == Place::globals ? globals_toc : root_toc;
            const auto bundle = source.place == Place::globals ? globals_bundle : source.place == Place::core ? core_bundle : root_bundle;
            try {
                result.images[i] = visible_part(textures.read(toc, bundle, source.name, 256));
            } catch (const std::exception& error) {
                ++missing;
                logging::log(logging::Level::debug, logging::Channel::graphics, "Replay Plus: the game's picture {} is unavailable: {}.", source.name,
                             error.what());
            }
        }
        if (missing)
            logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: {} of the game's {} button and tab pictures are unavailable.",
                         missing, picture_count);
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the game's button and tab pictures are unavailable: {}.", error.what());
    }
    return result;
}

struct Reads {
    std::mutex mutex;
    bool started{};
    std::future<Read> pending;
    std::optional<Read> done; // kept: each atlas the overlay builds (one a swapchain) needs them
};
Reads& reads() {
    static auto* state = new Reads;
    return *state;
}

// What the atlas the overlay draws with has of them.
struct Atlas {
    ImFontAtlas* atlas{};
    std::array<ImFont*, 3> faces{};
    std::array<float, 3> line_per_em{1, 1, 1};
    std::array<int, picture_count> rects{};
    std::array<fb::Image, picture_count> scaled; // at the size drawn on this screen, until filled in
    std::array<ImVec2, picture_count> uv0{}, uv1{};
    std::array<float, picture_count> aspects{};
    bool filled{};
};
Atlas& atlas_state() {
    static auto* state = new Atlas;
    return *state;
}
} // namespace

void prepare() noexcept {
    try {
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        if (std::exchange(state.started, true)) return;
        state.pending = std::async(std::launch::async, &read_all);
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the game's faces and pictures could not start reading.");
    }
}

void reserve(ImFontAtlas& atlas, float width, float height, std::chrono::milliseconds wait) noexcept {
    auto& a = atlas_state();
    a = {};
    a.rects.fill(-1);
    a.aspects.fill(1.0f);
    try {
        prepare();
        auto& state = reads();
        std::lock_guard lock(state.mutex);
        // A read still running stays pending for the next atlas.
        if (!state.done && state.pending.valid() && state.pending.wait_for(wait) == std::future_status::ready) state.done = state.pending.get();
        if (!state.done) return;
        const Read& read = *state.done;
        const float scale = ui_scale(width, height);
        a.atlas = &atlas;
        // The faces: the label and value faces made at the size the panel's largest text is drawn (its values and
        // prompts, an em of 27 pixels on a 1080-line screen), the title's at a title's (72); ImGui makes a face by its
        // line, taller than its em.
        static constexpr std::array<float, 3> made{28.0f, 28.0f, 74.0f};
        for (std::size_t i = 0; i < read.fonts.size(); ++i) {
            if (read.fonts[i].empty()) continue;
            a.line_per_em[i] = read.line_per_em[i];
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false; // kept in reads() for as long as the DLL is loaded
            config.OversampleH = 2;
            config.OversampleV = 1;
            config.PixelSnapH = false;
            std::snprintf(config.Name, sizeof config.Name, "Replay Plus %s", i == 0 ? "Gilroy SemiBold" : i == 1 ? "Gilroy ExtraBold" : "SkateMarker");
            a.faces[i] = atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(read.fonts[i].data()), static_cast<int>(read.fonts[i].size()),
                                                    std::round(made[i] * scale * read.line_per_em[i]), &config, atlas.GetGlyphRangesDefault());
        }
        // The pictures, each the size it is drawn at here (the atlas has no mips).
        for (std::size_t i = 0; i < picture_count; ++i) {
            if (!read.images[i]) continue;
            const auto& image = *read.images[i];
            if (!image.width || !image.height) continue;
            const float tall = std::max(8.0f, std::round(sources[i].size * scale));
            const float aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
            const auto h = static_cast<std::uint32_t>(std::min(tall, static_cast<float>(image.height)));
            const auto w = static_cast<std::uint32_t>(std::max(1.0f, std::round(static_cast<float>(h) * aspect)));
            a.scaled[i] = fb::resize(image, w, h);
            a.aspects[i] = aspect;
            atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
            atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
            a.rects[i] = atlas.AddCustomRectRegular(static_cast<int>(w), static_cast<int>(h));
        }
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the game's faces and pictures could not be added: {}.", error.what());
        a = {};
        a.rects.fill(-1);
    } catch (...) {
        a = {};
        a.rects.fill(-1);
    }
}

void fill(ImFontAtlas& atlas) noexcept {
    auto& a = atlas_state();
    if (a.atlas != &atlas) return;
    try {
        unsigned char* pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) {
            a.rects.fill(-1);
            return;
        }
        for (std::size_t i = 0; i < picture_count; ++i) {
            if (a.rects[i] < 0) continue;
            const auto& image = a.scaled[i];
            const auto* rect = atlas.GetCustomRectByIndex(a.rects[i]);
            if (!rect || !rect->IsPacked() || rect->X + image.width > static_cast<unsigned>(width) || rect->Y + image.height > static_cast<unsigned>(height)) {
                a.rects[i] = -1;
                continue;
            }
            for (std::uint32_t y = 0; y < image.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4, image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4,
                            static_cast<std::size_t>(image.width) * 4);
            atlas.CalcCustomRectUV(rect, &a.uv0[i], &a.uv1[i]);
            a.scaled[i] = {}; // in the atlas now
        }
        a.filled = true;
    } catch (...) {
        a.rects.fill(-1);
    }
}

ImFont* face(Face which) noexcept {
    const auto& a = atlas_state();
    if (!a.atlas || ImGui::GetCurrentContext() == nullptr || ImGui::GetIO().Fonts != a.atlas) return nullptr;
    auto* font = a.faces[static_cast<std::size_t>(which)];
    return font && font->IsLoaded() ? font : nullptr;
}

float line_per_em(Face which) noexcept { return face(which) ? atlas_state().line_per_em[static_cast<std::size_t>(which)] : 1.0f; }

bool loaded(Picture picture) noexcept {
    const auto& a = atlas_state();
    const auto i = static_cast<std::size_t>(picture);
    return i < picture_count && a.filled && a.rects[i] >= 0 && ImGui::GetCurrentContext() && ImGui::GetIO().Fonts == a.atlas;
}

float aspect(Picture picture) noexcept {
    const auto i = static_cast<std::size_t>(picture);
    return loaded(picture) ? atlas_state().aspects[i] : 1.0f;
}

bool draw(ImDrawList* draw, Picture picture, ImVec2 a, ImVec2 b, ImU32 tint) noexcept {
    if (!draw || !loaded(picture)) return false;
    const auto& s = atlas_state();
    const auto i = static_cast<std::size_t>(picture);
    draw->AddImage(s.atlas->TexID, a, b, s.uv0[i], s.uv1[i], tint);
    return true;
}

bool draw_wide_key(ImDrawList* draw, ImVec2 a, ImVec2 b) noexcept {
    if (!draw || !loaded(Picture::key)) return false;
    const auto& s = atlas_state();
    const auto i = static_cast<std::size_t>(Picture::key);
    const ImVec2 uv0 = s.uv0[i], uv1 = s.uv1[i];
    const float h = b.y - a.y, w = b.x - a.x;
    if (w <= h + 0.5f) {
        draw->AddImage(s.atlas->TexID, a, b, uv0, uv1);
        return true;
    }
    // Its left and right halves at their own size, its middle column stretched between them.
    const float half = h * 0.5f, mid_u = (uv0.x + uv1.x) * 0.5f;
    draw->AddImage(s.atlas->TexID, a, ImVec2(a.x + half, b.y), uv0, ImVec2(mid_u, uv1.y));
    draw->AddImage(s.atlas->TexID, ImVec2(a.x + half, a.y), ImVec2(b.x - half, b.y), ImVec2(mid_u, uv0.y), ImVec2(mid_u, uv1.y));
    draw->AddImage(s.atlas->TexID, ImVec2(b.x - half, a.y), b, ImVec2(mid_u, uv0.y), uv1);
    return true;
}
} // namespace dingosdk::replay_plus::native
