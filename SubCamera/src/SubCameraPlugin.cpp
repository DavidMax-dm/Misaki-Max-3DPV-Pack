#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <detours.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

constexpr std::uint32_t kMegaMixTimestamp = 0x635B31AB;
constexpr std::uint32_t kMegaMixImageSize = 0x23E73000;
constexpr std::uintptr_t kPass3dRva = 0x004DA440;
constexpr std::uintptr_t kPass3dSetupRva = 0x004DA090;
constexpr std::uintptr_t kCameraRootGetRva = 0x002D4820;
constexpr std::uintptr_t kPointTransformRva = 0x001A6660;
constexpr std::uintptr_t kNativeTargetCtorRva = 0x004DAB30;
constexpr std::uintptr_t kNativeTargetInitRva = 0x004DABD0;
constexpr std::uintptr_t kNativeTargetResetRva = 0x004DB080;
constexpr std::uintptr_t kNativeTargetDtorRva = 0x004DABC0;
constexpr std::uintptr_t kNativeTargetBindRva = 0x004DB120;
constexpr std::uintptr_t kSetViewportRva = 0x002BFCA0;
constexpr std::uintptr_t kClearDepthStencilRva = 0x002BFD50;
constexpr std::uintptr_t kSetBatchCameraRva = 0x004D5090;
constexpr std::uintptr_t kSortCategoryRva = 0x0045DC20;
constexpr std::uintptr_t kDrawCategoryRva = 0x0045B8D0;
constexpr std::uintptr_t kCategoryEnabledRva = 0x004D8AD0;
constexpr std::uintptr_t kSetAlphaBlendGroupRva = 0x004635E0;
constexpr std::uintptr_t kSetMarkerRva = 0x00460AD0;
constexpr std::uintptr_t kBindTextureRangeRva = 0x002C0050;
constexpr std::uintptr_t kBindSamplerRangeRva = 0x002C00B0;
constexpr std::uintptr_t kBindVertexShaderRva = 0x002BFEC0;
constexpr std::uintptr_t kBindPixelShaderRva = 0x002BFFE0;
constexpr std::uintptr_t kTextureWidthRva = 0x002BC980;
constexpr std::uintptr_t kTextureHeightRva = 0x002BC990;
constexpr std::uintptr_t kRenderAccessorRva = 0x0049F8E0;
constexpr std::uintptr_t kRenderDimensionsRva = 0x004A0CB0;
constexpr std::uintptr_t kSssCopyDrawRva = 0x005BD0C0;
constexpr std::uintptr_t kRenderTextureExportRva = 0x0049E870;
constexpr std::uintptr_t kRendererModeRva = 0x002C2460;
constexpr std::uintptr_t kSssAccessorRva = 0x005BDA20;
constexpr std::uintptr_t kSssFilterChainRva = 0x005BE2B0;
constexpr std::uintptr_t kSssFinalBindRva = 0x005BE630;
constexpr std::uintptr_t kCameraViewRva = 0x004CCDC0;
constexpr std::uintptr_t kCameraProjectionRva = 0x004CCDE0;
constexpr std::uintptr_t kCameraDerivedRva = 0x004CCFB0;
constexpr std::uintptr_t kCameraExtendedProjectionRva = 0x004CD1A0;
constexpr std::uintptr_t kMainCameraRva = 0x017594F0;
constexpr std::uintptr_t kNativeSortOptionRva = 0x017594EB;
constexpr std::uintptr_t kRenderObjectRva = 0x01758300;
constexpr std::uintptr_t kRefAssignRva = 0x002BC400;
constexpr std::uintptr_t kRefReleaseRva = 0x002BC7A0;
constexpr std::uintptr_t kTemporalDiscontinuity0Rva = 0x0CC2B7B9;
constexpr std::uintptr_t kTemporalDiscontinuity1Rva = 0x0CC2B7BA;
constexpr std::size_t kNativeTargetSize = 0x40;
constexpr std::size_t kSssOuterSize = 0x120;
constexpr std::size_t kCameraSize = 0x1B4;
constexpr std::size_t kCameraRootStride = 0xA98;
constexpr float kDegreesToRadians = 0.01745329251994329577f;

constexpr std::array<std::uint8_t, 16> kPass3dSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
};

struct Vec3 { float x; float y; float z; };
struct CameraEvalResult {
    std::uint8_t use_auxiliary_up;
    std::uint8_t padding[3];
    Vec3 eye;
    Vec3 interest;
    float fov_degrees;
    float roll_degrees;
    Vec3 auxiliary_up;
    float near_plane;
};
static_assert(sizeof(CameraEvalResult) == 0x34);

using Pass3dFn = void (__fastcall*)(void*, std::uintptr_t, std::uint64_t,
    std::int32_t, std::int32_t);
using Pass3dSetupFn = void (__fastcall*)(void*, std::uint64_t, bool);
using GetSubFrameRenderEnabledFn = bool (__cdecl*)();
using CameraRootGetFn = void (__fastcall*)(const std::uint8_t*,
    CameraEvalResult*, float, const std::uint8_t*);
using PointTransformFn = void (__fastcall*)(const void*, const Vec3*, Vec3*);
using NativeTargetCtorFn = void (__fastcall*)(void*);
using NativeTargetInitFn = int (__fastcall*)(void*, std::int32_t, std::int32_t,
    std::int32_t, std::int32_t, std::int32_t, bool);
using NativeTargetResetFn = void (__fastcall*)(void*);
using NativeTargetDtorFn = void (__fastcall*)(void*);
using NativeTargetBindFn = int (__fastcall*)(void*, void*, std::int32_t, bool);
using SetViewportFn = void (__fastcall*)(void*, std::int32_t, std::int32_t,
    std::int32_t, std::int32_t);
using ClearDepthStencilFn = void (__fastcall*)(void*, std::uint8_t, float, bool);
using CameraUnaryFn = void (__fastcall*)(void*);
using CameraExtendedProjectionFn = void (__fastcall*)(void*, float);
using SetBatchCameraFn = void (__fastcall*)(void*, const void*);
using SortCategoryFn = void (__fastcall*)(void*, std::int32_t, std::int32_t,
    const void*, bool);
using CategoryEnabledFn = bool (__fastcall*)(std::int32_t);
using SetAlphaBlendGroupFn = void (__fastcall*)(void*, bool);
using DrawCategoryFn = void (__fastcall*)(void*, std::int32_t, const void*,
    std::int32_t, std::int32_t, std::uintptr_t, std::uintptr_t, bool,
    std::int32_t, bool);
