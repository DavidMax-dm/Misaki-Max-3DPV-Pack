#include "ScriptPvMegaMix.hpp"

#include "ScriptPvDscParser.hpp"

#include <detours.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace script_pv_megamix {
namespace {

// Supplied DivaMegaMix.exe, SHA-256:
// 6F4DF714C28515C93B3023E7D6DAFA9095DD8835F762D54DB1726FB8BC04504D
constexpr std::uintptr_t kDscCtrlRva = 0x00250000;
constexpr std::uint32_t kExeTimestamp = 0x635B31AB;
constexpr std::uint32_t kExeImageSize = 0x23E73000;

constexpr std::ptrdiff_t kDscBufferOffset = 0x0000000C;
constexpr std::ptrdiff_t kDscPlayingOffset = 0x00000008;
constexpr std::ptrdiff_t kDscCounterOffset = 0x0002BF2C;
constexpr std::ptrdiff_t kRobIdOffset = 0x0002BF70;
constexpr std::ptrdiff_t kPvGamePointerOffset = 0x0002BF78;
constexpr std::ptrdiff_t kDscTimeOffset = 0x0002BFA8;
constexpr std::ptrdiff_t kHasPerfIdOffset = 0x0002C4E1;
constexpr std::ptrdiff_t kBranchModeOffset = 0x0002C554;
constexpr std::ptrdiff_t kPvIdOffset = 0x10;

constexpr std::int32_t kDscWordCapacity = 45000;
constexpr std::int32_t kScratchWordCount = 32;
constexpr std::int32_t kScratchWordIndex =
    kDscWordCapacity - kScratchWordCount;
// FT TIME uses 10 microsecond ticks. One 60 Hz frame is about 1667 ticks;
// 1000 incorrectly classified ordinary pause/sync jitter as a seek.
constexpr std::int32_t kRewindToleranceTicks = 10000;
constexpr std::int32_t kRestartNearStartTicks = 10000;
constexpr std::int32_t kRestartPreviousTicks = 100000;

constexpr std::uint8_t kDscCtrlSignature[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,
    0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
};

using DscCtrl = bool (__fastcall*)(std::uintptr_t, float, std::int64_t,
    float*, bool*, bool, bool);

HMODULE g_module{};
DscCtrl g_original_dsc_ctrl{};
bool g_in_supplemental_dispatch{};
bool g_hook_installed{};

std::vector<script_pv::DscEvent> g_events;
std::vector<std::filesystem::path> g_loaded_paths;
std::vector<std::filesystem::path> g_search_roots;
std::size_t g_next_event{};
std::int32_t g_loaded_pv_id{-1};
std::int32_t g_last_tick{-1};
std::int32_t g_supplemental_rob_id{};

thread_local void* g_frame_pv_game{};
thread_local std::uintptr_t g_frame_controller{};
thread_local std::uintptr_t g_frame_boundary_controller{};
thread_local bool g_frame_controller_playing{};
thread_local bool g_frame_boundary_playing{};

void* g_session_pv_game{};
bool g_session_active{};
std::int32_t g_session_last_tick{-1};

std::filesystem::path module_directory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_module, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(path).parent_path();
}

void log_line(const std::string& text) {
    const std::string line = "[Misaki&MaxSongPack ScriptPv] " + text + "\n";
    OutputDebugStringA(line.c_str());
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool is_disabled_path(const std::filesystem::path& path) {
    for (const auto& component : path)
        if (lowercase_ascii(component.string()) == "_off")
            return true;
    return false;
}

void collect_from_script_pv_directory(const std::filesystem::path& directory,
    std::int32_t pv_id, std::set<std::filesystem::path>& paths) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error)
        return;
    for (const auto& entry : std::filesystem::directory_iterator(directory,
            std::filesystem::directory_options::skip_permission_denied,
            error)) {
        if (error)
            break;
        if (entry.is_regular_file(error)
            && !is_disabled_path(entry.path())
            && script_pv::matches_pv_script_name(entry.path(), pv_id))
            paths.insert(entry.path().lexically_normal());
    }
}

