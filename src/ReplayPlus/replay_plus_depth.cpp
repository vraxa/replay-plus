#include "replay_plus_overlay.h"
#include "replay_plus_depth.h"
#include "replay_plus_link.h"
#include <Windows.h>
#include <atomic>
#include "Engine/Core/Log/logging.h"
#include "Extension/ReplayShake/replay_shake_export.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <array>
#include <format>
#include <mutex>
#include <vector>

// The game's depth for the camera light. ReSkate hooks ID3D12GraphicsCommandList::ResourceBarrier once
// (Extension/UI/Overlay/depth_tap.cpp, its depth tap for screen captures) and hands Replay Plus every
// barrier the game records, before passing it on. While the light is on, the game's main depth (the
// first depth target each frame that goes from being written to being read, a picture's shape: as the
// tap takes it) is copied into a texture of Replay Plus's own, on the game's own command list, where
// the depth is finished. The picture's pass then reads that copy, later on the same queue.
namespace dingosdk::replay_plus::depth {
namespace {
using Microsoft::WRL::ComPtr;
struct Retired {
    ComPtr<ID3D12Resource> texture;
    std::uint64_t epoch{};
};
struct Shared {
    std::mutex mutex;
    bool wanted{};
    std::uint64_t epoch{1}, copied_epoch{};
    std::uint32_t screen_width{}, screen_height{};
    ComPtr<ID3D12Resource> texture;   // R32_TYPELESS, in PIXEL_SHADER_RESOURCE between copies
    std::uint32_t width{}, height{};
    DXGI_FORMAT source_format{DXGI_FORMAT_UNKNOWN};
    std::uint64_t serial{};           // changes when the texture is made anew
    std::vector<Retired> retired;
    std::uint64_t copies{};
    bool said{};
    // The game has a half-size depth of the same view too (1280x720 beside 2560x1440, recorded first on
    // some frames): the largest one seen lately is the main view's, and only it is taken.
    std::uint64_t largest{}, largest_epoch{};
    // Exports (for_export in replay_plus_depth.h): the depth of the frame each video frame was drawn in,
    // copied from the depth copy as the game's video frame goes to be copied out, in a ring, oldest
    // first; let go a few seconds after the last export frame.
    struct Kept {
        ComPtr<ID3D12Resource> texture;
        std::uint32_t width{}, height{};
        std::uint64_t epoch{};
        bool own_frame{}; // the game's depth went by earlier in the same frame
        // A few rows of the video frame drawn in that frame, read back: which frame the encoder is given.
        ComPtr<ID3D12Resource> print;
        const std::uint8_t* print_cpu{};
        std::uint32_t print_width{}, print_height{};
    };
    // Six: the game reads its video frames back four frames on, and draws one or two at an export's start that
    // it never encodes (2026-11-17); a frame's own may be kept before or after the encoder takes one. Those
    // never encoded are the ones let go when it is full.
    std::array<Kept, 6> kept{};
    std::uint64_t kept_pushed{}, kept_popped{}, kept_epoch{}, kept_taken{}, kept_dropped{}, kept_skipped{}, kept_unfound{}, kept_exact{};
    bool kept_fallback_said{}, kept_late_said{}, kept_drop_said{}, kept_unfound_said{};
    int kept_inexact_said{};
    // Which of the game's depths the light's copy comes from (logged when it changes, a few times).
    ID3D12Resource* depth_source{};
    std::uint64_t depth_source_copies{};
    int depth_source_changes{};
    ID3D12GraphicsCommandList* own_list{}; // Replay Plus's list for exported frames (ignore_list)
    // The game's picture before its UI (Scene in replay_plus_depth.h).
    bool scene_wanted{}, ui_wanted{};
    std::array<ID3D12Resource*, 4> swap{};      // the swap chain's buffers, as presented
    std::size_t swap_next{};
    ID3D12GraphicsCommandList* composite_list{}; // the list the game's last pass is in, and when
    std::uint64_t composite_epoch{};
    std::uint64_t scene_epoch{}, ui_epoch{};    // when the picture and the UI were last taken
    ComPtr<ID3D12Resource> scene_in, scene_out;
    std::uint32_t scene_width{}, scene_height{};
    DXGI_FORMAT scene_format{DXGI_FORMAT_UNKNOWN};
    std::uint64_t scene_serial{};
    bool scene_ready{};                         // scene_out holds a picture for the next frame
    bool scene_swapped{};                       // scene_out went in when the picture was last taken
    bool scene_said{};
    // The UI's alpha along the button prompts: three strips, into four readback slots in turn.
    ComPtr<ID3D12Resource> ui_readback;
    std::array<std::uint64_t, 4> ui_written{};  // the epoch each slot was written in (0: none)
    std::uint64_t ui_frame{};
    std::uint32_t ui_width{}, ui_pitch{};
    DXGI_FORMAT ui_format{DXGI_FORMAT_UNKNOWN};
    float ui_shown{-1};
    float gap_from{}, gap_to{};
    std::uint32_t ui_height{};
    bool bar_found{};
    float bar_y{};
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
// Research (`replayplus trace`): every transition of a texture at least 640 by 360, with its list and
// thread, while frames are left to trace; the back buffer named as such.
std::atomic<ID3D12Resource*> traced_back_buffer{};
std::atomic<std::uint64_t> trace_line{0};
void trace(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        const auto d = b.Transition.pResource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < 640 || d.Height < 360) continue;
        wchar_t name[64]{};
        UINT size = sizeof(name) - sizeof(wchar_t);
        std::string named;
        // WKPDID_D3DDebugObjectNameW (d3dcommon.h), here so dxguid.lib is not needed.
        constexpr GUID debug_name = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x78, 0xeb, 0x99, 0x49, 0xae, 0x11, 0x77}};
        if (SUCCEEDED(b.Transition.pResource->GetPrivateData(debug_name, &size, name)))
            for (const wchar_t* c = name; *c && named.size() < 60; ++c) named += *c < 128 ? static_cast<char>(*c) : '?';
        logging::write(logging::Level::info, logging::Channel::graphics,
            std::format("Replay Plus trace {}: thread {} list {:x} res {:x}{} {}x{} fmt {} mips {} array {} flags {:x} sub {:x}: {:x} -> {:x}{}{}",
                trace_line.fetch_add(1), GetCurrentThreadId(), reinterpret_cast<std::uintptr_t>(list),
                reinterpret_cast<std::uintptr_t>(b.Transition.pResource),
                b.Transition.pResource == traced_back_buffer.load() ? " (BACK BUFFER)" : "", d.Width, d.Height, static_cast<int>(d.Format),
                d.MipLevels, d.DepthOrArraySize, static_cast<unsigned>(d.Flags), b.Transition.Subresource,
                static_cast<unsigned>(b.Transition.StateBefore), static_cast<unsigned>(b.Transition.StateAfter),
                b.Flags ? std::format(" split {:x}", static_cast<unsigned>(b.Flags)) : std::string(), named.empty() ? "" : " \"" + named + "\""));
    }
}
bool depth_capable(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && d.SampleDesc.Count == 1 &&
           d.DepthOrArraySize == 1;
}
// The picture's shape (within a pixel of the screen's aspect), at least half its size: the main view's
// depth, at whatever resolution the game renders it (Render.ResolutionScale), not a shadow map's.
bool picture_shaped(const Shared& s, std::uint64_t width, std::uint32_t height) {
    if (!s.screen_width || !s.screen_height || !height) return false;
    const double screen = static_cast<double>(s.screen_width) / s.screen_height, aspect = static_cast<double>(width) / height;
    return std::abs(screen - aspect) < 0.02 && width * 2 >= s.screen_width && width <= std::uint64_t{s.screen_width} * 4;
}
void transition(ID3D12GraphicsCommandList* list, BarrierFn original, ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    original(list, 1, &b);
}
void copy_from(Shared& s, ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b, BarrierFn original) {
    auto* source = b.Transition.pResource;
    const auto desc = source->GetDesc();
    if (!s.texture || s.width != desc.Width || s.height != desc.Height) {
        ComPtr<ID3D12Device> device;
        if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) return;
        if (s.texture) s.retired.push_back({std::move(s.texture), s.epoch});
        D3D12_RESOURCE_DESC t{};
        t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        t.Width = desc.Width;
        t.Height = desc.Height;
        t.DepthOrArraySize = 1;
        t.MipLevels = 1;
        t.Format = DXGI_FORMAT_R32_TYPELESS;
        t.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&s.texture)))) {
            s.texture.Reset();
            return;
        }
        s.texture->SetName(L"Replay Plus depth copy");
        s.width = static_cast<std::uint32_t>(desc.Width);
        s.height = desc.Height;
        s.source_format = desc.Format;
        ++s.serial;
        logging::write(logging::Level::info, logging::Channel::graphics,
                       std::format("Replay Plus: the camera light reads the game's depth ({}x{}, format {}).", desc.Width, desc.Height,
                                   static_cast<int>(desc.Format)));
    }
    // The game's depth to a copy source and back to where it was (its own barrier, passed on after
    // this, then takes it on as it meant to); the copy to a copy target and back to a shader's.
    transition(list, original, source, b.Transition.Subresource, b.Transition.StateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, original, s.texture.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = s.texture.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = 0;
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, original, s.texture.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, original, source, b.Transition.Subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, b.Transition.StateBefore);
    s.copied_epoch = s.epoch;
    ++s.copies;
    if (source != s.depth_source) {
        if (s.depth_source_changes < 12)
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("Replay Plus: the light's depth now comes from {:x} ({}x{}, format {}; {} copies from the one before).",
                                       reinterpret_cast<std::uintptr_t>(source), desc.Width, desc.Height, static_cast<int>(desc.Format),
                                       s.depth_source_copies));
        ++s.depth_source_changes;
        s.depth_source = source;
        s.depth_source_copies = 0;
    }
    ++s.depth_source_copies;
}

