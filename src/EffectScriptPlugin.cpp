#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "DebugLog.hpp"
#include "EffectScript.hpp"
#include "FogDepthHeightFixMM.hpp"
#include "ScreenDistortionMM.hpp"
#include "ScriptPvMegaMix.hpp"
#include "SubCameraMM.hpp"

#include <detours.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

HMODULE g_module = nullptr;
HANDLE g_plugin_instance_guard = nullptr;
std::mutex g_plugin_instance_mutex;
bool g_plugin_instance_checked = false;
bool g_primary_plugin_instance = false;
bool g_supported_executable = false;
std::mutex g_d3d_state_mutex;
IDXGISwapChain* g_d3d_swap_chain = nullptr;

void remember_d3d_swap_chain(IDXGISwapChain* swap_chain) {
    if (!swap_chain) return;
    std::lock_guard<std::mutex> lock(g_d3d_state_mutex);
    if (g_d3d_swap_chain == swap_chain) return;
    swap_chain->AddRef();
    IDXGISwapChain* previous = g_d3d_swap_chain;
    g_d3d_swap_chain = swap_chain;
    if (previous) previous->Release();
}

IDXGISwapChain* acquire_remembered_d3d_swap_chain() {
    std::lock_guard<std::mutex> lock(g_d3d_state_mutex);
    if (g_d3d_swap_chain) g_d3d_swap_chain->AddRef();
    return g_d3d_swap_chain;
}

void initialize_graphics_for_swap_chain(IDXGISwapChain* swap_chain) {
    if (!swap_chain || !g_primary_plugin_instance || !g_supported_executable)
        return;
    fog_depth_height_fix::ensure_device_hooks(swap_chain);
    screen_distortion::on_frame(swap_chain);
}

constexpr const char* kPluginNameA = "Misaki&MaxSongPack";
constexpr const wchar_t* kPluginNameW = L"Misaki&MaxSongPack";
constexpr const wchar_t* kPluginInstanceMutex =
    // Keep the historical ScriptPv mutex name so clean releases also refuse
    // to coexist with older copies that contain the same native dispatcher.
    L"Local\\MisakiMaxSongPack.ScriptPvMegaMix.6F4DF714";
constexpr uintptr_t kPvGameCtrlRva = 0x241DF0;
constexpr uintptr_t kParticleManagerRva = 0x16E4A70;
constexpr uintptr_t kLoadSceneRva = 0x416C90;
constexpr uintptr_t kFreeSceneEffectRva = 0x416E70;
constexpr uintptr_t kEffectInstXResetRva = 0x40FE60;
constexpr uintptr_t kDofDebugDataRva = 0x1753410;

constexpr ptrdiff_t kPvIdOffset = 0x10;
constexpr ptrdiff_t kCurrentTimeNsOffset = 0x2D348;
constexpr uint64_t kFnv1a64mEmpty = 0xCBF29CE44FD0BFC1ull;
constexpr int32_t kParticleBoneResetPvId = 948;
constexpr int32_t kTimeRewindResetToleranceTicks = 1000;
constexpr uint32_t kDofDebugUseUiParams = 0x01;
constexpr uint32_t kDofDebugEnableDof = 0x02;
constexpr uint32_t kDofDebugEnablePhysDof = 0x04;
constexpr uint32_t kDofDebugAutoFocus = 0x08;

using PvGameCtrlFn = int32_t(__fastcall*)(void*, float, int64_t);
using LoadSceneFn = uint32_t(__fastcall*)(void*, uint64_t, uint64_t);
using FreeSceneEffectFn = void(__fastcall*)(void*, uint32_t, uint64_t);
using EffectInstXResetFn = void(__fastcall*)(void*, void*);

PvGameCtrlFn g_original_pv_game_ctrl = nullptr;
LoadSceneFn g_load_scene = nullptr;
FreeSceneEffectFn g_free_scene_effect = nullptr;
EffectInstXResetFn g_original_effect_inst_x_reset = nullptr;
void* g_particle_manager = nullptr;
std::atomic<int32_t> g_native_pv_id{-1};

struct DofF2 {
    float focus = 10.0f;
    float focus_range = 1.0f;
    float fuzzing_range = 0.5f;
    float ratio = 1.0f;
};

struct DofDebug {
    uint32_t flags = 0;
    float focus = 10.0f;
    float focal_length = 0.04f;
    float f_number = 1.4f;
    DofF2 f2;
};

DofDebug* g_dof_debug = nullptr;

std::mutex g_runtime_mutex;
std::filesystem::path g_mod_directory;
std::vector<std::filesystem::path> g_data_roots;
bool g_hook_installed = false;
bool g_debug_enabled = false;
std::wstring g_hook_status = L"hook: not installed";

