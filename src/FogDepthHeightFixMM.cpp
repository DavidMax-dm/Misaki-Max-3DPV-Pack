#include "FogDepthHeightFixMM.hpp"

#include "DebugLog.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <detours.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fog_depth_height_fix {
namespace {

using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

CreateDeviceAndSwapChainFn g_create_device_and_swap_chain = nullptr;
CreateVertexShaderFn g_create_vertex_shader = nullptr;
CreatePixelShaderFn g_create_pixel_shader = nullptr;
std::filesystem::path g_override_directory;
std::mutex g_mutex;
bool g_device_create_hook_installed = false;
bool g_device_methods_hooked = false;
HMODULE g_plugin_module = nullptr;
std::unordered_map<ID3D11VertexShader*, ID3D11VertexShader*> g_character_vs;
std::unordered_map<ID3D11PixelShader*, ID3D11PixelShader*> g_fog_ps;
std::unordered_map<ID3D11PixelShader*, ID3D11PixelShader*> g_pv942_hair_ps;
std::unordered_map<ID3D11VertexShader*, uint64_t> g_vs_hashes;
std::unordered_map<ID3D11PixelShader*, uint64_t> g_ps_hashes;
std::set<ID3D11VertexShader*> g_skinned_vs;
std::set<ID3D11PixelShader*> g_character_ps;
std::set<std::pair<uint64_t, uint64_t>> g_logged_pairs;
std::set<uint64_t> g_logged_hair_npr;
thread_local ID3D11VertexShader* g_saved_vs = nullptr;
thread_local ID3D11PixelShader* g_saved_ps = nullptr;
std::atomic<int32_t> g_current_pv_id = -1;

constexpr uint64_t kHairPsOpaque = 0xF68885340D40F924ull;
constexpr uint64_t kHairPsAlpha = 0x8301BA98647E7314ull;
// Character fog shader replacement is opt-in: PV 70211 is the stage that
// currently needs the depth/height-fog correction.  Keep PV942's separate
// hair-fringe workaround below independent from this fog allowlist.
constexpr int32_t kFogPvAllowlist[] = { 70211 };

bool is_fog_pv_allowed(int32_t pv_id) {
    return std::find(std::begin(kFogPvAllowlist), std::end(kFogPvAllowlist),
        pv_id) != std::end(kFogPvAllowlist);
}

bool contains_ascii(const void* data, size_t size, const char* needle) {
    const auto* begin = static_cast<const char*>(data);
    const size_t length = std::strlen(needle);
    return length <= size && std::search(begin, begin + size,
        needle, needle + length) != begin + size;
}

uint64_t fnv1a64(const void* data, size_t size) {
    uint64_t hash = 0xCBF29CE484222325ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::wstring format_hash(uint64_t hash) {
    wchar_t value[17]{};
    swprintf_s(value, L"%016llX", static_cast<unsigned long long>(hash));
    return value;
}

bool read_constant_buffer(ID3D11DeviceContext* context, bool vertex_stage,
    UINT slot, std::vector<uint8_t>& bytes) {
    ID3D11Buffer* source = nullptr;
    if (vertex_stage)
        context->VSGetConstantBuffers(slot, 1, &source);
    else
        context->PSGetConstantBuffers(slot, 1, &source);
    if (!source)
        return false;

    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) {
        source->Release();
        return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    ID3D11Buffer* staging = nullptr;
    const HRESULT create_result = device->CreateBuffer(&desc, nullptr, &staging);
    device->Release();
    if (FAILED(create_result) || !staging) {
        source->Release();
        return false;
    }

    context->CopyResource(staging, source);
    source->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_result)) {
        staging->Release();
        return false;
    }
    bytes.resize(desc.ByteWidth);
    std::memcpy(bytes.data(), mapped.pData, desc.ByteWidth);
    context->Unmap(staging, 0);
    staging->Release();
    return true;
}

