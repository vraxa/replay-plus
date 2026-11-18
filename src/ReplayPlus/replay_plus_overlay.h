#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <cstdint>

// Replay Plus in ReSkate's overlay (Extension/UI/Overlay/overlay_render.cpp), on the render thread:
// the picture's pass (the lens, the grade, the bars and the hood, drawn over the game's finished frame
// ahead of ReSkate's own drawing) and the replay editor's panel (drawn with ReSkate's ImGui, in the
// game's look). What to draw comes from the game thread (replay_plus_link.h).
namespace dingosdk::replay_plus::overlay {
// This frame wants the overlay: the pass has something to draw, the panel is open, or a capture is
// asked for. Cheap: once a frame.
bool pending() noexcept;
struct Target {
    ID3D12Device* device{};
    ID3D12GraphicsCommandList* commands{};
    ID3D12Resource* buffer{};      // the back buffer, in the PRESENT state; left in it
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    DXGI_FORMAT format{};
    int transfer{};                // the overlay's output transfer: 0 SDR, 1 scRGB, 2 HDR10
    float white_nits{203};         // the overlay's paper white in HDR
    // Waits until the GPU has finished every frame submitted so far. False when it could not.
    bool (*wait_idle)(){};
    // The queue the game presents with (exported frames are finished on it: finish_export).
    ID3D12CommandQueue* queue{};
};
// Records the pass on the frame's command list, ahead of ImGui's draws. Sets its own descriptor heap
// (the overlay sets its own again before it draws). False when nothing was drawn.
bool record(const Target& target) noexcept;
// After ImGui has drawn on the back buffer (it is in the RENDER_TARGET state, and left in it): a
// capture of the whole screen, ReSkate's drawing included, when one is asked for.
void after_ui(ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer) noexcept;
// The overlay is tearing its graphics down (a resize, a lost device, stop).
void release() noexcept;
// An exported video frame (8-bit, blue first, rows `pitch` bytes apart), finished in place on the GPU with the
// pass the screen has (the lens, the grade, the bars, the hood and the camera light, from the game's depth):
// uploaded, drawn on the game's queue, read back, waited for. Any thread (the encoder's). False when it could
// not (nothing drawn on the screen yet, no queue): the frame is then as it was, for the CPU to finish.
bool finish_export(std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) noexcept;
// Inside the overlay's ImGui frame: the replay editor's panel, while it is open.
void draw_panel() noexcept;
} // namespace dingosdk::replay_plus::overlay