bool acquire_plugin_instance() {
    std::lock_guard<std::mutex> lock(g_plugin_instance_mutex);
    if (g_plugin_instance_checked)
        return g_primary_plugin_instance;

    g_plugin_instance_checked = true;
    SetLastError(ERROR_SUCCESS);
    g_plugin_instance_guard = CreateMutexW(nullptr, FALSE,
        kPluginInstanceMutex);
    if (!g_plugin_instance_guard)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_plugin_instance_guard);
        g_plugin_instance_guard = nullptr;
        std::printf("[Misaki&MaxSongPack] duplicate plugin instance disabled; first loaded copy remains active.\n");
        OutputDebugStringA("[Misaki&MaxSongPack] duplicate plugin instance disabled.\n");
        return false;
    }

    g_primary_plugin_instance = true;
    return true;
}

std::filesystem::path plugin_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = g_module
        ? GetModuleFileNameW(g_module, path.data(),
            static_cast<DWORD>(path.size()))
        : 0;
    if (!length || length >= path.size())
        return std::filesystem::current_path();
    return std::filesystem::path(path.data()).parent_path();
}

struct RuntimeState {
    void* pv_game_instance = nullptr;
    int32_t pv_id = -1;
    int64_t current_time_ns = 0;
    int32_t current_tick = 0;
    int32_t loaded_pv_id = -1;
    std::filesystem::path script_path;
    std::vector<effect_script::ResolvedCommand> commands;
    std::vector<effect_script::FieldChange> field_changes;
    int32_t current_field = -1;
    size_t next_command = 0;
    size_t dispatched_count = 0;
    size_t resolved_count = 0;
    std::wstring load_status = L"waiting for PV";
    std::wstring last_action = L"";
};

RuntimeState g_runtime;
std::unordered_map<uint64_t, uint32_t> g_pv948_particle_scenes;

constexpr uint32_t kEffectInstExtAnim = 0x0000004;
constexpr uint32_t kEffectInstExtAnimNonInit = 0x0000010;
constexpr uint32_t kEffectInstExtAnimChara = 0x0000020;
constexpr uint32_t kEffectInstExtAnimAuth = 0x0000040;
constexpr uint32_t kEffectInstExtAnimSetOnce = 0x0000080;
constexpr uint32_t kEffectInstNoExtAnimTransX = 0x0001000;
constexpr uint32_t kEffectInstNoExtAnimTransY = 0x0002000;
constexpr uint32_t kEffectInstNoExtAnimTransZ = 0x0004000;
constexpr uint32_t kEffectInstExtAnimTransOnly = 0x0008000;
constexpr uint32_t kEffectInstExtAnimGetThenUpdate = 0x0020000;

constexpr uint32_t kEffectExtAnimSetOnce = 0x00001;
constexpr uint32_t kEffectExtAnimTransOnly = 0x00002;
constexpr uint32_t kEffectExtAnimNoTransX = 0x00004;
constexpr uint32_t kEffectExtAnimNoTransY = 0x00008;
constexpr uint32_t kEffectExtAnimNoTransZ = 0x00010;
constexpr uint32_t kEffectExtAnimGetThenUpdate = 0x00040;
constexpr uint32_t kEffectExtAnimChara = 0x10000;

void restore_pv948_ext_anim_flags(void* effect_inst) {
    if (!effect_inst || g_native_pv_id.load(std::memory_order_relaxed)
        != kParticleBoneResetPvId)
        return;

    auto* base = reinterpret_cast<uint8_t*>(effect_inst);
    void* data_ext_anim = *reinterpret_cast<void**>(base + 0x30);
    void* runtime_ext_anim = *reinterpret_cast<void**>(base + 0xE8);
    if (!data_ext_anim || !runtime_ext_anim)
        return;

    const uint32_t ext_flags = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(data_ext_anim) + 0x04);
    uint32_t flags = *reinterpret_cast<uint32_t*>(base + 0xDC);
    flags |= kEffectInstExtAnim | kEffectInstExtAnimNonInit
        | kEffectInstExtAnimAuth;
    if (ext_flags & kEffectExtAnimSetOnce)
        flags |= kEffectInstExtAnimSetOnce;
    if (ext_flags & kEffectExtAnimTransOnly)
        flags |= kEffectInstExtAnimTransOnly;
    if (ext_flags & kEffectExtAnimNoTransX)
        flags |= kEffectInstNoExtAnimTransX;
    if (ext_flags & kEffectExtAnimNoTransY)
        flags |= kEffectInstNoExtAnimTransY;
    if (ext_flags & kEffectExtAnimNoTransZ)
        flags |= kEffectInstNoExtAnimTransZ;
    if (ext_flags & kEffectExtAnimGetThenUpdate)
        flags |= kEffectInstExtAnimGetThenUpdate;
    if (ext_flags & kEffectExtAnimChara)
        flags |= kEffectInstExtAnimChara;
    *reinterpret_cast<uint32_t*>(base + 0xDC) = flags;
}

