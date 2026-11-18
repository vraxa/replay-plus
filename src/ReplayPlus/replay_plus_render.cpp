#include "replay_plus_overlay.h"
#include "replay_plus_depth.h"
#include "replay_plus_hlsl.h" // generated from replay_plus.hlsl (ReplayPlus.cmake)
#include "replay_plus_lens.h"
#include "replay_plus_link.h"
#include "Engine/Core/Log/logging.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace dingosdk::replay_plus::overlay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DXGI_FORMAT linear_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr unsigned most_mips = 10;
constexpr unsigned constant_count = 20 * 4;
// The pass's constants go in a small upload buffer, a slot a frame in turn (a frame on the GPU still reads
// its own): too many now for root constants (64 values for the whole root signature).
constexpr unsigned constant_slot_bytes = 512, constant_slots = 16;
static_assert(constant_count * 4 <= constant_slot_bytes);
// The shader-visible heap: the frame's copy, the linear chain (all mips), the depth, the hood, then
// each mip on its own (the mip passes read the one above).
// The game's picture before its UI has a slot of its own (it is drawn into from there when it can be).
constexpr unsigned slot_copy = 0, slot_chain = 1, slot_depth = 2, slot_hood = 3, slot_scene = 4, slot_mips = 5, slot_count = slot_mips + most_mips;
// The render target views: each mip of the chain, then the picture Replay Plus draws for the game.
constexpr unsigned target_scene = most_mips, target_count = most_mips + 1;

using link::now_ms;

struct Graphics {
    // record (the presenting thread) and finish_export (the encoder's) share what is here.
    std::mutex lock;
    ComPtr<ID3D12CommandQueue> queue; // the game's, as the overlay last gave it
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> decode, down, main;
    DXGI_FORMAT main_format{DXGI_FORMAT_UNKNOWN};
    // The last pass again, drawing into the game's picture (its format), and the shaders it is made from.
    ComPtr<ID3D12PipelineState> scene_main;
    DXGI_FORMAT scene_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3DBlob> vs_blob, main_blob;
    std::uint64_t scene_serial{~std::uint64_t{0}};
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ID3D12Device* device{};
    // The constants' upload buffer (mapped for good) and the slot the next frame takes.
    ComPtr<ID3D12Resource> constants;
    std::uint8_t* constants_cpu{};
    unsigned constants_next{};
    // The frame's copy (the back buffer's format) and the linear chain.
    ComPtr<ID3D12Resource> copy, chain;
    D3D12_RESOURCE_STATES copy_state{D3D12_RESOURCE_STATE_COPY_DEST};
    std::uint32_t width{}, height{};           // the chain's: the size the pass works at
    std::uint32_t copy_width{}, copy_height{}; // the frame's copy's: the back buffer's
    DXGI_FORMAT copy_format{DXGI_FORMAT_UNKNOWN};
    unsigned mips{};
    // The hood's picture on the GPU, and what is uploading it.
    ComPtr<ID3D12Resource> hood, hood_upload;
    std::uint64_t hood_serial{~std::uint64_t{0}};
    bool hood_ready{};
    // The view of the game's depth (the camera light), for the depth copy it was made for.
    std::uint64_t depth_serial{~std::uint64_t{0}};
    // Autofocus: the middle of the depth copy, read back a few frames later (four slots in turn).
    ComPtr<ID3D12Resource> focus_readback;
    std::uint64_t focus_frame{};
    std::array<bool, 4> focus_written{};
    // Captures: a readback buffer of the back buffer, read once the GPU is done with it.
    ComPtr<ID3D12Resource> shot;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shot_layout{};
    std::uint32_t shot_rows{};
    std::string shot_path, shot_after_ui_path;
    bool shot_pending{};
    DXGI_FORMAT shot_format{DXGI_FORMAT_UNKNOWN};
    bool failed{};
    std::uint64_t frames{};
    std::string problem;
};
Graphics& graphics() {
    static auto* value = new Graphics;
    return *value;
}

void say_failed(const std::string& why) {
    auto& g = graphics();
    if (g.failed) return;
    g.failed = true;
    g.problem = why;
    logging::write(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the picture's pass is unavailable: " + why);
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
                UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_view(unsigned slot) {
    auto& g = graphics();
    auto handle = g.views->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * slot;
    return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpu_view(unsigned slot) {
    auto& g = graphics();
    auto handle = g.views->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * slot;
    return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE target_view(unsigned mip) {
    auto& g = graphics();
    auto handle = g.targets->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) * mip;
    return handle;
}
// A view that reads nothing (zeros): every table the shader declares is always bound.
void null_view(unsigned slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    graphics().device->CreateShaderResourceView(nullptr, &view, cpu_view(slot));
}

bool build_root(ID3D12Device* device) {
    auto& g = graphics();
    if (g.root) return true;
    std::array<D3D12_DESCRIPTOR_RANGE, 4> ranges{};
    for (UINT i = 0; i < ranges.size(); ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    std::array<D3D12_ROOT_PARAMETER, 5> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (UINT i = 0; i < samplers.size(); ++i) {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob) {
        say_failed("its root signature could not be built.");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        say_failed("its root signature was refused.");
        return false;
    }
    return true;
}

bool compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> error;
    const auto length = std::strlen(replay_plus_hlsl);
    if (FAILED(D3DCompile(replay_plus_hlsl, length, "replay_plus", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out,
                          &error)) ||
        !out) {
        say_failed(std::string("its shader ") + entry + " did not compile" +
                   (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    return true;
}

bool build_pipelines(ID3D12Device* device, DXGI_FORMAT format) {
    auto& g = graphics();
    if (g.decode && g.down && g.main && g.main_format == format) return true;
    if (!build_root(device)) return false;
    ComPtr<ID3DBlob> vs, decode, down, main;
    if (!compile("vs_main", "vs_5_0", vs) || !compile("ps_decode", "ps_5_0", decode) || !compile("ps_down", "ps_5_0", down) ||
        !compile("ps_main", "ps_5_0", main))
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.SampleDesc.Count = 1;
    const auto make = [&](ID3DBlob* pixel, DXGI_FORMAT target, ComPtr<ID3D12PipelineState>& out) {
        description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        description.RTVFormats[0] = target;
        return SUCCEEDED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&out)));
    };
    if (!make(decode.Get(), linear_format, g.decode) || !make(down.Get(), linear_format, g.down) || !make(main.Get(), format, g.main)) {
        say_failed(std::format("its pipelines were refused (the screen's format is {}).", static_cast<int>(format)));
        return false;
    }
    g.main_format = format;
    g.vs_blob = vs;
    g.main_blob = main;
    return true;
}
// The last pass again, into a target of another format (the game's picture before its UI, an exported frame).
bool make_main_pipeline(ID3D12Device* device, DXGI_FORMAT format, ComPtr<ID3D12PipelineState>& out) {
    auto& g = graphics();
    if (!g.vs_blob || !g.main_blob || !g.root) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {g.vs_blob->GetBufferPointer(), g.vs_blob->GetBufferSize()};
    description.PS = {g.main_blob->GetBufferPointer(), g.main_blob->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    out.Reset();
    return SUCCEEDED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&out)));
}
// The last pass, drawing into the game's picture before its UI (its own format). False: it cannot here.
bool build_scene_pipeline(ID3D12Device* device, DXGI_FORMAT format) {
    auto& g = graphics();
    if (g.scene_main && g.scene_format == format) return true;
    if (!g.vs_blob || !g.main_blob) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {g.vs_blob->GetBufferPointer(), g.vs_blob->GetBufferSize()};
    description.PS = {g.main_blob->GetBufferPointer(), g.main_blob->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    g.scene_main.Reset();
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.scene_main)))) return false;
    g.scene_format = format;
    return true;
}