// ---- The game's picture before its UI, and the UI's alpha ----------------------------------------
constexpr D3D12_RESOURCE_STATES shader_read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
bool swap_buffer(const Shared& s, const ID3D12Resource* resource) {
    for (const auto* buffer : s.swap)
        if (buffer && buffer == resource) return true;
    return false;
}
// The picture: R11G11B10 on this build (its last post pass); the UI: 8-bit with alpha (sRGB on this build).
bool picture_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R10G10B10A2_UNORM;
}
bool ui_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM;
}
// Its copy and Replay Plus's own, the picture's size and format (made anew when it changes).
bool make_scene(Shared& s, ID3D12Resource* picture, const D3D12_RESOURCE_DESC& d) {
    if (s.scene_in && s.scene_out && s.scene_width == d.Width && s.scene_height == d.Height && s.scene_format == d.Format) return true;
    ComPtr<ID3D12Device> device;
    if (FAILED(picture->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (s.scene_in) s.retired.push_back({std::move(s.scene_in), s.epoch});
    if (s.scene_out) s.retired.push_back({std::move(s.scene_out), s.epoch});
    s.scene_ready = false;
    D3D12_RESOURCE_DESC t{};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = d.Width;
    t.Height = d.Height;
    t.DepthOrArraySize = 1;
    t.MipLevels = 1;
    t.Format = d.Format;
    t.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&s.scene_in)))) {
        s.scene_in.Reset();
        return false;
    }
    t.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                               IID_PPV_ARGS(&s.scene_out)))) {
        s.scene_in.Reset();
        s.scene_out.Reset();
        return false;
    }
    s.scene_in->SetName(L"Replay Plus: the game's picture");
    s.scene_out->SetName(L"Replay Plus: the picture through its lens");
    s.scene_width = static_cast<std::uint32_t>(d.Width);
    s.scene_height = d.Height;
    s.scene_format = d.Format;
    ++s.scene_serial;
    if (!s.scene_said) {
        s.scene_said = true;
        logging::write(logging::Level::info, logging::Channel::graphics,
                       std::format("Replay Plus: it draws under the game's UI (the game's picture before it: {}x{}, format {}).", d.Width,
                                   d.Height, static_cast<int>(d.Format)));
    }
    return true;
}
// Still a render target here (its barrier to a shader resource is passed on after this).
void take_scene(Shared& s, ID3D12GraphicsCommandList* list, ID3D12Resource* picture, const D3D12_RESOURCE_DESC& d, BarrierFn original) {
    if (!make_scene(s, picture, d)) return;
    constexpr auto all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition(list, original, picture, all, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, original, s.scene_in.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(s.scene_in.Get(), picture);
    transition(list, original, s.scene_in.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s.scene_swapped = s.scene_ready;
    if (s.scene_ready) {
        transition(list, original, picture, all, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(picture, s.scene_out.Get());
        transition(list, original, picture, all, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        s.scene_ready = false;
    } else {
        transition(list, original, picture, all, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    s.scene_epoch = s.epoch;
}
constexpr std::uint32_t ui_rows = 8;
constexpr std::array<float, 3> ui_strips{0.895f, 0.924f, 0.953f}; // the replay editor's button prompts
// A band down the lower part of the UI, 48 pixels wide at a quarter of the width: the timeline's bar
// crosses it (between 10% and 90% of the width, at about 82% of the height on a 16:9 screen, lower at
// 4:3), as a run of rows of one even, see-through grey.
constexpr std::uint32_t band_width = 48, band_pitch = 256;
// Two bands, a quarter of the way in from either side: the playhead's marker can cross one of them.
constexpr std::array<float, 2> band_xs{0.26f, 0.72f};
constexpr float band_from = 0.70f, band_to = 0.96f;
std::uint32_t band_rows(std::uint32_t height) { return static_cast<std::uint32_t>(static_cast<float>(height) * (band_to - band_from)); }
std::uint64_t strips_bytes(std::uint32_t pitch) { return std::uint64_t{pitch} * ui_rows * ui_strips.size(); }
std::uint64_t band_bytes(std::uint32_t height) { return (std::uint64_t{band_rows(height)} * band_pitch + 511u) & ~std::uint64_t{511u}; }
// A readback slot: the strips, then the bands (each copy's place 512-byte aligned).
std::uint64_t ui_slot_bytes(std::uint32_t pitch, std::uint32_t height) { return strips_bytes(pitch) + band_bytes(height) * band_xs.size(); }
void take_ui(Shared& s, ID3D12GraphicsCommandList* list, ID3D12Resource* ui, const D3D12_RESOURCE_DESC& d, BarrierFn original) {
    const auto width = static_cast<std::uint32_t>(d.Width);
    const std::uint32_t pitch = (width * 4 + 255u) & ~255u;
    const std::uint64_t slot_size = ui_slot_bytes(pitch, d.Height);
    if (!s.ui_readback || s.ui_width != width || s.ui_height != d.Height || s.ui_format != d.Format) {
        ComPtr<ID3D12Device> device;
        if (FAILED(ui->GetDevice(IID_PPV_ARGS(&device)))) return;
        if (s.ui_readback) s.retired.push_back({std::move(s.ui_readback), s.epoch});
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = slot_size * 4;
        bytes.Height = 1;
        bytes.DepthOrArraySize = 1;
        bytes.MipLevels = 1;
        bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&s.ui_readback)))) {
            s.ui_readback.Reset();
            return;
        }
        s.ui_width = width;
        s.ui_height = d.Height;
        s.ui_pitch = pitch;
        s.ui_format = d.Format;
        s.ui_written = {};
    }
    const auto slot = static_cast<std::size_t>(s.ui_frame % s.ui_written.size());
    constexpr auto all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition(list, original, ui, all, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (std::size_t k = 0; k < ui_strips.size(); ++k) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = s.ui_readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = slot * slot_size + k * std::uint64_t{pitch} * ui_rows;
        to.PlacedFootprint.Footprint.Format = d.Format;
        to.PlacedFootprint.Footprint.Width = width;
        to.PlacedFootprint.Footprint.Height = ui_rows;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = pitch;
        from.pResource = ui;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT top = std::min<UINT>(d.Height - ui_rows, static_cast<UINT>(ui_strips[k] * static_cast<float>(d.Height)));
        const D3D12_BOX box{0, top, 0, width, top + ui_rows, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    for (std::size_t b = 0; b < band_xs.size(); ++b) {
        const auto rows = band_rows(d.Height);
        if (rows <= 8 || width <= band_width) break;
        const float band_x = band_xs[b];
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = s.ui_readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = slot * slot_size + strips_bytes(pitch) + band_bytes(d.Height) * b;
        to.PlacedFootprint.Footprint.Format = d.Format;
        to.PlacedFootprint.Footprint.Width = band_width;
        to.PlacedFootprint.Footprint.Height = rows;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = band_pitch;
        from.pResource = ui;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT left = std::min<UINT>(width - band_width, static_cast<UINT>(band_x * static_cast<float>(width)));
        const UINT top = static_cast<UINT>(band_from * static_cast<float>(d.Height));
        const D3D12_BOX box{left, top, 0, left + band_width, top + rows, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    transition(list, original, ui, all, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    s.ui_written[slot] = s.epoch;
    ++s.ui_frame;
    s.ui_epoch = s.epoch;
}
// Present: the alpha of the newest strips the GPU has surely written (three frames or more back).
void read_ui(Shared& s) {
    std::size_t best = s.ui_written.size();
    for (std::size_t i = 0; i < s.ui_written.size(); ++i)
        if (s.ui_written[i] && s.ui_written[i] + 3 <= s.epoch && (best == s.ui_written.size() || s.ui_written[i] > s.ui_written[best])) best = i;
    if (best == s.ui_written.size() || !s.ui_readback || s.ui_written[best] + 30 < s.epoch) {
        s.ui_shown = -1;
        return;
    }
    const std::uint64_t slot_size = ui_slot_bytes(s.ui_pitch, s.ui_height);
    const D3D12_RANGE range{static_cast<SIZE_T>(best * slot_size), static_cast<SIZE_T>((best + 1) * slot_size)};
    void* mapped{};
    if (FAILED(s.ui_readback->Map(0, &range, &mapped)) || !mapped) return;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped) + best * slot_size;
    std::uint8_t most = 0;
    for (std::size_t row = 0; row < ui_rows * ui_strips.size(); row += 2)
        for (std::uint32_t x = 0; x < s.ui_width; x += 6) most = std::max(most, bytes[row * s.ui_pitch + x * 4 + 3]);
    // The middle strip, through the prompts: its widest stretch with no UI (alpha under 8%), between the
    // screen's edges' tenths, for the hint to sit in.
    const auto* strip = bytes + std::size_t{1} * s.ui_pitch * ui_rows;
    const std::uint32_t step = 4, first = s.ui_width / 10, last = s.ui_width - s.ui_width / 10;
    std::uint32_t best_from = 0, best_to = 0, run_from = first;
    bool in_run = false;
    for (std::uint32_t x = first; x <= last; x += step) {
        std::uint8_t column = 0;
        if (x < last)
            for (std::uint32_t row = 0; row < ui_rows; ++row) column = std::max(column, strip[row * s.ui_pitch + x * 4 + 3]);
        const bool empty = x < last && column < 20;
        if (empty && !in_run) {
            run_from = x;
            in_run = true;
        } else if (!empty && in_run) {
            in_run = false;
            if (x - run_from > best_to - best_from) best_from = run_from, best_to = x;
        }
    }
    // The band: the highest run of rows of one even grey (most of a row within 20 of its middle value,
    // between 8% and 92% opaque, the rows within 25 of the run's first) as tall as the bar is (1% to 3.2%
    // of the height): the timeline's bar, above the prompts' boxes.
    bool bar_found = false;
    float bar_y = 0;
    for (std::size_t b = 0; b < band_xs.size() && !bar_found; ++b) {
        const auto rows = band_rows(s.ui_height);
        if (rows <= 8) break;
        const auto* band = bytes + strips_bytes(s.ui_pitch) + band_bytes(s.ui_height) * b;
        const auto shortest = static_cast<std::uint32_t>(0.010f * static_cast<float>(s.ui_height));
        const auto tallest = static_cast<std::uint32_t>(0.032f * static_cast<float>(s.ui_height)) + 1;
        std::uint32_t band_from_row = 0, run = 0;
        int band_level = -1;
        const auto finish = [&](std::uint32_t end) {
            if (!bar_found && run >= shortest && run <= tallest) {
                bar_found = true;
                bar_y = (band_from * static_cast<float>(s.ui_height) + static_cast<float>(band_from_row) + static_cast<float>(end - band_from_row) * 0.5f) /
                        static_cast<float>(s.ui_height);
            }
            run = 0;
            band_level = -1;
        };
        for (std::uint32_t row = 0; row < rows && !bar_found; ++row) {
            std::array<std::uint8_t, band_width> alpha{};
            for (std::uint32_t x = 0; x < band_width; ++x) alpha[x] = band[std::size_t{row} * band_pitch + x * 4 + 3];
            auto sorted = alpha;
            std::nth_element(sorted.begin(), sorted.begin() + band_width / 2, sorted.end());
            const int middle = sorted[band_width / 2];
            unsigned even = 0;
            for (const auto a : alpha) even += std::abs(static_cast<int>(a) - middle) <= 20 ? 1u : 0u;
            const bool grey = middle >= 20 && middle <= 235 && even * 5 >= band_width * 4;
            if (grey && (band_level < 0 || std::abs(middle - band_level) <= 25)) {
                if (band_level < 0) {
                    band_from_row = row;
                    band_level = middle;
                }
                ++run;
            } else {
                finish(row);
                if (grey) {
                    band_from_row = row;
                    band_level = middle;
                    run = 1;
                }
            }
        }
        finish(rows);
    }
    const D3D12_RANGE none{};
    s.ui_readback->Unmap(0, &none);
    s.bar_found = bar_found;
    if (bar_found) s.bar_y = bar_y;
    s.ui_shown = static_cast<float>(most) / 255.0f;
    s.gap_from = static_cast<float>(best_from) / static_cast<float>(s.ui_width);
    s.gap_to = static_cast<float>(best_to) / static_cast<float>(s.ui_width);
}
// Where the game's last pass begins (its list starts with a swap chain buffer going to a render
// target): its picture, and its UI, as each goes from a render target to being read.
void frame_taps(Shared& s, ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers, BarrierFn original) {
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource || (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY))
            continue;
        auto* resource = b.Transition.pResource;
        if (swap_buffer(s, resource)) {
            if (b.Transition.StateBefore == D3D12_RESOURCE_STATE_PRESENT && (b.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                s.composite_list = list;
                s.composite_epoch = s.epoch;
            }
            continue;
        }
        if (list != s.composite_list || s.composite_epoch != s.epoch) continue;
        if (!(b.Transition.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) || !(b.Transition.StateAfter & shader_read)) continue;
        const auto d = resource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1)
            continue;
        // The UI is drawn at the screen's size; the picture at whatever size the game renders it
        // (Render.ResolutionScale: Sharp middle's 200% is twice the screen's), always the screen's shape.
        const bool screen_size = d.Width == s.screen_width && d.Height == s.screen_height;
        if (picture_format(d.Format)) {
            if ((screen_size || picture_shaped(s, d.Width, d.Height)) && s.scene_wanted && s.scene_epoch != s.epoch)
                take_scene(s, list, resource, d, original);
        } else if (ui_format(d.Format)) {
            if (screen_size && s.ui_wanted && s.ui_epoch != s.epoch) take_ui(s, list, resource, d, original);
        }
    }
}
// ---- Exports: the depth of each video frame's own frame ----------------------------------------------
// The rows of a video frame kept to know it again: six, evenly down it; each 512-byte aligned in the readback.
constexpr std::uint32_t print_rows = 6;
std::uint32_t print_stride(std::uint32_t width) { return (width * 4u + 511u) & ~511u; }
std::uint32_t print_row(std::uint32_t height, std::uint32_t i) { return height * (2u * i + 1u) / (2u * print_rows); }
void retire_kept(Shared& s, Shared::Kept& k) {
    if (k.texture) s.retired.push_back({std::move(k.texture), s.epoch});
    if (k.print) {
        k.print->Unmap(0, nullptr);
        s.retired.push_back({std::move(k.print), s.epoch});
    }
    k.print_cpu = nullptr;
    k.print_width = k.print_height = 0;
}
void keep_for_export(Shared& s, ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b, BarrierFn original) {
    // A new export (no video frame for a second or so): nothing kept is for it.
    if (s.kept_epoch + 60 < s.epoch) {
        s.kept_popped = s.kept_pushed;
        s.kept_taken = s.kept_dropped = s.kept_skipped = s.kept_unfound = s.kept_exact = 0;
        s.kept_fallback_said = s.kept_late_said = s.kept_drop_said = s.kept_unfound_said = false;
        s.kept_inexact_said = 0;
    }
    s.kept_epoch = s.epoch;
    auto& k = s.kept[s.kept_pushed % s.kept.size()];
    if (!k.texture || k.width != s.width || k.height != s.height) {
        ComPtr<ID3D12Device> device;
        if (FAILED(s.texture->GetDevice(IID_PPV_ARGS(&device)))) return;
        if (k.texture) s.retired.push_back({std::move(k.texture), s.epoch});
        auto t = s.texture->GetDesc();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&k.texture)))) {
            k.texture.Reset();
            return;
        }
        k.texture->SetName(L"Replay Plus depth kept for an exported frame");
        k.width = s.width;
        k.height = s.height;
    }
    auto* video = b.Transition.pResource;
    const auto vd = video->GetDesc();
    const auto vw = static_cast<std::uint32_t>(vd.Width), vh = vd.Height;
    if (!k.print || k.print_width != vw || k.print_height != vh) {
        ComPtr<ID3D12Device> device;
        if (FAILED(video->GetDevice(IID_PPV_ARGS(&device)))) return;
        if (k.print) {
            k.print->Unmap(0, nullptr);
            s.retired.push_back({std::move(k.print), s.epoch});
        }
        k.print_cpu = nullptr;
        D3D12_RESOURCE_DESC t{};
        t.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        t.Width = std::uint64_t{print_stride(vw)} * print_rows;
        t.Height = 1;
        t.DepthOrArraySize = 1;
        t.MipLevels = 1;
        t.SampleDesc.Count = 1;
        t.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        void* mapped{};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&k.print))) ||
            FAILED(k.print->Map(0, nullptr, &mapped)) || !mapped) {
            k.print.Reset();
            return;
        }
        k.print->SetName(L"Replay Plus rows of an exported frame");
        k.print_cpu = static_cast<const std::uint8_t*>(mapped);
        k.print_width = vw;
        k.print_height = vh;
    }
    if (s.kept_pushed - s.kept_popped >= s.kept.size()) {
        // Full: the oldest was never taken (the encoder fell behind, or frames were drawn and never encoded).
        ++s.kept_popped;
        ++s.kept_dropped;
        if (!s.kept_drop_said) {
            s.kept_drop_said = true;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("Replay Plus export: the depth kept for a video frame was let go unused ({} kept, {} taken).", s.kept_pushed,
                                       s.kept_taken));
        }
    }
    // In the game's list, before its own barrier: the frame's depth copy (in a shader's state between
    // copies) into the slot.
    transition(list, original, s.texture.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, original, k.texture.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(k.texture.Get(), s.texture.Get());
    transition(list, original, k.texture.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, original, s.texture.Get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // The video frame's rows, as the game has finished drawing it (its own barrier, passed on after this,
    // takes it on to be copied out for the encoder).
    transition(list, original, video, b.Transition.Subresource, b.Transition.StateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (std::uint32_t i = 0; i < print_rows; ++i) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = k.print.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = std::uint64_t{print_stride(vw)} * i;
        to.PlacedFootprint.Footprint = {vd.Format, vw, 1, 1, print_stride(vw)};
        from.pResource = video;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        const auto row = print_row(vh, i);
        const D3D12_BOX box{0, row, 0, vw, row + 1, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    }
    transition(list, original, video, b.Transition.Subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, b.Transition.StateBefore);
    k.epoch = s.epoch;
    k.own_frame = s.copied_epoch == s.epoch;
    ++s.kept_pushed;
}
// The game's video frame as it goes from being drawn to being copied out for its encoder (a trace of an
// export, 2026-11-17: B8G8R8A8 at the video's size, RENDER_TARGET -> shader read -> COPY_SOURCE, each frame).
void export_taps(Shared& s, ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers, BarrierFn original) {
    const auto video = replay_shake::export_frames::size();
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource || (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY))
            continue;
        if (b.Transition.StateAfter != D3D12_RESOURCE_STATE_COPY_SOURCE || b.Transition.StateBefore == D3D12_RESOURCE_STATE_COPY_SOURCE) continue;
        const auto d = b.Transition.pResource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.DepthOrArraySize != 1 || d.Width < 640)
            continue;
        const bool video_size = video.width > 0 ? d.Width == static_cast<UINT64>(video.width) && d.Height == static_cast<UINT>(video.height)
                                                : picture_shaped(s, d.Width, d.Height);
        if (!video_size || swap_buffer(s, b.Transition.pResource) || d.MipLevels != 1) continue;
        keep_for_export(s, list, b, original);
        return;
    }
}
} // namespace

void barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers, BarrierFn original) noexcept {
    try {
        auto& s = shared();
        if (!list || !barriers || !original) return;
        if (link::trace_frames().load(std::memory_order_relaxed) > 0) trace(list, count, barriers);
        std::lock_guard lock(s.mutex);
        if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        if (s.scene_wanted || s.ui_wanted) frame_taps(s, list, count, barriers, original);
        if (s.wanted && s.texture && list != s.own_list) export_taps(s, list, count, barriers, original);
        if (!s.wanted || s.copied_epoch == s.epoch) return;
        constexpr auto read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
            if (!(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) || !(b.Transition.StateAfter & read)) continue;
            if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) continue;
            if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0) continue;
            const auto desc = b.Transition.pResource->GetDesc();
            if (!depth_capable(desc) || !picture_shaped(s, desc.Width, desc.Height)) continue;
            if (desc.Width > s.largest || s.largest_epoch + 120 < s.epoch) s.largest = desc.Width;
            if (desc.Width < s.largest) continue;
            s.largest_epoch = s.epoch;
            if (desc.Format != DXGI_FORMAT_R32_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT && desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS &&
                desc.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                continue;
            copy_from(s, list, b, original);
            return;
        }
    } catch (...) {}
}

void note_back_buffer(ID3D12Resource* buffer) noexcept {
    traced_back_buffer.store(buffer);
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!buffer || swap_buffer(s, buffer)) return;
    s.swap[s.swap_next] = buffer;
    s.swap_next = (s.swap_next + 1) % s.swap.size();
}
void begin_frame(bool wanted, bool scene_wanted, bool ui_wanted, std::uint32_t screen_width, std::uint32_t screen_height) noexcept {
    if (const int left = link::trace_frames().load(); left > 0) {
        logging::write(logging::Level::info, logging::Channel::graphics,
            std::format("Replay Plus trace: ---- present ({} frame(s) left) back buffer {:x} ----", left - 1,
                reinterpret_cast<std::uintptr_t>(traced_back_buffer.load())));
        link::trace_frames().store(left - 1);
    }
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.wanted = wanted;
    s.screen_width = screen_width;
    s.screen_height = screen_height;
    // The picture copied in the frame just recorded is this present's to draw from.
    s.scene_wanted = scene_wanted;
    if (!scene_wanted) s.scene_ready = false;
    s.ui_wanted = ui_wanted;
    if (ui_wanted) read_ui(s);
    else s.ui_shown = -1;
    ++s.epoch;
    // Textures replaced a few frames ago are done with on the GPU.
    std::erase_if(s.retired, [&](const Retired& r) { return s.epoch > r.epoch + 6; });
    // What was kept for an export goes a few seconds after its last frame.
    if (s.kept_epoch && s.kept_epoch + 300 < s.epoch) {
        for (auto& k : s.kept) retire_kept(s, k);
        s.kept_popped = s.kept_pushed;
        s.kept_epoch = 0;
    }
    if (!wanted && s.texture && s.copied_epoch + 600 < s.epoch) {
        // Ten seconds or so without the light: the copy goes too (kept a few frames for the GPU).
        s.retired.push_back({std::move(s.texture), s.epoch});
        s.width = s.height = 0;
        ++s.serial;
    }
}