using RefAssignFn = void* (__fastcall*)(void*, const void*);
using RefReleaseFn = void (__fastcall*)(void*);
using SetMarkerFn = void (__fastcall*)(void*, const char*);
using BindResourceRangeFn = void (__fastcall*)(void*, std::int32_t,
    std::int32_t, const void*);
using BindHandleFn = void (__fastcall*)(void*, const void*);
using TextureDimensionFn = std::int32_t (__fastcall*)(const void*);
using RenderAccessorFn = void* (__fastcall*)();
using RenderDimensionsFn = void (__fastcall*)(void*, float*, float*, float*,
    float*);
using SssCopyDrawFn = void (__fastcall*)(void*, void*, std::int32_t,
    std::int32_t, float, float, std::uintptr_t, std::uintptr_t, const float*);
using RenderTextureExportFn = void (__fastcall*)(void*, void*, std::int32_t,
    std::int32_t, std::int32_t, std::int32_t, std::int32_t,
    const void* const*, std::int32_t, float, float, float, const float*, bool);
using RendererModeFn = std::int32_t (__fastcall*)();
using SssAccessorFn = void* (__fastcall*)();
using SssFilterChainFn = void (__fastcall*)(void*, void*);
using SssFinalBindFn = void (__fastcall*)(void*, void*);

HMODULE g_module{};
std::uint8_t* g_executable_base{};
std::once_flag g_hook_once;
std::atomic<bool> g_supported_executable{};
std::atomic<GetSubFrameRenderEnabledFn> g_get_sub_frame_render_enabled{};
std::atomic<std::uint32_t> g_requested_width{};
std::atomic<std::uint32_t> g_requested_height{};
std::atomic<bool> g_resize_pending{true};
std::atomic<bool> g_logged_first_pass{};
std::atomic<bool> g_logged_missing_size{};
std::atomic<bool> g_logged_bind_failure{};
std::atomic<bool> g_root1_present{};
std::atomic<bool> g_logged_first_root1_eval{};
std::atomic<bool> g_logged_structural_failure{};
std::atomic<bool> g_logged_rendertex_export{};
std::atomic<bool> g_logged_rendertex_registration{};
std::atomic<bool> g_logged_presentation_failure{};
std::atomic<std::uint64_t> g_camera_diag_counter{};

std::mutex g_scene_observation_mutex;
const std::uint8_t* g_observed_scene{};
float g_observed_frame{};
bool g_observation_pending{};
CameraEvalResult g_observed_root0{};


alignas(16) std::array<std::byte, kNativeTargetSize> g_sub_target{};
alignas(16) std::array<std::byte, kNativeTargetSize> g_presentation_target{};
alignas(16) std::array<std::byte, kCameraSize> g_sub_camera{};
alignas(16) std::array<std::byte, kNativeTargetSize> g_sss_fullres{};
alignas(16) std::array<std::byte, kSssOuterSize> g_private_sss{};
bool g_target_constructed{};
bool g_target_initialized{};
bool g_presentation_constructed{};
bool g_presentation_initialized{};
std::uint32_t g_target_width{};
std::uint32_t g_target_height{};
bool g_sss_fullres_constructed{};
bool g_sss_fullres_initialized{};
bool g_sss_fixed_constructed{};
bool g_sss_fixed_initialized{};
std::uint32_t g_sss_fullres_width{};
std::uint32_t g_sss_fullres_height{};
bool g_logged_sss_created{};
bool g_logged_sss_prepass{};
bool g_logged_sss_640{};
bool g_logged_sss_final{};
bool g_logged_sss_bound{};
bool g_logged_sss_restored{};
bool g_logged_sss_skip{};

std::uintptr_t g_stage_texture{};
std::uintptr_t g_stage_original_backend{};
bool g_stage_texture_aliased{};
bool g_have_valid_sub_frame{};
bool g_publish_pending{};
bool g_rebind_due_recreate{};
bool g_destination_was_available{};
bool g_destination_state_known{};

Pass3dFn g_original_pass3d{};
Pass3dSetupFn g_original_pass3d_setup{};
CameraRootGetFn g_original_camera_root_get{};
PointTransformFn g_point_transform{};
NativeTargetCtorFn g_target_ctor{};
NativeTargetInitFn g_target_init{};
NativeTargetResetFn g_target_reset{};
NativeTargetDtorFn g_target_dtor{};
NativeTargetBindFn g_target_bind{};
SetViewportFn g_set_viewport{};
ClearDepthStencilFn g_clear_depth_stencil{};
CameraUnaryFn g_rebuild_view{};
CameraUnaryFn g_rebuild_projection{};
CameraUnaryFn g_rebuild_derived{};
CameraExtendedProjectionFn g_rebuild_extended_projection{};
SetBatchCameraFn g_set_batch_camera{};
SortCategoryFn g_sort_category{};
DrawCategoryFn g_draw_category{};
CategoryEnabledFn g_category_enabled{};
SetAlphaBlendGroupFn g_set_alpha_blend_group{};
RefAssignFn g_ref_assign{};
RefReleaseFn g_ref_release{};
SetMarkerFn g_set_marker{};
BindResourceRangeFn g_bind_texture_range{};
BindResourceRangeFn g_bind_sampler_range{};
BindHandleFn g_bind_vertex_shader{};
BindHandleFn g_bind_pixel_shader{};
TextureDimensionFn g_texture_width{};
TextureDimensionFn g_texture_height{};
RenderAccessorFn g_render_accessor{};
RenderDimensionsFn g_render_dimensions{};
SssCopyDrawFn g_sss_copy_draw{};
RenderTextureExportFn g_render_texture_export{};
RendererModeFn g_renderer_mode{};
SssAccessorFn g_sss_accessor{};
SssFilterChainFn g_sss_filter_chain{};
SssFinalBindFn g_sss_final_bind{};

void log(const char* message) {
    std::printf("[SubCamera] %s\n", message);
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer), "[SubCamera] %s\n", message);
    OutputDebugStringA(buffer);
}