void __fastcall effect_inst_x_reset_hook(void* effect_inst, void* scene) {
    g_original_effect_inst_x_reset(effect_inst, scene);
    restore_pv948_ext_anim_flags(effect_inst);
}

void free_pv948_particle_scenes_locked() {
    if (!g_particle_manager || !g_free_scene_effect) {
        g_pv948_particle_scenes.clear();
        return;
    }

    for (const auto& [effect_hash, scene_counter] :
        g_pv948_particle_scenes) {
        (void)effect_hash;
        if (scene_counter)
            g_free_scene_effect(g_particle_manager, scene_counter, 0);
    }
    if (!g_pv948_particle_scenes.empty())
        debug_log::line(L"PV 948 particle scenes released: "
            + std::to_wstring(g_pv948_particle_scenes.size()));
    g_pv948_particle_scenes.clear();
}

std::string trim_ascii(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::string::npos)
        return {};

    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string strip_toml_comment(std::string line) {
    bool in_quote = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if ((c == '"' || c == '\'') && (i == 0 || line[i - 1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = c;
            }
            else if (quote == c) {
                in_quote = false;
                quote = 0;
            }
        }
        else if (c == '#' && !in_quote) {
            return line.substr(0, i);
        }
    }

    return line;
}

bool parse_bool_value(std::string value, bool default_value) {
    value = to_lower_ascii(trim_ascii(value));
    if (value == "true" || value == "1" || value == "yes" || value == "on")
        return true;
    if (value == "false" || value == "0" || value == "no" || value == "off")
        return false;
    return default_value;
}

bool read_debug_enabled_from_config(const std::filesystem::path& mod_directory) {
    std::ifstream stream(mod_directory / "config.toml");
    if (!stream)
        return false;

    std::string section;
    std::string line;
    while (std::getline(stream, line)) {
        line = trim_ascii(strip_toml_comment(line));
        if (line.empty())
            continue;

        if (line.front() == '[' && line.back() == ']') {
            section = to_lower_ascii(trim_ascii(line.substr(1, line.size() - 2)));
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        std::string key = to_lower_ascii(trim_ascii(line.substr(0, equals)));
        std::string value = trim_ascii(line.substr(equals + 1));
        if (key.size() >= 2
            && ((key.front() == '"' && key.back() == '"')
                || (key.front() == '\'' && key.back() == '\'')))
            key = key.substr(1, key.size() - 2);
        key.erase(std::remove_if(key.begin(), key.end(),
            [](unsigned char c) { return std::isspace(c); }), key.end());

        if (key == "dll.debug" || key == "dll_debug" || (section == "dll" && key == "debug"))
            return parse_bool_value(value, false);
    }

    return false;
}

void log(const char* format, ...) {
    if (!g_debug_enabled)
        return;

    char buffer[1024]{};

    va_list args;
    va_start(args, format);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);

    OutputDebugStringA("[");
    OutputDebugStringA(kPluginNameA);
    OutputDebugStringA("] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
    debug_log::line_utf8(buffer);
}

std::wstring widen(const std::string& value) {
    if (value.empty())
        return {};

    const int length = MultiByteToWideChar(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring(value.begin(), value.end());

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring widen_path(const std::filesystem::path& path) {
    return path.filename().empty() ? path.wstring() : path.filename().wstring();
}

std::wstring action_name(effect_script::EventAction action) {
    switch (action) {
    case effect_script::EventAction::Play:
        return L"STAGE_EFFECT";
    case effect_script::EventAction::Stop:
        return L"STOP_EFFECT";
    case effect_script::EventAction::AutoDof:
        return L"AUTO_DOF";
    case effect_script::EventAction::Noise:
        return L"NOISE";
    case effect_script::EventAction::PjskDistortion:
        return L"PJSK_DISTORTION";
    case effect_script::EventAction::PjskChromatic:
        return L"PJSK_CHROMATIC";
    case effect_script::EventAction::PjskOverlay:
        return L"PJSK_OVERLAY";
    }

    return L"UNKNOWN";
}

std::wstring format_dsc_time(int32_t time) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.5fs/t=%d", static_cast<double>(time) / 100000.0, time);
    return buffer;
}

uint64_t hash_fnv1a64m(const std::string& value) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 0x100000001B3ull;
    }

    return (hash >> 32) ^ hash;
}