bool ensure_heaps(ID3D12Device* device) {
    auto& g = graphics();
    if (g.views && g.device == device) return true;
    g.device = device;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = slot_count;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.views)))) {
        say_failed("its descriptor heap could not be made.");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = target_count;
    if (FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g.targets)))) {
        say_failed("its render target heap could not be made.");
        return false;
    }
    for (unsigned slot = 0; slot < slot_count; ++slot) null_view(slot);
    return true;
}

bool ensure_constants(ID3D12Device* device) {
    auto& g = graphics();
    if (g.constants && g.constants_cpu) return true;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bytes{};
    bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bytes.Width = std::uint64_t{constant_slot_bytes} * constant_slots;
    bytes.Height = 1;
    bytes.DepthOrArraySize = 1;
    bytes.MipLevels = 1;
    bytes.SampleDesc.Count = 1;
    bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&g.constants)))) {
        say_failed("its constants buffer could not be made.");
        return false;
    }
    const D3D12_RANGE none{};
    void* mapped{};
    if (FAILED(g.constants->Map(0, &none, &mapped)) || !mapped) {
        g.constants.Reset();
        say_failed("its constants buffer could not be mapped.");
        return false;
    }
    g.constants->SetName(L"Replay Plus constants");
    g.constants_cpu = static_cast<std::uint8_t*>(mapped);
    return true;
}

// The frame's copy (the back buffer's size) and the linear chain, the size the pass works at: the back
// buffer's over the screen, the game's picture's under its UI (larger when the game renders finer than the
// screen, Render.ResolutionScale). Made again when either changes.
bool ensure_targets(const Target& t, std::uint32_t width, std::uint32_t height) {
    auto& g = graphics();
    const auto desc = t.buffer->GetDesc();
    if (g.copy && g.chain && g.width == width && g.height == height && g.copy_format == desc.Format && g.copy_width == desc.Width &&
        g.copy_height == desc.Height)
        return true;
    // Frames still on the GPU may read what is replaced.
    if ((g.copy || g.chain) && (!t.wait_idle || !t.wait_idle())) return false;
    g.copy.Reset();
    g.chain.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto copy = desc;
    copy.Flags = D3D12_RESOURCE_FLAG_NONE;
    copy.Alignment = 0;
    copy.MipLevels = 1;
    copy.DepthOrArraySize = 1;
    copy.SampleDesc = {1, 0};
    copy.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.copy)))) {
        say_failed("the copy of the frame could not be made.");
        return false;
    }
    g.copy->SetName(L"Replay Plus frame copy");
    g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
    unsigned mips = 1;
    for (std::uint32_t side = std::max(width, height); side > 16 && mips < most_mips; side /= 2) ++mips;
    D3D12_RESOURCE_DESC chain{};
    chain.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    chain.Width = width;
    chain.Height = height;
    chain.DepthOrArraySize = 1;
    chain.MipLevels = static_cast<UINT16>(mips);
    chain.Format = linear_format;
    chain.SampleDesc.Count = 1;
    chain.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = linear_format;
    if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &chain, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                 IID_PPV_ARGS(&g.chain)))) {
        g.copy.Reset();
        say_failed("the linear copy of the frame could not be made.");
        return false;
    }
    g.chain->SetName(L"Replay Plus linear chain");
    g.mips = mips;
    // Their views.
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    t.device->CreateShaderResourceView(g.copy.Get(), &view, cpu_view(slot_copy));
    view.Format = linear_format;
    view.Texture2D.MipLevels = mips;
    t.device->CreateShaderResourceView(g.chain.Get(), &view, cpu_view(slot_chain));
    for (unsigned mip = 0; mip < most_mips; ++mip) {
        if (mip >= mips) {
            null_view(slot_mips + mip);
            continue;
        }
        view.Texture2D.MostDetailedMip = mip;
        view.Texture2D.MipLevels = 1;
        t.device->CreateShaderResourceView(g.chain.Get(), &view, cpu_view(slot_mips + mip));
        D3D12_RENDER_TARGET_VIEW_DESC target{};
        target.Format = linear_format;
        target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        target.Texture2D.MipSlice = mip;
        t.device->CreateRenderTargetView(g.chain.Get(), &target, target_view(mip));
    }
    g.width = width;
    g.height = height;
    g.copy_width = static_cast<std::uint32_t>(desc.Width);
    g.copy_height = desc.Height;
    g.copy_format = desc.Format;
    return true;
}

// The hood's picture, uploaded once each time it changes (on this frame's list).
void ensure_hood(const Target& t, const std::shared_ptr<const link::HoodImage>& image) {
    auto& g = graphics();
    const std::uint64_t serial = image ? image->serial : 0;
    if (serial == g.hood_serial) return;
    // The last upload, and the picture it replaces, may still be on the GPU.
    if ((g.hood || g.hood_upload) && (!t.wait_idle || !t.wait_idle())) return;
    g.hood.Reset();
    g.hood_upload.Reset();
    g.hood_ready = false;
    g.hood_serial = serial;
    null_view(slot_hood);
    if (!image || !image->width || !image->height || image->rgba.size() < std::size_t{image->width} * image->height * 4) return;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = image->width;
    desc.Height = image->height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.hood))))
        return;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows{};
    UINT64 row_bytes{}, total{};
    t.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &row_bytes, &total);
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped{};
    if (FAILED(t.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&g.hood_upload))) ||
        FAILED(g.hood_upload->Map(0, nullptr, &mapped)) || !mapped) {
        g.hood.Reset();
        g.hood_upload.Reset();
        return;
    }
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(static_cast<std::uint8_t*>(mapped) + layout.Offset + std::size_t{y} * layout.Footprint.RowPitch,
                    image->rgba.data() + std::size_t{y} * image->width * 4, std::size_t{image->width} * 4);
    g.hood_upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = g.hood.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = g.hood_upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = layout;
    t.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(t.commands, g.hood.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    t.device->CreateShaderResourceView(g.hood.Get(), &view, cpu_view(slot_hood));
    g.hood_ready = true;
}

