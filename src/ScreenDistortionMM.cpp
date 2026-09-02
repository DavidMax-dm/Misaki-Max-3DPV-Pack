#include "ScreenDistortionMM.hpp"

#include "DebugLog.hpp"
#include "FogDepthHeightFixMM.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <detours.h>
#include <intrin.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace screen_distortion {
namespace {

// Release builds retain the bounded diagnostic branches for maintainability,
// but compile every console trace to a no-op.
template <typename... Args>
int release_printf(const char*, Args&&...) {
    return 0;
}

constexpr float kAmplitudeMultiplier = 2.5f;
// Unity's PJSK post shaders apply their serialized offsets in an internal
// screen-space domain; they are not direct 0..1 texture coordinates.  MM+
// samples normalized UVs, so convert them to the same few-pixel range.
// GreatVersion's compiled effect path retained these two calibration factors
// (0.07 for UV offsets and 0.25 for the overlay amount).  The shader itself is
// identical to this one; restoring the factors keeps the original visual
// strength while the draw still happens at the new scene-only boundary.
constexpr float kPjskUvScale = 0.07f;
constexpr float kPjskOverlayScale = 0.25f;
// Keep this false for normal play.  It can be enabled temporarily when a
// RenderDoc capture needs a continuous, clearly visible distortion.
constexpr bool kCaptureDiagnostic = false;
// Temporary address-mapping build: observe native Draw callers only.  It
// deliberately does not inject a pass or hook command-list execution.
// Keep the proven MM+ path: attaching ExecuteCommandList/deferred-context
// hooks here can conflict with other graphics mods and crash during startup.
// ScreenFX still uses the native render-event/final-scene boundary below.
constexpr bool kTraceNativeDrawCallersOnly = true;
// Probe the native render boundary directly.  `pass_sprite` is the first 2D
// pass; this build records its immediate D3D bindings but deliberately does
// not draw or redirect anything.
constexpr bool kTraceSpritePassBoundary = false;
// The probe established that the RT bound immediately before pass_sprite is
// the finished 3D scene target.  Composite there, then hand it untouched to
// native Sprite/2D rendering.
constexpr bool kEnableSpritePassInsertion = false;
// Record only D3D draws issued synchronously from pass_sprite.  This tells us
// whether that native function executes the 3D-to-Sprite composite itself or
// merely submits a deferred command list.
constexpr bool kTraceSpritePassDraws = false;
constexpr bool kTraceSpriteCommandLists = false;
constexpr bool kEnableTaggedSpriteListInsertion = false;
// The marker is used only to retain the real scene carrier.  It does not
// issue a draw, copy, or command-list replay; the actual insertion remains
// the immediately-following-frame D3D final post-process Draw hook.
constexpr bool kEnableNativeSpriteMarkerInsertion = true;
constexpr bool kTraceFullResDraws = false;
// The RDC-confirmed final post-process Draw(4): it writes a full-resolution
// intermediate into the scene carrier.  Sprite/2D/AET begins afterwards.
constexpr bool kEnableSceneCompositeCandidate = true;
constexpr std::uintptr_t kPassPostProcessRva = 0x004DA9B0;
constexpr std::uintptr_t kPassSpriteRva = 0x004DAA00;
constexpr std::uintptr_t kPassAdjustScreenRva = 0x004DAA60;
constexpr std::uintptr_t kNativeAdjustBlitRva = 0x0049DB50;
constexpr std::uintptr_t kNativeAdjustBlitReturnRva = 0x0049E853;
constexpr std::uintptr_t kNativeCommand21Rva = 0x002B8A00;
constexpr std::uintptr_t kNativeRenderEventRva = 0x002B8BE0;

struct Transition {
    float intensity_start = 0.0f;
    float intensity_end = 0.0f;
    float scale_start = 5.0f;
    float scale_end = 5.0f;
    float offset_start = 0.0f;
    float offset_end = 0.0f;
    std::int64_t start_time = 0;
    std::int64_t duration = 0;
};

struct Constants {
    float legacy[4];
    float distortion[4];
    float noise[4];
    float chroma_r[4];
    float chroma_g[4];
    float chroma_b[4];
    float overlay[4];
    float tint[4];
};

struct PjskTransition {
    std::array<float, 9> start{};
    std::array<float, 9> end{};
    std::int64_t start_time = 0;
    std::int64_t duration = 0;
    int texture = -1;
    int mode = 0;
    bool active = false;
};

struct Evaluated {
    Constants constants{};
    int noise_texture = -1;
    int overlay_texture = -1;
    bool active = false;
};

std::mutex g_mutex;
Transition g_transition;
std::int64_t g_song_time = 0;
bool g_playing = false;
bool g_transition_active = false;
PjskTransition g_pjsk_distortion;
PjskTransition g_pjsk_chromatic;
PjskTransition g_pjsk_overlay;
std::vector<std::filesystem::path> g_texture_paths;
std::atomic<std::uint64_t> g_distortion_draws = 0;
std::atomic<std::uint64_t> g_sprite_calls = 0;
std::atomic<std::uint64_t> g_noise_events = 0;
std::atomic<std::uint64_t> g_active_passes = 0;
std::atomic<std::uint64_t> g_valid_targets = 0;
std::atomic<std::uint64_t> g_captured_post_targets = 0;
std::atomic<std::uint64_t> g_captured_adjust_targets = 0;
std::atomic<std::uint64_t> g_command_lists = 0;
std::atomic<std::uint64_t> g_post_insertions = 0;
std::atomic<std::uint64_t> g_native_blit_calls = 0;
std::atomic<std::uint64_t> g_native_blit_matches = 0;
std::atomic<std::uint64_t> g_native_blit_targets = 0;
std::atomic<unsigned int> g_command21_effect_samples = 0;
std::atomic<unsigned int> g_draw_trace_effect_samples = 0;
std::atomic<unsigned int> g_sprite_boundary_effect_samples = 0;
std::atomic<std::uint64_t> g_present_serial = 1;
std::atomic<bool> g_inside_post_process = false;
thread_local bool g_inside_distortion = false;
thread_local bool g_inside_sprite_pass = false;
std::atomic<unsigned int> g_sprite_pass_depth = 0;
thread_local bool g_sprite_effect_drawn = false;
thread_local std::uint64_t g_ui_effect_serial = 0;
std::mutex g_post_target_mutex;
ID3D11RenderTargetView* g_last_post_process_view = nullptr;
ID3D11Texture2D* g_adjust_screen_texture = nullptr;
ID3D11RenderTargetView* g_adjust_screen_view = nullptr;
ID3D11Texture2D* g_rdc_final_scene_texture = nullptr;
// The last full-screen scene composite uses one stable native pixel shader.
// Caching it lets the Draw hook reject every other full-screen draw without
// repeatedly walking RTV/SRV resources and texture descriptions.
ID3D11PixelShader* g_rdc_final_scene_shader = nullptr;

using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11CommandList*, BOOL);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    BOOL, ID3D11CommandList**);
DrawFn g_draw = nullptr;
DrawIndexedFn g_draw_indexed = nullptr;
DrawInstancedFn g_draw_instanced = nullptr;
DrawIndexedInstancedFn g_draw_indexed_instanced = nullptr;
ExecuteCommandListFn g_execute_command_list = nullptr;
FinishCommandListFn g_finish_command_list = nullptr;
void* g_immediate_draw_entry = nullptr;
void* g_immediate_draw_indexed_entry = nullptr;
void* g_immediate_draw_instanced_entry = nullptr;
void* g_immediate_draw_indexed_instanced_entry = nullptr;
bool g_context_hooks_installed = false;
DrawFn g_deferred_draw = nullptr;
DrawIndexedFn g_deferred_draw_indexed = nullptr;
DrawInstancedFn g_deferred_draw_instanced = nullptr;
DrawIndexedInstancedFn g_deferred_draw_indexed_instanced = nullptr;
bool g_deferred_context_hooks_installed = false;
std::mutex g_tagged_command_list_mutex;
std::unordered_set<ID3D11CommandList*> g_sprite_command_lists;
IDXGISwapChain* g_swap_chain = nullptr;
using PassSpriteFn = void(__fastcall*)(void*, void*);
using PassPostProcessFn = void(__fastcall*)(void*, void*);
using PassAdjustScreenFn = void(__fastcall*)(void*, void*);
PassSpriteFn g_pass_sprite = nullptr;
PassPostProcessFn g_pass_post_process = nullptr;
PassAdjustScreenFn g_pass_adjust_screen = nullptr;
bool g_sprite_hook_installed = false;
bool g_post_process_hook_installed = false;
bool g_adjust_screen_hook_installed = false;
using NativeAdjustBlitFn = void(__fastcall*)(void*, void*, float,
    std::uint32_t, std::uint32_t, float, float, float, float, float,
    const void*);
NativeAdjustBlitFn g_native_adjust_blit = nullptr;
bool g_native_adjust_blit_hook_installed = false;
using NativeCommand21Fn = void(__fastcall*)(void*, void*, void**);
NativeCommand21Fn g_native_command21 = nullptr;
bool g_native_command21_hook_installed = false;
using NativeRenderEventFn = void(__fastcall*)(void*, void*, void**);
NativeRenderEventFn g_native_render_event = nullptr;
bool g_native_render_event_hook_installed = false;
std::uintptr_t g_exe_base = 0;

void distort_after_post_process_command_list(ID3D11DeviceContext* context);
void render_after_post_process(ID3D11DeviceContext* context,
    ID3D11RenderTargetView* post_process_view);

ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vertex_shader = nullptr;
ID3D11PixelShader* g_pixel_shader = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Buffer* g_constants = nullptr;
ID3D11Texture2D* g_back_buffer = nullptr;
ID3D11Texture2D* g_scene_copy = nullptr;
ID3D11ShaderResourceView* g_scene_view = nullptr;
ID3D11RenderTargetView* g_back_buffer_view = nullptr;
ID3D11Texture2D* g_pipeline_warm_texture = nullptr;
ID3D11RenderTargetView* g_pipeline_warm_view = nullptr;
bool g_pipeline_warmed = false;
bool g_effect_textures_warmed = false;
std::atomic<bool> g_final_scene_target_prewarmed = false;
std::vector<ID3D11ShaderResourceView*> g_effect_views;
UINT g_width = 0;
UINT g_height = 0;

const char* kShaderSource = R"HLSL(
cbuffer DistortionConstants : register(b0) {
    float4 legacy;
    float4 distortion;
    float4 noise;
    float4 chroma_r;
    float4 chroma_g;
    float4 chroma_b;
    float4 overlay;
    float4 tint;
};

Texture2D scene_texture : register(t0);
Texture2D noise_texture : register(t1);
Texture2D overlay_texture : register(t2);
SamplerState scene_sampler : register(s0);

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint vertex_id : SV_VertexID) {
    VsOut output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0)
        + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 ps_main(VsOut input) : SV_Target {
    float2 uv = input.uv;
    float phase_y = uv.y * legacy.y + legacy.z;
    float phase_x = uv.x * legacy.y + legacy.z;
    float2 wave = float2(sin(phase_y), sin(phase_x) * 0.16);
    uv += wave * legacy.x;
    if (distortion.w > 0.5) {
        float2 nuv = uv * noise.xy + noise.zw * distortion.z;
        float2 n = noise_texture.Sample(scene_sampler, frac(nuv)).rg * 2.0 - 1.0;
        uv += n * distortion.x;
    }
    uv = clamp(uv, 0.001, 0.999);
    float2 center = uv - 0.5;
    float2 ur = chroma_r.w > 0.5 ? 0.5 + center * (1.0 + chroma_r.z) + chroma_r.xy : uv + chroma_r.xy;
    float2 ug = chroma_r.w > 0.5 ? 0.5 + center * (1.0 + chroma_g.z) + chroma_g.xy : uv + chroma_g.xy;
    float2 ub = chroma_r.w > 0.5 ? 0.5 + center * (1.0 + chroma_b.z) + chroma_b.xy : uv + chroma_b.xy;
    float4 color = float4(scene_texture.Sample(scene_sampler, saturate(ur)).r,
        scene_texture.Sample(scene_sampler, saturate(ug)).g,
        scene_texture.Sample(scene_sampler, saturate(ub)).b,
        scene_texture.Sample(scene_sampler, uv).a);
    if (overlay.x > 0.00001) {
        float4 flash = overlay_texture.Sample(scene_sampler, input.uv) * tint;
        if (overlay.y < 0.5) color = lerp(color, flash, flash.a * overlay.x);
        else if (overlay.y < 1.5) color.rgb += flash.rgb * flash.a * overlay.x;
        else color.rgb = 1.0 - (1.0 - color.rgb) * (1.0 - flash.rgb * flash.a * overlay.x);
    }
    return color;
}
)HLSL";

