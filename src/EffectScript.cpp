#include "EffectScript.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace effect_script {
namespace {

constexpr int32_t kSignatureFt = 0x15122517;
constexpr int32_t kSignatureFtEditor = 0x14050921;
constexpr int32_t kSignatureF2 = 0x13120420;
constexpr int32_t kChunkEof = 0x43464F45; // "EOFC"
constexpr int32_t kFuncEnd = 0;
constexpr int32_t kFuncTime = 1;
constexpr int32_t kFuncChangeField = 14;
constexpr int32_t kFuncPvEnd = 32;
constexpr int32_t kFuncStageEffect = 111; // Native DSC_X_STAGE_EFFECT.
constexpr int32_t kFuncStopEffect = 164; // Plugin extension; avoids DSC_X_SONG_EFFECT at 112.
constexpr int32_t kFuncAutoDof = 165; // Plugin extension: AUTO_DOF(enable).
constexpr int32_t kFuncNoise = 166; // Plugin extension: NOISE(...), screen distortion.
constexpr int32_t kFuncPjskDistortion = 167;
constexpr int32_t kFuncPjskChromatic = 168;
constexpr int32_t kFuncPjskOverlay = 169;
constexpr const char* kGeneratedBegin = "# BEGIN Misaki&MaxSongPack generated pv_field";
constexpr const char* kGeneratedEnd = "# END Misaki&MaxSongPack generated pv_field";

constexpr int32_t kFtOldFuncLengths[] = {
    0, 1, 4, 2, 2, 2, 7, 4, 2, 6, 2, 1, 6, 2, 1, 1,
    3, 2, 3, 5, 5, 4, 4, 5, 2, 0, 2, 4, 2, 2, 1, 21,
    0, 3, 2, 5, 1, 1, 7, 1, 1, 2, 1, 2, 1, 2, 3, 3,
    1, 2, 2, 3, 6, 6, 1, 1, 2, 3, 1, 2, 2, 4, 4, 1,
    2, 1, 2, 1, 1, 3, 3, 2, 2, 1, 9, 3, 2, 4, 2, 3,
    2, 24, 1, 2, 1, 3, 1, 3, 4, 1, 2, 6, 3, 2, 3, 3,
    4, 1, 1, 3, 3, 4, 1, 3, 3, 8, 2,
};

std::string trim(std::string value) {
    const char* whitespace = " \t\r\n";
    const size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::string::npos)
        return {};

    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

std::string normalize_path_separators(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string narrow_path(const std::filesystem::path& path) {
    return path.string();
}

std::string strip_comment(const std::string& line) {
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

std::vector<std::string> parse_toml_string_array(const std::string& value) {
    std::vector<std::string> strings;
    static const std::regex string_regex(R"(["']([^"']+)["'])");

    for (std::sregex_iterator i(value.begin(), value.end(), string_regex), end; i != end; ++i)
        strings.push_back((*i)[1].str());

    return strings;
}

int32_t read_i32_le(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<int32_t>(
        static_cast<uint32_t>(bytes[offset + 0])
        | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24));
}

uint32_t read_u32_be(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(
        (static_cast<uint32_t>(bytes[offset + 0]) << 24)
        | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<uint32_t>(bytes[offset + 3]));
}

bool is_pvsc(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 4
        && bytes[0] == 'P'
        && bytes[1] == 'V'
        && bytes[2] == 'S'
        && bytes[3] == 'C';
}

bool parse_records(
    const std::vector<uint8_t>& bytes,
    size_t offset,
    std::vector<Event>& events,
    std::string& error) {

    int32_t time = 0;
    while (offset + 4 <= bytes.size()) {
        const int32_t func = read_i32_le(bytes, offset);
        offset += 4;

        switch (func) {
        case kFuncEnd:
        case kChunkEof:
            std::sort(events.begin(), events.end(),
                [](const Event& lhs, const Event& rhs) { return lhs.time < rhs.time; });
            return true;

        case kFuncTime:
            if (offset + 4 > bytes.size()) {
                error = "TIME is truncated";
                return false;
            }

            time = read_i32_le(bytes, offset);
            offset += 4;
            break;

        case kFuncStageEffect:
        case kFuncStopEffect: {
            if (offset + 8 > bytes.size()) {
                error = func == kFuncStageEffect
                    ? "STAGE_EFFECT is truncated"
                    : "STOP_EFFECT is truncated";
                return false;
            }

            Event event;
            event.action = func == kFuncStageEffect ? EventAction::Play : EventAction::Stop;
            event.time = time;
            event.resource_index = read_i32_le(bytes, offset);
            event.effect_index = read_i32_le(bytes, offset + 4);
            offset += 8;
            events.push_back(event);
            break;
        }

        case kFuncAutoDof: {
            if (offset + 4 > bytes.size()) {
                error = "AUTO_DOF is truncated";
                return false;
            }

            Event event;
            event.action = EventAction::AutoDof;
            event.time = time;
            event.value = read_i32_le(bytes, offset);
            offset += 4;
            events.push_back(event);
            break;
        }

        case kFuncNoise: {
            constexpr size_t parameter_count = 7;
            if (offset + parameter_count * 4 > bytes.size()) {
                error = "NOISE is truncated";
                return false;
            }

            Event event;
            event.action = EventAction::Noise;
            event.time = time;
            for (size_t i = 0; i < parameter_count; ++i)
                event.noise_values[i] = read_i32_le(bytes, offset + i * 4);
            offset += parameter_count * 4;
            events.push_back(event);
            break;
        }

        case kFuncPjskDistortion:
        case kFuncPjskChromatic:
        case kFuncPjskOverlay: {
            const size_t parameter_count = func == kFuncPjskDistortion ? 12
                : func == kFuncPjskChromatic ? 20 : 9;
            if (offset + parameter_count * 4 > bytes.size()) {
                error = "PJSK post-effect command is truncated";
                return false;
            }
            Event event;
            event.action = func == kFuncPjskDistortion ? EventAction::PjskDistortion
                : func == kFuncPjskChromatic ? EventAction::PjskChromatic
                : EventAction::PjskOverlay;
            event.time = time;
            if (func == kFuncPjskDistortion)
                for (size_t i = 0; i < parameter_count; ++i)
                    event.distortion_values[i] = read_i32_le(bytes, offset + i * 4);
            else if (func == kFuncPjskChromatic)
                for (size_t i = 0; i < parameter_count; ++i)
                    event.chromatic_values[i] = read_i32_le(bytes, offset + i * 4);
            else
                for (size_t i = 0; i < parameter_count; ++i)
                    event.overlay_values[i] = read_i32_le(bytes, offset + i * 4);
            offset += parameter_count * 4;
            events.push_back(event);
            break;
        }

        default:
            error = "effect DSC contains unsupported function ID " + std::to_string(func);
            return false;
        }
    }

    error = "effect DSC has no END record";
    return false;
}

bool parse_dsc(const std::vector<uint8_t>& bytes, std::vector<Event>& events, std::string& error) {
    if (bytes.size() < 8 || bytes.size() % 4) {
        error = "effect DSC size is invalid";
        return false;
    }

    if (is_pvsc(bytes)) {
        for (size_t offset = 4; offset + 8 <= bytes.size(); offset += 4) {
            if (read_i32_le(bytes, offset) != kSignatureF2)
                continue;

            // X-style PVSC scripts store an ID after the inner signature.
            return parse_records(bytes, offset + 8, events, error);
        }

        error = "PVSC effect DSC has no inner DSC signature";
        return false;
    }

    size_t offset = 0;
    if (read_i32_le(bytes, 0) == kSignatureFt
        || read_i32_le(bytes, 0) == kSignatureFtEditor)
        offset = 4;

    return parse_records(bytes, offset, events, error);
}

std::vector<std::filesystem::path> find_mdata_files(
    const std::vector<std::filesystem::path>& data_roots,
    const std::regex& file_regex,
    const char* direct_name) {

    std::vector<std::filesystem::path> files;

    for (const std::filesystem::path& root : data_roots) {
        if (!std::filesystem::exists(root))
            continue;

        const std::filesystem::path direct_file = root / direct_name;
        if (std::filesystem::exists(direct_file))
            files.push_back(direct_file);

        const std::filesystem::path rom_file = root / "rom" / direct_name;
        if (std::filesystem::exists(rom_file))
            files.push_back(rom_file);

        std::error_code ec;
        for (const std::filesystem::directory_entry& entry :
            std::filesystem::recursive_directory_iterator(root,
                std::filesystem::directory_options::skip_permission_denied, ec)) {

            if (ec)
                break;

            if (!entry.is_regular_file(ec))
                continue;

            const std::string name = entry.path().filename().string();
            if (std::regex_match(name, file_regex))
                files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::vector<std::filesystem::path> find_pv_db_files(const std::vector<std::filesystem::path>& data_roots) {
    static const std::regex file_regex(R"(.*pv_db.*\.txt$)", std::regex::icase);
    return find_mdata_files(data_roots, file_regex, "mdata_pv_db.txt");
}

std::vector<std::filesystem::path> find_pv_field_files(const std::vector<std::filesystem::path>& data_roots) {
    static const std::regex file_regex(R"(.*pv_field.*\.txt$)", std::regex::icase);
    return find_mdata_files(data_roots, file_regex, "mdata_pv_field.txt");
}

std::filesystem::path resolve_script_path(
    const std::filesystem::path& data_root,
    const std::string& script_file) {

    const std::string value = normalize_path_separators(trim(script_file));
    std::filesystem::path path = data_root / value;
    if (std::filesystem::exists(path))
        return path;

    if (value.rfind("rom\\", 0) == 0) {
        path = data_root / value.substr(4);
        if (std::filesystem::exists(path))
            return path;
    }

    return data_root / value;
}

std::filesystem::path find_particle_file(
    const std::vector<std::filesystem::path>& data_roots,
    const std::string& resource_name) {

    std::vector<std::string> file_names;
    file_names.push_back(resource_name);
    if (resource_name.find('.') == std::string::npos)
        file_names.push_back(resource_name + ".farc");

    for (const std::filesystem::path& root : data_roots)
        for (const std::string& file_name : file_names) {
            std::filesystem::path path = root / "rom" / "particle" / file_name;
            if (std::filesystem::exists(path))
                return path;

            path = root / "particle" / file_name;
            if (std::filesystem::exists(path))
                return path;
        }

    return {};
}

std::filesystem::path find_pv_field_output_file(
    const std::vector<std::filesystem::path>& data_roots) {

    for (const std::filesystem::path& root : data_roots) {
        std::filesystem::path path = root / "rom" / "mod_pv_field.txt";
        if (std::filesystem::exists(path))
            return path;

        path = root / "mod_pv_field.txt";
        if (std::filesystem::exists(path))
            return path;
    }

    if (data_roots.empty())
        return {};

    return data_roots.front() / "rom" / "mod_pv_field.txt";
}

bool rewrite_without_generated_block(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "failed to open pv_field for generated block cleanup";
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool skipping = false;
    bool changed = false;
    while (std::getline(input, line)) {
        const std::string trimmed = trim(line);
        if (trimmed == kGeneratedBegin) {
            skipping = true;
            changed = true;
            continue;
        }

        if (trimmed == kGeneratedEnd) {
            skipping = false;
            continue;
        }

        if (!skipping)
            lines.push_back(line);
    }

    if (!changed)
        return true;

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "failed to rewrite pv_field during generated block cleanup";
        return false;
    }

    for (const std::string& output_line : lines)
        output << output_line << "\n";

    return true;
}

void append_unique(std::vector<std::string>& list, const std::string& value) {
    if (std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}

FieldEffects* find_or_create_field(std::map<int32_t, FieldEffects>& fields, int32_t field_index) {
    FieldEffects& field = fields[field_index];
    field.field_index = field_index;
    return &field;
}

int32_t find_effect_field_for_time(
    const std::map<int32_t, int32_t>& fields_by_time,
    int32_t time,
    bool& exact) {

    exact = false;
    if (fields_by_time.empty())
        return time <= 0 ? 1 : 0;

    const auto upper = fields_by_time.upper_bound(time);
    if (upper == fields_by_time.begin())
        return time <= 0 ? 1 : 0;

    auto iter = upper;
    --iter;
    exact = iter->first == time;
    return iter->second;
}

std::string field_key_prefix(int32_t pv_id, int32_t field_index, const char* list_name) {
    char buffer[128]{};
    snprintf(buffer, sizeof(buffer), "pv_%03d.field.%02d.%s", pv_id, field_index, list_name);
    return buffer;
}

std::vector<std::string> parse_geff_effect_names(const std::vector<uint8_t>& bytes) {
    std::vector<std::string> names;

    for (size_t offset = 0; offset + 0x24 <= bytes.size(); offset++) {
        if (bytes[offset + 0] != 'G'
            || bytes[offset + 1] != 'E'
            || bytes[offset + 2] != 'F'
            || bytes[offset + 3] != 'F')
            continue;

        const size_t data_offset = offset + 0x20;
        if (data_offset + 4 > bytes.size())
            continue;

        const uint32_t count = static_cast<uint32_t>(read_i32_le(bytes, data_offset));
        const size_t names_offset = data_offset + 4;
        if (count > 0x10000 || names_offset + static_cast<size_t>(count) * 0x80 > bytes.size())
            continue;

        names.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            const size_t name_offset = names_offset + static_cast<size_t>(i) * 0x80;
            size_t length = 0;
            while (length < 0x80 && bytes[name_offset + length])
                length++;

            names.emplace_back(reinterpret_cast<const char*>(bytes.data() + name_offset), length);
        }
        return names;
    }

    return names;
}

std::vector<std::string> load_particle_effect_names(const std::filesystem::path& farc_path) {
    std::ifstream stream(farc_path, std::ios::binary);
    if (!stream)
        return {};

    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    if (bytes.size() < 0x10
        || bytes[0] != 'F'
        || bytes[1] != 'A'
        || bytes[2] != 'r'
        || bytes[3] != 'c')
        return {};

    const size_t header_size = static_cast<size_t>(read_u32_be(bytes, 4)) + 8;
    if (header_size > bytes.size())
        return {};

    size_t offset = 0x0C;
    while (offset < header_size) {
        const size_t name_offset = offset;
        while (offset < header_size && bytes[offset])
            offset++;

        if (offset >= header_size)
            break;

        std::string name(reinterpret_cast<const char*>(bytes.data() + name_offset), offset - name_offset);
        offset++;

        if (offset + 8 > header_size)
            break;

        const size_t data_offset = read_u32_be(bytes, offset);
        const size_t data_size = read_u32_be(bytes, offset + 4);
        offset += 8;

        if (name.size() < 4 || name.substr(name.size() - 4) != ".lst")
            continue;

        if (data_offset + data_size > bytes.size())
            return {};

        std::vector<uint8_t> list_bytes(bytes.begin() + data_offset, bytes.begin() + data_offset + data_size);
        return parse_geff_effect_names(list_bytes);
    }

    return {};
}

} // namespace

std::filesystem::path find_mod_directory(const std::filesystem::path& start) {
    std::filesystem::path dir = std::filesystem::absolute(start);
    if (std::filesystem::is_regular_file(dir))
        dir = dir.parent_path();

    while (!dir.empty()) {
        if (std::filesystem::exists(dir / "config.toml"))
            return dir;

        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }

    return std::filesystem::absolute(start);
}

std::vector<std::filesystem::path> find_data_roots_from_config(
    const std::filesystem::path& mod_directory) {

    std::vector<std::filesystem::path> roots;
    std::ifstream stream(mod_directory / "config.toml");
    if (!stream) {
        roots.push_back(mod_directory);
        return roots;
    }

    static const std::regex include_regex(R"(^\s*include\s*=\s*\[(.*)\]\s*$)",
        std::regex::icase);

    std::string line;
    while (std::getline(stream, line)) {
        std::smatch match;
        line = strip_comment(line);
        if (!std::regex_match(line, match, include_regex))
            continue;

        for (const std::string& include : parse_toml_string_array(match[1].str()))
            if (!include.empty())
                roots.push_back(mod_directory / normalize_path_separators(include));
    }

    if (roots.empty())
        roots.push_back(mod_directory);

    return roots;
}

std::vector<EffectScriptEntry> find_effect_script_entries(
    const std::vector<std::filesystem::path>& data_roots) {

    static const std::regex key_regex(
        R"((^|[.\s])pv_0*(\d+)\.effect\.script_file\s*=\s*(.+?)\s*$)",
        std::regex::icase);

    std::map<std::pair<int32_t, std::filesystem::path>, EffectScriptEntry> entries;
    const std::vector<std::filesystem::path> pv_db_files = find_pv_db_files(data_roots);
    for (const std::filesystem::path& pv_db_file : pv_db_files) {
        std::ifstream stream(pv_db_file);
        if (!stream)
            continue;

        const std::filesystem::path data_root = pv_db_file.parent_path().filename() == "rom"
            ? pv_db_file.parent_path().parent_path()
            : pv_db_file.parent_path();

        std::string line;
        while (std::getline(stream, line)) {
            std::smatch match;
            line = trim(strip_comment(line));
            if (!std::regex_search(line, match, key_regex))
                continue;

            EffectScriptEntry entry;
            entry.pv_id = std::stoi(match[2].str());
            entry.script_path = resolve_script_path(data_root, match[3].str());
            entries[{ entry.pv_id, entry.script_path }] = std::move(entry);
        }
    }

    std::vector<EffectScriptEntry> result;
    result.reserve(entries.size());
    for (auto& i : entries)
        result.push_back(std::move(i.second));

    return result;
}

std::filesystem::path find_script_file_from_pv_db(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id) {

    char pattern[128]{};
    snprintf(pattern, sizeof(pattern),
        R"((^|[.\s])pv_0*%d\.effect\.script_file\s*=\s*(.+?)\s*$)", pv_id);
    const std::regex key_regex(pattern, std::regex::icase);

    const std::vector<std::filesystem::path> pv_db_files = find_pv_db_files(data_roots);
    for (const std::filesystem::path& pv_db_file : pv_db_files) {
        std::ifstream stream(pv_db_file);
        if (!stream)
            continue;

        const std::filesystem::path data_root = pv_db_file.parent_path().filename() == "rom"
            ? pv_db_file.parent_path().parent_path()
            : pv_db_file.parent_path();

        std::string line;
        while (std::getline(stream, line)) {
            std::smatch match;
            line = trim(strip_comment(line));
            if (!std::regex_search(line, match, key_regex))
                continue;

            return resolve_script_path(data_root, match[2].str());
        }
    }

    return {};
}

std::vector<std::filesystem::path> find_effect_textures_from_pv_db(
    const std::vector<std::filesystem::path>& data_roots, int32_t pv_id) {
    char pattern[160]{};
    snprintf(pattern, sizeof(pattern),
        R"((^|[.\s])pv_0*%d\.effect_texture\.(\d+)\s*=\s*(.+?)\s*$)", pv_id);
    const std::regex key_regex(pattern, std::regex::icase);
    std::map<size_t, std::filesystem::path> indexed;
    for (const auto& db : find_pv_db_files(data_roots)) {
        std::ifstream stream(db);
        if (!stream) continue;
        const auto root = db.parent_path().filename() == "rom"
            ? db.parent_path().parent_path() : db.parent_path();
        std::string line;
        while (std::getline(stream, line)) {
            std::smatch match;
            line = trim(strip_comment(line));
            if (std::regex_search(line, match, key_regex))
                indexed[static_cast<size_t>(std::stoul(match[2].str()))]
                    = resolve_script_path(root, match[3].str());
        }
    }
    std::vector<std::filesystem::path> result;
    if (!indexed.empty()) result.resize(indexed.rbegin()->first + 1);
    for (auto& item : indexed) result[item.first] = std::move(item.second);
    return result;
}

std::vector<std::filesystem::path> find_chart_script_files_from_pv_db(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id) {

    char pattern[192]{};
    snprintf(pattern, sizeof(pattern),
        R"((^|[.\s])pv_0*%d\.difficulty\.[^.]+\.\d+\.script_file_name\s*=\s*(.+?)\s*$)",
        pv_id);
    const std::regex key_regex(pattern, std::regex::icase);

    std::vector<std::filesystem::path> paths;
    const std::vector<std::filesystem::path> pv_db_files = find_pv_db_files(data_roots);
    for (const std::filesystem::path& pv_db_file : pv_db_files) {
        std::ifstream stream(pv_db_file);
        if (!stream)
            continue;

        const std::filesystem::path data_root = pv_db_file.parent_path().filename() == "rom"
            ? pv_db_file.parent_path().parent_path()
            : pv_db_file.parent_path();

        std::string line;
        while (std::getline(stream, line)) {
            std::smatch match;
            line = trim(strip_comment(line));
            if (!std::regex_search(line, match, key_regex))
                continue;

            std::filesystem::path path = resolve_script_path(data_root, match[2].str());
            if (std::filesystem::exists(path))
                paths.push_back(path);
        }
    }

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

std::vector<FieldChange> load_field_changes_from_script(
    const std::filesystem::path& path,
    std::string& error) {

    std::vector<FieldChange> changes;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "failed to open chart DSC";
        return changes;
    }

    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    if (bytes.size() < 8 || bytes.size() % 4) {
        error = "chart DSC size is invalid";
        return changes;
    }

    size_t offset = 0;
    if (read_i32_le(bytes, 0) == kSignatureFt
        || read_i32_le(bytes, 0) == kSignatureFtEditor)
        offset = 4;
    else {
        error = "chart DSC format is not supported for field timing";
        return changes;
    }

    int32_t time = 0;
    while (offset + 4 <= bytes.size()) {
        const int32_t func = read_i32_le(bytes, offset);
        offset += 4;

        if (func == kFuncEnd || func == kFuncPvEnd)
            break;

        if (func < 0 || func >= static_cast<int32_t>(std::size(kFtOldFuncLengths))) {
            error = "chart DSC contains unsupported function ID " + std::to_string(func);
            changes.clear();
            return changes;
        }

        const int32_t length = kFtOldFuncLengths[func];
        const size_t data_offset = offset;
        const size_t byte_length = static_cast<size_t>(length) * 4;
        if (offset + byte_length > bytes.size()) {
            error = "chart DSC function is truncated";
            changes.clear();
            return changes;
        }

        if (func == kFuncTime && length >= 1) {
            time = read_i32_le(bytes, data_offset);
        }
        else if (func == kFuncChangeField && length >= 1) {
            FieldChange change;
            change.time = time;
            change.field_index = read_i32_le(bytes, data_offset);
            change.script_path = path;
            changes.push_back(std::move(change));
        }

        offset += byte_length;
    }

    std::sort(changes.begin(), changes.end(),
        [](const FieldChange& lhs, const FieldChange& rhs) {
            if (lhs.time != rhs.time)
                return lhs.time < rhs.time;
            return lhs.field_index < rhs.field_index;
        });
    return changes;
}

std::vector<FieldEffects> load_field_effects(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id) {

    char pattern[192]{};
    snprintf(pattern, sizeof(pattern),
        R"((^|[.\s])pv_0*%d\.field\.(\d+)\.(effect_rs_list|play_eff_list|stop_eff_list)\.(\d+)\s*=\s*(.+?)\s*$)",
        pv_id);
    const std::regex key_regex(pattern, std::regex::icase);

    std::map<int32_t, FieldEffects> fields;
    const std::vector<std::filesystem::path> pv_field_files = find_pv_field_files(data_roots);
    for (const std::filesystem::path& pv_field_file : pv_field_files) {
        std::ifstream stream(pv_field_file);
        if (!stream)
            continue;

        std::string line;
        while (std::getline(stream, line)) {
            std::smatch match;
            line = trim(strip_comment(line));
            if (!std::regex_search(line, match, key_regex))
                continue;

            const int32_t field_index = std::stoi(match[2].str());
            const std::string list_name = to_lower(match[3].str());
            const size_t item_index = static_cast<size_t>(std::stoi(match[4].str()));
            const std::string value = trim(match[5].str());

            FieldEffects& field = fields[field_index];
            field.field_index = field_index;

            std::vector<std::string>* list = nullptr;
            if (list_name == "effect_rs_list")
                list = &field.effect_rs_list;
            else if (list_name == "play_eff_list")
                list = &field.play_eff_list;
            else if (list_name == "stop_eff_list")
                list = &field.stop_eff_list;

            if (!list)
                continue;

            if (list->size() <= item_index)
                list->resize(item_index + 1);
            (*list)[item_index] = value;
        }
    }

    std::vector<FieldEffects> result;
    result.reserve(fields.size());
    for (auto& i : fields)
        result.push_back(std::move(i.second));

    return result;
}

std::vector<ParticleResource> load_particle_resources(
    const std::vector<std::filesystem::path>& data_roots,
    const std::vector<FieldEffects>& fields) {

    std::map<int32_t, ParticleResource> resources;
    for (const FieldEffects& field : fields)
        for (size_t i = 0; i < field.effect_rs_list.size(); i++) {
            const std::string& resource_name = field.effect_rs_list[i];
            if (resource_name.empty())
                continue;

            ParticleResource& resource = resources[static_cast<int32_t>(i)];
            resource.resource_index = static_cast<int32_t>(i);
            resource.resource_name = resource_name;
        }

    for (auto& i : resources) {
        ParticleResource& resource = i.second;
        resource.file_path = find_particle_file(data_roots, resource.resource_name);
        if (!resource.file_path.empty())
            resource.effect_names = load_particle_effect_names(resource.file_path);
    }

    std::vector<ParticleResource> result;
    result.reserve(resources.size());
    for (auto& i : resources)
        result.push_back(std::move(i.second));

    return result;
}

std::vector<ResolvedCommand> build_commands(
    const std::vector<Event>& events,
    const std::vector<ParticleResource>& particle_resources) {

    std::map<int32_t, const ParticleResource*> resources_by_index;
    for (const ParticleResource& resource : particle_resources)
        resources_by_index[resource.resource_index] = &resource;

    std::vector<ResolvedCommand> commands;
    commands.reserve(events.size());
    for (const Event& event : events) {
        ResolvedCommand command;
        command.event = event;
        if (event.action == EventAction::AutoDof
            || event.action == EventAction::Noise
            || event.action == EventAction::PjskDistortion
            || event.action == EventAction::PjskChromatic
            || event.action == EventAction::PjskOverlay) {
            command.effect_name = event.value ? "AUTO_DOF ON" : "AUTO_DOF OFF";
            if (event.action == EventAction::Noise)
                command.effect_name = "NOISE";
            else if (event.action == EventAction::PjskDistortion)
                command.effect_name = "PJSK_DISTORTION";
            else if (event.action == EventAction::PjskChromatic)
                command.effect_name = "PJSK_CHROMATIC";
            else if (event.action == EventAction::PjskOverlay)
                command.effect_name = "PJSK_OVERLAY";
            command.resolved = true;
            commands.push_back(std::move(command));
            continue;
        }

        auto resource_iter = resources_by_index.find(event.resource_index);
        if (resource_iter != resources_by_index.end()) {
            const ParticleResource* resource = resource_iter->second;
            command.resource_name = resource->resource_name;

            const int32_t effect_index = event.effect_index - 1;
            if (effect_index >= 0 && static_cast<size_t>(effect_index) < resource->effect_names.size()) {
                command.effect_name = resource->effect_names[static_cast<size_t>(effect_index)];
                command.resolved = true;
            }
        }

        commands.push_back(std::move(command));
    }

    return commands;
}

LoadResult load_for_pv(const std::filesystem::path& mod_directory, int32_t pv_id) {
    LoadResult result;
    const std::filesystem::path mod_root = find_mod_directory(mod_directory);
    const std::vector<std::filesystem::path> data_roots = find_data_roots_from_config(mod_root);

    result.script_path = find_script_file_from_pv_db(data_roots, pv_id);
    if (result.script_path.empty()) {
        result.error = "effect.script_file is not configured";
        return result;
    }

    return load_file(result.script_path);
}

LoadResult load_file(const std::filesystem::path& path) {
    LoadResult result;
    result.script_path = path;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "failed to open effect DSC";
        return result;
    }

    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    if (!parse_dsc(bytes, result.events, result.error))
        result.events.clear();

    return result;
}

PvFieldPatchResult apply_generated_pv_field_patch(
    const std::filesystem::path& mod_directory) {

    PvFieldPatchResult result;
    const std::filesystem::path mod_root = find_mod_directory(mod_directory);
    const std::vector<std::filesystem::path> data_roots = find_data_roots_from_config(mod_root);
    result.output_path = find_pv_field_output_file(data_roots);
    if (result.output_path.empty()) {
        result.error = "no data root for pv_field output";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(result.output_path.parent_path(), ec);
    if (!std::filesystem::exists(result.output_path)) {
        std::ofstream create(result.output_path);
        if (!create) {
            result.error = "failed to create pv_field output";
            return result;
        }
    }

    if (!rewrite_without_generated_block(result.output_path, result.error))
        return result;

    const std::vector<EffectScriptEntry> entries = find_effect_script_entries(data_roots);
    result.effect_entry_count = static_cast<int32_t>(entries.size());
    if (entries.empty()) {
        result.messages.push_back("no effect.script_file entries found");
        return result;
    }

    std::vector<std::string> generated_lines;
    generated_lines.push_back(kGeneratedBegin);

    for (const EffectScriptEntry& entry : entries) {
        LoadResult effect_script = load_file(entry.script_path);
        if (!effect_script.error.empty()) {
            result.messages.push_back("PV " + std::to_string(entry.pv_id)
                + ": " + effect_script.error);
            continue;
        }

        std::vector<FieldEffects> field_list = load_field_effects(data_roots, entry.pv_id);
        const std::vector<ParticleResource> resources = load_particle_resources(data_roots, field_list);

        std::map<int32_t, FieldEffects> fields;
        for (FieldEffects& field : field_list)
            fields[field.field_index] = std::move(field);

        const std::vector<ResolvedCommand> commands =
            build_commands(effect_script.events, resources);

        std::map<int32_t, int32_t> field_by_time;
        const std::vector<std::filesystem::path> chart_scripts =
            find_chart_script_files_from_pv_db(data_roots, entry.pv_id);
        for (const std::filesystem::path& chart_script : chart_scripts) {
            std::string error;
            const std::vector<FieldChange> changes =
                load_field_changes_from_script(chart_script, error);
            if (!error.empty()) {
                result.messages.push_back("PV " + std::to_string(entry.pv_id)
                    + ": " + error + " in " + chart_script.filename().string());
                continue;
            }

            for (const FieldChange& change : changes) {
                auto existing = field_by_time.find(change.time);
                if (existing == field_by_time.end())
                    field_by_time[change.time] = change.field_index;
                else if (existing->second != change.field_index)
                    result.messages.push_back("PV " + std::to_string(entry.pv_id)
                        + ": conflicting CHANGE_FIELD at TIME "
                        + std::to_string(change.time));
            }
        }

        if (field_by_time.empty())
            field_by_time[0] = 1;

        std::map<int32_t, std::vector<std::string>> generated_play;
        std::map<int32_t, std::vector<std::string>> generated_stop;

        for (const ResolvedCommand& command : commands) {
            if (command.event.action == EventAction::AutoDof
                || command.event.action == EventAction::Noise)
                continue;

            if (!command.resolved) {
                result.unresolved_command_count++;
                continue;
            }

            bool exact = false;
            const int32_t field_index =
                find_effect_field_for_time(field_by_time, command.event.time, exact);
            if (field_index <= 0) {
                result.unmatched_time_count++;
                result.messages.push_back("PV " + std::to_string(entry.pv_id)
                    + ": no CHANGE_FIELD for TIME " + std::to_string(command.event.time)
                    + " -> " + command.effect_name);
                continue;
            }

            if (!exact)
                result.messages.push_back("PV " + std::to_string(entry.pv_id)
                    + ": TIME " + std::to_string(command.event.time)
                    + " mapped to previous field " + std::to_string(field_index));

            FieldEffects* field = find_or_create_field(fields, field_index);
            std::vector<std::string>& list = command.event.action == EventAction::Play
                ? field->play_eff_list
                : field->stop_eff_list;
            std::map<int32_t, std::vector<std::string>>& generated =
                command.event.action == EventAction::Play ? generated_play : generated_stop;

            if (std::find(list.begin(), list.end(), command.effect_name) != list.end()) {
                result.applied_command_count++;
                result.existing_command_count++;
                continue;
            }

            append_unique(list, command.effect_name);
            generated[field_index].push_back(command.effect_name);
            result.applied_command_count++;
            result.generated_command_count++;
        }

        auto emit_generated_list =
            [&](const std::map<int32_t, std::vector<std::string>>& generated,
                const char* list_name) {

            for (const auto& item : generated) {
                const int32_t field_index = item.first;
                const FieldEffects& field = fields[field_index];
                const std::vector<std::string>& full_list =
                    std::string(list_name) == "play_eff_list"
                    ? field.play_eff_list
                    : field.stop_eff_list;
                const size_t generated_count = item.second.size();
                const size_t start_index = full_list.size() - generated_count;
                const std::string prefix = field_key_prefix(entry.pv_id, field_index, list_name);

                for (size_t i = 0; i < generated_count; i++) {
                    generated_lines.push_back(prefix + "." + std::to_string(start_index + i)
                        + "=" + item.second[i]);
                }
                generated_lines.push_back(prefix + ".length=" + std::to_string(full_list.size()));
            }
        };

        emit_generated_list(generated_play, "play_eff_list");
        emit_generated_list(generated_stop, "stop_eff_list");
    }

    generated_lines.push_back(kGeneratedEnd);

    if (generated_lines.size() > 2) {
        std::ofstream output(result.output_path, std::ios::app);
        if (!output) {
            result.error = "failed to append generated pv_field block";
            return result;
        }

        output << "\n";
        for (const std::string& line : generated_lines)
            output << line << "\n";
        result.generated_line_count = static_cast<int32_t>(generated_lines.size());
    }
    else {
        result.generated_line_count = 0;
    }

    return result;
}

} // namespace effect_script