bool sub_frame_render_enabled() {
    auto getter = g_get_sub_frame_render_enabled.load(std::memory_order_acquire);
    if (!getter) {
        const HMODULE owner = GetModuleHandleW(L"Misaki&MaxSongPack.dll");
        if (owner) {
            getter = reinterpret_cast<GetSubFrameRenderEnabledFn>(GetProcAddress(
                owner, "MisakiMax_GetSubFrameRenderEnabled"));
            if (getter)
                g_get_sub_frame_render_enabled.store(getter,
                    std::memory_order_release);
        }
    }
    return getter && getter();
}

template <typename T> T at_rva(std::uintptr_t rva) {
    return reinterpret_cast<T>(g_executable_base + rva);
}

std::uintptr_t render_object() {
    return *at_rva<std::uintptr_t*>(kRenderObjectRva);
}

std::uintptr_t first_stage_render_texture(std::uintptr_t render) {
    if (!render)
        return 0;
    constexpr std::size_t kFramebufferCopySlots = 15;
    const auto* slots = reinterpret_cast<const std::uintptr_t*>(render + 0x11B0);
    for (std::size_t i = 0; i < kFramebufferCopySlots; ++i) {
        if (slots[i])
            return slots[i];
    }
    return 0;
}

void release_saved_stage_backend() {
    if (g_stage_original_backend)
        g_ref_release(&g_stage_original_backend);
}

void refresh_stage_render_texture_destination() {
    const std::uintptr_t selected = first_stage_render_texture(render_object());
    if (selected == g_stage_texture)
        return;

    // A removed slot may already have released its texture object.  Do not
    // dereference the old pointer here; our saved backend reference is enough
    // to keep the original GPU resource alive until this release.
    release_saved_stage_backend();
    g_stage_texture = selected;
    g_stage_texture_aliased = false;
    g_publish_pending = false;
    if (selected) {
        g_ref_assign(&g_stage_original_backend,
            reinterpret_cast<const void*>(selected + 0x28));
        log("[RENDERTEX] destination resolved");
        g_destination_was_available = true;
        g_destination_state_known = true;
    }
    else if (!g_destination_state_known || g_destination_was_available) {
        log("[RENDERTEX] destination unavailable");
        g_destination_was_available = false;
        g_destination_state_known = true;
    }
}

void restore_stage_render_texture_for_sub_pass() {
    if (!g_stage_texture || !g_stage_texture_aliased)
        return;
    g_ref_assign(reinterpret_cast<void*>(g_stage_texture + 0x28),
        &g_stage_original_backend);
    g_stage_texture_aliased = false;
}

void publish_sub_color_to_stage_render_texture() {
    if (!g_stage_texture || !g_have_valid_sub_frame
        || !g_presentation_initialized)
        return;
    const auto color_texture = *reinterpret_cast<const std::uintptr_t*>(
        g_presentation_target.data() + 0x08);
    if (!color_texture)
        return;
    g_ref_assign(reinterpret_cast<void*>(g_stage_texture + 0x28),
        reinterpret_cast<const void*>(color_texture + 0x28));
    g_stage_texture_aliased = true;
    if (!g_logged_rendertex_registration.exchange(true))
        log("[RENDERTEX] registered Y-flipped presentation color");
    if (g_rebind_due_recreate) {
        log("[RENDERTEX] sub target recreated -> rebound");
        g_rebind_due_recreate = false;
    }
    if (!g_logged_rendertex_export.exchange(true))
        log("[RENDERTEX] high-resolution native-orientation export/alias executed");
}

bool supported_executable() {
    g_executable_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!g_executable_base)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_executable_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || dos->e_lfanew > 0x100000)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        g_executable_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    return nt->Signature == IMAGE_NT_SIGNATURE
        && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
        && nt->FileHeader.TimeDateStamp == kMegaMixTimestamp
        && nt->OptionalHeader.SizeOfImage == kMegaMixImageSize;
}

void remember_dimensions(IDXGISwapChain* swap_chain) {
    if (!swap_chain)
        return;
    std::uint32_t width{};
    std::uint32_t height{};
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swap_chain->GetDesc(&desc))) {
        width = desc.BufferDesc.Width;
        height = desc.BufferDesc.Height;
    }
    if (!width || !height) {
        ID3D11Texture2D* back_buffer{};
        if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
            D3D11_TEXTURE2D_DESC texture_desc{};
            back_buffer->GetDesc(&texture_desc);
            back_buffer->Release();
            width = texture_desc.Width;
            height = texture_desc.Height;
        }
    }
    if (width && height) {
        g_requested_width.store(width, std::memory_order_release);
        g_requested_height.store(height, std::memory_order_release);
        g_resize_pending.store(true, std::memory_order_release);
    }
}

void destroy_unreferenced_target() {
    if (g_presentation_constructed) {
        g_target_reset(g_presentation_target.data());
        g_target_dtor(g_presentation_target.data());
        g_presentation_target.fill(std::byte{});
    }
    g_presentation_constructed = false;
    g_presentation_initialized = false;
    if (g_target_constructed) {
        g_target_reset(g_sub_target.data());
        g_target_dtor(g_sub_target.data());
        g_sub_target.fill(std::byte{});
    }
    g_target_constructed = false;
    g_target_initialized = false;
    g_target_width = 0;
    g_target_height = 0;
}

bool ensure_target_at_render_boundary() {
    const auto width = g_requested_width.load(std::memory_order_acquire);
    const auto height = g_requested_height.load(std::memory_order_acquire);
    if (!width || !height) {
        if (!g_logged_missing_size.exchange(true))
            log("sub pass skipped: target dimensions unavailable");
        return false;
    }
    const bool recreate = g_resize_pending.exchange(false,
        std::memory_order_acq_rel) || !g_target_initialized
        || width != g_target_width || height != g_target_height;
    if (!recreate)
        return true;

    const bool was_initialized = g_target_initialized;
    restore_stage_render_texture_for_sub_pass();
    g_have_valid_sub_frame = false;
    g_rebind_due_recreate = g_rebind_due_recreate || was_initialized;
    if (!g_target_constructed) {
        g_target_ctor(g_sub_target.data());
        g_target_constructed = true;
        log("native target constructed");
    }
    if (!g_presentation_constructed) {
        g_target_ctor(g_presentation_target.data());
        g_presentation_constructed = true;
        log("presentation target constructed");
    }
    const int result = g_target_init(g_sub_target.data(),
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
        0, 6, 0x1B, false);
    if (result < 0) {
        char message[160]{};
        std::snprintf(message, sizeof(message),
            "native target initialization failed: %ux%u result=%d",
            width, height, result);
        log(message);
        destroy_unreferenced_target();
        g_resize_pending.store(true, std::memory_order_release);
        return false;
    }
    const int presentation_result = g_target_init(g_presentation_target.data(),
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
        0, 6, 0, false);
    if (presentation_result < 0) {
        char message[176]{};
        std::snprintf(message, sizeof(message),
            "presentation target initialization failed: %ux%u result=%d",
            width, height, presentation_result);
        log(message);
        destroy_unreferenced_target();
        g_resize_pending.store(true, std::memory_order_release);
        return false;
    }
    g_target_initialized = true;
    g_presentation_initialized = true;
    g_target_width = width;
    g_target_height = height;
    char message[128]{};
    std::snprintf(message, sizeof(message),
        was_initialized ? "native target resized/recreated: %ux%u"
                        : "native target initialized: %ux%u", width, height);
    log(message);
    return true;
}