template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void release_targets() {
    release(g_back_buffer_view);
    release(g_scene_view);
    release(g_scene_copy);
    release(g_back_buffer);
    g_width = 0;
    g_height = 0;
    g_final_scene_target_prewarmed.store(false, std::memory_order_release);
}

void release_effect_views() {
    for (auto*& view : g_effect_views)
        release(view);
    g_effect_views.clear();
}

ID3D11ShaderResourceView* load_wic_texture(ID3D11Device* device,
    const std::filesystem::path& path) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
    std::vector<std::uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, width * 4,
        static_cast<UINT>(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{ pixels.data(), width * 4, 0 };
        hr = device->CreateTexture2D(&desc, &data, &texture);
    }
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture, nullptr, &view);
    release(texture); release(converter); release(frame); release(decoder); release(factory);
    return SUCCEEDED(hr) ? view : nullptr;
}

void load_effect_views(ID3D11Device* device) {
    release_effect_views();
    for (const auto& path : g_texture_paths)
        g_effect_views.push_back(load_wic_texture(device, path));
}

void release_device_resources() {
    release_targets();
    release(g_pipeline_warm_view);
    release(g_pipeline_warm_texture);
    g_pipeline_warmed = false;
    g_effect_textures_warmed = false;
    release_effect_views();
    release(g_constants);
    release(g_sampler);
    release(g_pixel_shader);
    release(g_vertex_shader);
    release(g_device);
}

bool compile_shader(ID3D11Device* device, const char* entry, const char* target,
    ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(kShaderSource, std::strlen(kShaderSource),
        "ScreenDistortionMM.hlsl", nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(result)) {
        std::string message = "Screen distortion shader compilation failed";
        if (errors && errors->GetBufferPointer()) {
            message += ": ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        debug_log::line_utf8(message.c_str());
    }
    release(errors);
    return SUCCEEDED(result);
}

bool ensure_device_resources(ID3D11Device* device) {
    if (g_device == device && g_vertex_shader && g_pixel_shader
        && g_sampler && g_constants)
        return true;

    release_device_resources();
    g_device = device;
    g_device->AddRef();

    ID3DBlob* vertex_blob = nullptr;
    ID3DBlob* pixel_blob = nullptr;
    if (!compile_shader(device, "vs_main", "vs_5_0", &vertex_blob)
        || !compile_shader(device, "ps_main", "ps_5_0", &pixel_blob)) {
        release(vertex_blob);
        release(pixel_blob);
        return false;
    }

    HRESULT result = device->CreateVertexShader(vertex_blob->GetBufferPointer(),
        vertex_blob->GetBufferSize(), nullptr, &g_vertex_shader);
    if (SUCCEEDED(result))
        result = device->CreatePixelShader(pixel_blob->GetBufferPointer(),
            pixel_blob->GetBufferSize(), nullptr, &g_pixel_shader);
    release(vertex_blob);
    release(pixel_blob);
    if (FAILED(result))
        return false;

    load_effect_views(device);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    result = device->CreateSamplerState(&sampler, &g_sampler);

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result))
        result = device->CreateBuffer(&buffer, nullptr, &g_constants);
    if (FAILED(result))
        return false;

    debug_log::line(L"Screen distortion: D3D11 pass initialized");
    return true;
}

bool ensure_targets(ID3D11Texture2D* target,
    ID3D11RenderTargetView* target_view, ID3D11Device* device) {
    if (!target || !target_view)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    if (g_back_buffer == target && g_scene_copy
        && g_width == desc.Width && g_height == desc.Height) {
        return true;
    }

    release_targets();
    g_back_buffer = target;
    g_back_buffer->AddRef();
    g_back_buffer_view = target_view;
    g_back_buffer_view->AddRef();
    g_width = desc.Width;
    g_height = desc.Height;

    D3D11_TEXTURE2D_DESC copy_desc = desc;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy_desc.CPUAccessFlags = 0;
    copy_desc.MiscFlags = 0;
    HRESULT result = device->CreateTexture2D(&copy_desc, nullptr, &g_scene_copy);
    if (SUCCEEDED(result))
        result = device->CreateShaderResourceView(g_scene_copy, nullptr, &g_scene_view);
    return SUCCEEDED(result);
}

// The actual ScreenFX draw must copy the finished 3D scene into g_scene_copy.
// Creating that full-resolution texture on the first effect event used to put
// a visible stall immediately before the first distortion/chromatic/overlay.
// pass_sprite gives us the exact same scene carrier every PV frame, so build
// the copy target as soon as that carrier has been observed, without drawing
// anything.  This remains strictly before native Sprite/2D/AET composition.
bool prewarm_final_scene_target(ID3D11Device* device) {
    if (!device)
        return false;
    if (g_final_scene_target_prewarmed.load(std::memory_order_acquire))
        return true;

    ID3D11Texture2D* target = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        target = g_rdc_final_scene_texture;
        if (target)
            target->AddRef();
    }
    if (!target)
        return false;

    // The normal effect path will use an RTV for this identical texture.  If
    // it is already prepared, do not create another view every Present.
    if (g_back_buffer == target && g_scene_copy) {
        target->Release();
        g_final_scene_target_prewarmed.store(true, std::memory_order_release);
        return true;
    }

    ID3D11RenderTargetView* target_view = nullptr;
    const HRESULT result = device->CreateRenderTargetView(target, nullptr,
        &target_view);
    const bool ready = SUCCEEDED(result) && target_view
        && ensure_targets(target, target_view, device);
    if (ready) {
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        release_printf("[MM ScreenFX] prewarmed final 3D target %ux%u before effects\n",
            desc.Width, desc.Height);
        g_final_scene_target_prewarmed.store(true, std::memory_order_release);
    }
    release(target_view);
    target->Release();
    return ready;
}

// Some drivers defer the actual PSO/JIT work until the first Draw, even after
// CreatePixelShader has succeeded.  Run the exact ScreenFX shader once into a
// private 1x1 RT after the real scene source exists.  The command list restores
// all immediate-context state and never touches the scene or the 2D pipeline.
bool prewarm_effect_pipeline(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || (g_pipeline_warmed && g_effect_textures_warmed)
        || !g_scene_view
        || !g_vertex_shader || !g_pixel_shader || !g_sampler || !g_constants)
        return false;

    if (!g_pipeline_warm_view) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_pipeline_warm_texture))
            || FAILED(device->CreateRenderTargetView(g_pipeline_warm_texture,
                nullptr, &g_pipeline_warm_view))) {
            release(g_pipeline_warm_view);
            release(g_pipeline_warm_texture);
            return false;
        }
    }

    ID3D11DeviceContext* deferred = nullptr;
    if (FAILED(device->CreateDeferredContext(0, &deferred)) || !deferred)
        return false;
    Constants constants{};
    constants.legacy[1] = 1.0f;
    // Force the PS to sample both effect slots.  This is intentionally done
    // while PV is loading so PNG decode/upload and first texture residency do
    // not turn into a hitch at the first flash or glitch frame.
    constants.distortion[3] = 1.0f;
    constants.noise[0] = 1.0f;
    constants.noise[1] = 1.0f;
    constants.overlay[0] = 1.0f;
    constants.tint[0] = 1.0f;
    constants.tint[1] = 1.0f;
    constants.tint[2] = 1.0f;
    constants.tint[3] = 1.0f;
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
    deferred->OMSetRenderTargets(1, &g_pipeline_warm_view, nullptr);
    deferred->RSSetViewports(1, &viewport);
    deferred->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    deferred->OMSetDepthStencilState(nullptr, 0);
    deferred->RSSetState(nullptr);
    deferred->IASetInputLayout(nullptr);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(g_vertex_shader, nullptr, 0);
    deferred->PSSetShader(g_pixel_shader, nullptr, 0);
    deferred->UpdateSubresource(g_constants, 0, nullptr, &constants, 0, 0);
    deferred->PSSetConstantBuffers(0, 1, &g_constants);
    deferred->PSSetSamplers(0, 1, &g_sampler);
    // One draw without a mask covers the baseline shader path.  One further
    // draw for every configured image forces its actual sample path resident.
    ID3D11ShaderResourceView* views[3]{ g_scene_view, nullptr, nullptr };
    deferred->PSSetShaderResources(0, 3, views);
    deferred->Draw(3, 0);
    for (ID3D11ShaderResourceView* effect_view : g_effect_views) {
        if (!effect_view)
            continue;
        views[1] = effect_view;
        views[2] = effect_view;
        deferred->PSSetShaderResources(0, 3, views);
        deferred->Draw(3, 0);
    }
    ID3D11CommandList* command_list = nullptr;
    const HRESULT finished = deferred->FinishCommandList(FALSE, &command_list);
    deferred->Release();
    if (FAILED(finished) || !command_list)
        return false;

    g_inside_distortion = true;
    context->ExecuteCommandList(command_list, TRUE);
    g_inside_distortion = false;
    command_list->Release();

    // ExecuteCommandList is asynchronous.  Without a completion barrier the
    // first visible effect can still become the point where the driver waits
    // for its first shader specialization / texture residency work.  Wait
    // here, during PV loading, so the gameplay timeline starts only after
    // the tiny offscreen warm-up has actually reached the GPU.
    ID3D11Query* completion = nullptr;
    D3D11_QUERY_DESC completion_desc{};
    completion_desc.Query = D3D11_QUERY_EVENT;
    const ULONGLONG wait_start = GetTickCount64();
    bool completed = false;
    if (SUCCEEDED(device->CreateQuery(&completion_desc, &completion)) && completion) {
        context->End(completion);
        context->Flush();
        for (;;) {
            const HRESULT status = context->GetData(completion, nullptr, 0, 0);
            if (status == S_OK) {
                completed = true;
                break;
            }
            // Never allow a broken driver/device to deadlock a PV load.
            if (status != S_FALSE || GetTickCount64() - wait_start > 3000)
                break;
            Sleep(0);
        }
    }
    release(completion);
    g_pipeline_warmed = true;
    g_effect_textures_warmed = true;
    release_printf("[MM ScreenFX] prewarmed ScreenFX pipeline and %zu effect texture(s) before playback (%s, %llums)\n",
        g_effect_views.size(), completed ? "GPU complete" : "GPU wait timed out",
        static_cast<unsigned long long>(GetTickCount64() - wait_start));
    return true;
}

float transition_t(PjskTransition& value) {
    if (!value.active) return 1.0f;
    if (value.duration <= 0) { value.active = false; return 1.0f; }
    const float t = std::clamp(static_cast<float>(static_cast<double>(g_song_time
        - value.start_time) / static_cast<double>(value.duration)), 0.0f, 1.0f);
    if (t >= 1.0f) value.active = false;
    return t;
}

float lerp_value(const PjskTransition& v, size_t i, float t) {
    return v.start[i] + (v.end[i] - v.start[i]) * t;
}

