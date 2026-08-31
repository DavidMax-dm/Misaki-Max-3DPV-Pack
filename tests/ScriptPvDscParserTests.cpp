#include "ScriptPvDscParser.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void write_words(const std::filesystem::path& path,
    const std::vector<std::int32_t>& words) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(words.data()),
        static_cast<std::streamsize>(
            words.size() * sizeof(std::int32_t)));
}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAILED: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / "misaki_script_pv_parser_tests";
    std::filesystem::create_directories(directory);
    const std::filesystem::path valid = directory / "pv_943_look.dsc";
    const std::filesystem::path invalid = directory / "bad.dsc";

    write_words(valid, {
        0x15122517,
        1, 1200,
        21, 0, 3, 100, 1000,
        1, 2400,
        19, 0, 0, 3, 80, 750,
        0,
    });
    write_words(invalid, {
        0x15122517,
        1, 1200,
        999,
        0,
    });

    std::vector<script_pv::DscEvent> events;
    std::string error;
    bool ok = script_pv::parse_ft_dsc(valid, events, error);
    bool passed = expect(ok, "valid FT script parses")
        && expect(events.size() == 2, "two non-TIME events are emitted")
        && expect(events[0].time == 1200, "LOOK uses first TIME")
        && expect(events[0].opcode == 21, "LOOK opcode is preserved")
        && expect(events[0].parameters.size() == 4,
            "modern FT LOOK has four parameters")
        && expect(events[1].time == 2400, "MOUTH uses second TIME")
        && expect(events[1].opcode == 19, "MOUTH opcode is preserved")
        && expect(events[1].parameters.size() == 5,
            "modern FT MOUTH has five parameters");

    std::vector<script_pv::DscEvent> timeline(5);
    timeline[0].time = 100;
    timeline[1].time = 200;
    timeline[2].time = 200;
    timeline[3].time = 200;
    timeline[4].time = 300;
    passed = expect(script_pv::first_event_at_or_after(timeline, 200) == 1,
        "load retains all equal-time merged events") && passed;
    passed = expect(script_pv::first_event_after(timeline, 200) == 4,
        "resume skips consumed equal-time events") && passed;
    passed = expect(script_pv::first_event_after(timeline, 150) == 1,
        "seek retains every future merged event") && passed;

    passed = expect(script_pv::matches_pv_script_name(
            "pv_943_look.dsc", 943), "padded PV filename matches")
        && expect(script_pv::matches_pv_script_name(
            "pv_7.dsc", 7), "natural-number PV filename matches")
        && expect(script_pv::matches_pv_script_name(
            "PV_007-mouth.DSC", 7), "case and padded suffix match")
        && expect(script_pv::matches_pv_script_name(
            "pv_1001_extra.dsc", 1001), "four-digit PV filename matches")
        && expect(!script_pv::matches_pv_script_name(
            "pv_70.dsc", 7), "different PV ID is rejected")
        && expect(!script_pv::matches_pv_script_name(
            "pv_7x.dsc", 7), "invalid suffix is rejected")
        && passed;

    events.clear();
    error.clear();
    ok = script_pv::parse_ft_dsc(invalid, events, error);
    passed = expect(!ok, "unknown FT opcode is rejected")
        && expect(error.find("999") != std::string::npos,
            "unknown opcode appears in the error") && passed;

    std::error_code remove_error;
    std::filesystem::remove_all(directory, remove_error);
    if (passed)
        std::cout << "Integrated ScriptPv parser tests passed.\n";
    return passed ? 0 : 1;
}