std::wstring format_hex(uint64_t value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

void log_line(const std::wstring& line) {
    if (!g_debug_enabled)
        return;

    debug_log::line(line);
}

std::vector<std::wstring> build_debug_lines(
    const std::filesystem::path& mod_directory,
    const std::vector<std::filesystem::path>& data_roots) {

    std::vector<std::wstring> lines;
    lines.push_back(std::wstring(kPluginNameW) + L" DEBUG");
    lines.push_back(L"Mod: " + widen_path(mod_directory));
    lines.push_back(L"Data roots: " + std::to_wstring(data_roots.size()));

    const std::vector<effect_script::EffectScriptEntry> entries =
        effect_script::find_effect_script_entries(data_roots);

    if (entries.empty()) {
        lines.push_back(L"pv_db: no pv_*.effect.script_file found");
        return lines;
    }

    for (const effect_script::EffectScriptEntry& entry : entries) {
        effect_script::LoadResult script = effect_script::load_file(entry.script_path);
        const std::vector<effect_script::FieldEffects> fields =
            effect_script::load_field_effects(data_roots, entry.pv_id);
        const std::vector<effect_script::ParticleResource> resources =
            effect_script::load_particle_resources(data_roots, fields);
        const std::vector<effect_script::ResolvedCommand> commands =
            effect_script::build_commands(script.events, resources);

        size_t resolved_count = 0;
        std::set<int32_t> used_resource_indices;
        for (const effect_script::ResolvedCommand& command : commands) {
            if (command.resolved)
                resolved_count++;
            if (command.event.action != effect_script::EventAction::AutoDof
                && command.event.action != effect_script::EventAction::Noise
                && command.event.action != effect_script::EventAction::PjskDistortion
                && command.event.action != effect_script::EventAction::PjskChromatic
                && command.event.action != effect_script::EventAction::PjskOverlay)
                used_resource_indices.insert(command.event.resource_index);
        }

        std::wstring script_state = L"OK";
        if (!script.error.empty())
            script_state = L"ERR " + widen(script.error);

        lines.push_back(L"PV " + std::to_wstring(entry.pv_id)
            + L": script " + script_state
            + L" events=" + std::to_wstring(script.events.size())
            + L" resolved=" + std::to_wstring(resolved_count)
            + L"/" + std::to_wstring(commands.size()));
        lines.push_back(L"  dispatch: DIRECT RUNTIME (script_effect)");
        lines.push_back(L"  " + g_hook_status);
        lines.push_back(L"  file: " + widen_path(entry.script_path));

        for (const effect_script::ParticleResource& resource : resources) {
            if (!used_resource_indices.empty()
                && used_resource_indices.find(resource.resource_index) == used_resource_indices.end())
                continue;

            const bool file_found = !resource.file_path.empty();
            const bool effects_found = !resource.effect_names.empty();
            lines.push_back(L"  RS " + std::to_wstring(resource.resource_index)
                + L": " + widen(resource.resource_name)
                + L" " + (file_found ? L"FARC OK" : L"FARC MISSING")
                + L" effects=" + std::to_wstring(resource.effect_names.size())
                + (file_found && !effects_found ? L" LIST EMPTY" : L""));
        }

        size_t shown_commands = 0;
        for (const effect_script::ResolvedCommand& command : commands) {
            if (shown_commands >= 4)
                break;

            lines.push_back(L"  " + format_dsc_time(command.event.time)
                + L": " + action_name(command.event.action)
                + L"(" + (command.event.action == effect_script::EventAction::AutoDof
                    ? std::to_wstring(command.event.value)
                    : command.event.action == effect_script::EventAction::Noise
                    ? std::to_wstring(command.event.noise_values[0]) + L",..."
                    : std::to_wstring(command.event.resource_index)
                    + L"," + std::to_wstring(command.event.effect_index))
                + L") -> "
                + (command.resolved ? widen(command.effect_name) : L"UNRESOLVED"));
            shown_commands++;
        }
    }

    constexpr size_t kMaxLines = 16;
    if (lines.size() > kMaxLines) {
        const size_t hidden_count = lines.size() - kMaxLines + 1;
        lines.resize(kMaxLines - 1);
        lines.push_back(L"... " + std::to_wstring(hidden_count) + L" more debug lines");
    }

    return lines;
}

void publish_debug_lines(const std::vector<std::wstring>& lines) {
    if (!g_debug_enabled)
        return;

    for (const std::wstring& line : lines)
        log_line(line);
}

std::vector<std::wstring> build_runtime_debug_lines_locked() {
    std::vector<std::wstring> lines;
    lines.push_back(std::wstring(kPluginNameW) + L" DEBUG");
    lines.push_back(g_hook_status);
    lines.push_back(L"PV " + std::to_wstring(g_runtime.pv_id)
        + L"  frame60=" + std::to_wstring(static_cast<int64_t>(
            static_cast<double>(g_runtime.current_time_ns) * 60.0 / 1000000000.0))
        + L"  time=" + format_dsc_time(g_runtime.current_tick));
    lines.push_back(L"commands dispatched=" + std::to_wstring(g_runtime.dispatched_count)
        + L" next=" + std::to_wstring(g_runtime.next_command)
        + L"/" + std::to_wstring(g_runtime.commands.size())
        + L" resolved=" + std::to_wstring(g_runtime.resolved_count));
    lines.push_back(L"status: " + g_runtime.load_status);

    if (!g_runtime.script_path.empty())
        lines.push_back(L"script: " + widen_path(g_runtime.script_path));
    if (!g_runtime.last_action.empty())
        lines.push_back(L"last: " + g_runtime.last_action);

    return lines;
}

bool valid_positive(float value) {
    return value > 0.0f && value < 10000.0f;
}

void ensure_dof_defaults(DofDebug& debug) {
    if (!valid_positive(debug.focus))
        debug.focus = 10.0f;
    if (!valid_positive(debug.focal_length))
        debug.focal_length = 0.04f;
    if (!valid_positive(debug.f_number))
        debug.f_number = 1.4f;
    if (!valid_positive(debug.f2.focus))
        debug.f2.focus = 10.0f;
    if (!valid_positive(debug.f2.focus_range))
        debug.f2.focus_range = 1.0f;
    if (!valid_positive(debug.f2.fuzzing_range))
        debug.f2.fuzzing_range = 0.5f;
    if (!valid_positive(debug.f2.ratio))
        debug.f2.ratio = 1.0f;
}

void set_auto_dof_enabled_locked(bool enabled, bool report_action) {
    if (!g_dof_debug) {
        if (report_action)
            g_runtime.last_action = L"AUTO_DOF unavailable";
        return;
    }

    if (enabled) {
        ensure_dof_defaults(*g_dof_debug);
        g_dof_debug->flags |= kDofDebugUseUiParams
            | kDofDebugEnableDof
            | kDofDebugEnablePhysDof
            | kDofDebugAutoFocus;
    }
    else {
        g_dof_debug->flags &= ~(kDofDebugUseUiParams
            | kDofDebugEnableDof
            | kDofDebugEnablePhysDof
            | kDofDebugAutoFocus);
    }

    if (report_action)
        g_runtime.last_action = enabled ? L"AUTO_DOF ON" : L"AUTO_DOF OFF";
}

void load_runtime_for_pv_locked(int32_t pv_id, void* pv_game_instance, const wchar_t* reason) {
    // ReDIVA keeps the SceneCounter returned by LoadSceneEffect and frees
    // that exact instance before resetting a PV. The old bridge discarded
    // it and stopped only by effect hash, allowing an X glitter scene with
    // stale character/bone ExtAnim state to be reused on the second run.
    if (g_runtime.loaded_pv_id == kParticleBoneResetPvId)
        free_pv948_particle_scenes_locked();

    set_auto_dof_enabled_locked(false, false);
    screen_distortion::reset();

    g_runtime = RuntimeState{};
    g_runtime.pv_game_instance = pv_game_instance;
    g_runtime.pv_id = pv_id;
    g_runtime.loaded_pv_id = pv_id;

    screen_distortion::set_effect_textures(
        effect_script::find_effect_textures_from_pv_db(g_data_roots, pv_id));

    if (pv_id <= 0) {
        g_runtime.load_status = L"invalid/no PV id";
        return;
    }

    for (const auto& chart_path :
        effect_script::find_chart_script_files_from_pv_db(g_data_roots, pv_id)) {
        std::string field_error;
        auto changes = effect_script::load_field_changes_from_script(
            chart_path, field_error);
        g_runtime.field_changes.insert(g_runtime.field_changes.end(),
            changes.begin(), changes.end());
    }
    std::sort(g_runtime.field_changes.begin(), g_runtime.field_changes.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.time != rhs.time) return lhs.time < rhs.time;
            return lhs.field_index < rhs.field_index;
        });
    g_runtime.field_changes.erase(std::unique(g_runtime.field_changes.begin(),
        g_runtime.field_changes.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.time == rhs.time && lhs.field_index == rhs.field_index;
        }), g_runtime.field_changes.end());

    const std::filesystem::path script_path =
        effect_script::find_script_file_from_pv_db(g_data_roots, pv_id);
    g_runtime.script_path = script_path;
    if (script_path.empty()) {
        g_runtime.load_status = L"pv_db effect.script_file not found";
        return;
    }

    effect_script::LoadResult script = effect_script::load_file(script_path);
    if (!script.error.empty()) {
        g_runtime.load_status = L"script error: " + widen(script.error);
        return;
    }

    const std::vector<effect_script::FieldEffects> fields =
        effect_script::load_field_effects(g_data_roots, pv_id);
    const std::vector<effect_script::ParticleResource> resources =
        effect_script::load_particle_resources(g_data_roots, fields);

    g_runtime.commands = effect_script::build_commands(script.events, resources);
    std::stable_sort(g_runtime.commands.begin(), g_runtime.commands.end(),
        [](const effect_script::ResolvedCommand& left,
            const effect_script::ResolvedCommand& right) {
            return left.event.time < right.event.time;
        });

    g_runtime.resolved_count = 0;
    for (const effect_script::ResolvedCommand& command : g_runtime.commands)
        if (command.resolved)
            g_runtime.resolved_count++;

    g_runtime.load_status = L"script loaded";
    debug_log::line(L"runtime load PV " + std::to_wstring(pv_id)
        + L" reason=" + (reason ? std::wstring(reason) : std::wstring(L"unknown"))
        + L" events=" + std::to_wstring(g_runtime.commands.size())
        + L" resolved=" + std::to_wstring(g_runtime.resolved_count)
        + L" file=" + script_path.wstring());
}