bool evaluate(Evaluated& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_playing)
        return false;
    if constexpr (kCaptureDiagnostic) {
        out.constants.legacy[0] = 0.025f;
        out.constants.legacy[1] = 15.0f;
        out.active = true;
        return true;
    }
    float t = 1.0f;
    if (g_transition_active && g_transition.duration > 0) {
        t = std::clamp(static_cast<float>(static_cast<double>(g_song_time
            - g_transition.start_time) / static_cast<double>(g_transition.duration)),
            0.0f, 1.0f);
        if (t >= 1.0f)
            g_transition_active = false;
    }
    const float intensity = g_transition.intensity_start
        + (g_transition.intensity_end - g_transition.intensity_start) * t;
    const float scale = g_transition.scale_start
        + (g_transition.scale_end - g_transition.scale_start) * t;
    const float offset = g_transition.offset_start
        + (g_transition.offset_end - g_transition.offset_start) * t;
    out.constants.legacy[0] = intensity * kAmplitudeMultiplier;
    out.constants.legacy[1] = scale;
    out.constants.legacy[2] = offset;

    const float dt = transition_t(g_pjsk_distortion);
    out.constants.distortion[0] = lerp_value(g_pjsk_distortion, 0, dt)
        * kPjskUvScale;
    out.constants.distortion[1] = lerp_value(g_pjsk_distortion, 1, dt);
    out.constants.distortion[2] = lerp_value(g_pjsk_distortion, 2, dt);
    out.noise_texture = g_pjsk_distortion.texture;
    const bool noise_valid = out.noise_texture >= 0
        && static_cast<size_t>(out.noise_texture) < g_effect_views.size()
        && g_effect_views[out.noise_texture];
    out.constants.distortion[3] = noise_valid ? 1.0f : 0.0f;
    out.constants.noise[0] = g_pjsk_distortion.start[3];
    out.constants.noise[1] = g_pjsk_distortion.start[4];
    out.constants.noise[2] = g_pjsk_distortion.start[5];
    out.constants.noise[3] = g_pjsk_distortion.start[6];

    const float ct = transition_t(g_pjsk_chromatic);
    for (size_t c = 0; c < 3; ++c) {
        float* dst = c == 0 ? out.constants.chroma_r
            : c == 1 ? out.constants.chroma_g : out.constants.chroma_b;
        dst[0] = lerp_value(g_pjsk_chromatic, c * 2, ct) * kPjskUvScale;
        dst[1] = lerp_value(g_pjsk_chromatic, c * 2 + 1, ct) * kPjskUvScale;
        dst[2] = lerp_value(g_pjsk_chromatic, 6 + c, ct) * kPjskUvScale;
    }
    out.constants.chroma_r[3] = static_cast<float>(g_pjsk_chromatic.mode);

    const float ot = transition_t(g_pjsk_overlay);
    out.constants.overlay[0] = lerp_value(g_pjsk_overlay, 0, ot)
        * kPjskOverlayScale;
    out.constants.overlay[1] = static_cast<float>(g_pjsk_overlay.mode);
    out.overlay_texture = g_pjsk_overlay.texture;
    for (size_t i = 0; i < 4; ++i)
        out.constants.tint[i] = g_pjsk_overlay.start[i + 1];
    const bool overlay_valid = out.overlay_texture >= 0
        && static_cast<size_t>(out.overlay_texture) < g_effect_views.size()
        && g_effect_views[out.overlay_texture];
    if (!overlay_valid) out.constants.overlay[0] = 0.0f;
    // A chromatic transition may be scheduled well before it becomes visible.
    // `active` alone used to run the full-resolution copy/composite while all
    // nine chromatic values were still identity (zero), producing stutter in
    // the lead-in to an effect.  Do not render until the actual shader inputs
    // have departed from identity; the epsilon is below a visible UV shift.
    constexpr float kVisibleEffectEpsilon = 0.00001f;
    const auto visible = [](float value) {
        return std::fabs(value) > kVisibleEffectEpsilon;
    };
    const bool chromatic_visible = visible(out.constants.chroma_r[0])
        || visible(out.constants.chroma_r[1]) || visible(out.constants.chroma_r[2])
        || visible(out.constants.chroma_g[0]) || visible(out.constants.chroma_g[1])
        || visible(out.constants.chroma_g[2]) || visible(out.constants.chroma_b[0])
        || visible(out.constants.chroma_b[1]) || visible(out.constants.chroma_b[2]);
    out.active = visible(out.constants.legacy[0])
        || visible(out.constants.distortion[0]) || chromatic_visible
        || visible(out.constants.overlay[0]);
    return out.active;
}

void release_adjust_screen_target() {
    std::lock_guard<std::mutex> lock(g_post_target_mutex);
    release(g_adjust_screen_view);
    release(g_adjust_screen_texture);
    release(g_rdc_final_scene_texture);
    release(g_rdc_final_scene_shader);
}

// RenderDoc shows that MM+'s final pass_adjust_screen keeps its input scene
// texture bound to PS t0 all the way through Present.  That texture is the
// post-processed 3D target which pass_sprite writes into immediately before
// pass_adjust_screen.  Remember it here, then distort it at the end of the
// next pass_post_process -- precisely between 3D post processing and Sprite.
void capture_adjust_screen_target(ID3D11DeviceContext* context,
    IDXGISwapChain* swap_chain, ID3D11Device* device) {
    if (!context || !swap_chain || !device)
        return;

    ID3D11ShaderResourceView* source_view = nullptr;
    context->PSGetShaderResources(0, 1, &source_view);
    if (!source_view)
        return;

    ID3D11Resource* source_resource = nullptr;
    ID3D11Texture2D* source_texture = nullptr;
    source_view->GetResource(&source_resource);
    if (source_resource) {
        source_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&source_texture));
    }
    release(source_resource);
    release(source_view);
    if (!source_texture)
        return;

    DXGI_SWAP_CHAIN_DESC swap_desc{};
    D3D11_TEXTURE2D_DESC texture_desc{};
    source_texture->GetDesc(&texture_desc);
    if (FAILED(swap_chain->GetDesc(&swap_desc))
        || texture_desc.Width != swap_desc.BufferDesc.Width
        || texture_desc.Height != swap_desc.BufferDesc.Height
        || (texture_desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0
        || (texture_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        source_texture->Release();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        if (g_adjust_screen_texture == source_texture) {
            source_texture->Release();
            return;
        }
    }

    ID3D11RenderTargetView* target_view = nullptr;
    if (FAILED(device->CreateRenderTargetView(source_texture, nullptr,
        &target_view)) || !target_view) {
        source_texture->Release();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        release(g_adjust_screen_view);
        release(g_adjust_screen_texture);
        g_adjust_screen_texture = source_texture;
        g_adjust_screen_view = target_view;
    }
    g_captured_adjust_targets.fetch_add(1, std::memory_order_relaxed);
    debug_log::line(L"Screen distortion: captured adjust_screen scene t0 "
        + std::to_wstring(texture_desc.Width) + L"x"
        + std::to_wstring(texture_desc.Height));
}

void capture_post_process_target(ID3D11DeviceContext* context) {
    if (!g_inside_post_process.load(std::memory_order_acquire)
        || g_inside_distortion)
        return;
    ID3D11RenderTargetView* view = nullptr;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (!view)
        return;
    std::lock_guard<std::mutex> lock(g_post_target_mutex);
    release(g_last_post_process_view);
    g_last_post_process_view = view;
}

void draw_screen_effect_before_first_sprite(ID3D11DeviceContext* context) {
    if (!context || !g_swap_chain || g_inside_distortion)
        return;

    const std::uint64_t serial = g_present_serial.load(
        std::memory_order_acquire);
    if (g_ui_effect_serial == serial)
        return;

    Evaluated effect{};
    if (!evaluate(effect))
        return;

    ID3D11RenderTargetView* scene_view = nullptr;
    context->OMGetRenderTargets(1, &scene_view, nullptr);
    if (!scene_view)
        return;
    ID3D11Resource* scene_resource = nullptr;
    ID3D11Texture2D* scene_texture = nullptr;
    scene_view->GetResource(&scene_resource);
    if (scene_resource) {
        scene_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&scene_texture));
    }
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&back_buffer));
    const bool full_scene = scene_texture && back_buffer
        && scene_texture == back_buffer;
    release(back_buffer);
    release(scene_texture);
    release(scene_resource);
    if (!full_scene) {
        release(scene_view);
        return;
    }

    // Mark before drawing because the fullscreen effect itself reaches the
    // hooked Draw entry recursively.
    g_ui_effect_serial = serial;
    g_sprite_effect_drawn = true;
    static std::atomic<bool> logged{};
    if (!logged.exchange(true, std::memory_order_acq_rel))
        release_printf("[MM ScreenFX] pre-UI backbuffer boundary active\n");
    render_after_post_process(context, scene_view);
    release(scene_view);
}

ID3D11RenderTargetView* capture_final_scene_blit_target(
    ID3D11DeviceContext* context) {
    if (!context || !g_swap_chain || g_inside_distortion)
        return nullptr;
    const std::uint64_t serial = g_present_serial.load(
        std::memory_order_acquire);
    if (g_ui_effect_serial == serial)
        return nullptr;
    Evaluated effect{};
    if (!evaluate(effect))
        return nullptr;

    ID3D11RenderTargetView* target_view = nullptr;
    ID3D11ShaderResourceView* source_view = nullptr;
    context->OMGetRenderTargets(1, &target_view, nullptr);
    context->PSGetShaderResources(0, 1, &source_view);
    if (!target_view || !source_view) {
        release(target_view);
        release(source_view);
        return nullptr;
    }

    ID3D11Resource* target_resource = nullptr;
    ID3D11Resource* source_resource = nullptr;
    ID3D11Texture2D* target_texture = nullptr;
    ID3D11Texture2D* source_texture = nullptr;
    ID3D11Texture2D* back_buffer = nullptr;
    target_view->GetResource(&target_resource);
    source_view->GetResource(&source_resource);
    if (target_resource) {
        target_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&target_texture));
    }
    if (source_resource) {
        source_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&source_texture));
    }
    g_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&back_buffer));

    D3D11_TEXTURE2D_DESC target_desc{};
    D3D11_TEXTURE2D_DESC source_desc{};
    if (target_texture)
        target_texture->GetDesc(&target_desc);
    if (source_texture)
        source_texture->GetDesc(&source_desc);
    const bool final_scene_blit = target_texture && source_texture
        && back_buffer && target_texture == back_buffer
        && source_texture != target_texture
        && source_desc.Width == target_desc.Width
        && source_desc.Height == target_desc.Height;

    release(back_buffer);
    release(source_texture);
    release(target_texture);
    release(source_resource);
    release(target_resource);
    release(source_view);
    if (!final_scene_blit) {
        release(target_view);
        return nullptr;
    }
    return target_view;
}

void draw_screen_effect_after_final_scene_blit(
    ID3D11RenderTargetView* target_view) {
    if (!target_view)
        return;
    g_ui_effect_serial = g_present_serial.load(std::memory_order_acquire);
    g_sprite_effect_drawn = true;
    static std::atomic<bool> logged{};
    if (!logged.exchange(true, std::memory_order_acq_rel))
        release_printf("[MM ScreenFX] scene-only insertion after final 3D blit active\n");
    render_after_post_process(nullptr, target_view);
    release(target_view);
}