void log_character_fog_state(ID3D11DeviceContext* context, uint64_t ps_hash) {
    std::vector<uint8_t> flags_bytes;
    std::vector<uint8_t> scene_bytes;
    const bool have_flags = read_constant_buffer(
        context, false, 0, flags_bytes) && flags_bytes.size() >= 16;
    const bool have_scene = read_constant_buffer(
        context, false, 1, scene_bytes) && scene_bytes.size() >= 42 * 16;

    ID3D11ShaderResourceView* effect_texture = nullptr;
    context->VSGetShaderResources(14, 1, &effect_texture);
    const bool have_effect_texture = effect_texture != nullptr;
    if (effect_texture)
        effect_texture->Release();

    if (have_flags) {
        const auto* flags = reinterpret_cast<const uint32_t*>(flags_bytes.data());
        wchar_t line[256]{};
        swprintf_s(line,
            L"Fog draw flags=%08X,%08X,%08X,%08X FOGMAP=%u CHARA_COLOR=%u VS-t14=%u",
            flags[0], flags[1], flags[2], flags[3],
            (flags[1] >> 2) & 3u, (flags[0] >> 10) & 1u,
            have_effect_texture ? 1u : 0u);
        debug_log::line(line);
    }
    if (have_scene) {
        const auto* values = reinterpret_cast<const float*>(scene_bytes.data());
        const float* depth_color = values + 37 * 4;
        const float* height_params = values + 38 * 4;
        const float* height_color = values + 39 * 4;
        const float* state_params = values + 41 * 4;
        wchar_t line[512]{};
        swprintf_s(line,
            L"Fog constants depthColor=(%.4f,%.4f,%.4f,%.4f) "
            L"heightParams=(%.4f,%.4f,%.4f,%.4f) "
            L"heightColor=(%.4f,%.4f,%.4f,%.4f) "
            L"stateParams=(%.4f,%.4f,%.4f,%.4f)",
            depth_color[0], depth_color[1], depth_color[2], depth_color[3],
            height_params[0], height_params[1], height_params[2], height_params[3],
            height_color[0], height_color[1], height_color[2], height_color[3],
            state_params[0], state_params[1], state_params[2], state_params[3]);
        debug_log::line(line);
    }
    ID3D11RasterizerState* rasterizer = nullptr;
    context->RSGetState(&rasterizer);
    if (rasterizer) {
        D3D11_RASTERIZER_DESC desc{};
        rasterizer->GetDesc(&desc);
        wchar_t line[256]{};
        swprintf_s(line,
            L"Fog draw raster PS=%ls fill=%u cull=%u frontCCW=%u depthBias=%d slopeBias=%.4f",
            format_hash(ps_hash).c_str(), static_cast<unsigned>(desc.FillMode),
            static_cast<unsigned>(desc.CullMode), desc.FrontCounterClockwise ? 1u : 0u,
            desc.DepthBias, desc.SlopeScaledDepthBias);
        debug_log::line(line);
        rasterizer->Release();
    }
    if (ps_hash == kHairPsOpaque || ps_hash == kHairPsAlpha) {
        std::vector<uint8_t> npr_bytes;
        if (read_constant_buffer(context, false, 4, npr_bytes)
            && npr_bytes.size() >= 9 * 16) {
            const auto* npr_flags = reinterpret_cast<const uint32_t*>(npr_bytes.data());
            const auto* npr = reinterpret_cast<const float*>(npr_bytes.data() + 16);
            wchar_t line[640]{};
            swprintf_s(line,
                L"PV942 hair NPR PS=%ls flags=%08X,%08X,%08X,%08X "
                L"p4=(%.5f,%.5f,%.5f,%.5f) p5=(%.5f,%.5f,%.5f,%.5f) "
                L"p6=(%.5f,%.5f,%.5f,%.5f) p7=(%.5f,%.5f,%.5f,%.5f)",
                format_hash(ps_hash).c_str(),
                npr_flags[0], npr_flags[1], npr_flags[2], npr_flags[3],
                npr[16], npr[17], npr[18], npr[19],
                npr[20], npr[21], npr[22], npr[23],
                npr[24], npr[25], npr[26], npr[27],
                npr[28], npr[29], npr[30], npr[31]);
            debug_log::line(line);
        }
    }
}

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return false;
    const std::streamsize size = stream.tellg();
    if (size <= 0)
        return false;
    bytes.resize(static_cast<size_t>(size));
    stream.seekg(0);
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(bytes.data()), size));
}