void dispatch_effect_locked(const effect_script::ResolvedCommand& command) {
    if (command.event.action == effect_script::EventAction::AutoDof) {
        set_auto_dof_enabled_locked(command.event.value != 0, true);
        return;
    }

    if (command.event.action == effect_script::EventAction::Noise) {
        screen_distortion::begin_noise(command.event.noise_values,
            g_runtime.current_time_ns);
        g_runtime.last_action = L"NOISE screen distortion";
        return;
    }

    if (command.event.action == effect_script::EventAction::PjskDistortion) {
        screen_distortion::begin_pjsk_distortion(command.event.distortion_values,
            g_runtime.current_time_ns);
        g_runtime.last_action = L"PJSK_DISTORTION";
        return;
    }

    if (command.event.action == effect_script::EventAction::PjskChromatic) {
        screen_distortion::begin_pjsk_chromatic(command.event.chromatic_values,
            g_runtime.current_time_ns);
        g_runtime.last_action = L"PJSK_CHROMATIC";
        return;
    }

    if (command.event.action == effect_script::EventAction::PjskOverlay) {
        screen_distortion::begin_pjsk_overlay(command.event.overlay_values,
            g_runtime.current_time_ns);
        g_runtime.last_action = L"PJSK_OVERLAY";
        return;
    }

    if (!command.resolved) {
        g_runtime.last_action = action_name(command.event.action) + L" unresolved";
        return;
    }

    if (!g_particle_manager || !g_load_scene || !g_free_scene_effect) {
        g_runtime.last_action = L"native particle functions unavailable";
        return;
    }

    const uint64_t effect_hash = hash_fnv1a64m(command.effect_name);
    if (command.event.action == effect_script::EventAction::Play) {
        if (g_runtime.pv_id == kParticleBoneResetPvId) {
            const auto active = g_pv948_particle_scenes.find(effect_hash);
            if (active != g_pv948_particle_scenes.end()) {
                if (active->second)
                    g_free_scene_effect(g_particle_manager,
                        active->second, 0);
                g_pv948_particle_scenes.erase(active);
            }
        }

        const uint32_t scene = g_load_scene(g_particle_manager, kFnv1a64mEmpty, effect_hash);
        if (g_runtime.pv_id == kParticleBoneResetPvId && scene)
            g_pv948_particle_scenes[effect_hash] = scene;
        g_runtime.last_action = L"PLAY " + widen(command.effect_name)
            + L" scene=" + std::to_wstring(scene)
            + L" hash=" + format_hex(effect_hash);
    }
    else {
        if (g_runtime.pv_id == kParticleBoneResetPvId) {
            const auto active = g_pv948_particle_scenes.find(effect_hash);
            if (active != g_pv948_particle_scenes.end()) {
                if (active->second)
                    g_free_scene_effect(g_particle_manager,
                        active->second, 0);
                g_pv948_particle_scenes.erase(active);
            }
            else
                g_free_scene_effect(g_particle_manager, 0, effect_hash);
        }
        else
            g_free_scene_effect(g_particle_manager, 0, effect_hash);
        g_runtime.last_action = L"STOP " + widen(command.effect_name)
            + L" hash=" + format_hex(effect_hash);
    }
}

