#include "ScriptPvDscParser.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace script_pv {
namespace {

constexpr std::int32_t kFtSignature = 0x15122517;
constexpr std::int32_t kEndOpcode = 0;
constexpr std::int32_t kTimeOpcode = 1;

// Modern FT parameter counts from ReDIVA's dsc_ac_func table. The count does
// not include the opcode word itself.
constexpr std::array<std::uint8_t, 107> kFtParameterCounts = {
     0,  1,  4,  2,  2,  2,  7,  4,  2,  6,  2,  1,  6,  2,  1,  1,
     3,  2,  3,  5,  5,  4,  4,  5,  2,  0,  2,  4,  2,  2,  1, 21,
     0,  3,  2,  5,  1,  1,  7,  1,  1,  2,  1,  2,  1,  2,  3,  3,
     1,  2,  2,  3,  6,  6,  1,  1,  2,  3,  1,  2,  2,  4,  4,  1,
     2,  1,  2,  1,  1,  3,  3,  3,  2,  1,  9,  3,  2,  4,  2,  3,
     2, 24,  1,  2,  1,  3,  1,  3,  4,  1,  2,  6,  3,  2,  3,  3,
     4,  1,  1,  3,  3,  4,  1,  3,  3,  8,  2,
};

std::int32_t read_i32(const std::vector<std::uint8_t>& bytes,
    std::size_t offset) {
    std::int32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

int ft_parameter_count(std::int32_t opcode) {
    // MegaMix+ build 6F4DF714 reads both movie index and display mode in its
    // MOVIE_CUT_CHG (102) handler.  ReDIVA's generic table lists one, which
    // desynchronizes every following word in an MM+ supplemental DSC.
    if (opcode == 102)
        return 2;
    if (opcode < 0
        || static_cast<std::size_t>(opcode) >= kFtParameterCounts.size())
        return -1;
    return kFtParameterCounts[static_cast<std::size_t>(opcode)];
}

std::size_t first_event_at_or_after(const std::vector<DscEvent>& events,
    std::int32_t tick) {
    return static_cast<std::size_t>(std::lower_bound(events.begin(),
        events.end(), tick, [](const DscEvent& event, std::int32_t value) {
            return event.time < value;
        }) - events.begin());
}

std::size_t first_event_after(const std::vector<DscEvent>& events,
    std::int32_t tick) {
    return static_cast<std::size_t>(std::upper_bound(events.begin(),
        events.end(), tick, [](std::int32_t value, const DscEvent& event) {
            return value < event.time;
        }) - events.begin());
}

bool matches_pv_script_name(const std::filesystem::path& path,
    std::int32_t pv_id) {
    if (pv_id < 0)
        return false;

    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (extension != ".dsc")
        return false;

    std::string stem = path.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (stem.size() <= 3 || stem.compare(0, 3, "pv_") != 0)
        return false;

    std::size_t position = 3;
    std::int32_t parsed_id = 0;
    bool found_digit = false;
    while (position < stem.size()
        && std::isdigit(static_cast<unsigned char>(stem[position]))) {
        found_digit = true;
        const int digit = stem[position] - '0';
        // Mod PV IDs are not constrained to the original four-digit range
        // (for example, 70212).  Reject only a genuine int32 overflow.
        if (parsed_id > (std::numeric_limits<std::int32_t>::max() - digit) / 10)
            return false;
        parsed_id = parsed_id * 10 + digit;
        ++position;
    }
    if (!found_digit || parsed_id != pv_id)
        return false;
    return position == stem.size()
        || stem[position] == '_' || stem[position] == '-';
}

bool parse_ft_dsc(const std::filesystem::path& path,
    std::vector<DscEvent>& events, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open FT DSC";
        return false;
    }

    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)), {});
    if (bytes.size() < 8 || bytes.size() % sizeof(std::int32_t)) {
        error = "FT DSC size is not a non-empty 32-bit word stream";
        return false;
    }
    if (read_i32(bytes, 0) != kFtSignature) {
        error = "missing FT signature 0x15122517";
        return false;
    }

    std::size_t offset = sizeof(std::int32_t);
    std::int32_t time = 0;
    bool found_end = false;
    while (offset + sizeof(std::int32_t) <= bytes.size()) {
        const std::int32_t opcode = read_i32(bytes, offset);
        offset += sizeof(std::int32_t);
        if (opcode == kEndOpcode) {
            found_end = true;
            break;
        }

        const int parameter_count = ft_parameter_count(opcode);
        if (parameter_count < 0) {
            error = "unsupported FT opcode " + std::to_string(opcode);
            return false;
        }
        const std::size_t parameter_bytes =
            static_cast<std::size_t>(parameter_count) * sizeof(std::int32_t);
        if (offset + parameter_bytes > bytes.size()) {
            error = "truncated parameters for FT opcode "
                + std::to_string(opcode);
            return false;
        }

        if (opcode == kTimeOpcode) {
            time = read_i32(bytes, offset);
        }
        else {
            DscEvent event{};
            event.time = time;
            event.opcode = opcode;
            event.source = path;
            event.parameters.reserve(static_cast<std::size_t>(parameter_count));
            for (int i = 0; i < parameter_count; ++i)
                event.parameters.push_back(read_i32(bytes,
                    offset + static_cast<std::size_t>(i)
                    * sizeof(std::int32_t)));
            events.push_back(std::move(event));
        }
        offset += parameter_bytes;
    }

    if (!found_end) {
        error = "FT DSC is missing END";
        return false;
    }
    return true;
}

} // namespace script_pv
