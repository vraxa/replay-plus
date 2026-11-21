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

// The game's depth for the autofocus. ReSkate hooks ID3D12GraphicsCommandList::ResourceBarrier once
// (Extension/UI/Overlay/depth_tap.cpp, its depth tap for screen captures) and hands Replay Plus every
// barrier the game records, before passing it on. While the autofocus wants it, the game's main depth (the
// first depth target each frame that goes from being written to being read, a picture's shape: as the
// tap takes it) is copied into a texture of Replay Plus's own, on the game's own command list, where
// the depth is finished; the autofocus reads it later on the same queue. (The camera light drawn over the
// picture read it too, and each exported frame's own, until 1.3.2: the light is a real light of the game's now,
// replay_plus_light.h.)
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
// The game's video frames going to be copied out for its encoder, whatever their size (a trace of an export,
// 2026-11-17: B8G8R8A8 at the video's size, RENDER_TARGET -> shader read -> COPY_SOURCE, each frame): their size is
// what an exported frame is finished at (link::export_frame_size), MP4 or WebM, light or none.
void note_video_frames(const Shared& s, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    for (UINT i = 0; i < count; ++i) {
        const auto& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource || (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY))
            continue;
        if (b.Transition.StateAfter != D3D12_RESOURCE_STATE_COPY_SOURCE || b.Transition.StateBefore == D3D12_RESOURCE_STATE_COPY_SOURCE) continue;
        const auto d = b.Transition.pResource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.DepthOrArraySize != 1 || d.MipLevels != 1 ||
            d.SampleDesc.Count != 1 || d.Width < 64 || d.Height < 64 || d.Width > 16384 || d.Height > 16384)
            continue;
        if (swap_buffer(s, b.Transition.pResource)) continue;
        link::note_video_frame(static_cast<std::uint32_t>(d.Width), d.Height);
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
        if (list != s.own_list) note_video_frames(s, count, barriers);
        if (s.scene_wanted || s.ui_wanted) frame_taps(s, list, count, barriers, original);
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
    if (!wanted && s.texture && s.copied_epoch + 600 < s.epoch) {
        // Ten seconds or so without the autofocus: the copy goes too (kept a few frames for the GPU).
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
}
bool holding() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.texture || s.scene_in || s.scene_out || s.ui_readback || !s.retired.empty();
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
}
} // namespace dingosdk::replay_plus::depth