void trace_active_draw_target(ID3D11DeviceContext* context,
    const void* caller) {
    const bool sprite_draw = g_inside_sprite_pass
        || g_sprite_pass_depth.load(std::memory_order_acquire) != 0;
    if (!kTraceFullResDraws && !(kTraceSpritePassDraws && sprite_draw))
        return;
    if (!context || g_inside_distortion)
        return;
    Evaluated effect{};
    if (!evaluate(effect))
        return;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    context->PSGetShaderResources(0, 1, &srv);
    ID3D11Resource* rt_resource = nullptr;
    ID3D11Resource* src_resource = nullptr;
    ID3D11Texture2D* rt_texture = nullptr;
    ID3D11Texture2D* src_texture = nullptr;
    if (rtv) rtv->GetResource(&rt_resource);
    if (srv) srv->GetResource(&src_resource);
    if (rt_resource) rt_resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&rt_texture));
    if (src_resource) src_resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&src_texture));
    D3D11_TEXTURE2D_DESC rt_desc{};
    D3D11_TEXTURE2D_DESC src_desc{};
    if (rt_texture) rt_texture->GetDesc(&rt_desc);
    if (src_texture) src_texture->GetDesc(&src_desc);
    // The candidate scene-composite boundary is full-size.  Ignore the
    // preceding atlas/shadow/light passes so each active effect produces a
    // compact trace of the final chain only.
    if (!sprite_draw && (rt_desc.Width != 2560 || rt_desc.Height != 1440)) {
        release(src_texture); release(rt_texture);
        release(src_resource); release(rt_resource);
        release(srv); release(rtv);
        return;
    }
    const unsigned int index = g_draw_trace_effect_samples.fetch_add(1,
        std::memory_order_relaxed);
    if (index >= 64) {
        release(src_texture); release(rt_texture);
        release(src_resource); release(rt_resource);
        release(srv); release(rtv);
        return;
    }
    const auto caller_rva = caller && g_exe_base
        ? reinterpret_cast<std::uintptr_t>(caller) - g_exe_base : 0;
    ID3D11PixelShader* pixel_shader = nullptr;
    context->PSGetShader(&pixel_shader, nullptr, nullptr);
    if (sprite_draw) {
        release_printf("[MM ScreenFX SpriteDraw] #%u context_type=%d caller=%p rva=%08llX ps=%p rt=%p %ux%u src0=%p %ux%u",
            index, static_cast<int>(context->GetType()), caller,
            static_cast<unsigned long long>(caller_rva),
            static_cast<void*>(pixel_shader),
            static_cast<void*>(rt_texture), rt_desc.Width, rt_desc.Height,
            static_cast<void*>(src_texture), src_desc.Width, src_desc.Height);
    } else {
        release_printf("[MM ScreenFX FullResTrace] #%u caller=%p rva=%08llX ps=%p rt=%p %ux%u src0=%p %ux%u",
            index, caller, static_cast<unsigned long long>(caller_rva),
            static_cast<void*>(pixel_shader),
            static_cast<void*>(rt_texture), rt_desc.Width, rt_desc.Height,
            static_cast<void*>(src_texture), src_desc.Width, src_desc.Height);
    }
    ID3D11ShaderResourceView* extra_srvs[7]{};
    context->PSGetShaderResources(1, static_cast<UINT>(std::size(extra_srvs)),
        extra_srvs);
    for (size_t slot = 0; slot < std::size(extra_srvs); ++slot) {
        ID3D11Resource* extra_resource = nullptr;
        ID3D11Texture2D* extra_texture = nullptr;
        D3D11_TEXTURE2D_DESC extra_desc{};
        if (extra_srvs[slot]) extra_srvs[slot]->GetResource(&extra_resource);
        if (extra_resource) extra_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&extra_texture));
        if (extra_texture) extra_texture->GetDesc(&extra_desc);
        if (extra_texture)
            release_printf(" s%zu=%p %ux%u", slot + 1,
                static_cast<void*>(extra_texture), extra_desc.Width, extra_desc.Height);
        release(extra_texture); release(extra_resource); release(extra_srvs[slot]);
    }
    release_printf("\n");
    release(pixel_shader);
    // The immediate caller is MM+'s D3D command interpreter.  Capture the
    // frames above it for a small sample so we can identify the native batch
    // executor rather than guessing from D3D vtable slots.
    if (caller_rva == 0x002B89B9 || caller_rva == 0x002B89ED) {
        static std::atomic<unsigned int> stack_index{};
        const unsigned int stack_sample = stack_index.fetch_add(1,
            std::memory_order_relaxed);
        if (stack_sample < 12) {
            void* frames[16]{};
            const USHORT count = CaptureStackBackTrace(0,
                static_cast<DWORD>(std::size(frames)), frames, nullptr);
            release_printf("[MM ScreenFX DrawStack] #%u", stack_sample);
            for (USHORT i = 0; i < count; ++i) {
                const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
                const auto rva = address >= g_exe_base
                    ? address - g_exe_base : 0;
                release_printf(" %p[%08llX]", frames[i],
                    static_cast<unsigned long long>(rva));
            }
            release_printf("\n");
        }
    }
    release(src_texture); release(rt_texture);
    release(src_resource); release(rt_resource);
    release(srv); release(rtv);
}

void trace_sprite_pass_boundary(const char* phase) {
    if (!g_swap_chain || g_inside_distortion)
        return;
    Evaluated effect{};
    if (!evaluate(effect))
        return;
    const unsigned int sample = g_sprite_boundary_effect_samples.fetch_add(1,
        std::memory_order_relaxed);
    if (sample >= 12)
        return;

    ID3D11Device* device = nullptr;
    if (FAILED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    device->Release();
    device = nullptr;
    if (!context)
        return;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srvs[4]{};
    context->OMGetRenderTargets(1, &rtv, nullptr);
    context->PSGetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);

    ID3D11Resource* rt_resource = nullptr;
    ID3D11Texture2D* rt_texture = nullptr;
    D3D11_TEXTURE2D_DESC rt_desc{};
    if (rtv) rtv->GetResource(&rt_resource);
    if (rt_resource) rt_resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&rt_texture));
    if (rt_texture) rt_texture->GetDesc(&rt_desc);
    release_printf("[MM ScreenFX SpriteBoundary] #%u %s rt=%p %ux%u",
        sample, phase, static_cast<void*>(rt_texture), rt_desc.Width, rt_desc.Height);
    for (size_t slot = 0; slot < std::size(srvs); ++slot) {
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        if (srvs[slot]) srvs[slot]->GetResource(&resource);
        if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
        if (texture) texture->GetDesc(&desc);
        if (texture)
            release_printf(" s%zu=%p %ux%u", slot,
                static_cast<void*>(texture), desc.Width, desc.Height);
        release(texture);
        release(resource);
        release(srvs[slot]);
    }
    release_printf("\n");
    release(rt_texture);
    release(rt_resource);
    release(rtv);
    context->Release();
}

void render_before_sprite_pass() {
    if (!g_swap_chain || g_inside_distortion)
        return;
    ID3D11Device* device = nullptr;
    if (FAILED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    device->Release();
    device = nullptr;
    if (!context)
        return;
    // At the Sprite boundary the output RT is the screen-buffer carrier.
    // The completed 3D scene is the shared full-resolution source bound in
    // slots 1/2.  Slot 0 was tested and is an earlier transient input; native
    // composition did not consume the modified result from it.
    ID3D11ShaderResourceView* scene_srv = nullptr;
    context->PSGetShaderResources(1, 1, &scene_srv);
    ID3D11Resource* scene_resource = nullptr;
    ID3D11Texture2D* scene_texture = nullptr;
    ID3D11RenderTargetView* scene_rtv = nullptr;
    if (scene_srv) scene_srv->GetResource(&scene_resource);
    if (scene_resource) scene_resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&scene_texture));
    // The original swap-chain device reference is released above; obtain a
    // temporary reference from the source texture for RTV creation.
    if (scene_texture) scene_texture->GetDevice(&device);
    if (scene_texture && device)
        device->CreateRenderTargetView(scene_texture, nullptr, &scene_rtv);
    release(device);
    if (scene_rtv) {
        // D3D11 forbids a resource being bound as input and output in the
        // same pass.  Restore the exact s0 binding after our draw.
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(1, 1, &null_srv);
        static std::atomic<bool> logged{};
        if (!logged.exchange(true, std::memory_order_acq_rel))
            release_printf("[MM ScreenFX] processed shared 3D source s1 immediately before pass_sprite\n");
        render_after_post_process(context, scene_rtv);
        context->PSSetShaderResources(1, 1, &scene_srv);
    }
    release(scene_rtv);
    release(scene_texture);
    release(scene_resource);
    release(scene_srv);
    context->Release();
}

ID3D11RenderTargetView* capture_scene_composite_candidate(
    ID3D11DeviceContext* context, UINT vertex_count) {
    if (!kEnableSceneCompositeCandidate || !context || g_inside_distortion)
        return nullptr;
    // RenderDoc: the final post-process event is a fullscreen Draw(4).  It
    // samples a different full-resolution texture and writes the result to
    // the scene carrier which pass_adjust_screen consumes later.  Reject all
    // Sprite/AET draws (atlas inputs) before issuing our own fullscreen pass.
    if (vertex_count != 4)
        return nullptr;
    // Sprite/UI glyphs are also Draw(4).  Once this present has already
    // received its single scene composite, reject all of those calls before
    // taking the effect mutex or querying D3D state.  The prior code kept
    // doing PSGetShader/AddRef/Release for every UI quad while an effect was
    // active, which is exactly the frame-time spike visible in the capture.
    const std::uint64_t serial = g_present_serial.load(std::memory_order_acquire);
    if (g_ui_effect_serial == serial)
        return nullptr;
    Evaluated effect{};
    if (!evaluate(effect))
        return nullptr;

    // Once the final native composite shader has been identified, every
    // other Draw(4) in the frame can be discarded with a single COM getter.
    // The previous code queried three SRVs, their resources and descriptors
    // for all of them, which is needlessly expensive during strong effects.
    ID3D11PixelShader* known_shader = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        known_shader = g_rdc_final_scene_shader;
        if (known_shader)
            known_shader->AddRef();
    }
    if (known_shader) {
        ID3D11PixelShader* active_shader = nullptr;
        context->PSGetShader(&active_shader, nullptr, nullptr);
        const bool matches = active_shader == known_shader;
        release(active_shader);
        release(known_shader);
        if (!matches)
            return nullptr;

        ID3D11RenderTargetView* target = nullptr;
        context->OMGetRenderTargets(1, &target, nullptr);
        return target;
    }

    ID3D11RenderTargetView* target = nullptr;
    ID3D11ShaderResourceView* inputs[3]{};
    context->OMGetRenderTargets(1, &target, nullptr);
    context->PSGetShaderResources(0, static_cast<UINT>(std::size(inputs)), inputs);
    ID3D11Resource* target_resource = nullptr;
    ID3D11Texture2D* target_texture = nullptr;
    D3D11_TEXTURE2D_DESC target_desc{};
    if (target) target->GetResource(&target_resource);
    if (target_resource) target_resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&target_texture));
    if (target_texture) target_texture->GetDesc(&target_desc);

    D3D11_TEXTURE2D_DESC input_desc[3]{};
    for (size_t i = 0; i < std::size(inputs); ++i) {
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* texture = nullptr;
        if (inputs[i]) inputs[i]->GetResource(&resource);
        if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
        if (texture) texture->GetDesc(&input_desc[i]);
        release(texture); release(resource); release(inputs[i]);
    }
    ID3D11Texture2D* scene_carrier = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        scene_carrier = g_rdc_final_scene_texture;
        if (scene_carrier)
            scene_carrier->AddRef();
    }
    const bool matches_final_scene_composite = target && target_texture
        && scene_carrier && target_texture == scene_carrier
        && input_desc[0].Width == target_desc.Width
        && input_desc[0].Height == target_desc.Height
        && target_desc.Width >= 2 && target_desc.Height >= 2;
    release(scene_carrier);
    release(target_texture); release(target_resource);
    if (!matches_final_scene_composite) {
        release(target);
        return nullptr;
    }

    // This only runs once after a swap-chain/target reset.  Its cached shader
    // is then used by the fast path above on subsequent frames.
    ID3D11PixelShader* final_shader = nullptr;
    context->PSGetShader(&final_shader, nullptr, nullptr);
    if (final_shader) {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        if (!g_rdc_final_scene_shader) {
            g_rdc_final_scene_shader = final_shader;
            g_rdc_final_scene_shader->AddRef();
        }
    }
    release(final_shader);
    return target;
}