void update_runtime_from_pv_game(void* pv_game) {
    if (!pv_game)
        return;

    const auto base = reinterpret_cast<uint8_t*>(pv_game);
    const int32_t pv_id = *reinterpret_cast<int32_t*>(base + kPvIdOffset);
    const int64_t current_time_ns = *reinterpret_cast<int64_t*>(base + kCurrentTimeNsOffset);
    const int32_t current_tick = current_time_ns > 0
        ? static_cast<int32_t>(current_time_ns / 10000)
        : 0;


    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    const bool pv_changed = pv_id != g_runtime.loaded_pv_id;
    const bool time_rewound = pv_id == g_runtime.loaded_pv_id
        && g_runtime.current_tick > current_tick + kTimeRewindResetToleranceTicks;
    // The community pause implementation temporarily alternates between
    // multiple pv_game/controller instances while keeping the same native PV
    // timeline.  Treating that pointer change as a new run reloads the DSC
    // and immediately redispatches every event before current_tick.  A real
    // replay is already identified by PV-id change or a genuine timeline
    // rewind, exactly like the main DSC track.
    // AFT/MM+ community pause and frame-control paths can move the exposed
    // clock backwards without starting a new PV session. Reloading here
    // re-arms all screen events and makes every resume flash again. A real
    // exit/restart passes through a different/invalid PV id, so PV identity
    // is the session boundary; a same-PV rewind is only a seek.
    const bool reset = pv_changed;
    if (reset)
        load_runtime_for_pv_locked(pv_id, pv_game,
            L"pv changed");

    if (time_rewound && !reset) {
        g_runtime.next_command = static_cast<size_t>(std::upper_bound(
            g_runtime.commands.begin(), g_runtime.commands.end(), current_tick,
            [](int32_t tick, const effect_script::ResolvedCommand& command) {
                return tick < command.event.time;
            }) - g_runtime.commands.begin());
    }

    g_runtime.pv_game_instance = pv_game;
    g_runtime.pv_id = pv_id;
    g_runtime.current_time_ns = current_time_ns;
    g_runtime.current_tick = current_tick;
    g_runtime.current_field = -1;
    for (const auto& change : g_runtime.field_changes) {
        if (change.time > current_tick)
            break;
        g_runtime.current_field = change.field_index;
    }
    fog_depth_height_fix::update_pv_id(pv_id);
    sub_camera_mm::update_pv_time(pv_id, current_time_ns,
        g_runtime.current_field);
    screen_distortion::update_time(pv_id > 0, current_time_ns);

    while (g_runtime.next_command < g_runtime.commands.size()) {
        const effect_script::ResolvedCommand& command =
            g_runtime.commands[g_runtime.next_command];
        if (command.event.time > current_tick)
            break;

        dispatch_effect_locked(command);
        g_runtime.next_command++;
        g_runtime.dispatched_count++;
    }
}