void* sss_target(std::size_t offset) {
    return g_private_sss.data() + offset;
}

const void* target_texture_backend(void* target, std::size_t texture_offset) {
    const auto texture = *reinterpret_cast<const std::uintptr_t*>(
        static_cast<std::byte*>(target) + texture_offset);
    return texture ? reinterpret_cast<const void*>(texture + 0x28) : nullptr;
}

bool export_root1_for_native_presentation(void* ctx) {
    if (!ctx || !g_target_initialized || !g_presentation_initialized)
        return false;
    const auto render = render_object();
    const void* source_color = target_texture_backend(g_sub_target.data(), 0x08);
    const auto destination_texture = *reinterpret_cast<const std::uintptr_t*>(
        g_presentation_target.data() + 0x08);
    if (!render || !source_color || !destination_texture)
        return false;
    auto* transfer = *reinterpret_cast<void**>(render + 0x1940);
    if (!transfer)
        return false;

    const auto destination_width = static_cast<std::int32_t>(g_target_width);
    const auto destination_height = static_cast<std::int32_t>(g_target_height);
    const auto native_width = *reinterpret_cast<const std::int32_t*>(render + 0xFF8);
    const auto native_height = *reinterpret_cast<const std::int32_t*>(render + 0x100C);
    const float transfer_scale = *reinterpret_cast<const float*>(
        static_cast<const std::byte*>(transfer) + 0x4E0);
    if (native_width <= 0 || native_height <= 0 || transfer_scale <= 0.0f)
        return false;
    const auto scaled_width = static_cast<std::int32_t>(native_width * transfer_scale);
    const auto scaled_height = static_cast<std::int32_t>(native_height * transfer_scale);
    const float source_scale_x = static_cast<float>(scaled_width)
        / static_cast<float>(native_width)
        * *reinterpret_cast<const float*>(render + 0x1048);
    const float source_scale_y = static_cast<float>(scaled_height)
        / static_cast<float>(native_height)
        * *reinterpret_cast<const float*>(render + 0x104C);

    const bool special_texture =
        (*reinterpret_cast<const std::uint32_t*>(destination_texture + 0x08)
            & 0x02U) != 0;
    const bool destination_is_small = native_width > destination_width * 2
        || native_height > destination_height * 2;
    const std::int32_t variant = destination_is_small
        ? (special_texture ? 10 : g_renderer_mode())
        : (special_texture ? 9 : 0);

    if (g_target_bind(g_presentation_target.data(), ctx, 0, false) < 0)
        return false;
    const void* sources[1]{source_color};
    constexpr float color_scale[4]{1.0f, 1.0f, 1.0f, 1.0f};
    // The final true argument is the native framebuffer-copy Y-orientation
    // selector. It produces inverted storage without reversing U.
    g_render_texture_export(transfer, ctx, variant, 0, 0,
        destination_width, destination_height, sources, 1,
        source_scale_x, source_scale_y, 1.0f, color_scale, true);
    return true;
}

bool run_private_sss_copy(void* ctx, void* helper) {
    if (!ctx || !helper)
        return false;
    auto* helper_bytes = static_cast<std::byte*>(helper);
    const void* source_color = target_texture_backend(g_sss_fullres.data(), 0x08);
    if (!source_color
        || !*reinterpret_cast<const std::uintptr_t*>(helper_bytes + 0x00)
        || !*reinterpret_cast<const std::uintptr_t*>(helper_bytes + 0x10)
        || !*reinterpret_cast<const std::uintptr_t*>(helper_bytes + 0x20)
        || !*reinterpret_cast<const std::uintptr_t*>(helper_bytes + 0x28))
        return false;

    void* render = g_render_accessor();
    if (!render)
        return false;
    float numerator_x{};
    float numerator_y{};
    float denominator_x{};
    float denominator_y{};
    g_render_dimensions(render, &numerator_x, &numerator_y,
        &denominator_x, &denominator_y);
    if (denominator_x == 0.0f || denominator_y == 0.0f)
        return false;

    const std::int32_t source_width = g_texture_width(source_color);
    const std::int32_t source_height = g_texture_height(source_color);
    if (source_width <= 0 || source_height <= 0)
        return false;

    // Mirror FUN_1405BE2B0's optional sss_copy block, substituting only the
    // private Root1 full-resolution color backend for the native scene source.
    if (g_target_bind(sss_target(0x08), ctx, 0, false) < 0)
        return false;
    g_set_viewport(ctx, 0, 0, 640, 360);
    g_bind_texture_range(ctx, 0, 1, source_color);
    g_bind_sampler_range(ctx, 0, 1, helper);
    g_bind_vertex_shader(ctx, helper_bytes + 0x20);
    g_bind_pixel_shader(ctx, helper_bytes + 0x28);
    constexpr float copy_scale[4]{1.0f, 1.0f, 1.0f, 1.0f};
    g_sss_copy_draw(helper, ctx, source_width, source_height,
        numerator_x / denominator_x, numerator_y / denominator_y,
        0, 0, copy_scale);
    return true;
}