void finish_scene_composite_candidate(ID3D11DeviceContext* context,
    ID3D11RenderTargetView* target) {
    if (!target)
        return;
    // Do it once per present even if the renderer submits duplicate final
    // scene draws.
    static std::mutex mutex;
    static std::uint64_t observed_serial = 0;
    static unsigned int draw_count = 0;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const std::uint64_t serial = g_present_serial.load(std::memory_order_acquire);
        if (serial != observed_serial) {
            observed_serial = serial;
            draw_count = 0;
        }
        ready = ++draw_count == 1;
    }
    if (ready) {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true, std::memory_order_acq_rel))
            release_printf("[MM ScreenFX] RDC final post-process Draw(4) boundary active\n");
        render_after_post_process(context, target);
    }
    release(target);
}

void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext* context,
    UINT vertex_count, UINT start_vertex) {
    trace_active_draw_target(context, _ReturnAddress());
    capture_post_process_target(context);
    ID3D11RenderTargetView* scene_composite =
            capture_scene_composite_candidate(context, vertex_count);
    const bool fog_override = !g_inside_distortion
        && fog_depth_height_fix::begin_character_draw(context);
    g_draw(context, vertex_count, start_vertex);
    if (fog_override)
        fog_depth_height_fix::end_character_draw(context);
    finish_scene_composite_candidate(context, scene_composite);
}

void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext* context,
    UINT index_count, UINT start_index, INT base_vertex) {
    trace_active_draw_target(context, _ReturnAddress());
    capture_post_process_target(context);
    ID3D11RenderTargetView* scene_composite =
            capture_scene_composite_candidate(context, index_count);
    const bool fog_override = !g_inside_distortion
        && fog_depth_height_fix::begin_character_draw(context);
    g_draw_indexed(context, index_count, start_index, base_vertex);
    if (fog_override)
        fog_depth_height_fix::end_character_draw(context);
    finish_scene_composite_candidate(context, scene_composite);
}

void STDMETHODCALLTYPE hooked_draw_instanced(ID3D11DeviceContext* context,
    UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance) {
    trace_active_draw_target(context, _ReturnAddress());
    capture_post_process_target(context);
    ID3D11RenderTargetView* scene_composite =
            capture_scene_composite_candidate(context, vertex_count);
    const bool fog_override = !g_inside_distortion
        && fog_depth_height_fix::begin_character_draw(context);
    g_draw_instanced(context, vertex_count, instance_count, start_vertex, start_instance);
    if (fog_override)
        fog_depth_height_fix::end_character_draw(context);
    finish_scene_composite_candidate(context, scene_composite);
}

void STDMETHODCALLTYPE hooked_draw_indexed_instanced(ID3D11DeviceContext* context,
    UINT index_count, UINT instance_count, UINT start_index, INT base_vertex,
    UINT start_instance) {
    trace_active_draw_target(context, _ReturnAddress());
    capture_post_process_target(context);
    ID3D11RenderTargetView* scene_composite =
            capture_scene_composite_candidate(context, index_count);
    const bool fog_override = !g_inside_distortion
        && fog_depth_height_fix::begin_character_draw(context);
    g_draw_indexed_instanced(context, index_count, instance_count, start_index,
        base_vertex, start_instance);
    if (fog_override)
        fog_depth_height_fix::end_character_draw(context);
    finish_scene_composite_candidate(context, scene_composite);
}

void trace_sprite_command_list_execution(ID3D11DeviceContext* context,
    ID3D11CommandList* command_list) {
    if (!context || !command_list)
        return;
    Evaluated effect{};
    if (!evaluate(effect))
        return;
    const unsigned int sample = g_sprite_boundary_effect_samples.fetch_add(1,
        std::memory_order_relaxed);
    if (sample >= 16)
        return;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srvs[3]{};
    context->OMGetRenderTargets(1, &rtv, nullptr);
    context->PSGetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
    ID3D11Resource* rt_resource = nullptr;
    ID3D11Texture2D* rt_texture = nullptr;
    D3D11_TEXTURE2D_DESC rt_desc{};
    if (rtv) rtv->GetResource(&rt_resource);
    if (rt_resource) rt_resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&rt_texture));
    if (rt_texture) rt_texture->GetDesc(&rt_desc);
    release_printf("[MM ScreenFX SpriteCommandList] #%u list=%p context_type=%d rt=%p %ux%u",
        sample, static_cast<void*>(command_list), static_cast<int>(context->GetType()),
        static_cast<void*>(rt_texture), rt_desc.Width, rt_desc.Height);
    for (size_t slot = 0; slot < std::size(srvs); ++slot) {
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        if (srvs[slot]) srvs[slot]->GetResource(&resource);
        if (resource) resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
        if (texture) texture->GetDesc(&desc);
        if (texture)
            release_printf(" s%zu=%p %ux%u", slot,
                static_cast<void*>(texture), desc.Width, desc.Height);
        release(texture); release(resource); release(srvs[slot]);
    }
    release_printf("\n");
    release(rt_texture); release(rt_resource); release(rtv);
}

HRESULT STDMETHODCALLTYPE hooked_finish_command_list(
    ID3D11DeviceContext* context, BOOL restore_deferred_context_state,
    ID3D11CommandList** command_list) {
    const HRESULT result = g_finish_command_list(context,
        restore_deferred_context_state, command_list);
    if (SUCCEEDED(result) && command_list && *command_list
        && g_sprite_pass_depth.load(std::memory_order_acquire) != 0) {
        std::lock_guard<std::mutex> lock(g_tagged_command_list_mutex);
        g_sprite_command_lists.insert(*command_list);
        release_printf("[MM ScreenFX SpriteCommandList] recorded list=%p context_type=%d\n",
            static_cast<void*>(*command_list), static_cast<int>(context->GetType()));
    }
    return result;
}

void STDMETHODCALLTYPE hooked_execute_command_list(ID3D11DeviceContext* context,
    ID3D11CommandList* command_list, BOOL restore_context_state) {
    bool is_sprite_list = false;
    {
        std::lock_guard<std::mutex> lock(g_tagged_command_list_mutex);
        const auto it = g_sprite_command_lists.find(command_list);
        if (it != g_sprite_command_lists.end()) {
            is_sprite_list = true;
            g_sprite_command_lists.erase(it);
        }
    }

    if (is_sprite_list)
        trace_sprite_command_list_execution(context, command_list);

    // Optional production path; disabled in this trace build.
    if (is_sprite_list && kEnableTaggedSpriteListInsertion
        && !g_inside_distortion) {
        ID3D11RenderTargetView* scene_view = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_post_target_mutex);
            scene_view = g_adjust_screen_view;
            if (scene_view)
                scene_view->AddRef();
        }
        if (!scene_view)
            context->OMGetRenderTargets(1, &scene_view, nullptr);
        if (scene_view) {
            const std::uint64_t serial = g_present_serial.load(
                std::memory_order_acquire);
            g_ui_effect_serial = serial;
            g_sprite_effect_drawn = true;
            static std::atomic<bool> logged{};
            if (!logged.exchange(true, std::memory_order_acq_rel))
                release_printf("[MM ScreenFX] tagged Sprite command-list boundary active\n");
            static std::atomic<bool> native_logged{};
            if (!native_logged.exchange(true, std::memory_order_acq_rel))
                release_printf("[MM ScreenFX] native command-list 3D/UI boundary active\n");
            render_after_post_process(context, scene_view);
            scene_view->Release();
        }
    }
    g_execute_command_list(context, command_list, restore_context_state);
    g_command_lists.fetch_add(1, std::memory_order_relaxed);
}

bool install_context_hooks(ID3D11DeviceContext* context) {
    if (g_context_hooks_installed)
        return true;
    void** vtable = *reinterpret_cast<void***>(context);
    g_immediate_draw_indexed_entry = vtable[12];
    g_immediate_draw_entry = vtable[13];
    g_immediate_draw_indexed_instanced_entry = vtable[20];
    g_immediate_draw_instanced_entry = vtable[21];
    g_draw_indexed = reinterpret_cast<DrawIndexedFn>(vtable[12]);
    g_draw = reinterpret_cast<DrawFn>(vtable[13]);
    g_draw_indexed_instanced = reinterpret_cast<DrawIndexedInstancedFn>(vtable[20]);
    g_draw_instanced = reinterpret_cast<DrawInstancedFn>(vtable[21]);
    g_execute_command_list = reinterpret_cast<ExecuteCommandListFn>(vtable[57]);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG result = DetourAttach(reinterpret_cast<PVOID*>(&g_draw_indexed),
        reinterpret_cast<PVOID>(hooked_draw_indexed));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_draw),
            reinterpret_cast<PVOID>(hooked_draw));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_draw_indexed_instanced),
            reinterpret_cast<PVOID>(hooked_draw_indexed_instanced));
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_draw_instanced),
            reinterpret_cast<PVOID>(hooked_draw_instanced));
    if (result == NO_ERROR && (!kTraceNativeDrawCallersOnly
            || kTraceSpriteCommandLists))
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_execute_command_list),
            reinterpret_cast<PVOID>(hooked_execute_command_list));
    const LONG commit = result == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_context_hooks_installed = result == NO_ERROR && commit == NO_ERROR;
    debug_log::line(g_context_hooks_installed
        ? L"Fog character Draw hooks installed"
        : L"Fog character Draw hooks failed to install");
    return g_context_hooks_installed;
}

void STDMETHODCALLTYPE hooked_deferred_draw(ID3D11DeviceContext* context,
    UINT vertex_count, UINT start_vertex) {
    capture_post_process_target(context);
    g_deferred_draw(context, vertex_count, start_vertex);
}

void STDMETHODCALLTYPE hooked_deferred_draw_indexed(ID3D11DeviceContext* context,
    UINT index_count, UINT start_index, INT base_vertex) {
    capture_post_process_target(context);
    g_deferred_draw_indexed(context, index_count, start_index, base_vertex);
}

void STDMETHODCALLTYPE hooked_deferred_draw_instanced(ID3D11DeviceContext* context,
    UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance) {
    capture_post_process_target(context);
    g_deferred_draw_instanced(context, vertex_count, instance_count,
        start_vertex, start_instance);
}

void STDMETHODCALLTYPE hooked_deferred_draw_indexed_instanced(
    ID3D11DeviceContext* context, UINT index_count, UINT instance_count,
    UINT start_index, INT base_vertex, UINT start_instance) {
    capture_post_process_target(context);
    g_deferred_draw_indexed_instanced(context, index_count, instance_count,
        start_index, base_vertex, start_instance);
}

bool install_deferred_context_hooks(ID3D11Device* device) {
    if (g_deferred_context_hooks_installed)
        return true;
    ID3D11DeviceContext* deferred = nullptr;
    if (!device || FAILED(device->CreateDeferredContext(0, &deferred)) || !deferred) {
        debug_log::line(L"Screen distortion: could not create deferred context probe");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(deferred);
    g_deferred_draw_indexed = reinterpret_cast<DrawIndexedFn>(vtable[12]);
    g_deferred_draw = reinterpret_cast<DrawFn>(vtable[13]);
    g_deferred_draw_indexed_instanced =
        reinterpret_cast<DrawIndexedInstancedFn>(vtable[20]);
    g_deferred_draw_instanced = reinterpret_cast<DrawInstancedFn>(vtable[21]);
    g_finish_command_list = reinterpret_cast<FinishCommandListFn>(vtable[75]);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG result = NO_ERROR;
    if (vtable[12] != g_immediate_draw_indexed_entry)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_deferred_draw_indexed),
            reinterpret_cast<PVOID>(hooked_deferred_draw_indexed));
    if (result == NO_ERROR && vtable[13] != g_immediate_draw_entry)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_deferred_draw),
            reinterpret_cast<PVOID>(hooked_deferred_draw));
    if (result == NO_ERROR && vtable[20] != g_immediate_draw_indexed_instanced_entry)
        result = DetourAttach(
            reinterpret_cast<PVOID*>(&g_deferred_draw_indexed_instanced),
            reinterpret_cast<PVOID>(hooked_deferred_draw_indexed_instanced));
    if (result == NO_ERROR && vtable[21] != g_immediate_draw_instanced_entry)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_deferred_draw_instanced),
            reinterpret_cast<PVOID>(hooked_deferred_draw_instanced));
    if (result == NO_ERROR && (!kTraceNativeDrawCallersOnly
            || kTraceSpriteCommandLists))
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_finish_command_list),
            reinterpret_cast<PVOID>(hooked_finish_command_list));
    const LONG commit = result == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_deferred_context_hooks_installed = result == NO_ERROR && commit == NO_ERROR;
    deferred->Release();
    debug_log::line(g_deferred_context_hooks_installed
        ? L"Screen distortion: deferred-context Draw hooks installed"
        : L"Screen distortion: deferred-context Draw hooks failed");
    return g_deferred_context_hooks_installed;
}

} // namespace