int32_t __fastcall pv_game_ctrl_hook(void* pv_game, float delta_time, int64_t curr_time) {
    const int32_t native_pv_id = pv_game
        ? *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(pv_game) + kPvIdOffset)
        : -1;
    g_native_pv_id.store(native_pv_id, std::memory_order_relaxed);
    script_pv_megamix::begin_pv_frame(pv_game);
    const int32_t result = g_original_pv_game_ctrl(pv_game, delta_time, curr_time);
    const int64_t current_time_ns = pv_game
        ? *reinterpret_cast<int64_t*>(
            reinterpret_cast<uint8_t*>(pv_game) + kCurrentTimeNsOffset)
        : curr_time;
    script_pv_megamix::end_pv_frame(pv_game, delta_time, current_time_ns);
    update_runtime_from_pv_game(pv_game);
    return result;
}

bool validate_bytes(uint8_t* address, const std::vector<uint8_t>& expected) {
    return std::equal(expected.begin(), expected.end(), address);
}

void install_hooks() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        g_hook_status = L"hook: failed (no exe base)";
        return;
    }

    auto* pv_game_ctrl = reinterpret_cast<uint8_t*>(base + kPvGameCtrlRva);
    if (!validate_bytes(pv_game_ctrl, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20 })) {
        g_hook_status = L"hook: failed (pv_game::ctrl signature mismatch)";
        debug_log::line(g_hook_status);
        return;
    }

    g_original_pv_game_ctrl = reinterpret_cast<PvGameCtrlFn>(pv_game_ctrl);
    g_particle_manager = reinterpret_cast<void*>(base + kParticleManagerRva);
    g_load_scene = reinterpret_cast<LoadSceneFn>(base + kLoadSceneRva);
    g_free_scene_effect = reinterpret_cast<FreeSceneEffectFn>(base + kFreeSceneEffectRva);
    auto* effect_inst_x_reset = reinterpret_cast<uint8_t*>(
        base + kEffectInstXResetRva);
    if (!validate_bytes(effect_inst_x_reset,
        { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74,
          0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20 })) {
        g_hook_status = L"hook: failed (EffectInstX::Reset signature mismatch)";
        debug_log::line(g_hook_status);
        return;
    }
    g_original_effect_inst_x_reset =
        reinterpret_cast<EffectInstXResetFn>(effect_inst_x_reset);
    g_dof_debug = reinterpret_cast<DofDebug*>(base + kDofDebugDataRva);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attach_result = DetourAttach(
        reinterpret_cast<PVOID*>(&g_original_pv_game_ctrl),
        reinterpret_cast<PVOID>(pv_game_ctrl_hook));
    const LONG particle_attach_result = attach_result == NO_ERROR
        ? DetourAttach(
            reinterpret_cast<PVOID*>(&g_original_effect_inst_x_reset),
            reinterpret_cast<PVOID>(effect_inst_x_reset_hook))
        : attach_result;
    const LONG commit_result = attach_result == NO_ERROR
        && particle_attach_result == NO_ERROR
        ? DetourTransactionCommit()
        : DetourTransactionAbort();

    if (attach_result == NO_ERROR && particle_attach_result == NO_ERROR
        && commit_result == NO_ERROR) {
        g_hook_installed = true;
        g_hook_status = L"hook: installed pv_game::ctrl + PV948 particle reset";
    }
    else {
        wchar_t buffer[128]{};
        swprintf_s(buffer,
            L"hook: failed pv=%ld particle=%ld commit=%ld",
            attach_result, particle_attach_result, commit_result);
        g_hook_status = buffer;
    }

    debug_log::line(g_hook_status);
}