bool ensure_private_sss_resources() {
    if (!g_sss_fullres_constructed) {
        g_target_ctor(g_sss_fullres.data());
        g_sss_fullres_constructed = true;
    }
    if (!g_sss_fixed_constructed) {
        g_target_ctor(sss_target(0x08)); // 640x360 source for native filters
        g_target_ctor(sss_target(0x48)); // 320x180 final (+0x50 texture)
        g_target_ctor(sss_target(0x88)); // 320x180 intermediate
        g_target_ctor(sss_target(0xC8)); // reserved native outer slot
        g_sss_fixed_constructed = true;
    }

    if (!g_sss_fullres_initialized || g_sss_fullres_width != g_target_width
        || g_sss_fullres_height != g_target_height) {
        const int result = g_target_init(g_sss_fullres.data(),
            static_cast<std::int32_t>(g_target_width),
            // MM+ native SSS descriptor table at VA 0x140C199C0 uses
            // color format 0x17 (R16G16B16A16_FLOAT). 0x0C is R32_FLOAT.
            static_cast<std::int32_t>(g_target_height), 0, 0x17, 0x1B, false);
        if (result < 0) {
            if (!g_logged_sss_skip) {
                log("[SSS] private SSS skipped: full-resolution target init failed");
                g_logged_sss_skip = true;
            }
            g_sss_fullres_initialized = false;
            return false;
        }
        g_sss_fullres_initialized = true;
        g_sss_fullres_width = g_target_width;
        g_sss_fullres_height = g_target_height;
    }
    if (!g_sss_fixed_initialized) {
        const int r640 = g_target_init(sss_target(0x08), 640, 360,
            0, 0x17, 0x1B, false);
        const int rfinal = g_target_init(sss_target(0x48), 320, 180,
            0, 0x17, 0, false);
        const int rintermediate = g_target_init(sss_target(0x88), 320, 180,
            0, 0x17, 0, false);
        if (r640 < 0 || rfinal < 0 || rintermediate < 0) {
            if (!g_logged_sss_skip) {
                log("[SSS] private SSS skipped: fixed target init failed");
                g_logged_sss_skip = true;
            }
            return false;
        }
        g_sss_fixed_initialized = true;
    }
    if (!g_logged_sss_created) {
        log("[SSS] private resources created");
        g_logged_sss_created = true;
    }
    return true;
}

bool generate_private_sss(void* ctx, void*& native_sss) {
    native_sss = g_sss_accessor();
    if (!native_sss || !*static_cast<const std::uint8_t*>(native_sss)) {
        if (!g_logged_sss_skip) {
            log("[SSS] private SSS skipped: native SSS is not initialized");
            g_logged_sss_skip = true;
        }
        return false;
    }
    void* helper = *reinterpret_cast<void**>(
        static_cast<std::byte*>(native_sss) + 0x118);
    if (!helper || !ensure_private_sss_resources()) {
        if (!helper && !g_logged_sss_skip) {
            log("[SSS] private SSS skipped: native filter helper unavailable");
            g_logged_sss_skip = true;
        }
        return false;
    }
    *reinterpret_cast<void**>(g_private_sss.data() + 0x118) = helper;
    g_private_sss[0] = std::byte{1};
    g_private_sss[2] = std::byte{0}; // 640 source is prepared explicitly.

    if (g_target_bind(g_sss_fullres.data(), ctx, 0, true) < 0)
        return false;
    g_set_viewport(ctx, 0, 0, static_cast<std::int32_t>(g_target_width),
        static_cast<std::int32_t>(g_target_height));
    g_clear_depth_stencil(ctx, std::uint8_t{0}, 0.0f, true);
    g_set_batch_camera(ctx, g_sub_camera.data());
    g_set_marker(ctx, "SSS_SKIN");
    constexpr std::uintptr_t abi_unused = 0;
    g_draw_category(ctx, 0x13, g_sub_camera.data(), 0, -1,
        abi_unused, abi_unused, true, -1, true);
    g_set_marker(ctx, nullptr);
    if (!g_logged_sss_prepass) {
        log("[SSS] Root1 SSS prepass executed");
        g_logged_sss_prepass = true;
    }

    if (!run_private_sss_copy(ctx, helper))
        return false;
    // The native wrapper must skip its internally sourced first copy stage;
    // run_private_sss_copy already populated our private +0x08 target.
    g_private_sss[2] = std::byte{0};
    if (!g_logged_sss_640) {
        log("[SSS] private sss_copy: Root1 fullres -> private 640x360, viewport 640x360, borrowed helper; remaining first-stage flag=0");
        g_logged_sss_640 = true;
    }

    g_sss_filter_chain(g_private_sss.data(), ctx);
    if (!g_logged_sss_final) {
        log("[SSS] private final generated");
        g_logged_sss_final = true;
    }
    return true;
}

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}
bool normalize(Vec3& value) {
    const float length_sq = value.x * value.x + value.y * value.y
        + value.z * value.z;
    if (!(length_sq > 1.0e-8f) || !std::isfinite(length_sq))
        return false;
    const float scale = 1.0f / std::sqrt(length_sq);
    value.x *= scale;
    value.y *= scale;
    value.z *= scale;
    return true;
}