bool load_override(const wchar_t* stage, const void* bytecode, size_t length,
    std::vector<uint8_t>& replacement) {
    const uint64_t hash = fnv1a64(bytecode, length);
    const std::wstring resource_name = std::wstring(stage) + L"_"
        + format_hash(hash);
    if (g_plugin_module) {
        HRSRC resource = FindResourceW(g_plugin_module,
            resource_name.c_str(), MAKEINTRESOURCEW(10));
        if (resource) {
            HGLOBAL loaded = LoadResource(g_plugin_module, resource);
            const DWORD size = SizeofResource(g_plugin_module, resource);
            const void* data = loaded ? LockResource(loaded) : nullptr;
            if (data && size) {
                const auto* begin = static_cast<const uint8_t*>(data);
                replacement.assign(begin, begin + size);
                return true;
            }
        }
    }
    const std::wstring filename = resource_name + L".cso";
    return read_file(g_override_directory / filename, replacement);
}

bool load_embedded_override(const wchar_t* resource_name,
    std::vector<uint8_t>& replacement) {
    if (!g_plugin_module)
        return false;
    HRSRC resource = FindResourceW(g_plugin_module, resource_name,
        MAKEINTRESOURCEW(10));
    if (!resource)
        return false;
    HGLOBAL loaded = LoadResource(g_plugin_module, resource);
    const DWORD size = SizeofResource(g_plugin_module, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !size)
        return false;
    const auto* begin = static_cast<const uint8_t*>(data);
    replacement.assign(begin, begin + size);
    return true;
}

bool is_p5_late_cloth_vs(const void* bytecode, size_t length) {
    // RenderDoc event 8207: DXBC 27a2ab32-9a608077-0adf9325-470a4548.
    if (!bytecode || length < 20)
        return false;
    const auto* words = static_cast<const uint32_t*>(bytecode);
    return words[0] == 0x43425844u
        && words[1] == 0x27A2AB32u
        && words[2] == 0x9A608077u
        && words[3] == 0x0ADF9325u
        && words[4] == 0x470A4548u;
}

HRESULT STDMETHODCALLTYPE hooked_create_vertex_shader(ID3D11Device* device,
    const void* bytecode, SIZE_T length, ID3D11ClassLinkage* linkage,
    ID3D11VertexShader** shader) {
    const HRESULT result = g_create_vertex_shader(device, bytecode, length,
        linkage, shader);
    if (FAILED(result) || !shader || !*shader)
        return result;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_vs_hashes[*shader] = fnv1a64(bytecode, length);
    }
    const bool skinned = contains_ascii(bytecode, length,
        "g_joint_transforms");
    const bool p5_late_cloth = is_p5_late_cloth_vs(bytecode, length);
    if (!skinned && !p5_late_cloth)
        return result;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_skinned_vs.insert(*shader);
    }

    std::vector<uint8_t> replacement;
    ID3D11VertexShader* alternate = nullptr;
    const bool have_override = p5_late_cloth
        ? load_embedded_override(L"vs_P5_LATE", replacement)
        : load_override(L"vs", bytecode, length, replacement);
    if (have_override
        && SUCCEEDED(g_create_vertex_shader(device, replacement.data(),
            replacement.size(), linkage, &alternate)) && alternate) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_character_vs[*shader] = alternate;
        debug_log::line(p5_late_cloth
            ? L"Fog fix: prepared P5 late CLOTH VS override"
            : L"Fog fix: prepared skinned character VS override");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE hooked_create_pixel_shader(ID3D11Device* device,
    const void* bytecode, SIZE_T length, ID3D11ClassLinkage* linkage,
    ID3D11PixelShader** shader) {
    const uint64_t hash = fnv1a64(bytecode, length);
    const HRESULT result = g_create_pixel_shader(device, bytecode, length,
        linkage, shader);
    if (FAILED(result) || !shader || !*shader)
        return result;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ps_hashes[*shader] = hash;
        if (contains_ascii(bytecode, length, "g_light_env_chara")
            || contains_ascii(bytecode, length, "g_chara_color0"))
            g_character_ps.insert(*shader);
    }

    std::vector<uint8_t> replacement;
    ID3D11PixelShader* alternate = nullptr;
    if (load_override(L"ps", bytecode, length, replacement)
        && SUCCEEDED(g_create_pixel_shader(device, replacement.data(),
            replacement.size(), linkage, &alternate)) && alternate) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_fog_ps[*shader] = alternate;
    }
    if (hash == kHairPsOpaque || hash == kHairPsAlpha) {
        replacement.clear();
        ID3D11PixelShader* hair_alternate = nullptr;
        if (load_override(L"hair_ps", bytecode, length, replacement)
            && SUCCEEDED(g_create_pixel_shader(device, replacement.data(),
                replacement.size(), linkage, &hair_alternate)) && hair_alternate) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pv942_hair_ps[*shader] = hair_alternate;
            debug_log::line(L"PV942 hair: prepared no-self-shadow alternate");
        }
    }
    return result;
}

