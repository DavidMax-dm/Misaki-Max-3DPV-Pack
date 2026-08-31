#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace script_pv {

struct DscEvent {
    std::int32_t time{};
    std::int32_t opcode{};
    std::vector<std::int32_t> parameters;
    std::filesystem::path source;
    std::size_t source_order{};
};

bool parse_ft_dsc(const std::filesystem::path& path,
    std::vector<DscEvent>& events, std::string& error);

int ft_parameter_count(std::int32_t opcode);

// Cursor helpers for attaching mid-song and seeking without replaying
// already-consumed events. Events must be sorted by time.
std::size_t first_event_at_or_after(const std::vector<DscEvent>& events,
    std::int32_t tick);
std::size_t first_event_after(const std::vector<DscEvent>& events,
    std::int32_t tick);

// Accepts both zero-padded and natural-number PV filenames, for example
// pv_007.dsc, pv_7.dsc, pv_007_look.dsc, and pv_7-mouth.dsc.
bool matches_pv_script_name(const std::filesystem::path& path,
    std::int32_t pv_id);

} // namespace script_pv