std::vector<std::wstring> build_patch_lines(const effect_script::PvFieldPatchResult& result) {
    std::vector<std::wstring> lines;
    lines.push_back(std::wstring(kPluginNameW) + L" PreInit OK");
    lines.push_back(L"pv_field patch: "
        + (result.error.empty() ? std::wstring(L"OK") : std::wstring(L"ERR ") + widen(result.error)));
    lines.push_back(L"entries=" + std::to_wstring(result.effect_entry_count)
        + L" applied=" + std::to_wstring(result.applied_command_count)
        + L" existing=" + std::to_wstring(result.existing_command_count)
        + L" generated=" + std::to_wstring(result.generated_command_count)
        + L" unresolved=" + std::to_wstring(result.unresolved_command_count)
        + L" unmatched=" + std::to_wstring(result.unmatched_time_count));
    if (!result.output_path.empty())
        lines.push_back(L"out: " + result.output_path.filename().wstring());

    for (size_t i = 0; i < result.messages.size() && lines.size() < 10; i++)
        lines.push_back(L"  " + widen(result.messages[i]));

    return lines;
}

// DML loads the DLL before Init/PostInit and calls exported functions by name.
// Keep game hooks out of DllMain; install them from Init or PreInit instead.
} // namespace

extern "C" __declspec(dllexport) void PreInit() {
    if (!acquire_plugin_instance())
        return;
    debug_log::init(g_module, kPluginNameW);
    debug_log::line(L"PreInit called");
    debug_log::line(L"PreInit cwd: " + debug_log::current_directory());
}

extern "C" __declspec(dllexport) void Init() {
    if (!acquire_plugin_instance())
        return;
    const std::filesystem::path mod_directory = effect_script::find_mod_directory(
        plugin_directory());
    const std::vector<std::filesystem::path> data_roots =
        effect_script::find_data_roots_from_config(mod_directory);
    // Release DLL: diagnostic logging and the in-game debug overlay are
    // intentionally unavailable regardless of user configuration.
    g_debug_enabled = false;
    debug_log::set_enabled(false);
    debug_log::init(g_module, kPluginNameW);

    g_supported_executable = script_pv_megamix::is_supported_executable();
    if (!g_supported_executable) {
        std::printf("[Misaki&MaxSongPack] unsupported DivaMegaMix.exe; native hooks disabled safely.\n");
        debug_log::line(L"unsupported DivaMegaMix.exe; all native hooks disabled");
        g_hook_status = L"hook: disabled (unsupported DivaMegaMix.exe)";
        return;
    }

    fog_depth_height_fix::initialize(g_module);
    screen_distortion::initialize();
    if (IDXGISwapChain* swap_chain = acquire_remembered_d3d_swap_chain()) {
        initialize_graphics_for_swap_chain(swap_chain);
        swap_chain->Release();
    }
    const bool script_pv_installed =
        script_pv_megamix::initialize(g_module);
    debug_log::line(script_pv_installed
        ? L"script_pv: installed MegaMix FT dispatcher"
        : L"script_pv: unsupported MegaMix executable or hook failure");

    log("Init");
    debug_log::line(L"Init cwd: " + debug_log::current_directory());

    log("mod directory: %ls", mod_directory.c_str());
    for (const std::filesystem::path& data_root : data_roots)
        log("data root: %ls", data_root.c_str());

    publish_debug_lines(build_debug_lines(mod_directory, data_roots));
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        g_mod_directory = mod_directory;
        g_data_roots = data_roots;
    }

    install_hooks();
}

extern "C" __declspec(dllexport) void PostInit() {
    if (!g_primary_plugin_instance || !g_supported_executable)
        return;
    log("PostInit");
    sub_camera_mm::initialize_probe();
}

extern "C" __declspec(dllexport) void D3DInit(IDXGISwapChain* swap_chain,
    ID3D11Device*, ID3D11DeviceContext*) {
    remember_d3d_swap_chain(swap_chain);
    initialize_graphics_for_swap_chain(swap_chain);
}

extern "C" __declspec(dllexport) void OnFrame(IDXGISwapChain* swap_chain) {
    if (!g_primary_plugin_instance || !g_supported_executable)
        return;
    remember_d3d_swap_chain(swap_chain);
    fog_depth_height_fix::ensure_device_hooks(swap_chain);
    sub_camera_mm::on_frame(swap_chain);
    screen_distortion::on_frame(swap_chain);
}

extern "C" __declspec(dllexport) void OnResize(IDXGISwapChain*) {
    if (!g_primary_plugin_instance || !g_supported_executable)
        return;
    sub_camera_mm::on_resize();
    screen_distortion::on_resize();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }

    return TRUE;
}