void install_device_method_hooks(ID3D11Device* device) {
    if (!device)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device_methods_hooked)
        return;

    void** vtable = *reinterpret_cast<void***>(device);
    g_create_vertex_shader = reinterpret_cast<CreateVertexShaderFn>(vtable[12]);
    g_create_pixel_shader = reinterpret_cast<CreatePixelShaderFn>(vtable[15]);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG vertex_result = DetourAttach(
        reinterpret_cast<PVOID*>(&g_create_vertex_shader),
        reinterpret_cast<PVOID>(hooked_create_vertex_shader));
    LONG pixel_result = vertex_result == NO_ERROR
        ? DetourAttach(reinterpret_cast<PVOID*>(&g_create_pixel_shader),
            reinterpret_cast<PVOID>(hooked_create_pixel_shader))
        : vertex_result;
    LONG commit_result = vertex_result == NO_ERROR && pixel_result == NO_ERROR
        ? DetourTransactionCommit()
        : DetourTransactionAbort();

    g_device_methods_hooked = vertex_result == NO_ERROR
        && pixel_result == NO_ERROR && commit_result == NO_ERROR;
    debug_log::line(g_device_methods_hooked
        ? L"Fog fix: D3D11 shader capture/override hooks installed"
        : L"Fog fix: failed to install D3D11 shader hooks");
}

HRESULT WINAPI hooked_create_device_and_swap_chain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* feature_levels, UINT feature_level_count,
    UINT sdk_version, const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
    IDXGISwapChain** swap_chain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* selected_feature_level, ID3D11DeviceContext** context) {
    const HRESULT result = g_create_device_and_swap_chain(adapter, driver_type,
        software, flags, feature_levels, feature_level_count, sdk_version,
        swap_chain_desc, swap_chain, device, selected_feature_level, context);
    if (SUCCEEDED(result) && device && *device)
        install_device_method_hooks(*device);
    return result;
}

} // namespace

void initialize(HMODULE plugin_module) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device_create_hook_installed)
        return;
    g_plugin_module = plugin_module;

    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(plugin_module, module_path, MAX_PATH);
    const std::filesystem::path plugin_directory =
        std::filesystem::path(module_path).parent_path();
    g_override_directory = plugin_directory / L"fog_shader_overrides";

    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11)
        d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11)
        return;
    g_create_device_and_swap_chain =
        reinterpret_cast<CreateDeviceAndSwapChainFn>(
            GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!g_create_device_and_swap_chain)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attach_result = DetourAttach(
        reinterpret_cast<PVOID*>(&g_create_device_and_swap_chain),
        reinterpret_cast<PVOID>(hooked_create_device_and_swap_chain));
    const LONG commit_result = attach_result == NO_ERROR
        ? DetourTransactionCommit()
        : DetourTransactionAbort();
    g_device_create_hook_installed = attach_result == NO_ERROR
        && commit_result == NO_ERROR;
    debug_log::line(g_device_create_hook_installed
        ? L"Fog fix: early D3D11 device hook installed"
        : L"Fog fix: early D3D11 device hook unavailable");
}