void collect_script_pv_root(const std::filesystem::path& directory,
    std::int32_t pv_id, std::set<std::filesystem::path>& paths,
    std::set<std::filesystem::path>& roots) {
    const std::filesystem::path normalized = directory.lexically_normal();
    if (!roots.insert(normalized).second)
        return;
    g_search_roots.push_back(normalized);
    collect_from_script_pv_directory(normalized, pv_id, paths);
}

std::vector<std::filesystem::path> find_script_pv_files(
    std::int32_t pv_id) {
    std::set<std::filesystem::path> paths;
    std::set<std::filesystem::path> roots;
    g_search_roots.clear();

    // Resolve data relative to the DLL itself. DivaModLoader may start the
    // game with a different working directory and MegaMix mod locations are
    // user-configurable, while the DLL and its virtual rom always travel
    // together inside the same mod.
    collect_script_pv_root(module_directory() / L"rom" / L"script_pv",
        pv_id, paths, roots);

    return { paths.begin(), paths.end() };
}

// Supplemental visual timelines must not control chart flow or scoring.
bool is_control_opcode(std::int32_t opcode) {
    switch (opcode) {
    case 0:  // END
    case 1:  // TIME
    case 6:  // TARGET
    case 25: // MUSIC_PLAY
    case 26: // MODE_SELECT
    case 28: // BAR_TIME_SET
    case 32: // PV_END
    case 35: // EDIT_TARGET
    case 58: // TARGET_FLYING_TIME
    case 65: // PV_BRANCH_MODE
    case 82: // EDIT_MODE_SELECT
    case 83: // PV_END_FADEOUT
    case 84: // TARGET_FLAG
        return true;
    default:
        return false;
    }
}

bool has_direct_performer_index(std::int32_t opcode) {
    switch (opcode) {
    case 2:  // MIKU_MOVE
    case 3:  // MIKU_ROT
    case 4:  // MIKU_DISP
    case 5:  // MIKU_SHADOW
    case 7:  // SET_MOTION
    case 8:  // SET_PLAYDATA
    case 18: // EYE_ANIM
    case 19: // MOUTH_ANIM
    case 20: // HAND_ANIM
    case 21: // LOOK_ANIM
    case 22: // EXPRESSION
    case 23: // LOOK_CAMERA
        return true;
    default:
        return false;
    }
}

bool has_valid_performer_index(const script_pv::DscEvent& event) {
    if (!has_direct_performer_index(event.opcode))
        return true;
    return !event.parameters.empty()
        && event.parameters[0] >= 0 && event.parameters[0] < 6;
}

void reset_timeline_for_load(std::int32_t tick) {
    // Keep commands exactly at the load position, but do not replay the
    // elapsed part of a song if initialization finishes late.
    g_next_event = script_pv::first_event_at_or_after(g_events, tick);
    g_last_tick = tick;
    g_supplemental_rob_id = 0;
}

void reset_timeline_for_seek(std::int32_t tick) {
    // Pause/audio resync can report an earlier timestamp without restarting
    // the PV. Never move the consumed cursor backwards: doing so replays the
    // same animation every time pause is toggled. A genuine PV restart is
    // handled by load_scripts_for_pv when the owning runtime is reloaded.
    g_next_event = std::max(g_next_event,
        script_pv::first_event_after(g_events, tick));
    g_last_tick = tick;
}

void load_scripts_for_pv(std::int32_t pv_id, std::int32_t tick) {
    g_events.clear();
    g_loaded_paths = find_script_pv_files(pv_id);
    g_loaded_pv_id = pv_id;

    std::size_t source_order = 0;
    std::size_t rejected_control_events = 0;
    std::size_t rejected_invalid_performer_events = 0;
    for (const auto& path : g_loaded_paths) {
        std::vector<script_pv::DscEvent> parsed;
        std::string error;
        if (!script_pv::parse_ft_dsc(path, parsed, error)) {
            log_line("Rejected " + path.string() + ": " + error);
            continue;
        }
        for (auto& event : parsed) {
            if (is_control_opcode(event.opcode)) {
                ++rejected_control_events;
                continue;
            }
            if (!has_valid_performer_index(event)) {
                ++rejected_invalid_performer_events;
                continue;
            }
            event.source_order = source_order++;
            g_events.push_back(std::move(event));
        }
    }

    std::stable_sort(g_events.begin(), g_events.end(),
        [](const script_pv::DscEvent& left,
            const script_pv::DscEvent& right) {
            if (left.time != right.time)
                return left.time < right.time;
            return left.source_order < right.source_order;
        });

    reset_timeline_for_load(tick);

    char message[256]{};
    std::snprintf(message, std::size(message),
        "PV %d: loaded %zu script_pv file(s), %zu event(s), "
        "ignored %zu flow/scoring and %zu invalid performer event(s).",
        pv_id, g_loaded_paths.size(), g_events.size(),
        rejected_control_events, rejected_invalid_performer_events);
    log_line(message);
    for (const auto& root : g_search_roots)
        log_line("  searched " + root.string());
    for (const auto& path : g_loaded_paths)
        log_line("  " + path.string());
}

