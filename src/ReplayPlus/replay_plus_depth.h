#pragma once
#include <d3d12.h>
#include <cstdint>

// The game's depth, copied for Replay Plus's autofocus (replay_plus_depth.cpp).
namespace dingosdk::replay_plus::depth {
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
// From ReSkate's ResourceBarrier hook (Extension/UI/Overlay/depth_tap.cpp), on whichever thread the game
// records on, before the barriers are passed on: the frame's main depth is copied while wanted.
void barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers, BarrierFn original) noexcept;
// Render thread, once a presented frame: whether the autofocus wants the depth, and the screen's size
// (the main depth has its shape).
// Present, each frame Replay Plus draws: whether the depth is wanted (the autofocus), the
// game's picture before its UI (the pass draws under the game's UI), and how much of that UI shows.
void begin_frame(bool depth_wanted, bool scene_wanted, bool ui_wanted, std::uint32_t screen_width, std::uint32_t screen_height) noexcept;
struct Copy {
    ID3D12Resource* texture{}; // R32_TYPELESS, read as R32_FLOAT; held by the depth module
    std::uint32_t width{}, height{};
    std::uint64_t serial{};    // changes when the texture is made anew (its view must be too)
    bool fresh{};              // the game's depth went by this frame
    std::uint64_t copies{};
};
Copy current() noexcept;
// Replay Plus's own list for exported frames: its barriers are not the game's (its finished frame is the
// video's format and size too, and names cannot be read back from resources in the game).
void ignore_list(ID3D12GraphicsCommandList* list) noexcept;

// ---- The game's picture before its UI ----------------------------------------------------------
// The game finishes its 3D picture in a texture of its own, draws its UI into another, and lays the UI
// over the picture in a last pass onto the back buffer (a trace of its frame, 2026-11-16: in one list,
// a swap chain buffer goes from PRESENT to RENDER_TARGET, then the picture and the UI from
// RENDER_TARGET to shader resources). Where that pass begins, on the game's own list and with copies
// alone (nothing of the game's drawing state is touched), the picture is copied out (`input`) and what
// Replay Plus drew from the frame before (`output`) is put in its place: the game then lays its UI over
// the lens, the grade and the light instead of the lens bending its UI. The picture runs a frame behind
// the UI.
struct Scene {
    ID3D12Resource* input{};   // this frame's picture (PIXEL_SHADER_RESOURCE)
    ID3D12Resource* output{};  // where the next frame's is drawn (a render target, COPY_SOURCE between frames)
    std::uint32_t width{}, height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint64_t serial{};    // changes when they are made anew
    bool fresh{};              // copied in the frame just recorded
    bool swapped{};            // and `output` went in for it: that frame's screen shows `output` through the game's last pass
};
Scene scene() noexcept;
// Present: whether Replay Plus drew `output` this frame (it goes in for the next frame's picture).
void scene_drawn(bool drawn) noexcept;
// How much the game's UI shows along its button prompts at the foot of the screen (0..1: its alpha
// there, a few frames back), or -1 while it is not known.
float ui_shown() noexcept;
// Along the middle of that row of prompts: the widest stretch with none of the game's UI in it (from
// and to, as shares of the width), or none (both 0).
struct Gap {
    float from{}, to{};
};
Gap ui_gap() noexcept;
// The replay editor's timeline bar in the game's UI (its translucent band, above the prompts): its middle,
// as a share of the height. Not found: `found` false (the UI hidden, or another screen).
struct Bar {
    bool found{};
    float y{};
};
Bar timeline_bar() noexcept;
void release() noexcept;
// Out of the replay editor: every texture kept here (the depth's copy, the game's picture's two, the UI's
// readback) goes, retired and freed a few frames on as begin_frame moves on, so none of their video memory
// stays taken while skating. And whether any is still held (begin_frame goes on being called until none is).
void trim() noexcept;
bool holding() noexcept;
// The back buffer of the frame being drawn (the swap chain's buffers are told apart by it).
void note_back_buffer(ID3D12Resource* buffer) noexcept;
} // namespace dingosdk::replay_plus::depth