Vec3 scale(const Vec3& value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}
Vec3 add(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 effective_up(const CameraEvalResult& result) {
    Vec3 backward = subtract(result.eye, result.interest);
    if (!normalize(backward))
        return {0.0f, 1.0f, 0.0f};
    Vec3 seed = result.use_auxiliary_up
        ? result.auxiliary_up : Vec3{0.0f, 1.0f, 0.0f};
    Vec3 right = cross(seed, backward);
    if (!normalize(right)) {
        seed = std::fabs(backward.y) < 0.999f
            ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        right = cross(seed, backward);
        normalize(right);
    }
    Vec3 up = cross(backward, right);
    normalize(up);
    const float roll = result.roll_degrees * kDegreesToRadians;
    Vec3 rolled = add(scale(up, std::cos(roll)), scale(right, -std::sin(roll)));
    normalize(rolled);
    return rolled;
}

bool take_scene_observation(const std::uint8_t*& scene, float& frame,
    CameraEvalResult& root0) {
    std::lock_guard<std::mutex> lock(g_scene_observation_mutex);
    if (!g_observation_pending)
        return false;
    scene = g_observed_scene;
    frame = g_observed_frame;
    root0 = g_observed_root0;
    g_observation_pending = false;
    return true;
}

bool build_root1_camera() {
    const std::uint8_t* scene{};
    float frame{};
    CameraEvalResult root0{};
    if (!take_scene_observation(scene, frame, root0)) {
        if (g_root1_present.exchange(false))
            log("Root1 lost");
        return false;
    }
    const auto begin = *reinterpret_cast<const std::uint8_t* const*>(scene + 0x178);
    const auto end = *reinterpret_cast<const std::uint8_t* const*>(scene + 0x180);
    const bool structurally_valid = begin && end >= begin
        && static_cast<std::size_t>(end - begin) % kCameraRootStride == 0;
    const std::size_t count = structurally_valid
        ? static_cast<std::size_t>(end - begin) / kCameraRootStride : 0;
    if (!structurally_valid || count < 2) {
        if (!structurally_valid && !g_logged_structural_failure.exchange(true))
            log("CameraRoot vector structural validation failure");
        if (g_root1_present.exchange(false))
            log("Root1 lost");
        return false;
    }
    if (!g_root1_present.exchange(true))
        log("first valid Root1 detected");

    CameraEvalResult result{};
    auto* temporal0 = g_executable_base + kTemporalDiscontinuity0Rva;
    auto* temporal1 = g_executable_base + kTemporalDiscontinuity1Rva;
    const std::uint8_t saved_temporal0 = *temporal0;
    const std::uint8_t saved_temporal1 = *temporal1;
    g_original_camera_root_get(begin + kCameraRootStride, &result, frame, scene);
    *temporal0 = saved_temporal0;
    *temporal1 = saved_temporal1;
    if (!g_logged_first_root1_eval.exchange(true))
        log("first Root1 evaluation completed");

    const auto diag_index = g_camera_diag_counter.fetch_add(1,
        std::memory_order_relaxed);
    const bool log_diag = diag_index == 0 || diag_index % 300 == 0;
    const CameraEvalResult raw_result = result;
    if (log_diag) {
        char message[512]{};
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] sample=%llu frame=%.6f scene=%p reverse=%u",
            static_cast<unsigned long long>(diag_index), frame,
            static_cast<const void*>(scene), unsigned(*(scene + 0x0E)));
        log(message);
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root0 raw flag00=%u eye=(%.6f,%.6f,%.6f) interest=(%.6f,%.6f,%.6f) fov=%.6f roll=%.6f aux=(%.6f,%.6f,%.6f) scalar30=%.6f",
            unsigned(root0.use_auxiliary_up), root0.eye.x, root0.eye.y,
            root0.eye.z, root0.interest.x, root0.interest.y,
            root0.interest.z, root0.fov_degrees, root0.roll_degrees,
            root0.auxiliary_up.x, root0.auxiliary_up.y, root0.auxiliary_up.z,
            root0.near_plane);
        log(message);
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root1 stage0 raw flag00=%u eye=(%.6f,%.6f,%.6f) interest=(%.6f,%.6f,%.6f) fov=%.6f roll=%.6f aux=(%.6f,%.6f,%.6f) scalar30=%.6f",
            unsigned(raw_result.use_auxiliary_up), raw_result.eye.x,
            raw_result.eye.y, raw_result.eye.z, raw_result.interest.x,
            raw_result.interest.y, raw_result.interest.z,
            raw_result.fov_degrees, raw_result.roll_degrees,
            raw_result.auxiliary_up.x, raw_result.auxiliary_up.y,
            raw_result.auxiliary_up.z, raw_result.near_plane);
        log(message);
    }

    if (*(scene + 0x0E)) {
        result.eye.x = -result.eye.x;
        result.interest.x = -result.interest.x;
        result.roll_degrees = -result.roll_degrees;
    }
    if (log_diag) {
        char message[384]{};
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root1 stage1 reverse eye=(%.6f,%.6f,%.6f) interest=(%.6f,%.6f,%.6f) roll=%.6f",
            result.eye.x, result.eye.y, result.eye.z, result.interest.x,
            result.interest.y, result.interest.z, result.roll_degrees);
        log(message);
    }
    Vec3 transformed{};
    g_point_transform(scene + 0x10, &result.interest, &transformed);
    result.interest = transformed;
    g_point_transform(scene + 0x10, &result.eye, &transformed);
    result.eye = transformed;
    if (log_diag) {
        char message[384]{};
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root1 stage2 scene eye=(%.6f,%.6f,%.6f) interest=(%.6f,%.6f,%.6f)",
            result.eye.x, result.eye.y, result.eye.z, result.interest.x,
            result.interest.y, result.interest.z);
        log(message);
    }

    std::memcpy(g_sub_camera.data(), g_executable_base + kMainCameraRva,
        g_sub_camera.size());
    auto* eye = reinterpret_cast<Vec3*>(g_sub_camera.data() + 0x00);
    auto* interest = reinterpret_cast<Vec3*>(g_sub_camera.data() + 0x0C);
    auto* up = reinterpret_cast<Vec3*>(g_sub_camera.data() + 0x18);
    *eye = result.eye;
    *interest = result.interest;
    *up = effective_up(result);
    *reinterpret_cast<float*>(g_sub_camera.data() + 0x24)
        = result.fov_degrees;
    *reinterpret_cast<float*>(g_sub_camera.data() + 0x28)
        = static_cast<float>(g_target_width) / static_cast<float>(g_target_height);
    *reinterpret_cast<float*>(g_sub_camera.data() + 0x2C) = result.near_plane;
    g_rebuild_view(g_sub_camera.data());
    g_rebuild_projection(g_sub_camera.data());
    g_rebuild_derived(g_sub_camera.data());
    g_rebuild_extended_projection(g_sub_camera.data(), result.near_plane);
    if (log_diag) {
        Vec3 backward = subtract(result.eye, result.interest);
        normalize(backward);
        const Vec3 forward = scale(backward, -1.0f);
        Vec3 seed = result.use_auxiliary_up
            ? result.auxiliary_up : Vec3{0.0f, 1.0f, 0.0f};
        Vec3 right = cross(seed, backward);
        normalize(right);
        Vec3 reconstructed_up = cross(backward, right);
        normalize(reconstructed_up);
        const Vec3 final_up = *up;
        char message[512]{};
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root1 stage3 eye=(%.6f,%.6f,%.6f) interest=(%.6f,%.6f,%.6f) forward=(%.6f,%.6f,%.6f) backward=(%.6f,%.6f,%.6f)",
            result.eye.x, result.eye.y, result.eye.z, result.interest.x,
            result.interest.y, result.interest.z, forward.x, forward.y,
            forward.z, backward.x, backward.y, backward.z);
        log(message);
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root1 basis seed_up=(%.6f,%.6f,%.6f) right=(%.6f,%.6f,%.6f) reconstructed_up=(%.6f,%.6f,%.6f) effective_up=(%.6f,%.6f,%.6f)",
            seed.x, seed.y, seed.z, right.x, right.y, right.z,
            reconstructed_up.x, reconstructed_up.y, reconstructed_up.z,
            final_up.x, final_up.y, final_up.z);
        log(message);
        const auto* main_eye = reinterpret_cast<const Vec3*>(
            g_executable_base + kMainCameraRva + 0x00);
        const auto* main_interest = reinterpret_cast<const Vec3*>(
            g_executable_base + kMainCameraRva + 0x0C);
        const auto* main_up = reinterpret_cast<const Vec3*>(
            g_executable_base + kMainCameraRva + 0x18);
        Vec3 main_forward = subtract(*main_interest, *main_eye);
        normalize(main_forward);
        std::snprintf(message, sizeof(message),
            "[CAMDIAG] Root0 native eye=(%.6f,%.6f,%.6f) interest=(%.6f,%.6f,%.6f) forward=(%.6f,%.6f,%.6f) up=(%.6f,%.6f,%.6f)",
            main_eye->x, main_eye->y, main_eye->z, main_interest->x,
            main_interest->y, main_interest->z, main_forward.x,
            main_forward.y, main_forward.z, main_up->x, main_up->y,
            main_up->z);
        log(message);
    }
    return true;
}