std::int32_t current_pv_id(std::uintptr_t self) {
    if (!self)
        return -1;
    void* pv_game =
        *reinterpret_cast<void**>(self + kPvGamePointerOffset);
    if (!pv_game)
        return -1;
    const auto* base = reinterpret_cast<const std::uint8_t*>(pv_game);
    const std::int32_t pv_id =
        *reinterpret_cast<const std::int32_t*>(base + kPvIdOffset);
    // Custom song packs use five-digit PV IDs such as 70212.  The PV game
    // stores this as a signed int32, so only negative values are invalid.
    return pv_id >= 0 ? pv_id : -1;
}

void dispatch_supplemental_event(std::uintptr_t self, float delta_time,
    std::int64_t current_time, const script_pv::DscEvent& event) {
    const std::size_t command_words = event.parameters.size() + 1;
    if (command_words + 1 > kScratchWordCount) {
        log_line("Skipped oversized supplemental opcode "
            + std::to_string(event.opcode));
        return;
    }

    auto* object = reinterpret_cast<std::uint8_t*>(self);
    auto* dsc_buffer =
        reinterpret_cast<std::int32_t*>(object + kDscBufferOffset);
    auto* scratch = dsc_buffer + kScratchWordIndex;

    std::array<std::int32_t, kScratchWordCount> saved_scratch{};
    std::memcpy(saved_scratch.data(), scratch,
        saved_scratch.size() * sizeof(std::int32_t));
    const std::int32_t saved_counter =
        *reinterpret_cast<std::int32_t*>(object + kDscCounterOffset);
    const std::int32_t saved_rob_id =
        *reinterpret_cast<std::int32_t*>(object + kRobIdOffset);
    const std::int64_t saved_dsc_time =
        *reinterpret_cast<std::int64_t*>(object + kDscTimeOffset);
    const std::uint8_t saved_has_perf_id =
        *reinterpret_cast<std::uint8_t*>(object + kHasPerfIdOffset);
    const std::int32_t saved_branch_mode =
        *reinterpret_cast<std::int32_t*>(object + kBranchModeOffset);
    const std::uint8_t saved_play = object[8];

    std::fill_n(scratch, kScratchWordCount, 0);
    scratch[0] = event.opcode;
    std::copy(event.parameters.begin(), event.parameters.end(), scratch + 1);
    scratch[command_words] = 0;

    *reinterpret_cast<std::int32_t*>(object + kDscCounterOffset) =
        kScratchWordIndex;
    *reinterpret_cast<std::int32_t*>(object + kRobIdOffset) =
        g_supplemental_rob_id;
    *reinterpret_cast<std::int64_t*>(object + kDscTimeOffset) =
        static_cast<std::int64_t>(event.time) * 10000LL;
    *reinterpret_cast<std::uint8_t*>(object + kHasPerfIdOffset) = 1;
    *reinterpret_cast<std::int32_t*>(object + kBranchModeOffset) = 0;

    float time_offset = static_cast<float>(
        static_cast<double>(current_time
            - static_cast<std::int64_t>(event.time) * 10000LL)
        * 0.000000001);
    bool music_play = false;
    g_in_supplemental_dispatch = true;
    g_original_dsc_ctrl(self, delta_time, current_time, &time_offset,
        &music_play, false, true);
    g_in_supplemental_dispatch = false;

    g_supplemental_rob_id =
        *reinterpret_cast<std::int32_t*>(object + kRobIdOffset);
    object[8] = saved_play;
    *reinterpret_cast<std::int32_t*>(object + kDscCounterOffset) =
        saved_counter;
    *reinterpret_cast<std::int32_t*>(object + kRobIdOffset) = saved_rob_id;
    *reinterpret_cast<std::int64_t*>(object + kDscTimeOffset) =
        saved_dsc_time;
    *reinterpret_cast<std::uint8_t*>(object + kHasPerfIdOffset) =
        saved_has_perf_id;
    *reinterpret_cast<std::int32_t*>(object + kBranchModeOffset) =
        saved_branch_mode;
    std::memcpy(scratch, saved_scratch.data(),
        saved_scratch.size() * sizeof(std::int32_t));
}