void reset(std::int64_t song_time) {
    g_command21_effect_samples.store(0, std::memory_order_release);
    g_draw_trace_effect_samples.store(0, std::memory_order_release);
    g_sprite_boundary_effect_samples.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_transition = {};
    g_transition.scale_start = 5.0f;
    g_transition.scale_end = 5.0f;
    g_transition.start_time = song_time;
    g_song_time = song_time;
    g_playing = false;
    g_transition_active = false;
    g_pjsk_distortion = {};
    g_pjsk_chromatic = {};
    g_pjsk_overlay = {};
}

void set_effect_textures(const std::vector<std::filesystem::path>& paths) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_texture_paths = paths;
    if (g_device)
        load_effect_views(g_device);
    // The next prewarm pass must sample the newly configured image set, even
    // when the generic shader pipeline was already warmed in a menu frame.
    g_effect_textures_warmed = false;
    debug_log::line(L"PJSK post effects: configured "
        + std::to_wstring(paths.size()) + L" texture(s)");
}

void update_time(bool playing, std::int64_t song_time) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_playing = playing;
    g_song_time = song_time;
}

void begin_noise(const std::array<std::int32_t, 7>& values,
    std::int64_t scheduled_time) {
    g_command21_effect_samples.store(0, std::memory_order_release);
    g_draw_trace_effect_samples.store(0, std::memory_order_release);
    g_sprite_boundary_effect_samples.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_transition.intensity_start = static_cast<float>(values[0]) / 1000000.0f;
    g_transition.intensity_end = static_cast<float>(values[1]) / 1000000.0f;
    g_transition.scale_start = std::max(static_cast<float>(values[2]) / 1000.0f,
        0.001f);
    g_transition.scale_end = std::max(static_cast<float>(values[3]) / 1000.0f,
        0.001f);
    g_transition.offset_start = static_cast<float>(values[4]) / 1000000.0f;
    g_transition.offset_end = static_cast<float>(values[5]) / 1000000.0f;
    g_transition.start_time = scheduled_time;
    g_transition.duration = static_cast<std::int64_t>(std::max(values[6], 0))
        * 1000000000LL / 60LL;
    g_transition_active = g_transition.duration > 0;
    g_playing = true;
    g_noise_events.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> dispatched{};
    if (!dispatched.exchange(true, std::memory_order_acq_rel))
        release_printf("[MM ScreenFX] dispatch NOISE time=%lld\n",
            static_cast<long long>(scheduled_time));
    debug_log::line(L"Screen distortion: NOISE intensity "
        + std::to_wstring(values[0]) + L" -> " + std::to_wstring(values[1])
        + L", frames=" + std::to_wstring(values[6]));
}

void begin_pjsk_distortion(const std::array<std::int32_t, 12>& v,
    std::int64_t scheduled_time) {
    g_command21_effect_samples.store(0, std::memory_order_release);
    g_draw_trace_effect_samples.store(0, std::memory_order_release);
    g_sprite_boundary_effect_samples.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& p = g_pjsk_distortion;
    p = {};
    p.start[0] = v[0] / 1000000.0f; p.end[0] = v[1] / 1000000.0f;
    p.start[1] = v[2] / 1000.0f; p.end[1] = v[3] / 1000.0f;
    p.start[2] = v[4] / 1000000.0f; p.end[2] = v[5] / 1000000.0f;
    p.texture = v[6];
    p.start[3] = v[7] / 1000.0f; p.start[4] = v[8] / 1000.0f;
    p.start[5] = v[9] / 1000.0f; p.start[6] = v[10] / 1000.0f;
    p.start_time = scheduled_time;
    p.duration = static_cast<std::int64_t>(std::max(v[11], 0)) * 1000000000LL / 60LL;
    p.active = p.duration > 0; g_playing = true;
    static std::atomic<bool> dispatched{};
    if (!dispatched.exchange(true, std::memory_order_acq_rel))
        release_printf("[MM ScreenFX] dispatch DISTORTION time=%lld\n",
            static_cast<long long>(scheduled_time));
}

void begin_pjsk_chromatic(const std::array<std::int32_t, 20>& v,
    std::int64_t scheduled_time) {
    g_command21_effect_samples.store(0, std::memory_order_release);
    g_draw_trace_effect_samples.store(0, std::memory_order_release);
    g_sprite_boundary_effect_samples.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& p = g_pjsk_chromatic; p = {}; p.mode = v[0];
    for (size_t i = 0; i < 6; ++i) {
        p.start[i] = v[1 + i] / 1000000.0f;
        p.end[i] = v[7 + i] / 1000000.0f;
    }
    for (size_t i = 0; i < 3; ++i) {
        p.start[6 + i] = v[13 + i] / 1000000.0f;
        p.end[6 + i] = v[16 + i] / 1000000.0f;
    }
    p.start_time = scheduled_time;
    p.duration = static_cast<std::int64_t>(std::max(v[19], 0)) * 1000000000LL / 60LL;
    p.active = p.duration > 0; g_playing = true;
    static std::atomic<bool> dispatched{};
    if (!dispatched.exchange(true, std::memory_order_acq_rel))
        release_printf("[MM ScreenFX] dispatch CHROMATIC time=%lld\n",
            static_cast<long long>(scheduled_time));
}

void begin_pjsk_overlay(const std::array<std::int32_t, 9>& v,
    std::int64_t scheduled_time) {
    g_command21_effect_samples.store(0, std::memory_order_release);
    g_draw_trace_effect_samples.store(0, std::memory_order_release);
    g_sprite_boundary_effect_samples.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& p = g_pjsk_overlay; p = {}; p.texture = v[0];
    p.start[0] = v[1] / 1000000.0f; p.end[0] = v[2] / 1000000.0f;
    p.mode = v[3];
    for (size_t i = 0; i < 4; ++i) p.start[i + 1] = v[4 + i] / 1000000.0f;
    p.start_time = scheduled_time;
    p.duration = static_cast<std::int64_t>(std::max(v[8], 0)) * 1000000000LL / 60LL;
    p.active = p.duration > 0; g_playing = true;
    static std::atomic<bool> dispatched{};
    if (!dispatched.exchange(true, std::memory_order_acq_rel))
        release_printf("[MM ScreenFX] dispatch OVERLAY time=%lld\n",
            static_cast<long long>(scheduled_time));
}

namespace {

void draw_distortion(ID3D11DeviceContext* context, const Evaluated& effect,
    ID3D11RenderTargetView* target_override) {
    IDXGISwapChain* swap_chain = g_swap_chain;
    if (!swap_chain || !context)
        return;

    // The device and final scene carrier are already retained by on_frame's
    // prewarm path.  Avoid DXGI GetDevice + RTV->Resource->Texture queries on
    // every active effect frame; they showed up as avoidable CPU bubbles.
    ID3D11Device* device = g_device;
    if (device)
        device->AddRef();
    else if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device),
        reinterpret_cast<void**>(&device))) || !device)
        return;
    if (!ensure_device_resources(device)) {
        device->Release();
        return;
    }

    ID3D11RenderTargetView* old_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* old_dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
        old_rtv, &old_dsv);
    ID3D11RenderTargetView* target_view = target_override
        ? target_override : old_rtv[0];
    // Once prewarmed, g_back_buffer is precisely the target verified by the
    // RDC final-composite hook.  Reusing it avoids a resource query and the
    // associated driver synchronization for every frame of an effect.
    const bool cached_target_ready = target_override && g_back_buffer
        && g_scene_copy && g_scene_view && g_back_buffer_view;
    ID3D11Texture2D* target_texture = nullptr;
    if (!cached_target_ready) {
        ID3D11Resource* target_resource = nullptr;
        if (target_view)
            target_view->GetResource(&target_resource);
        if (target_resource)
            target_resource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&target_texture));
        release(target_resource);
    }
    if ((!cached_target_ready && (!target_texture
            || !ensure_targets(target_texture, target_view, device)))) {
        static std::atomic<bool> failed_logged{};
        if (!failed_logged.exchange(true, std::memory_order_acq_rel))
            release_printf("[MM ScreenFX] fullscreen pass target setup failed context_type=%d\n",
                static_cast<int>(context->GetType()));
        release(target_texture);
        for (auto*& view : old_rtv) release(view);
        release(old_dsv);
        device->Release();
        return;
    }
    g_valid_targets.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> target_logged{};
    if (!target_logged.exchange(true, std::memory_order_acq_rel)) {
        D3D11_TEXTURE2D_DESC target_desc{};
        (cached_target_ready ? g_back_buffer : target_texture)->GetDesc(&target_desc);
        release_printf("[MM ScreenFX] fullscreen pass recorded context_type=%d target=%ux%u\n",
            static_cast<int>(context->GetType()), target_desc.Width,
            target_desc.Height);
    }
    release(target_texture);
    ID3D11VertexShader* old_vs = nullptr;
    ID3D11PixelShader* old_ps = nullptr;
    ID3D11GeometryShader* old_gs = nullptr;
    ID3D11HullShader* old_hs = nullptr;
    ID3D11DomainShader* old_ds = nullptr;
    context->VSGetShader(&old_vs, nullptr, nullptr);
    context->PSGetShader(&old_ps, nullptr, nullptr);
    context->GSGetShader(&old_gs, nullptr, nullptr);
    context->HSGetShader(&old_hs, nullptr, nullptr);
    context->DSGetShader(&old_ds, nullptr, nullptr);
    ID3D11ShaderResourceView* old_srvs[3]{};
    ID3D11SamplerState* old_sampler = nullptr;
    ID3D11Buffer* old_cb = nullptr;
    context->PSGetShaderResources(0, 3, old_srvs);
    context->PSGetSamplers(0, 1, &old_sampler);
    context->PSGetConstantBuffers(0, 1, &old_cb);
    ID3D11InputLayout* old_layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY old_topology{};
    context->IAGetInputLayout(&old_layout);
    context->IAGetPrimitiveTopology(&old_topology);
    ID3D11BlendState* old_blend = nullptr;
    FLOAT old_blend_factor[4]{};
    UINT old_sample_mask = 0;
    context->OMGetBlendState(&old_blend, old_blend_factor, &old_sample_mask);
    ID3D11DepthStencilState* old_depth = nullptr;
    UINT old_stencil_ref = 0;
    context->OMGetDepthStencilState(&old_depth, &old_stencil_ref);
    ID3D11RasterizerState* old_raster = nullptr;
    context->RSGetState(&old_raster);
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT old_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&viewport_count, old_viewports);

    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(g_scene_copy, g_back_buffer);
    context->OMSetRenderTargets(1, &g_back_buffer_view, nullptr);
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(g_width),
        static_cast<float>(g_height), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vertex_shader, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_pixel_shader, nullptr, 0);
    context->UpdateSubresource(g_constants, 0, nullptr, &effect.constants, 0, 0);
    context->PSSetConstantBuffers(0, 1, &g_constants);
    ID3D11ShaderResourceView* srvs[3]{ g_scene_view, nullptr, nullptr };
    if (effect.noise_texture >= 0 && static_cast<size_t>(effect.noise_texture) < g_effect_views.size())
        srvs[1] = g_effect_views[effect.noise_texture];
    if (effect.overlay_texture >= 0 && static_cast<size_t>(effect.overlay_texture) < g_effect_views.size())
        srvs[2] = g_effect_views[effect.overlay_texture];
    context->PSSetShaderResources(0, 3, srvs);
    context->PSSetSamplers(0, 1, &g_sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* null_srvs[3]{};
    context->PSSetShaderResources(0, 3, null_srvs);

    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
        old_rtv, old_dsv);
    context->RSSetViewports(viewport_count, old_viewports);
    context->OMSetBlendState(old_blend, old_blend_factor, old_sample_mask);
    context->OMSetDepthStencilState(old_depth, old_stencil_ref);
    context->RSSetState(old_raster);
    context->IASetInputLayout(old_layout);
    context->IASetPrimitiveTopology(old_topology);
    context->VSSetShader(old_vs, nullptr, 0);
    context->GSSetShader(old_gs, nullptr, 0);
    context->HSSetShader(old_hs, nullptr, 0);
    context->DSSetShader(old_ds, nullptr, 0);
    context->PSSetShader(old_ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &old_cb);
    context->PSSetShaderResources(0, 3, old_srvs);
    context->PSSetSamplers(0, 1, &old_sampler);

    for (auto*& view : old_rtv) release(view);
    release(old_dsv);
    release(old_vs); release(old_ps); release(old_gs); release(old_hs); release(old_ds);
    for (auto*& srv : old_srvs) release(srv);
    release(old_sampler); release(old_cb); release(old_layout);
    release(old_blend); release(old_depth); release(old_raster);
    device->Release();
}