Scene scene() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    Scene c;
    c.input = s.scene_in.Get();
    c.output = s.scene_out.Get();
    c.width = s.scene_width;
    c.height = s.scene_height;
    c.format = s.scene_format;
    c.serial = s.scene_serial;
    // begin_frame has moved the epoch on: copied in the one before is this frame's.
    c.fresh = s.scene_in && s.scene_epoch + 1 == s.epoch;
    c.swapped = c.fresh && s.scene_swapped && s.scene_out;
    return c;
}
void scene_drawn(bool drawn) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.scene_ready = drawn && s.scene_out;
}
float ui_shown() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.ui_shown;
}
Gap ui_gap() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return {s.gap_from, s.gap_to};
}
Bar timeline_bar() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return {s.bar_found, s.bar_y};
}

Copy current() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    Copy c;
    c.texture = s.texture.Get();
    c.width = s.width;
    c.height = s.height;
    c.serial = s.serial;
    // The copy is this frame's only when the game's depth went by this frame.
    c.fresh = s.texture && s.copied_epoch + 2 >= s.epoch;
    c.copies = s.copies;
    return c;
}

void ignore_list(ID3D12GraphicsCommandList* list) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.own_list = list;
}

Copy for_export(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, bool* matched) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (matched) *matched = false;
    Copy c;
    if (s.kept_popped < s.kept_pushed) {
        // Which kept frame this is, by its rows: the colour's differences summed, the oldest of the closest;
        // taken only when clearly the closest (frames drawn and never encoded, before it, are passed over); else
        // in order. (The encoder's frame is rarely the drawn one to the byte: about 5% of its colour bytes differ,
        // by up to 80, the next frame's some 5 to 25 times as much; its alpha never does. 2026-11-17)
        std::uint64_t found = s.kept_pushed;
        std::uint64_t best_sad = ~std::uint64_t{0}, next_sad = ~std::uint64_t{0};
        std::uint32_t best_alpha = 0, best_bytes = 0;
        int best_max = 0;
        if (pixels && width && height && pitch >= width * 4u) {
            for (std::uint64_t j = s.kept_popped; j < s.kept_pushed; ++j) {
                const auto& k = s.kept[j % s.kept.size()];
                if (!k.texture || !k.print_cpu || k.print_width != width || k.print_height != height) continue;
                std::uint64_t sad = 0;
                std::uint32_t alpha = 0, bytes = 0;
                int most = 0;
                for (std::uint32_t i = 0; i < print_rows; ++i) {
                    const auto* a = k.print_cpu + std::size_t{print_stride(width)} * i;
                    const auto* p = pixels + std::size_t{print_row(height, i)} * pitch;
                    for (std::uint32_t x = 0; x < width * 4u; x += 4) {
                        for (std::uint32_t ch = 0; ch < 3; ++ch) {
                            const int d = std::abs(static_cast<int>(a[x + ch]) - static_cast<int>(p[x + ch]));
                            sad += static_cast<std::uint64_t>(d);
                            bytes += d != 0;
                            most = std::max(most, d);
                        }
                        alpha += a[x + 3] != p[x + 3];
                    }
                }
                if (sad < best_sad) {
                    next_sad = best_sad;
                    best_sad = sad;
                    best_alpha = alpha;
                    best_bytes = bytes;
                    best_max = most;
                    found = j;
                } else if (sad < next_sad) {
                    next_sad = sad;
                }
            }
            // Clearly the closest: none of the others within two and a half times its difference (when there are others).
            if (found != s.kept_pushed && best_sad != 0 && next_sad != ~std::uint64_t{0} && best_sad * 5 > next_sad * 2) found = s.kept_pushed;
        }
        const bool by_pixels = found != s.kept_pushed;
        if (by_pixels && best_sad == 0) ++s.kept_exact;
        if (by_pixels && best_sad != 0 && s.kept_inexact_said < 1) {
            ++s.kept_inexact_said;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("Replay Plus export: frame {} found by its pixels, not exactly: {} of {} colour bytes differ (by up to {}, {:.3f} "
                                       "a byte), {} of {} alphas; the next closest {:.3f} a byte.",
                                       s.kept_taken + 1, best_bytes, print_rows * width * 3u, best_max,
                                       static_cast<double>(best_sad) / (print_rows * width * 3.0), best_alpha, print_rows * width,
                                       next_sad == ~std::uint64_t{0} ? -1.0 : static_cast<double>(next_sad) / (print_rows * width * 3.0)));
        }
        if (!by_pixels) {
            found = s.kept_popped;
            ++s.kept_unfound;
            if (!s.kept_unfound_said) {
                s.kept_unfound_said = true;
                logging::write(logging::Level::info, logging::Channel::graphics,
                               std::format("Replay Plus export: frame {} was not found by its pixels among the {} kept (closest {:.3f} a byte, next "
                                           "{:.3f}); it is lit from the oldest.",
                                           s.kept_taken + 1, s.kept_pushed - s.kept_popped,
                                           best_sad == ~std::uint64_t{0} ? -1.0 : static_cast<double>(best_sad) / (print_rows * width * 3.0),
                                           next_sad == ~std::uint64_t{0} ? -1.0 : static_cast<double>(next_sad) / (print_rows * width * 3.0)));
            }
        }
        const std::uint64_t skipped = found - s.kept_popped;
        s.kept_skipped += skipped;
        const auto& k = s.kept[found % s.kept.size()];
        s.kept_popped = found + 1;
        if (k.texture) {
            ++s.kept_taken;
            c.texture = k.texture.Get();
            c.width = k.width;
            c.height = k.height;
            c.serial = s.serial;
            c.fresh = true;
            c.copies = s.copies;
            if (matched) *matched = true;
            if (s.kept_taken <= 2 || s.kept_taken % 600 == 0 || skipped)
                logging::write(logging::Level::info, logging::Channel::graphics,
                               std::format("Replay Plus export: frame {} is lit from the depth of the frame it was drawn in ({}{} frame(s) back, {} passed "
                                           "over, {} more waiting; {} passed over, {} exact, {} not found by pixels, {} let go in all).",
                                           s.kept_taken, by_pixels ? "found by its pixels, " : "in order, ", s.epoch - k.epoch, skipped,
                                           s.kept_pushed - s.kept_popped, s.kept_skipped, s.kept_exact, s.kept_unfound, s.kept_dropped));
            if (!k.own_frame && !s.kept_late_said) {
                s.kept_late_said = true;
                logging::write(logging::Level::info, logging::Channel::graphics,
                               "Replay Plus export: a video frame was finished before the game's depth went by in its frame; it is lit from the frame before.");
            }
            return c;
        }
    }
    if (!s.kept_fallback_said) {
        s.kept_fallback_said = true;
        logging::write(logging::Level::info, logging::Channel::graphics,
                       "Replay Plus export: no depth was kept for a video frame; it is lit from the latest.");
    }
    c.texture = s.texture.Get();
    c.width = s.width;
    c.height = s.height;
    c.serial = s.serial;
    c.fresh = s.texture && s.copied_epoch + 2 >= s.epoch;
    c.copies = s.copies;
    return c;
}