// ---- Captures --------------------------------------------------------------------------------
bool ensure_shot_buffer(ID3D12Device* device, ID3D12Resource* buffer) {
    auto& g = graphics();
    const auto desc = buffer->GetDesc();
    UINT rows{};
    UINT64 row_bytes{}, total{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &row_bytes, &total);
    if (g.shot && g.shot_layout.Footprint.Width == layout.Footprint.Width && g.shot_layout.Footprint.Height == layout.Footprint.Height &&
        g.shot_format == desc.Format)
        return true;
    g.shot.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bytes{};
    bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bytes.Width = total;
    bytes.Height = 1;
    bytes.DepthOrArraySize = 1;
    bytes.MipLevels = 1;
    bytes.SampleDesc.Count = 1;
    bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.shot))))
        return false;
    g.shot_layout = layout;
    g.shot_rows = rows;
    g.shot_format = desc.Format;
    return true;
}
// The back buffer (in COPY_SOURCE) into the readback buffer; written out once the GPU is done.
void copy_shot(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, const std::string& path) {
    auto& g = graphics();
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = g.shot.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = g.shot_layout;
    from.pResource = buffer;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    g.shot_path = path;
    g.shot_pending = true;
}
// One pixel of the back buffer's formats as 8-bit sRGB-ish blue, green, red (HDR is clipped).
std::array<std::uint8_t, 3> pixel_bgr(const std::uint8_t* p, DXGI_FORMAT format) {
    const auto to8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L)); };
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t v{};
        std::memcpy(&v, p, 4);
        return {to8(static_cast<float>((v >> 20) & 1023) / 1023.0f), to8(static_cast<float>((v >> 10) & 1023) / 1023.0f),
                to8(static_cast<float>(v & 1023) / 1023.0f)};
    }
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return {p[0], p[1], p[2]};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<std::uint16_t, 3> h{};
        std::memcpy(h.data(), p, 6);
        const auto half = [](std::uint16_t x) {
            const std::uint32_t sign = (x >> 15) & 1, exponent = (x >> 10) & 31, mantissa = x & 1023;
            float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                        : exponent == 31 ? 65504.0f
                                         : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
            return sign ? -value : value;
        };
        return {to8(lens::linear_to_srgb(half(h[2]))), to8(lens::linear_to_srgb(half(h[1]))), to8(lens::linear_to_srgb(half(h[0])))};
    }
    default: return {p[2], p[1], p[0]};
    }
}
void write_shot(const Target* t) {
    auto& g = graphics();
    if (!g.shot_pending || !g.shot) return;
    if (t && t->wait_idle && !t->wait_idle()) return;
    g.shot_pending = false;
    void* mapped{};
    const D3D12_RANGE all{0, static_cast<SIZE_T>(g.shot_layout.Footprint.RowPitch) * g.shot_rows};
    if (FAILED(g.shot->Map(0, &all, &mapped)) || !mapped) return;
    const std::uint32_t w = g.shot_layout.Footprint.Width, h = g.shot_rows;
    const std::uint32_t row = (w * 3 + 3) & ~3u;
    std::vector<std::uint8_t> file(54 + std::size_t{row} * h, 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(file.data() + at, &v, 4); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<std::uint32_t>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    file[26] = 1;
    file[28] = 24;
    put32(34, row * h);
    const std::size_t step = g.shot_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* in = static_cast<const std::uint8_t*>(mapped) + g.shot_layout.Offset + std::size_t{y} * g.shot_layout.Footprint.RowPitch;
        auto* out = file.data() + 54 + std::size_t{h - 1 - y} * row;
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto bgr = pixel_bgr(in + x * step, g.shot_format);
            out[x * 3] = bgr[0];
            out[x * 3 + 1] = bgr[1];
            out[x * 3 + 2] = bgr[2];
        }
    }
    const D3D12_RANGE none{};
    g.shot->Unmap(0, &none);
    try {
        const std::filesystem::path path(g.shot_path); // (shots are named in plain ASCII)
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        logging::write(logging::Level::info, logging::Channel::graphics,
                       std::format("Replay Plus: saved {} ({}x{}, format {}).", g.shot_path, w, h, static_cast<int>(g.shot_format)));
    } catch (...) {}
}

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

