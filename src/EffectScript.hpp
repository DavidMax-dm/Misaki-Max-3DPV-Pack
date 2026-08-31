#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace effect_script {

enum class EventAction {
    Play,
    Stop,
    AutoDof,
    Noise,
    PjskDistortion,
    PjskChromatic,
    PjskOverlay,
};

struct Event {
    EventAction action = EventAction::Play;
    int32_t time = 0;
    int32_t resource_index = 0;
    int32_t effect_index = 0;
    int32_t value = 0;
    std::array<int32_t, 7> noise_values{};
    std::array<int32_t, 12> distortion_values{};
    std::array<int32_t, 20> chromatic_values{};
    std::array<int32_t, 9> overlay_values{};
};

struct LoadResult {
    std::filesystem::path script_path;
    std::vector<Event> events;
    std::string error;
};

struct FieldEffects {
    int32_t field_index = 0;
    std::vector<std::string> effect_rs_list;
    std::vector<std::string> play_eff_list;
    std::vector<std::string> stop_eff_list;
};

struct ParticleResource {
    int32_t resource_index = 0;
    std::string resource_name;
    std::filesystem::path file_path;
    std::vector<std::string> effect_names;
};

struct ResolvedCommand {
    Event event;
    std::string resource_name;
    std::string effect_name;
    bool resolved = false;
};

struct EffectScriptEntry {
    int32_t pv_id = 0;
    std::filesystem::path script_path;
};

struct FieldChange {
    int32_t time = 0;
    int32_t field_index = 0;
    std::filesystem::path script_path;
};

struct PvFieldPatchResult {
    std::filesystem::path output_path;
    int32_t effect_entry_count = 0;
    int32_t generated_line_count = 0;
    int32_t applied_command_count = 0;
    int32_t existing_command_count = 0;
    int32_t generated_command_count = 0;
    int32_t unresolved_command_count = 0;
    int32_t unmatched_time_count = 0;
    std::string error;
    std::vector<std::string> messages;
};

std::filesystem::path find_mod_directory(const std::filesystem::path& start);
std::vector<std::filesystem::path> find_data_roots_from_config(
    const std::filesystem::path& mod_directory);

std::vector<EffectScriptEntry> find_effect_script_entries(
    const std::vector<std::filesystem::path>& data_roots);

std::filesystem::path find_script_file_from_pv_db(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id);

std::vector<std::filesystem::path> find_effect_textures_from_pv_db(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id);

std::vector<std::filesystem::path> find_chart_script_files_from_pv_db(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id);

std::vector<FieldChange> load_field_changes_from_script(
    const std::filesystem::path& path,
    std::string& error);

std::vector<FieldEffects> load_field_effects(
    const std::vector<std::filesystem::path>& data_roots,
    int32_t pv_id);

std::vector<ParticleResource> load_particle_resources(
    const std::vector<std::filesystem::path>& data_roots,
    const std::vector<FieldEffects>& fields);

std::vector<ResolvedCommand> build_commands(
    const std::vector<Event>& events,
    const std::vector<ParticleResource>& particle_resources);

LoadResult load_for_pv(const std::filesystem::path& mod_directory, int32_t pv_id);
LoadResult load_file(const std::filesystem::path& path);

PvFieldPatchResult apply_generated_pv_field_patch(
    const std::filesystem::path& mod_directory);

} // namespace effect_script