void draw_base_categories(void* ctx, std::int32_t opaque_begin,
    std::int32_t opaque_count) {
    const bool native_option = *(g_executable_base + kNativeSortOptionRva) != 0;
    g_sort_category(ctx, 1, 1, g_sub_camera.data(), native_option);
    g_sort_category(ctx, 2, 2, g_sub_camera.data(), false);
    g_sort_category(ctx, 0, 0, g_sub_camera.data(), false);
    constexpr std::uintptr_t abi_unused = 0;
    if (g_category_enabled(0))
        g_draw_category(ctx, 0, g_sub_camera.data(), opaque_begin, opaque_count,
            abi_unused, abi_unused, true, -1, true);
    if (g_category_enabled(2))
        g_draw_category(ctx, 3, g_sub_camera.data(), 0, -1,
            abi_unused, abi_unused, true, -1, true);
    if (g_category_enabled(1)) {
        g_set_alpha_blend_group(ctx, true);
        g_draw_category(ctx, 2, g_sub_camera.data(), 0, -1,
            abi_unused, abi_unused, true, -1, true);
        g_draw_category(ctx, 1, g_sub_camera.data(), 0, -1,
            abi_unused, abi_unused, true, -1, true);
        g_set_alpha_blend_group(ctx, false);
    }
}

void run_sub_camera_poc(void* ctx, std::uintptr_t pass_state,
    std::int32_t opaque_begin, std::int32_t opaque_count) {
    if (!sub_frame_render_enabled())
        return;
    if (!ctx || !ensure_target_at_render_boundary())
        return;
    if (!build_root1_camera())
        return;
    // Root1 runs before the native main pass. Prepare the same scene-global
    // shadow/reflection SRVs and samplers that FUN_1404DA440 prepares through
    // FUN_1404DA090. Disable its optional depth clear; the sub target is bound
    // and cleared immediately below, and the main pass performs its own setup.
    g_original_pass3d_setup(ctx, pass_state, false);
    const int bind_result = g_target_bind(g_sub_target.data(), ctx, 0, true);
    if (bind_result < 0) {
        if (!g_logged_bind_failure.exchange(true)) {
            char message[128]{};
            std::snprintf(message, sizeof(message),
                "sub target bind failed: result=%d", bind_result);
            log(message);
        }
        return;
    }
    g_set_viewport(ctx, 0, 0, static_cast<std::int32_t>(g_target_width),
        static_cast<std::int32_t>(g_target_height));
    g_clear_depth_stencil(ctx, std::uint8_t{0}, 0.0f, true);
    g_set_batch_camera(ctx, g_sub_camera.data());
    void* native_sss{};
    const bool private_sss_ready = generate_private_sss(ctx, native_sss);

    // The SSS prepass/filter chain changes framebuffer and viewport. Restore
    // the verified Root1 ordinary target before installing the private final.
    if (g_target_bind(g_sub_target.data(), ctx, 0, true) < 0)
        return;
    g_set_viewport(ctx, 0, 0, static_cast<std::int32_t>(g_target_width),
        static_cast<std::int32_t>(g_target_height));
    g_set_batch_camera(ctx, g_sub_camera.data());
    if (private_sss_ready) {
        g_sss_final_bind(g_private_sss.data(), ctx);
        if (!g_logged_sss_bound) {
            log("[SSS] private final bound to t16");
            g_logged_sss_bound = true;
        }
    }
    draw_base_categories(ctx, opaque_begin, opaque_count);
    if (private_sss_ready) {
        g_sss_final_bind(native_sss, ctx);
        if (!g_logged_sss_restored) {
            log("[SSS] native final restored");
            g_logged_sss_restored = true;
        }
    }
    const bool had_valid_presentation = g_have_valid_sub_frame;
    if (!export_root1_for_native_presentation(ctx)) {
        g_have_valid_sub_frame = had_valid_presentation;
        if (!g_logged_presentation_failure.exchange(true))
            log("[RENDERTEX] presentation export failed; previous stage frame retained");
        return;
    }
    g_have_valid_sub_frame = true;
    g_logged_presentation_failure.store(false);
    if (!g_logged_first_pass.exchange(true))
        log("first sub-camera pass recorded successfully");
}

void __fastcall pass3d_setup_hook(void* ctx, std::uint64_t flags,
    bool clear_depth) {
    g_original_pass3d_setup(ctx, flags, clear_depth);
    if (g_publish_pending)
        publish_sub_color_to_stage_render_texture();
    g_publish_pending = false;
}

void __fastcall pass3d_hook(void* ctx, std::uintptr_t pass_state,
    std::uint64_t flags, std::int32_t begin, std::int32_t count) {
    refresh_stage_render_texture_destination();
    const bool enabled = sub_frame_render_enabled();
    if (enabled) {
        // Keep the native stage destination's original framebuffer-copy
        // resource visible while Root1 draws, so a screen in that scene never
        // samples the render target currently being written.
        restore_stage_render_texture_for_sub_pass();
        run_sub_camera_poc(ctx, pass_state, begin, count);
        g_publish_pending = g_stage_texture && g_have_valid_sub_frame;
    }
    g_original_pass3d(ctx, pass_state, flags, begin, count);
    g_publish_pending = false;
}