// Autofocus: a 16 by 16 block from the middle of the game's depth copied into one of four slots each
// frame, and the slot copied three frames ago read (the GPU is done with it by then): the median of
// its distances, as a camcorder's autofocus takes the middle of its picture.
constexpr std::uint32_t focus_side = 16, focus_pitch = 256, focus_slot = 4096;
void read_focus(const Target& t, const link::Picture& picture) {
    auto& g = graphics();
    const auto copy = depth::current();
    if (!copy.texture || !copy.fresh || copy.width < 32 || copy.height < 32) return;
    if (!g.focus_readback) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bytes{};
        bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bytes.Width = focus_slot * 4;
        bytes.Height = 1;
        bytes.DepthOrArraySize = 1;
        bytes.MipLevels = 1;
        bytes.SampleDesc.Count = 1;
        bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g.focus_readback))))
            return;
        g.focus_written = {};
    }
    const auto slot = static_cast<std::size_t>(g.focus_frame % 4);
    // The slot three frames old: its median distance.
    const auto old = static_cast<std::size_t>((g.focus_frame + 1) % 4);
    if (g.focus_written[old]) {
        void* mapped{};
        const D3D12_RANGE range{old * focus_slot, old * focus_slot + focus_pitch * focus_side};
        if (SUCCEEDED(g.focus_readback->Map(0, &range, &mapped)) && mapped) {
            std::array<float, focus_side * focus_side> distances{};
            std::size_t n = 0;
            const float near_plane = std::max(picture.near_plane, 0.001f), far_plane = std::max(picture.far_plane, 10.0f);
            for (std::uint32_t y = 0; y < focus_side; ++y)
                for (std::uint32_t x = 0; x < focus_side; ++x) {
                    float d{};
                    std::memcpy(&d, static_cast<const std::uint8_t*>(mapped) + old * focus_slot + y * focus_pitch + x * 4, 4);
                    if (!(d > 0.0f) || d > 1.0f) continue; // the sky, or nothing
                    distances[n++] = near_plane * far_plane / (d * (far_plane - near_plane) + near_plane);
                }
            const D3D12_RANGE none{};
            g.focus_readback->Unmap(0, &none);
            if (n >= 8) {
                std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(n / 2), distances.begin() + static_cast<std::ptrdiff_t>(n));
                link::note_focus_depth(distances[n / 2]);
            }
        }
    }
    // This frame's block, from the middle of the copy.
    transition(t.commands, copy.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = g.focus_readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Offset = slot * focus_slot;
    to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS;
    to.PlacedFootprint.Footprint.Width = focus_side;
    to.PlacedFootprint.Footprint.Height = focus_side;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = focus_pitch;
    from.pResource = copy.texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{copy.width / 2 - focus_side / 2, copy.height / 2 - focus_side / 2, 0, copy.width / 2 + focus_side / 2,
                        copy.height / 2 + focus_side / 2, 1};
    t.commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    transition(t.commands, copy.texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g.focus_written[slot] = true;
    ++g.focus_frame;
}
// The field of view the game built this picture with, while it is being told (`fresh_ms`); else the one
// Replay Plus sets (fixed, Auto), else the editor's usual.
float fov_for(const link::Picture& picture, std::uint64_t fresh_ms) {
    float fov = link::fov();
    const auto& s = picture.settings;
    if (!(fov > 0.02f) || now_ms() - link::fov_at() > fresh_ms)
        fov = (s.fov_mode == FovMode::fixed ? s.fov : s.fov_mode == FovMode::automatic ? picture.fov_auto : 65.0f) * lens::pi / 180.0f;
    return fov;
}
// The pass's constants (replay_plus.hlsl's cbuffer), for a picture the pass works on at width x height.
struct PassInputs {
    float width{}, height{};
    int transfer{};
    float white{1};
    unsigned mips{1};
    bool hood_ready{};
    depth::Copy depth_copy;
    bool depth_ready{};
    std::uint64_t frame{};
};
std::array<std::uint32_t, constant_count> pass_constants(const link::Picture& picture, const lens::Finish& finish, const PassInputs& in) {
    const auto& s = picture.settings;
    const auto& f = finish.frame;
    const auto win = lens::window(f);
    const auto tg = lens::tangents(f);
    const float half = s.bars == Bars::scope ? std::min(1.0f, f.view_aspect / lens::scope_aspect) : 1.0f;
    const bool lens_on = f.fisheye;
    const bool textured = s.hood == Hood::picture && in.hood_ready && s.hood_opacity > 0;
    const float width = in.width, height = in.height;
    const auto& depth_copy = in.depth_copy;
    const bool depth_ready = in.depth_ready;
    const auto& gr = finish.grade;
    const auto wb = lens::filter_gains(gr); // white balance and the colour filter
    const auto cm = lens::colour_matrix(gr);
    const auto low = lens::split_offset(gr.split_shadows_hue, gr.split_shadows), high = lens::split_offset(gr.split_highlights_hue, gr.split_highlights);
    const bool passthrough = !lens_on && finish.grade.identity() && s.hood != Hood::circle && !depth_ready;
    const auto& glass = finish.glass;
    // The light: its beam's half angle and the bright core inside it, its colour (a cold LED to a warm
    // bulb, kept as bright), and where the game's depth puts things.
    const float half_beam = std::clamp(s.light_cone, 10.0f, 180.0f) * 0.5f * lens::pi / 180.0f;
    const float cos_outer = s.light_cone >= 179.5f ? -0.01f : std::cos(half_beam);
    const float cos_inner = std::max(cos_outer + 0.002f, std::cos(half_beam * (1.0f - 0.9f * std::clamp(s.light_soft, 0.0f, 1.0f))));
    const float w = std::clamp(s.light_warmth, -1.0f, 1.0f);
    std::array<float, 3> colour = w >= 0 ? std::array<float, 3>{1.0f, 1.0f - 0.18f * w, 1.0f - 0.42f * w}
                                         : std::array<float, 3>{1.0f + 0.12f * w, 1.0f + 0.05f * w, 1.0f};
    const float colour_y = 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2];
    for (auto& v : colour) v /= colour_y;
    return {
        bits(lens_on ? 2.0f : 0.0f), bits(finish.fit), bits(tg[0]), bits(tg[1]),
        bits(f.view_aspect), bits(win[0]), bits(win[1]), bits(half),
        bits(width), bits(height), bits(static_cast<float>(in.transfer)), bits(in.white),
        bits(f.hood_circle), bits(s.hood == Hood::circle ? std::clamp(s.hood_opacity, 0.0f, 1.0f) : 0.0f),
        bits(textured ? std::clamp(s.hood_opacity, 0.0f, 1.0f) : 0.0f), bits(static_cast<float>(in.mips)),
        bits(std::exp2(s.exposure)), bits(s.contrast), bits(s.saturation), bits(s.black),
        bits(wb[0]), bits(wb[1]), bits(wb[2]), bits(s.vignette),
        bits(passthrough ? 1.0f : 0.0f), bits(f.curve), bits(f.scale), bits(glass.fringe),
        bits(depth_ready ? s.light_power : 0.0f), bits(std::max(s.light_range, 0.5f)), bits(cos_outer), bits(cos_inner),
        bits(colour[0]), bits(colour[1]), bits(colour[2]), bits(std::max(picture.near_plane, 0.001f)),
        bits(std::max(picture.far_plane, 10.0f)), bits(depth_ready ? 1.0f : 0.0f),
        bits(depth_copy.width ? 1.0f / static_cast<float>(depth_copy.width) : 0.0f),
        bits(depth_copy.height ? 1.0f / static_cast<float>(depth_copy.height) : 0.0f),
        bits(glass.shade), bits(glass.soft), bits(f.squeeze), bits(lens_on ? f.ease : 0.0f),
        bits(gr.camera ? 1.0f : 0.0f), bits(lens_on ? glass.glare : 0.0f), bits(std::max(f.hood_scale, 1e-3f)),
        bits(lens::camera_toe_slope(gr.camera_gamma)),
        bits(cm[0]), bits(cm[1]), bits(cm[2]), bits(gr.lift),
        bits(cm[3]), bits(cm[4]), bits(cm[5]), bits(gr.gamma),
        bits(cm[6]), bits(cm[7]), bits(cm[8]), bits(gr.gain),
        bits(low[0]), bits(low[1]), bits(low[2]), bits(std::clamp(gr.split_balance, -1.0f, 1.0f)),
        bits(high[0]), bits(high[1]), bits(high[2]), bits(std::clamp(gr.grain, 0.0f, 1.0f)),
        bits(std::max(gr.grain_size, 0.5f) * height / 1080.0f), bits(static_cast<float>(in.frame % 1024)),
        bits(lens::vignette_power(gr.vignette_soft)), bits(std::clamp(gr.vignette_round, 0.0f, 1.0f)),
        bits(gr.camera_gamma), bits(gr.camera_knee), bits(gr.camera_knee_slope), bits(gr.camera_black),
        bits(gr.camera_clip), bits(gr.camera_colour), bits(0.0f), bits(0.0f)};
}


// ---- Exported frames, on the GPU ------------------------------------------------------------------
// The game films the replay into a frame of its own (8-bit, blue first, the video's size), copies it back to
// the CPU and hands it to its encoder, apart from what it presents (a trace of an export, 2026-11-17): the
// pass under the UI never reaches it. So each frame the encoder is given is uploaded, drawn through the same
// passes as the screen (the camera light from the game's depth as it is now: the frame is a frame or two
// old, as near as the light can be), read back into it, on the game's own queue, and waited for. About 10
// ms a 1920x1440 frame; finishing it on the CPU took 0.4 s and could not light it.
struct Exporter {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 value{};
    ComPtr<ID3D12DescriptorHeap> views, targets; // its own: the screen's may be on the GPU meanwhile
    ComPtr<ID3D12PipelineState> main;            // the last pass, into the frame's format
    ComPtr<ID3D12Resource> constants, upload, readback, source, chain, output;
    std::uint8_t *constants_cpu{}, *upload_cpu{}, *readback_cpu{};
    std::uint32_t width{}, height{}, row{};
    unsigned mips{};
    std::uint64_t frames{};
    bool failed{}, said{};
};
Exporter& exporter() {
    static auto* value = new Exporter;
    return *value;
}
constexpr DXGI_FORMAT export_format = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr unsigned export_slot_source = 0, export_slot_chain = 1, export_slot_depth = 2, export_slot_hood = 3, export_slot_mips = 4,
                   export_slot_count = export_slot_mips + most_mips;