void update_supplemental_timeline(std::uintptr_t self, float delta_time,
    std::int64_t current_time) {
    const std::int32_t pv_id = current_pv_id(self);
    if (pv_id < 0)
        return;
    const std::int32_t tick = current_time > 0
        ? static_cast<std::int32_t>(current_time / 10000LL) : 0;
    // A PV may drive more than one native DSC controller. They share the
    // active PV timeline, so changing controller instances must not reload
    // and rewind the supplemental stream.
    if (pv_id != g_loaded_pv_id)
        load_scripts_for_pv(pv_id, tick);
    else if (g_last_tick >= 0
        && tick + kRewindToleranceTicks < g_last_tick)
        reset_timeline_for_seek(tick);

    g_last_tick = tick;

    const std::size_t due_end = script_pv::first_event_after(g_events, tick);
    if (due_end <= g_next_event)
        return;

    // Animation commands 18..23 are state setters. After a dropped frame,
    // only the newest state for each command/performer can be visible. Avoid
    // hundreds of stale native-dispatch calls while preserving every other
    // event and the stable merged ordering of final states.
    std::vector<bool> dispatch(due_end - g_next_event, true);
    std::set<std::uint64_t> latest_animation_state;
    for (std::size_t i = due_end; i-- > g_next_event;) {
        const script_pv::DscEvent& event = g_events[i];
        if (event.opcode < 18 || event.opcode > 23
            || event.parameters.empty())
            continue;
        const std::uint64_t key =
            (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(event.opcode)) << 32)
            | static_cast<std::uint32_t>(event.parameters[0]);
        if (!latest_animation_state.insert(key).second)
            dispatch[i - g_next_event] = false;
    }

    const std::size_t due_begin = g_next_event;
    // Mark the batch consumed before entering the detour chain. This prevents
    // pause hooks or another plugin's re-entry from dispatching it twice.
    g_next_event = due_end;
    for (std::size_t i = due_begin; i < due_end; ++i) {
        if (dispatch[i - due_begin])
            dispatch_supplemental_event(self, delta_time, current_time,
                g_events[i]);
    }
}

bool __fastcall dsc_ctrl_hook(std::uintptr_t self, float delta_time,
    std::int64_t current_time, float* dsc_time_offset, bool* music_play,
    bool end_pv, bool ignore_targets) {
    const bool result = g_original_dsc_ctrl(self, delta_time, current_time,
        dsc_time_offset, music_play, end_pv, ignore_targets);
    if (!g_in_supplemental_dispatch && g_frame_pv_game && self) {
        void* owner = *reinterpret_cast<void**>(
            self + kPvGamePointerOffset);
        if (owner == g_frame_pv_game) {
            g_frame_controller = self;
            g_frame_controller_playing =
                *reinterpret_cast<const std::uint8_t*>(
                    self + kDscPlayingOffset) != 0;
            // Prefer the controller that reached its native frame boundary.
            if (!result) {
                g_frame_boundary_controller = self;
                g_frame_boundary_playing = g_frame_controller_playing;
            }
        }
    }
    return result;
}

const IMAGE_NT_HEADERS64* executable_nt_headers() {
    const auto base = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleW(nullptr));
    if (!base)
        return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE
        || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    return nt;
}