void render_after_post_process(ID3D11DeviceContext* supplied_context,
    ID3D11RenderTargetView* post_process_view) {
    Evaluated effect{};
    if (!g_swap_chain || !post_process_view
        || !evaluate(effect))
        return;
    g_active_passes.fetch_add(1, std::memory_order_relaxed);

    ID3D11Device* device = nullptr;
    if (FAILED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
        reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context = supplied_context;
    if (context)
        context->AddRef();
    else
        device->GetImmediateContext(&context);
    if (context) {
        g_inside_distortion = true;
        g_distortion_draws.fetch_add(1, std::memory_order_relaxed);
        draw_distortion(context, effect, post_process_view);
        g_inside_distortion = false;
        context->Release();
    }
    device->Release();
}

void distort_after_post_process_command_list(ID3D11DeviceContext* context) {
    if (!context || g_inside_distortion)
        return;

    ID3D11Texture2D* scene_texture = nullptr;
    ID3D11RenderTargetView* scene_view = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        scene_texture = g_adjust_screen_texture;
        scene_view = g_adjust_screen_view;
        if (scene_texture)
            scene_texture->AddRef();
        if (scene_view)
            scene_view->AddRef();
    }
    if (!scene_texture || !scene_view) {
        release(scene_texture);
        release(scene_view);
        return;
    }

    // A post-process command list ends with Resource 49 as its output and a
    // different, full-resolution render texture in PS t0.  Sprite also writes
    // Resource 49, but its t0 is the 1024x512 UI atlas, so it is excluded.
    ID3D11RenderTargetView* current_rtv = nullptr;
    context->OMGetRenderTargets(1, &current_rtv, nullptr);
    ID3D11Resource* current_resource = nullptr;
    ID3D11Texture2D* current_texture = nullptr;
    if (current_rtv)
        current_rtv->GetResource(&current_resource);
    if (current_resource) {
        current_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&current_texture));
    }
    release(current_resource);

    ID3D11ShaderResourceView* input_view = nullptr;
    ID3D11Resource* input_resource = nullptr;
    ID3D11Texture2D* input_texture = nullptr;
    context->PSGetShaderResources(0, 1, &input_view);
    if (input_view)
        input_view->GetResource(&input_resource);
    if (input_resource) {
        input_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&input_texture));
    }
    release(input_resource);
    release(input_view);

    bool is_post_process_end = current_texture == scene_texture
        && input_texture && input_texture != scene_texture;
    if (is_post_process_end) {
        D3D11_TEXTURE2D_DESC scene_desc{};
        D3D11_TEXTURE2D_DESC input_desc{};
        scene_texture->GetDesc(&scene_desc);
        input_texture->GetDesc(&input_desc);
        is_post_process_end = input_desc.Width == scene_desc.Width
            && input_desc.Height == scene_desc.Height
            && (input_desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0
            && (input_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    }

    release(current_rtv);
    release(current_texture);
    release(input_texture);
    release(scene_texture);

    if (is_post_process_end) {
        const std::uint64_t serial = g_present_serial.load(
            std::memory_order_acquire);
        if (g_ui_effect_serial == serial) {
            release(scene_view);
            return;
        }
        g_ui_effect_serial = serial;
        g_sprite_effect_drawn = true;
        g_post_insertions.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<bool> logged{};
        if (!logged.exchange(true, std::memory_order_acq_rel))
            release_printf("[MM ScreenFX] native 3D/UI command-list boundary active\n");
        render_after_post_process(context, scene_view);
    }
    release(scene_view);
}

void __fastcall hooked_pass_sprite(void* render_data_context,
    void* render_pass_data) {
    // This is the exact MM+ counterpart of AFT render_single_pass::SPRITE:
    // post processing has completed, while Sprite/UI has not drawn yet.
    g_sprite_calls.fetch_add(1, std::memory_order_relaxed);
    g_inside_sprite_pass = true;
    g_sprite_pass_depth.fetch_add(1, std::memory_order_acq_rel);

    if (kTraceSpritePassBoundary)
        trace_sprite_pass_boundary("before");
    if (kEnableSpritePassInsertion)
        render_before_sprite_pass();

    g_pass_sprite(render_data_context, render_pass_data);
    if (kTraceSpritePassBoundary)
        trace_sprite_pass_boundary("after");
    g_sprite_pass_depth.fetch_sub(1, std::memory_order_acq_rel);
    g_inside_sprite_pass = false;
}

void __fastcall hooked_pass_adjust_screen(void* render_data_context,
    void* render_pass_data) {
    g_pass_adjust_screen(render_data_context, render_pass_data);

    // pass_adjust_screen has copied the completed 3D image into MM+'s final
    // output target.  Composite plugin ScreenFX here, before later UI layers.
    if (g_swap_chain && !g_inside_distortion) {
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
                reinterpret_cast<void**>(&device))) && device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                ID3D11RenderTargetView* final_scene_view = nullptr;
                context->OMGetRenderTargets(1, &final_scene_view, nullptr);
                if (final_scene_view) {
                    render_after_post_process(context, final_scene_view);
                    final_scene_view->Release();
                }
                context->Release();
            }
            device->Release();
        }
    }
}

bool install_adjust_screen_hook() {
    if (g_adjust_screen_hook_installed)
        return true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_pass_adjust_screen = reinterpret_cast<PassAdjustScreenFn>(
        base + kPassAdjustScreenRva);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(
        reinterpret_cast<PVOID*>(&g_pass_adjust_screen),
        reinterpret_cast<PVOID>(hooked_pass_adjust_screen));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_adjust_screen_hook_installed = attached == NO_ERROR
        && committed == NO_ERROR;
    release_printf("[MM ScreenFX] pass_adjust_screen hook %s (%ld/%ld)\n",
        g_adjust_screen_hook_installed ? "installed" : "failed",
        attached, committed);
    return g_adjust_screen_hook_installed;
}

void __fastcall hooked_pass_post_process(void* render_data_context,
    void* render_pass_data) {
    g_inside_post_process.store(true, std::memory_order_release);
    g_pass_post_process(render_data_context, render_pass_data);
    g_inside_post_process.store(false, std::memory_order_release);

    // MM+ records most post-process Draw calls on deferred contexts before
    // this pass executes, so Draw hooks cannot reliably observe them while
    // g_inside_post_process is true.  At return, however, the immediate
    // context is bound to the completed 3D target.  Capture it directly.
    if (g_swap_chain) {
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
                reinterpret_cast<void**>(&device))) && device) {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            if (context) {
                // The OM binding at this CPU callback is not reliable: MM+
                // may already have restored its command-builder state.  The
                // texture sampled by native ADJUST_SCREEN at PS t0 is the
                // authoritative screen_buffer that receives both 3D and,
                // later, Sprite/UI.
                ID3D11RenderTargetView* view = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_post_target_mutex);
                    view = g_adjust_screen_view;
                    if (view)
                        view->AddRef();
                }
                if (view) {
                    g_captured_post_targets.fetch_add(1,
                        std::memory_order_relaxed);

                    // Exact MM+ order (confirmed from 0x1404D87E0's pass
                    // table): POST_PROCESS (12) has just returned and
                    // SPRITE/UI (13) has not begun yet.  Draw into the
                    // completed post-process target here so the effect is a
                    // native part of the captured game frame.
                    Evaluated effect{};
                    if (evaluate(effect)) {
                        static std::atomic<bool> logged{};
                        if (!logged.exchange(true, std::memory_order_acq_rel)) {
                            ID3D11Resource* resource = nullptr;
                            ID3D11Texture2D* texture = nullptr;
                            view->GetResource(&resource);
                            if (resource)
                                resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&texture));
                            D3D11_TEXTURE2D_DESC desc{};
                            if (texture)
                                texture->GetDesc(&desc);
                            release_printf("[MM ScreenFX] native POST_PROCESS return boundary active target=%ux%u\n",
                                desc.Width, desc.Height);
                            release(texture);
                            release(resource);
                        }
                        render_after_post_process(context, view);
                    }
                    view->Release();
                }
                context->Release();
            }
            device->Release();
        }
    }
}

bool install_post_process_hook() {
    if (g_post_process_hook_installed)
        return true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_pass_post_process = reinterpret_cast<PassPostProcessFn>(
        base + kPassPostProcessRva);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(
        reinterpret_cast<PVOID*>(&g_pass_post_process),
        reinterpret_cast<PVOID>(hooked_pass_post_process));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_post_process_hook_installed = attached == NO_ERROR
        && committed == NO_ERROR;
    release_printf("[MM ScreenFX] native POST_PROCESS hook %s (%ld/%ld)\n",
        g_post_process_hook_installed ? "installed" : "failed",
        attached, committed);
    return g_post_process_hook_installed;
}

bool install_sprite_hook() {
    if (g_sprite_hook_installed)
        return true;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(base + kPassSpriteRva);
    static constexpr std::uint8_t expected[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9,
    };
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        debug_log::line(L"Screen distortion: MM+ pass_sprite signature mismatch");
        return false;
    }

    g_pass_sprite = reinterpret_cast<PassSpriteFn>(target);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(
        reinterpret_cast<PVOID*>(&g_pass_sprite),
        reinterpret_cast<PVOID>(hooked_pass_sprite));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_sprite_hook_installed = attached == NO_ERROR && committed == NO_ERROR;
    debug_log::line(g_sprite_hook_installed
        ? L"Screen distortion: hooked MM+ before pass_sprite"
        : L"Screen distortion: failed to hook MM+ pass_sprite");
    return g_sprite_hook_installed;
}

void __fastcall hooked_native_adjust_blit(void* quad, void* render_context,
    float depth, std::uint32_t height, std::uint32_t source_width,
    float source_height, float scale_x, float scale_y, float offset_x,
    float offset_y, const void* color) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_native_blit_calls.fetch_add(1, std::memory_order_relaxed);
    g_native_adjust_blit(quad, render_context, depth, height, source_width,
        source_height, scale_x, scale_y, offset_x, offset_y, color);

    // 0x49E750 is MM+'s pass_adjust_screen blit builder.  Its call to
    // 0x49DB50 occurs while the final 3D render target is still bound.  Draw
    // here, before the builder returns and before Sprite/UI changes targets.
    if (caller != g_exe_base + kNativeAdjustBlitReturnRva
        || !g_swap_chain || g_inside_distortion)
        return;
    g_native_blit_matches.fetch_add(1, std::memory_order_relaxed);
}