constexpr unsigned export_target_output = most_mips, export_target_count = most_mips + 1;
D3D12_CPU_DESCRIPTOR_HANDLE export_cpu(Exporter& e, unsigned slot) {
    auto h = e.views->GetCPUDescriptorHandleForHeapStart();
    h.ptr += std::size_t{slot} * e.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE export_gpu(Exporter& e, unsigned slot) {
    auto h = e.views->GetGPUDescriptorHandleForHeapStart();
    h.ptr += std::uint64_t{slot} * e.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE export_rtv(Exporter& e, unsigned slot) {
    auto h = e.targets->GetCPUDescriptorHandleForHeapStart();
    h.ptr += std::size_t{slot} * e.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}
void export_null_view(Exporter& e, unsigned slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    e.device->CreateShaderResourceView(nullptr, &view, export_cpu(e, slot));
}
ComPtr<ID3D12Resource> export_buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, std::uint64_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC bytes{};
    bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bytes.Width = size;
    bytes.Height = 1;
    bytes.DepthOrArraySize = 1;
    bytes.MipLevels = 1;
    bytes.SampleDesc.Count = 1;
    bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, state, nullptr, IID_PPV_ARGS(&out)))) return nullptr;
    return out;
}
ComPtr<ID3D12Resource> export_texture(ID3D12Device* device, std::uint32_t w, std::uint32_t h, DXGI_FORMAT format, unsigned mips,
                                      D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mips);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    ComPtr<ID3D12Resource> out;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                               (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clear : nullptr, IID_PPV_ARGS(&out))))
        return nullptr;
    return out;
}
// Once: its list, fence, heaps, constants and last pass (needs the screen's pipelines, built by then).
bool export_setup(Exporter& e, ID3D12Device* device) {
    if (e.device.Get() == device && e.list && e.main) return true;
    e = Exporter{};
    e.device = device;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, e.allocator.Get(), nullptr, IID_PPV_ARGS(&e.list))) ||
        FAILED(e.list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&e.fence))))
        return false;
    // Its finished frame goes to be copied out as the game's video frame does: the depth module's watch for
    // the game's passes over this list.
    depth::ignore_list(e.list.Get());
    e.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = export_slot_count;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = export_target_count;
    if (!e.event || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&e.views))) || FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&e.targets))))
        return false;
    for (unsigned slot = 0; slot < export_slot_count; ++slot) export_null_view(e, slot);
    e.constants = export_buffer(device, D3D12_HEAP_TYPE_UPLOAD, constant_slot_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{};
    const D3D12_RANGE none{};
    if (!e.constants || FAILED(e.constants->Map(0, &none, &mapped)) || !mapped) return false;
    e.constants_cpu = static_cast<std::uint8_t*>(mapped);
    return make_main_pipeline(device, export_format, e.main);
}
// Its textures, for frames of this size.
bool export_targets(Exporter& e, std::uint32_t w, std::uint32_t h) {
    if (e.source && e.width == w && e.height == h) return true;
    e.upload.Reset();
    e.readback.Reset();
    e.source.Reset();
    e.chain.Reset();
    e.output.Reset();
    e.upload_cpu = e.readback_cpu = nullptr;
    e.width = e.height = 0;
    const std::uint32_t row = (w * 4 + 255u) & ~255u;
    unsigned mips = 1;
    for (std::uint32_t side = std::max(w, h); side > 16 && mips < most_mips; side /= 2) ++mips;
    auto* device = e.device.Get();
    e.upload = export_buffer(device, D3D12_HEAP_TYPE_UPLOAD, std::uint64_t{row} * h, D3D12_RESOURCE_STATE_GENERIC_READ);
    e.readback = export_buffer(device, D3D12_HEAP_TYPE_READBACK, std::uint64_t{row} * h, D3D12_RESOURCE_STATE_COPY_DEST);
    e.source = export_texture(device, w, h, export_format, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    e.chain = export_texture(device, w, h, linear_format, mips, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    e.output = export_texture(device, w, h, export_format, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!e.upload || !e.readback || !e.source || !e.chain || !e.output) return false;
    // Named, for graphics debuggers.
    e.source->SetName(L"Replay Plus export: the game's frame");
    e.chain->SetName(L"Replay Plus export: its linear chain");
    e.output->SetName(L"Replay Plus export: the finished frame");
    void* up{};
    void* back{};
    const D3D12_RANGE none{};
    if (FAILED(e.upload->Map(0, &none, &up)) || FAILED(e.readback->Map(0, nullptr, &back)) || !up || !back) return false;
    e.upload_cpu = static_cast<std::uint8_t*>(up);
    e.readback_cpu = static_cast<std::uint8_t*>(back);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = export_format;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(e.source.Get(), &view, export_cpu(e, export_slot_source));
    view.Format = linear_format;
    view.Texture2D.MipLevels = mips;
    device->CreateShaderResourceView(e.chain.Get(), &view, export_cpu(e, export_slot_chain));
    for (unsigned mip = 0; mip < most_mips; ++mip) {
        if (mip >= mips) {
            export_null_view(e, export_slot_mips + mip);
            continue;
        }
        view.Texture2D.MostDetailedMip = mip;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(e.chain.Get(), &view, export_cpu(e, export_slot_mips + mip));
        D3D12_RENDER_TARGET_VIEW_DESC target{};
        target.Format = linear_format;
        target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        target.Texture2D.MipSlice = mip;
        device->CreateRenderTargetView(e.chain.Get(), &target, export_rtv(e, mip));
    }
    D3D12_RENDER_TARGET_VIEW_DESC out{};
    out.Format = export_format;
    out.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(e.output.Get(), &out, export_rtv(e, export_target_output));
    e.width = w;
    e.height = h;
    e.row = row;
    e.mips = mips;
    return true;
}
void export_release() {
    auto& e = exporter();
    if (e.event) CloseHandle(e.event);
    e = Exporter{};
    depth::ignore_list(nullptr);
}
} // namespace

bool pending() noexcept {
    try {
        auto& g = graphics();
        if (g.shot_pending || !g.shot_after_ui_path.empty() || link::shot_asked()) return true;
        if (link::panel_open().load(std::memory_order_acquire) || link::panel_requested()) return true;
        // Out of the replay editor, until the textures the pass took are gone (record lets them go).
        if (g.copy || g.chain || depth::holding()) return true;
        const auto picture = link::picture();
        return picture && (picture->on || picture->autofocus || picture->in_replay) && !g.failed;
    } catch (...) { return false; }
}

bool record(const Target& t) noexcept {
    try {
        std::lock_guard lock(graphics().lock);
        if (t.queue) graphics().queue = t.queue;
        auto& g = graphics();
        // A capture asked for last frame is written once the GPU has finished it.
        if (g.shot_pending) write_shot(&t);
        std::string shot_path;
        bool shot_clean = false;
        if (link::take_shot(shot_path)) {
            // "...|clean": before ReSkate's drawing (straight after this pass); otherwise after it.
            if (shot_path.size() > 6 && shot_path.ends_with("|clean")) {
                shot_path.resize(shot_path.size() - 6);
                shot_clean = true;
            } else {
                g.shot_after_ui_path = shot_path;
            }
        }
        const auto picture = link::picture();
        const bool drawing = picture && picture->on && !g.failed;
        const bool autofocus = picture && picture->autofocus && !g.failed;
        if (!t.device || !t.commands || !t.buffer) return false;
        const auto desc = t.buffer->GetDesc();
        if (desc.Width < 16 || desc.Height < 16) return false;
        depth::note_back_buffer(t.buffer);
        {
            const auto luid = t.device->GetAdapterLuid();
            link::note_adapter((std::uint64_t{static_cast<std::uint32_t>(luid.HighPart)} << 32) | luid.LowPart);
        }
        // The replay editor's UI is watched (how much of it shows) for Replay Plus's button hint.
        const bool ui_wanted = picture && picture->in_replay;
        if (!drawing) {
            // Autofocus alone: the game's depth is read, the picture left as it is.
            depth::begin_frame(autofocus, false, ui_wanted, static_cast<std::uint32_t>(desc.Width), desc.Height);
            if (autofocus) read_focus(t, *picture);
        }
        if (!drawing) {
            link::note_frame_sizes(static_cast<std::uint32_t>(desc.Width), desc.Height, 0, 0);
            link::note_drawn_under_ui(false);
            if (shot_clean && ensure_shot_buffer(t.device, t.buffer)) {
                transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
                copy_shot(t.commands, t.buffer, shot_path);
                transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            }
            // Out of the replay editor: the textures the pass and the game's picture took go (at the editor's
            // render scale they come to a few hundred MB of video memory, which a big map needs back). The
            // chain and the frame's copy once the GPU is done with the frames that read them; the depth
            // module's retired, freed a few frames on.
            if (!picture->in_replay && !autofocus) {
                depth::trim();
                if ((g.copy || g.chain) && t.wait_idle && t.wait_idle()) {
                    g.copy.Reset();
                    g.chain.Reset();
                    g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
                    g.width = g.height = g.copy_width = g.copy_height = 0;
                    g.copy_format = DXGI_FORMAT_UNKNOWN;
                    g.mips = 0;
                    g.scene_serial = ~std::uint64_t{0};
                    logging::write(logging::Level::info, logging::Channel::graphics,
                                   "Replay Plus: out of the replay editor, its textures are let go.");
                }
            }
            return false;
        }
        const auto& s = picture->settings;
        const bool light_on = s.light && s.light_power > 0;
        // Under the game's UI whenever its picture before the UI is found, a video being filmed too: the game
        // films its frame after its last pass, so the video gets the pass as the screen does, the camera light
        // and Sharp middle's finer picture with it, at the GPU's speed. (Finishing exported frames on the CPU,
        // replay_plus_lens.cpp, took about 0.4 s a 1920x1440 frame and could not light them: it is only for
        // frames the pass did not draw under the UI.)
        const bool under_ui = true;
        depth::begin_frame(light_on || autofocus, under_ui, ui_wanted, static_cast<std::uint32_t>(desc.Width), desc.Height);
        if (autofocus) read_focus(t, *picture);
        // Under the game's UI: the game's picture before its UI, copied this frame (replay_plus_depth.h Scene),
        // drawn through the passes into Replay Plus's own, which goes in for the game's next frame; at its own
        // size (larger than the screen when the game renders finer: the game's last pass scales it down with
        // the UI over it). Its values are linear as they are (as scRGB's, with 1 as white). Otherwise over
        // the screen, as before.
        const auto scene = under_ui ? depth::scene() : depth::Scene{};
        const bool scene_shape = scene.width && scene.height &&
                                 std::abs(static_cast<double>(scene.width) / scene.height - static_cast<double>(desc.Width) / desc.Height) < 0.01;
        bool into_scene = scene.input && scene.output && scene.fresh && scene_shape && scene.width >= desc.Width / 2 && scene.width <= desc.Width * 4 &&
                          build_scene_pipeline(t.device, scene.format);
        const std::uint32_t work_w = into_scene ? scene.width : static_cast<std::uint32_t>(desc.Width);
        const std::uint32_t work_h = into_scene ? scene.height : desc.Height;
        // For the video memory guard: the screen, and what the game rendered its picture at.
        link::note_frame_sizes(static_cast<std::uint32_t>(desc.Width), desc.Height, scene.fresh && scene_shape ? scene.width : 0,
                               scene.fresh && scene_shape ? scene.height : 0);
        if (!ensure_heaps(t.device) || !ensure_constants(t.device) || !build_pipelines(t.device, t.format) || !ensure_targets(t, work_w, work_h))
            return false;
        // The overlay's picture: on the GPU, and its see-through middle, which the lens is fitted to.
        const auto hood_image = link::hood();
        ensure_hood(t, hood_image);
        // The depth's view, whenever the depth module made its copy anew.
        const auto depth_copy = depth::current();
        if (depth_copy.serial != g.depth_serial) {
            g.depth_serial = depth_copy.serial;
            if (depth_copy.texture) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R32_FLOAT;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                t.device->CreateShaderResourceView(depth_copy.texture, &view, cpu_view(slot_depth));
            } else {
                null_view(slot_depth);
            }
        }
        const bool depth_ready = light_on && depth_copy.texture && depth_copy.fresh && depth_copy.width && depth_copy.height;

        // The pass works at the chain's size (the game's picture's, under its UI); the picture's shape is the screen's.
        const float width = static_cast<float>(work_w), height = static_cast<float>(work_h);
        const float aspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
        const auto finish = lens::finish_for(s, aspect, fov_for(*picture, 1000), hood_image ? &hood_image->hole : nullptr);
        const bool lens_on = finish.frame.fisheye;
        const float white = t.transfer == 1 ? t.white_nits / 80.0f : t.transfer == 2 ? t.white_nits : 1.0f;
        const auto constants = pass_constants(*picture, finish, {width, height, t.transfer, white, g.mips, g.hood_ready, depth_copy, depth_ready, g.frames});

        if (into_scene && scene.serial != g.scene_serial) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = scene.format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            t.device->CreateShaderResourceView(scene.input, &view, cpu_view(slot_scene));
            D3D12_RENDER_TARGET_VIEW_DESC target{};
            target.Format = scene.format;
            target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            t.device->CreateRenderTargetView(scene.output, &target, target_view(target_scene));
            g.scene_serial = scene.serial;
        }
        auto drawn = constants;
        const unsigned source_slot = into_scene ? slot_scene : slot_copy;
        if (into_scene) {
            drawn[10] = bits(1.0f); // transfer: linear, as it is
            drawn[11] = bits(1.0f); // white
        } else {
            // Copy the frame, then draw it back through the passes.
            transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(t.commands, g.copy.Get(), g.copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
            t.commands->CopyResource(g.copy.Get(), t.buffer);
            transition(t.commands, g.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g.copy_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        ID3D12DescriptorHeap* heaps[] = {g.views.Get()};
        t.commands->SetDescriptorHeaps(1, heaps);
        t.commands->SetGraphicsRootSignature(g.root.Get());
        {
            const unsigned slot = g.constants_next++ % constant_slots;
            std::memcpy(g.constants_cpu + std::size_t{slot} * constant_slot_bytes, drawn.data(), sizeof(std::uint32_t) * constant_count);
            t.commands->SetGraphicsRootConstantBufferView(0, g.constants->GetGPUVirtualAddress() + std::uint64_t{slot} * constant_slot_bytes);
        }
        t.commands->SetGraphicsRootDescriptorTable(1, gpu_view(source_slot));
        t.commands->SetGraphicsRootDescriptorTable(2, gpu_view(slot_chain));
        t.commands->SetGraphicsRootDescriptorTable(3, gpu_view(slot_depth));
        t.commands->SetGraphicsRootDescriptorTable(4, gpu_view(slot_hood));
        t.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto viewport = [&](std::uint32_t w, std::uint32_t h) {
            const D3D12_VIEWPORT v{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
            const D3D12_RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
            t.commands->RSSetViewports(1, &v);
            t.commands->RSSetScissorRects(1, &r);
        };
        // 1. The frame in linear light (the chain starts in RENDER_TARGET).
        const auto mip0 = target_view(0);
        t.commands->OMSetRenderTargets(1, &mip0, FALSE, nullptr);
        viewport(g.width, g.height);
        t.commands->SetPipelineState(g.decode.Get());
        t.commands->DrawInstanced(3, 1, 0, 0);
        // 2. Its mips, each from the one above, only where the fisheye squeezes the picture.
        if (lens_on) {
            t.commands->SetPipelineState(g.down.Get());
            for (unsigned mip = 1; mip < g.mips; ++mip) {
                transition(t.commands, g.chain.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, mip - 1);
                const auto target = target_view(mip);
                t.commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
                t.commands->SetGraphicsRootDescriptorTable(1, gpu_view(slot_mips + mip - 1));
                viewport(std::max(1u, g.width >> mip), std::max(1u, g.height >> mip));
                t.commands->DrawInstanced(3, 1, 0, 0);
            }
            transition(t.commands, g.chain.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, g.mips - 1);
        } else {
            transition(t.commands, g.chain.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0);
        }
        // 3. Lens, grade, bars and hood: into the game's picture (for its next frame), or onto the screen.
        if (into_scene) {
            transition(t.commands, scene.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto target = target_view(target_scene);
            t.commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
            t.commands->SetPipelineState(g.scene_main.Get());
        } else {
            t.commands->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
            t.commands->SetPipelineState(g.main.Get());
        }
        viewport(work_w, work_h);
        t.commands->SetGraphicsRootDescriptorTable(1, gpu_view(source_slot));
        t.commands->DrawInstanced(3, 1, 0, 0);
        if (into_scene) transition(t.commands, scene.output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        depth::scene_drawn(into_scene);
        link::note_drawn_under_ui(into_scene);
        // The chain goes back to RENDER_TARGET for the next frame's first pass.
        const unsigned used = lens_on ? g.mips : 1;
        for (unsigned mip = 0; mip < used; ++mip)
            transition(t.commands, g.chain.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, mip);
        // The back buffer: left as the game made it when the pass went under its UI.
        const auto buffer_state = into_scene ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (shot_clean && ensure_shot_buffer(t.device, t.buffer)) {
            transition(t.commands, t.buffer, buffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy_shot(t.commands, t.buffer, shot_path);
            transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        } else {
            transition(t.commands, t.buffer, buffer_state, D3D12_RESOURCE_STATE_PRESENT);
        }
        ++g.frames;
        if (g.frames == 1)
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("Replay Plus: the picture's pass is drawing ({}x{}, format {}, transfer {}, {} mips).", desc.Width,
                                       desc.Height, static_cast<int>(desc.Format), t.transfer, g.mips));
        static bool said_under{};
        static std::uint32_t said_w{}, said_h{};
        if (into_scene != said_under || (into_scene && (work_w != said_w || work_h != said_h))) {
            said_under = into_scene;
            said_w = work_w;
            said_h = work_h;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           into_scene ? std::format("Replay Plus: drawing under the game's UI ({}x{}).", work_w, work_h)
                                      : std::format("Replay Plus: drawing over the screen (the game's UI included; the game's picture {}).",
                                                    scene.width ? std::format("{}x{}", scene.width, scene.height) : std::string("not found")));
        }
        if (g.frames % 60 == 1) {
            link::RenderReport report;
            report.width = static_cast<std::uint32_t>(desc.Width);
            report.height = desc.Height;
            report.format = static_cast<std::uint32_t>(desc.Format);
            report.transfer = t.transfer;
            report.frames = g.frames;
            report.fit = finish.fit;
            link::report(report);
        }
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing it.");
        return false;
    }
}

bool finish_export(std::uint8_t* pixels, std::uint32_t w, std::uint32_t h, std::uint32_t pitch) noexcept {
    try {
        if (!pixels || w < 16 || h < 16 || pitch < w * 4) return false;
        auto& g = graphics();
        std::lock_guard lock(g.lock);
        auto& e = exporter();
        if (e.failed || !g.device || !g.queue || !g.root || !g.decode || !g.down || !g.vs_blob || !g.main_blob) return false;
        const auto picture = link::picture();
        if (!picture) return false;
        const auto& s = picture->settings;
        const auto hood_image = link::hood();
        const auto finish = lens::finish_for(s, static_cast<float>(w) / static_cast<float>(h), fov_for(*picture, 2000), hood_image ? &hood_image->hole : nullptr);
        const bool light_on = s.light && s.light_power > 0;
        if (!lens::changes(finish) && !light_on) return true; // nothing to change in it
        if (!export_setup(e, g.device) || !export_targets(e, w, h)) {
            e.failed = true;
            logging::write(logging::Level::warning, logging::Channel::graphics,
                           "Replay Plus export: the GPU could not be set up for exported frames; they are finished on the CPU.");
            return false;
        }
        auto* device = e.device.Get();
        // The hood's picture and the game's depth (the camera light), as the screen's pass has them.
        if (g.hood && g.hood_ready) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = g.hood->GetDesc().MipLevels;
            view.Format = g.hood->GetDesc().Format;
            device->CreateShaderResourceView(g.hood.Get(), &view, export_cpu(e, export_slot_hood));
        } else {
            export_null_view(e, export_slot_hood);
        }
        // The depth of the frame this one was drawn in (it comes read back a few frames later), for the light.
        // (Test builds can light them from the latest instead, as before, to compare: link::export_depth_latest.)
        const auto kept = light_on ? depth::for_export(pixels, w, h, pitch) : depth::Copy{};
        const auto depth_copy = light_on && !link::export_depth_latest().load(std::memory_order_relaxed) ? kept : depth::current();
        ComPtr<ID3D12Resource> depth_held = depth_copy.texture; // held while the GPU reads it
        const bool depth_ready = light_on && depth_copy.texture && depth_copy.width && depth_copy.height;
        if (depth_ready) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_FLOAT;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(depth_copy.texture, &view, export_cpu(e, export_slot_depth));
        } else {
            export_null_view(e, export_slot_depth);
        }
        const auto constants = pass_constants(*picture, finish, {static_cast<float>(w), static_cast<float>(h), 0, 1.0f, e.mips, g.hood_ready,
                                                                 depth_copy, depth_ready, e.frames++});
        std::memcpy(e.constants_cpu, constants.data(), sizeof(std::uint32_t) * constant_count);
        for (std::uint32_t y = 0; y < h; ++y) std::memcpy(e.upload_cpu + std::size_t{y} * e.row, pixels + std::size_t{y} * pitch, std::size_t{w} * 4);

        if (FAILED(e.allocator->Reset()) || FAILED(e.list->Reset(e.allocator.Get(), nullptr))) return false;
        auto* list = e.list.Get();
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = e.upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint = {export_format, w, h, 1, e.row};
        to.pResource = e.source.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list, e.source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[] = {e.views.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(g.root.Get());
        list->SetGraphicsRootConstantBufferView(0, e.constants->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(1, export_gpu(e, export_slot_source));
        list->SetGraphicsRootDescriptorTable(2, export_gpu(e, export_slot_chain));
        list->SetGraphicsRootDescriptorTable(3, export_gpu(e, export_slot_depth));
        list->SetGraphicsRootDescriptorTable(4, export_gpu(e, export_slot_hood));
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto viewport = [&](std::uint32_t vw, std::uint32_t vh) {
            const D3D12_VIEWPORT v{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
            const D3D12_RECT r{0, 0, static_cast<LONG>(vw), static_cast<LONG>(vh)};
            list->RSSetViewports(1, &v);
            list->RSSetScissorRects(1, &r);
        };
        // 1. The frame in linear light (and the camera light), 2. its mips where the fisheye squeezes.
        auto mip0 = export_rtv(e, 0);
        list->OMSetRenderTargets(1, &mip0, FALSE, nullptr);
        viewport(w, h);
        list->SetPipelineState(g.decode.Get());
        list->DrawInstanced(3, 1, 0, 0);
        const bool lens_on = finish.frame.fisheye;
        if (lens_on) {
            list->SetPipelineState(g.down.Get());
            for (unsigned mip = 1; mip < e.mips; ++mip) {
                transition(list, e.chain.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, mip - 1);
                const auto target = export_rtv(e, mip);
                list->OMSetRenderTargets(1, &target, FALSE, nullptr);
                list->SetGraphicsRootDescriptorTable(1, export_gpu(e, export_slot_mips + mip - 1));
                viewport(std::max(1u, w >> mip), std::max(1u, h >> mip));
                list->DrawInstanced(3, 1, 0, 0);
            }
            transition(list, e.chain.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, e.mips - 1);
        } else {
            transition(list, e.chain.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0);
        }
        // 3. Lens, grade, bars and hood, into the frame's own format.
        const auto out = export_rtv(e, export_target_output);
        list->OMSetRenderTargets(1, &out, FALSE, nullptr);
        list->SetPipelineState(e.main.Get());
        viewport(w, h);
        list->SetGraphicsRootDescriptorTable(1, export_gpu(e, export_slot_source));
        list->DrawInstanced(3, 1, 0, 0);
        // Back to the CPU; every texture as it started.
        transition(list, e.output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION back{}, frame{};
        back.pResource = e.readback.Get();
        back.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        back.PlacedFootprint.Footprint = {export_format, w, h, 1, e.row};
        frame.pResource = e.output.Get();
        frame.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&back, 0, 0, 0, &frame, nullptr);
        transition(list, e.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(list, e.source.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (unsigned mip = 0; mip < (lens_on ? e.mips : 1u); ++mip)
            transition(list, e.chain.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, mip);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list};
        g.queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++e.value;
        if (FAILED(g.queue->Signal(e.fence.Get(), value)) || FAILED(e.fence->SetEventOnCompletion(value, e.event)) ||
            WaitForSingleObject(e.event, 3000) != WAIT_OBJECT_0) {
            e.failed = true;
            logging::write(logging::Level::warning, logging::Channel::graphics,
                           "Replay Plus export: an exported frame was not finished on the GPU in time; the rest are finished on the CPU.");
            return false;
        }
        for (std::uint32_t y = 0; y < h; ++y) std::memcpy(pixels + std::size_t{y} * pitch, e.readback_cpu + std::size_t{y} * e.row, std::size_t{w} * 4);
        if (!e.said) {
            e.said = true;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("Replay Plus export: its frames ({}x{}) are finished on the GPU, the camera light {}.", w, h,
                                       depth_ready ? "with them" : light_on ? "waiting for the game's depth" : "off"));
        }
        return true;
    } catch (...) {
        return false;
    }
}

void after_ui(ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer) noexcept {
    try {
        auto& g = graphics();
        if (g.shot_after_ui_path.empty() || !commands || !buffer) return;
        ComPtr<ID3D12Device> device;
        if (FAILED(buffer->GetDevice(IID_PPV_ARGS(&device))) || !ensure_shot_buffer(device.Get(), buffer)) return;
        transition(commands, buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_shot(commands, buffer, g.shot_after_ui_path);
        transition(commands, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g.shot_after_ui_path.clear();
    } catch (...) {}
}

void release() noexcept {
    auto& g = graphics();
    std::lock_guard lock(g.lock);
    export_release();
    g.queue.Reset();
    depth::release();
    g.depth_serial = ~std::uint64_t{0};
    g.focus_readback.Reset();
    g.focus_written = {};
    g.copy.Reset();
    g.chain.Reset();
    g.hood.Reset();
    g.hood_upload.Reset();
    g.hood_serial = ~std::uint64_t{0};
    g.hood_ready = false;
    g.shot.Reset();
    g.shot_pending = false;
    g.decode.Reset();
    g.down.Reset();
    g.main.Reset();
    g.main_format = DXGI_FORMAT_UNKNOWN;
    g.root.Reset();
    g.views.Reset();
    g.targets.Reset();
    g.constants_cpu = nullptr;
    g.constants.Reset();
    g.device = nullptr;
    g.width = g.height = 0;
    g.copy_format = DXGI_FORMAT_UNKNOWN;
}
} // namespace dingosdk::replay_plus::overlay

namespace dingosdk::replay_plus::link {
bool finish_export_on_gpu(std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) noexcept {
    return overlay::finish_export(pixels, width, height, pitch);
}
} // namespace dingosdk::replay_plus::link