void __fastcall camera_root_get_hook(const std::uint8_t* root,
    CameraEvalResult* output, float frame, const std::uint8_t* scene) {
    g_original_camera_root_get(root, output, frame, scene);
    if (!scene)
        return;
    const auto begin = *reinterpret_cast<const std::uint8_t* const*>(scene + 0x178);
    if (root == begin) {
        std::lock_guard<std::mutex> lock(g_scene_observation_mutex);
        g_observed_scene = scene;
        g_observed_frame = frame;
        g_observed_root0 = *output;
        g_observation_pending = true;
    }
}

void resolve_native_functions() {
    g_target_ctor = at_rva<NativeTargetCtorFn>(kNativeTargetCtorRva);
    g_target_init = at_rva<NativeTargetInitFn>(kNativeTargetInitRva);
    g_target_reset = at_rva<NativeTargetResetFn>(kNativeTargetResetRva);
    g_target_dtor = at_rva<NativeTargetDtorFn>(kNativeTargetDtorRva);
    g_target_bind = at_rva<NativeTargetBindFn>(kNativeTargetBindRva);
    g_set_viewport = at_rva<SetViewportFn>(kSetViewportRva);
    g_clear_depth_stencil = at_rva<ClearDepthStencilFn>(kClearDepthStencilRva);
    g_rebuild_view = at_rva<CameraUnaryFn>(kCameraViewRva);
    g_rebuild_projection = at_rva<CameraUnaryFn>(kCameraProjectionRva);
    g_rebuild_derived = at_rva<CameraUnaryFn>(kCameraDerivedRva);
    g_rebuild_extended_projection =
        at_rva<CameraExtendedProjectionFn>(kCameraExtendedProjectionRva);
    g_set_batch_camera = at_rva<SetBatchCameraFn>(kSetBatchCameraRva);
    g_sort_category = at_rva<SortCategoryFn>(kSortCategoryRva);
    g_draw_category = at_rva<DrawCategoryFn>(kDrawCategoryRva);
    g_category_enabled = at_rva<CategoryEnabledFn>(kCategoryEnabledRva);
    g_set_alpha_blend_group =
        at_rva<SetAlphaBlendGroupFn>(kSetAlphaBlendGroupRva);
    g_point_transform = at_rva<PointTransformFn>(kPointTransformRva);
    g_ref_assign = at_rva<RefAssignFn>(kRefAssignRva);
    g_ref_release = at_rva<RefReleaseFn>(kRefReleaseRva);
    g_set_marker = at_rva<SetMarkerFn>(kSetMarkerRva);
    g_bind_texture_range = at_rva<BindResourceRangeFn>(kBindTextureRangeRva);
    g_bind_sampler_range = at_rva<BindResourceRangeFn>(kBindSamplerRangeRva);
    g_bind_vertex_shader = at_rva<BindHandleFn>(kBindVertexShaderRva);
    g_bind_pixel_shader = at_rva<BindHandleFn>(kBindPixelShaderRva);
    g_texture_width = at_rva<TextureDimensionFn>(kTextureWidthRva);
    g_texture_height = at_rva<TextureDimensionFn>(kTextureHeightRva);
    g_render_accessor = at_rva<RenderAccessorFn>(kRenderAccessorRva);
    g_render_dimensions = at_rva<RenderDimensionsFn>(kRenderDimensionsRva);
    g_render_texture_export =
        at_rva<RenderTextureExportFn>(kRenderTextureExportRva);
    g_renderer_mode = at_rva<RendererModeFn>(kRendererModeRva);
    g_sss_copy_draw = at_rva<SssCopyDrawFn>(kSssCopyDrawRva);
    g_sss_accessor = at_rva<SssAccessorFn>(kSssAccessorRva);
    g_sss_filter_chain = at_rva<SssFilterChainFn>(kSssFilterChainRva);
    g_sss_final_bind = at_rva<SssFinalBindFn>(kSssFinalBindRva);
}

void install_hook() {
    if (!supported_executable()) {
        log("unsupported executable; renderer hook not installed");
        return;
    }
    auto* target = g_executable_base + kPass3dRva;
    if (std::memcmp(target, kPass3dSignature.data(), kPass3dSignature.size()) != 0) {
        log("FUN_1404DA440 signature mismatch; renderer hook not installed");
        return;
    }
    resolve_native_functions();
    g_original_pass3d = reinterpret_cast<Pass3dFn>(target);
    g_original_pass3d_setup = at_rva<Pass3dSetupFn>(kPass3dSetupRva);
    g_original_camera_root_get = at_rva<CameraRootGetFn>(kCameraRootGetRva);
    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR)
        result = DetourUpdateThread(GetCurrentThread());
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_original_pass3d),
            pass3d_hook);
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_original_pass3d_setup),
            pass3d_setup_hook);
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&g_original_camera_root_get),
            camera_root_get_hook);
    const LONG commit = result == NO_ERROR
        ? DetourTransactionCommit() : (DetourTransactionAbort(), result);
    if (result == NO_ERROR && commit == NO_ERROR) {
        g_supported_executable.store(true, std::memory_order_release);
        log("renderer hooks installed at RVAs 0x004DA440 and 0x004DA090");
    }
    else {
        char message[160]{};
        std::snprintf(message, sizeof(message),
            "Detours hook installation failed: attach=%ld commit=%ld",
            result, commit);
        log(message);
    }
}

} // namespace

extern "C" __declspec(dllexport) void PreInit() { log("PreInit"); }
extern "C" __declspec(dllexport) void Init() {
    std::call_once(g_hook_once, install_hook);
}
extern "C" __declspec(dllexport) void PostInit() { log("PostInit"); }
extern "C" __declspec(dllexport) void D3DInit(IDXGISwapChain* swap_chain,
    ID3D11Device*, ID3D11DeviceContext*) {
    if (g_supported_executable.load(std::memory_order_acquire))
        remember_dimensions(swap_chain);
}
extern "C" __declspec(dllexport) void OnFrame(IDXGISwapChain*) {}
extern "C" __declspec(dllexport) void OnResize(IDXGISwapChain* swap_chain) {
    if (!g_supported_executable.load(std::memory_order_acquire))
        return;
    remember_dimensions(swap_chain);
    log("resize observed; native target recreation queued");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