void trim() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    const auto retire = [&](ComPtr<ID3D12Resource>& texture) {
        if (texture) s.retired.push_back({std::move(texture), s.epoch});
    };
    if (s.texture) {
        retire(s.texture);
        s.width = s.height = 0;
        ++s.serial;
    }
    if (s.scene_in || s.scene_out) {
        retire(s.scene_in);
        retire(s.scene_out);
        s.scene_ready = false;
        s.scene_width = s.scene_height = 0;
        ++s.scene_serial;
    }
    if (s.ui_readback) {
        retire(s.ui_readback);
        s.ui_written = {};
        s.ui_shown = -1;
    }
    for (auto& k : s.kept) retire_kept(s, k);
    s.kept_popped = s.kept_pushed;
    s.kept_epoch = 0;
}
bool holding() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    bool kept = false;
    for (const auto& k : s.kept) kept = kept || k.texture;
    return s.texture || s.scene_in || s.scene_out || s.ui_readback || kept || !s.retired.empty();
}
void release() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.texture.Reset();
    s.retired.clear();
    s.width = s.height = 0;
    ++s.serial;
    s.scene_in.Reset();
    s.scene_out.Reset();
    s.scene_ready = false;
    s.scene_width = s.scene_height = 0;
    ++s.scene_serial;
    s.ui_readback.Reset();
    s.ui_written = {};
    s.ui_shown = -1;
    s.swap = {};
    s.composite_list = nullptr;
    for (auto& k : s.kept) {
        k.texture.Reset();
        if (k.print) k.print->Unmap(0, nullptr);
        k.print.Reset();
        k.print_cpu = nullptr;
        k.print_width = k.print_height = 0;
    }
    s.kept_popped = s.kept_pushed;
    s.kept_epoch = 0;
}
} // namespace dingosdk::replay_plus::depth