bool executable_matches_megamix() {
    const IMAGE_NT_HEADERS64* nt = executable_nt_headers();
    if (!nt
        || nt->FileHeader.TimeDateStamp != kExeTimestamp
        || nt->OptionalHeader.SizeOfImage != kExeImageSize
        || nt->OptionalHeader.SizeOfImage
            < kDscCtrlRva + sizeof(kDscCtrlSignature))
        return false;

    const auto base = reinterpret_cast<std::uintptr_t>(
        GetModuleHandleW(nullptr));
    return std::memcmp(reinterpret_cast<const void*>(base + kDscCtrlRva),
        kDscCtrlSignature, sizeof(kDscCtrlSignature)) == 0;
}

bool install_dsc_ctrl_hook() {
    const auto base =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target =
        reinterpret_cast<std::uint8_t*>(base + kDscCtrlRva);

    g_original_dsc_ctrl = reinterpret_cast<DscCtrl>(target);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(
        reinterpret_cast<PVOID*>(&g_original_dsc_ctrl),
        reinterpret_cast<PVOID>(dsc_ctrl_hook));
    const LONG committed = attached == NO_ERROR
        ? DetourTransactionCommit() : DetourTransactionAbort();
    if (attached != NO_ERROR || committed != NO_ERROR) {
        log_line("Detours could not attach the integrated MegaMix FT dispatcher hook.");
        return false;
    }
    log_line("Integrated ScriptPv attached to DivaMegaMix FT dispatcher.");
    return true;
}

} // namespace

bool is_supported_executable() {
    return executable_matches_megamix();
}

bool initialize(HMODULE plugin_module) {
    if (g_hook_installed)
        return true;
    g_module = plugin_module;
    if (!executable_matches_megamix())
        return false;

    log_line("Misaki&MaxSongPack: initializing integrated ScriptPv for MegaMix.");
    g_hook_installed = install_dsc_ctrl_hook();
    return g_hook_installed;
}

void begin_pv_frame(void* pv_game) {
    g_frame_pv_game = pv_game;
    g_frame_controller = 0;
    g_frame_boundary_controller = 0;
    g_frame_controller_playing = false;
    g_frame_boundary_playing = false;
}

void end_pv_frame(void* pv_game, float delta_time,
    std::int64_t current_time_ns) {
    if (!pv_game || pv_game != g_frame_pv_game) {
        g_frame_pv_game = nullptr;
        g_frame_controller = 0;
        g_frame_boundary_controller = 0;
        g_frame_controller_playing = false;
        g_frame_boundary_playing = false;
        return;
    }

    const std::uintptr_t controller = g_frame_boundary_controller
        ? g_frame_boundary_controller : g_frame_controller;
    const bool playing = g_frame_boundary_controller
        ? g_frame_boundary_playing : g_frame_controller_playing;
    const std::int32_t tick = current_time_ns > 0
        ? static_cast<std::int32_t>(current_time_ns / 10000LL) : 0;
    // Clear frame capture before dispatching through the native trampoline so
    // other detours cannot make supplemental calls look like main DSC calls.
    g_frame_pv_game = nullptr;
    g_frame_controller = 0;
    g_frame_boundary_controller = 0;
    g_frame_controller_playing = false;
    g_frame_boundary_playing = false;
    if (!g_hook_installed || !controller)
        return;

    const std::int32_t pv_id = current_pv_id(controller);
    const bool owner_changed = g_session_pv_game != pv_game;
    const bool restarted_at_beginning = !owner_changed
        && g_session_last_tick > kRestartPreviousTicks
        && tick <= kRestartNearStartTicks;
    const bool session_started = playing
        && (owner_changed || !g_session_active || restarted_at_beginning);

    if (!playing) {
        // The native END command clears this controller flag. End the
        // supplemental tracks in the same frame instead of allowing their
        // remaining events to leak into a later run of the same PV.
        g_session_pv_game = pv_game;
        g_session_active = false;
        g_session_last_tick = tick;
        return;
    }

    if (session_started) {
        if (pv_id == g_loaded_pv_id)
            reset_timeline_for_load(tick);
        if (pv_id >= 0)
            log_line("PV " + std::to_string(pv_id)
                + ": native DSC session started; supplemental tracks reset.");
    }

    g_session_pv_game = pv_game;
    g_session_active = true;
    g_session_last_tick = tick;
    if (pv_id >= 0)
        update_supplemental_timeline(controller, delta_time,
            current_time_ns);
}

} // namespace script_pv_megamix