bool install_native_adjust_blit_hook() {
    if (g_native_adjust_blit_hook_installed)
        return true;
    g_exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(
        g_exe_base + kNativeAdjustBlitRva);
    static constexpr std::uint8_t expected[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C,
        0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83,
        0xEC, 0x50,
    };
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        release_printf("[MM ScreenFX] native adjust blit signature mismatch\n");
        return false;
    }
    g_native_adjust_blit = reinterpret_cast<NativeAdjustBlitFn>(target);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(
        reinterpret_cast<PVOID*>(&g_native_adjust_blit),
        reinterpret_cast<PVOID>(hooked_native_adjust_blit));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_native_adjust_blit_hook_installed = attached == NO_ERROR
        && committed == NO_ERROR;
    release_printf("[MM ScreenFX] native adjust blit hook %s (%ld/%ld)\n",
        g_native_adjust_blit_hook_installed ? "installed" : "failed",
        attached, committed);
    return g_native_adjust_blit_hook_installed;
}

// Opcode 0x15 is the command written by 0x14049DB50.  This is its actual
// execution handler in MM+'s command interpreter (0x1402B8370), not the
// earlier command-builder callback.  Trace only for now: several unrelated
// render features use opcode 0x15, so the command payload/RT pair identifies
// the exact adjust-screen instance before any effect is injected.
void __fastcall hooked_native_command21(void* renderer, void* render_data,
    void** command_cursor) {
    std::uint32_t header[6]{};
    if (command_cursor && *command_cursor)
        std::memcpy(header, *command_cursor, sizeof(header));
    g_native_command21(renderer, render_data, command_cursor);

    Evaluated active_effect{};
    if (!evaluate(active_effect) || !g_swap_chain)
        return;
    const unsigned int sample = g_command21_effect_samples.fetch_add(1,
        std::memory_order_relaxed);
    if (sample >= 48)
        return;
    ID3D11Device* device = nullptr;
    if (FAILED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11RenderTargetView* target = nullptr;
    ID3D11ShaderResourceView* source_view = nullptr;
    if (context)
        context->OMGetRenderTargets(1, &target, nullptr);
    if (context)
        context->PSGetShaderResources(0, 1, &source_view);
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11Resource* source_resource = nullptr;
    ID3D11Texture2D* source_texture = nullptr;
    if (target)
        target->GetResource(&resource);
    if (resource)
        resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
    if (source_view)
        source_view->GetResource(&source_resource);
    if (source_resource)
        source_resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&source_texture));
    D3D11_TEXTURE2D_DESC desc{};
    D3D11_TEXTURE2D_DESC source_desc{};
    if (texture)
        texture->GetDesc(&desc);
    if (source_texture)
        source_texture->GetDesc(&source_desc);
    release_printf("[MM ScreenFX Command21] #%u h=%08X,%08X,%08X,%08X,%08X,%08X rt=%p %ux%u src=%p %ux%u\n",
        sample, header[0], header[1], header[2], header[3], header[4], header[5],
        static_cast<void*>(texture), desc.Width, desc.Height,
        static_cast<void*>(source_texture), source_desc.Width, source_desc.Height);
    release(source_texture); release(source_resource); release(source_view);
    release(texture); release(resource); release(target);
    if (context) context->Release();
    device->Release();
}

bool install_native_command21_hook() {
    if (g_native_command21_hook_installed)
        return true;
    g_exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(g_exe_base + kNativeCommand21Rva);
    static constexpr std::uint8_t expected[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
        0xEC, 0x50, 0x48, 0x8B, 0x05,
    };
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        release_printf("[MM ScreenFX] native command21 signature mismatch\n");
        return false;
    }
    g_native_command21 = reinterpret_cast<NativeCommand21Fn>(target);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(reinterpret_cast<PVOID*>(&g_native_command21),
        reinterpret_cast<PVOID>(hooked_native_command21));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_native_command21_hook_installed = attached == NO_ERROR && committed == NO_ERROR;
    release_printf("[MM ScreenFX] native command21 hook %s (%ld/%ld)\n",
        g_native_command21_hook_installed ? "installed" : "failed", attached, committed);
    return g_native_command21_hook_installed;
}

void capture_rdc_final_scene_target() {
    if (!g_swap_chain || g_inside_distortion)
        return;
    // The scene carrier is stable for the lifetime of a swap chain.  Querying
    // OM state from the native render-event handler every frame is needless
    // and can force expensive driver bookkeeping on some systems.
    {
        std::lock_guard<std::mutex> lock(g_post_target_mutex);
        if (g_rdc_final_scene_texture)
            return;
    }
    ID3D11Device* device = nullptr;
    if (FAILED(g_swap_chain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    device->Release();
    if (!context)
        return;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    context->OMGetRenderTargets(1, &target, nullptr);
    if (target)
        target->GetResource(&resource);
    if (resource)
        resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
    D3D11_TEXTURE2D_DESC desc{};
    if (texture)
        texture->GetDesc(&desc);
    if (texture && desc.Width >= 2 && desc.Height >= 2
        && (desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0
        && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_post_target_mutex);
            changed = g_rdc_final_scene_texture != texture;
            if (changed) {
                release(g_rdc_final_scene_texture);
                g_rdc_final_scene_texture = texture;
                g_rdc_final_scene_texture->AddRef();
            }
        }
        if (changed) {
            release_printf("[MM ScreenFX] captured RDC final 3D target at pass_sprite: %ux%u\n",
                desc.Width, desc.Height);
        }
    }
    release(texture);
    release(resource);
    release(target);
    context->Release();
}

void __fastcall hooked_native_render_event(void* renderer, void* render_data,
    void** command_cursor) {
    // Dispatcher 0x2B8370 has already consumed the opcode.  The marker body
    // is [byte_length][NUL-terminated event name].
    const wchar_t* marker = nullptr;
    if (command_cursor && *command_cursor)
        marker = reinterpret_cast<const wchar_t*>(
            static_cast<const std::uint8_t*>(*command_cursor)
            + sizeof(std::uint32_t));
    // MMSubCamera owns no render hook.  It receives the already-proven native
    // pass boundary from this stable hook solely to identify the full 3D
    // render context; this callback performs no rendering and cannot alter
    // ScreenFX, UI, or the main camera.
    using SubCameraRenderEventFn = void(__cdecl*)(void*, void*, const wchar_t*);
    if (const HMODULE subcamera = GetModuleHandleW(L"MMSubCamera.dll")) {
        if (const auto callback = reinterpret_cast<SubCameraRenderEventFn>(
                GetProcAddress(subcamera, "MMSubCamera_OnRenderEvent")))
            callback(renderer, render_data, marker);
    }
    if (marker && std::wcsncmp(marker, L"pass_sprite", 11) == 0)
        capture_rdc_final_scene_target();
    g_native_render_event(renderer, render_data, command_cursor);
}

bool install_native_render_event_hook() {
    if (g_native_render_event_hook_installed)
        return true;
    g_exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<std::uint8_t*>(
        g_exe_base + kNativeRenderEventRva);
    static constexpr std::uint8_t expected[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x49, 0x8B, 0xD8, 0x4C, 0x8B, 0xC2,
        0x48, 0x8B, 0x3B,
    };
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        release_printf("[MM ScreenFX] native render-event signature mismatch\n");
        return false;
    }
    g_native_render_event = reinterpret_cast<NativeRenderEventFn>(target);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(
        reinterpret_cast<PVOID*>(&g_native_render_event),
        reinterpret_cast<PVOID>(hooked_native_render_event));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    g_native_render_event_hook_installed = attached == NO_ERROR
        && committed == NO_ERROR;
    release_printf("[MM ScreenFX] native render-event hook %s (%ld/%ld)\n",
        g_native_render_event_hook_installed ? "installed" : "failed",
        attached, committed);
    return g_native_render_event_hook_installed;
}

} // namespace

void initialize() {
    g_exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (kEnableNativeSpriteMarkerInsertion) {
        install_native_render_event_hook();
    } else if (!kEnableSceneCompositeCandidate && !kTraceSpritePassBoundary
        && !kEnableSpritePassInsertion && !kTraceSpritePassDraws)
        install_native_command21_hook();
    if (kTraceSpritePassBoundary || kEnableSpritePassInsertion
        || kTraceSpritePassDraws) {
        install_sprite_hook();
    } else if (!kTraceNativeDrawCallersOnly) {
        install_native_adjust_blit_hook();
        install_sprite_hook();
    }
}

void on_frame(IDXGISwapChain* swap_chain) {
    // DML calls this once at the presentation boundary.  The next frame's
    // first UI-atlas draw consumes this serial and inserts ScreenFX exactly
    // once before 2D UI.
    g_present_serial.fetch_add(1, std::memory_order_release);
    static unsigned int diagnostic_frame = 0;
    constexpr bool kPeriodicScreenFxDiagnostic = false;
    if (kPeriodicScreenFxDiagnostic && (++diagnostic_frame % 120) == 0) {
        const auto sprite = g_sprite_calls.exchange(0);
        const auto noise = g_noise_events.exchange(0);
        const auto active = g_active_passes.exchange(0);
        const auto captured = g_captured_post_targets.exchange(0);
        const auto adjust = g_captured_adjust_targets.exchange(0);
        const auto commands = g_command_lists.exchange(0);
        const auto insertions = g_post_insertions.exchange(0);
        const auto targets = g_valid_targets.exchange(0);
        const auto draws = g_distortion_draws.exchange(0);
        const auto blits = g_native_blit_calls.exchange(0);
        const auto matches = g_native_blit_matches.exchange(0);
        const auto blit_targets = g_native_blit_targets.exchange(0);
        release_printf("[MM ScreenFX] sprite=%llu noise=%llu active=%llu captured=%llu adjust=%llu cmd=%llu insert=%llu target=%llu draw=%llu blit=%llu/%llu/%llu\n",
            static_cast<unsigned long long>(sprite),
            static_cast<unsigned long long>(noise),
            static_cast<unsigned long long>(active),
            static_cast<unsigned long long>(captured),
            static_cast<unsigned long long>(adjust),
            static_cast<unsigned long long>(commands),
            static_cast<unsigned long long>(insertions),
            static_cast<unsigned long long>(targets),
            static_cast<unsigned long long>(draws),
            static_cast<unsigned long long>(blits),
            static_cast<unsigned long long>(matches),
            static_cast<unsigned long long>(blit_targets));
        if (debug_log::enabled())
            debug_log::line(L"Screen distortion state: sprite="
                + std::to_wstring(sprite) + L" noise=" + std::to_wstring(noise)
                + L" active=" + std::to_wstring(active)
                + L" captured=" + std::to_wstring(captured)
                + L" adjust=" + std::to_wstring(adjust)
                + L" cmd=" + std::to_wstring(commands)
                + L" insert=" + std::to_wstring(insertions)
                + L" target=" + std::to_wstring(targets)
                + L" draw=" + std::to_wstring(draws));
    }
    if (!swap_chain)
        return;

    if (g_swap_chain != swap_chain) {
        release(g_swap_chain);
        g_swap_chain = swap_chain;
        g_swap_chain->AddRef();
        release_targets();
        release_adjust_screen_target();
    }

    ID3D11Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device),
        reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context) {
        capture_adjust_screen_target(context, swap_chain, device);
        install_context_hooks(context);
        install_deferred_context_hooks(device);
        if (ensure_device_resources(device)
            && (!g_final_scene_target_prewarmed.load(std::memory_order_acquire)
                || !g_pipeline_warmed || !g_effect_textures_warmed)) {
            // This becomes available one frame after pass_sprite captures the
            // PV's final 3D carrier.  It shifts the large RT allocation and
            // SRV creation from the first scripted effect to PV startup.
            if (prewarm_final_scene_target(device))
                prewarm_effect_pipeline(device, context);
        }
        context->Release();
    }

    // Do not composite here: by the presentation boundary MM+ has already
    // drawn lyrics and HUD into the swap-chain buffer.  Applying distortion,
    // chromatic aberration or flash here necessarily modifies 2D UI as well.
    device->Release();
}

void on_resize() {
    release_targets();
    release_adjust_screen_target();
}

} // namespace screen_distortion
