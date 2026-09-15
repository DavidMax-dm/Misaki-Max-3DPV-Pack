#include "EffectScript.hpp"

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
        static_cast<std::streamsize>(words.size() * sizeof(std::int32_t)));
}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAILED: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path()
        / "misaki_effect_script_extension_tests";
    std::filesystem::create_directories(directory);
    const auto valid = directory / "script_effect_subframerender.dsc";
    const auto truncated = directory / "script_effect_truncated.dsc";

    // Plugin-owned FT stream: ON at 1000; same-tick OFF then ON at 2000;
    // final OFF at 3000. It must never be submitted to MM+'s native loader.
    write_words(valid, {
        0x15122517,
        1, 1000, 170, 1,
        1, 2000, 170, 0, 170, 1,
        1, 3000, 170, 0,
        0,
    });
    write_words(truncated, {0x15122517, 1, 1000, 170});

    const auto loaded = effect_script::load_file(valid);
    bool passed = expect(loaded.error.empty(), "valid extension parses")
        && expect(loaded.events.size() == 4, "all four latch events parse")
        && expect(loaded.events[1].time == 2000
            && loaded.events[1].value == 0, "same-tick OFF stays first")
        && expect(loaded.events[2].time == 2000
            && loaded.events[2].value == 1, "same-tick ON stays last");

    const auto bad = effect_script::load_file(truncated);
    passed = expect(!bad.error.empty(), "truncated 170 is rejected") && passed;

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    if (passed)
        std::cout << "Effect-script SUBFRAMERENDER tests passed.\n";
    return passed ? 0 : 1;
}