void ensure_device_hooks(IDXGISwapChain* swap_chain) {
    if (!swap_chain || g_device_methods_hooked)
        return;
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device),
        reinterpret_cast<void**>(&device))) && device) {
        install_device_method_hooks(device);
        device->Release();
    }
}

bool begin_character_draw(ID3D11DeviceContext* context) {
    if (!context || g_saved_vs || g_saved_ps)
        return false;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    context->PSGetShader(&ps, nullptr, nullptr);

    ID3D11VertexShader* alternate_vs = nullptr;
    ID3D11PixelShader* alternate_ps = nullptr;
    bool log_pair_state = false;
    bool log_hair_npr_state = false;
    uint64_t pair_ps_hash = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto vs_it = g_character_vs.find(vs);
        const auto ps_it = g_fog_ps.find(ps);
        const auto hair_ps_it = g_pv942_hair_ps.find(ps);
        const uint64_t vs_hash = g_vs_hashes.contains(vs) ? g_vs_hashes[vs] : 0;
        const uint64_t ps_hash = g_ps_hashes.contains(ps) ? g_ps_hashes[ps] : 0;
        pair_ps_hash = ps_hash;
        const bool character_draw = g_skinned_vs.contains(vs)
            || g_character_ps.contains(ps);
        if (character_draw
            && g_logged_pairs.insert({ vs_hash, ps_hash }).second) {
            debug_log::line(L"Fog character pair VS=" + format_hash(vs_hash)
                + L" PS=" + format_hash(ps_hash)
                + (vs_it != g_character_vs.end() ? L" VSoverride=yes" : L" VSoverride=no")
                + (ps_it != g_fog_ps.end() ? L" PSoverride=yes" : L" PSoverride=no"));
            log_pair_state = true;
        }
        if (is_fog_pv_allowed(g_current_pv_id.load())
            && (ps_hash == kHairPsOpaque || ps_hash == kHairPsAlpha)
            && g_logged_hair_npr.insert(ps_hash).second)
            log_hair_npr_state = true;
        if (vs_it != g_character_vs.end() && ps_it != g_fog_ps.end()) {
            alternate_vs = vs_it->second;
            // The alpha-tested FT hair variant keeps both stock discard paths,
            // but disables the self-shadow calculation which can feed NaN
            // into transparent fringe pixels in PV 942.  The dedicated shader
            // was already prepared above; select it only for this PV/variant.
            if (g_current_pv_id.load() == 942
                && ps_hash == kHairPsAlpha
                && hair_ps_it != g_pv942_hair_ps.end())
                alternate_ps = hair_ps_it->second;
            else
                alternate_ps = ps_it->second;
        }
    }
    if (log_pair_state)
        log_character_fog_state(context, pair_ps_hash);
    else if (log_hair_npr_state)
        log_character_fog_state(context, pair_ps_hash);
    if (!is_fog_pv_allowed(g_current_pv_id.load())) {
        if (vs) vs->Release();
        if (ps) ps->Release();
        return false;
    }
    if (!alternate_vs || !alternate_ps) {
        if (vs) vs->Release();
        if (ps) ps->Release();
        return false;
    }

    g_saved_vs = vs;
    g_saved_ps = ps;
    context->VSSetShader(alternate_vs, nullptr, 0);
    context->PSSetShader(alternate_ps, nullptr, 0);
    return true;
}

void end_character_draw(ID3D11DeviceContext* context) {
    if (!context || !g_saved_vs || !g_saved_ps)
        return;
    context->VSSetShader(g_saved_vs, nullptr, 0);
    context->PSSetShader(g_saved_ps, nullptr, 0);
    g_saved_vs->Release();
    g_saved_ps->Release();
    g_saved_vs = nullptr;
    g_saved_ps = nullptr;
}

void update_pv_id(int32_t pv_id) {
    g_current_pv_id.store(pv_id);
}

} // namespace fog_depth_height_fix
