#include <recomp_mod/archive.hpp>
/* Adapted from snesrecomp runner/src/mod_runtime.cpp, Copyright 2026 Matthew Stan.
 * This file remains under PolyForm Noncommercial 1.0.0; see ../LICENSE.
 * VB adaptation: exact ROM hashing; no SNES APU/SRAM/MSU policy. */
#include "legacy_runtime.h"

#include "rmod_crc32.h"
#include "rmod_sha256.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <stdexcept>
#include "recomp_mod.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace RecompMod {
namespace {

constexpr uint64_t kMaxArchiveBytes = 256ull * 1024ull * 1024ull;
constexpr uint32_t kMaxArchiveFiles = 4096;

enum class OptionType {
    Boolean,
    Choice,
    Integer,
};

struct Choice {
    std::string value;
    std::string label;
};

struct Option {
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string group;
    OptionType type = OptionType::Boolean;
    std::string default_value = "false";
    int64_t min_value = 0;
    int64_t max_value = 1;
    int64_t step = 1;
    std::vector<Choice> choices;
};

struct Feature {
    std::string id;
    std::string name;
    std::string description;
    std::string group = "General";
    bool default_enabled = false;
    std::string channel = "stable";
    std::vector<std::string> plugins;
    std::vector<std::string> requirements;
};

struct Target {
    std::string game_id;
    std::string rom_sha256;
};

struct Resource {
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string format;
    std::string sha256;
    std::string file_patterns;
    std::string file_description;
    uint64_t size = 0;
    bool required = true;
};

struct Package {
    uint32_t format_version = 0;
    std::string id;
    std::string version;
    std::string name;
    std::string author;
    std::string description;
    std::string license;
    fs::path root;
    std::vector<Target> targets;
    std::vector<Feature> features;
    std::vector<Option> options;
    std::vector<Resource> resources;
};

struct FeatureSelection {
    bool enabled = false;
    bool has_enabled = false;
    std::map<std::string, std::string> values;
    std::map<std::string, std::string> resources;
};

struct PackageSelection {
    std::string version;
    std::map<std::string, FeatureSelection> features;
};

struct Diagnostic {
    std::string package_id;
    std::string feature_id;
    std::string related_package_id;
    std::string related_feature_id;
    std::string resource;
    std::string message;
};

struct ResolvedPlugin {
    std::string id;
    std::string package_id;
    std::string feature_id;
    InternalModActivationCallback callback = nullptr;
    std::map<std::string, std::string> options;
    fs::path root;
    std::map<std::string, std::string> resources;
};

struct Validation {
    bool ok = true;
    std::vector<Diagnostic> diagnostics;
    std::vector<ResolvedPlugin> plugins;
};

struct RegisteredPlugin {
    InternalModActivationCallback activation = nullptr;
    std::string resource;
};

struct Runtime {
    std::map<std::string, RegisteredPlugin> plugins;
    std::vector<InternalModActivationCallback> resets;
    std::vector<InternalModFrameCallback> frames;
    fs::path root;
    std::string game_id;
    std::string rom_sha256;
    std::map<std::string, std::map<std::string, Package>> packages;
    std::map<std::string, PackageSelection> selections;
    Validation validation;
    Validation committed;
    std::string error;
    bool initialized = false;
    bool committed_ready = false;
    std::array<uint8_t,32> committed_identity{};
    const ResolvedPlugin* activating = nullptr;
};

thread_local Runtime* active_runtime = nullptr;
Runtime& state() { if (!active_runtime) throw std::logic_error("no active mod context"); return *active_runtime; }
struct ContextScope { Runtime* previous; explicit ContextScope(Runtime& r):previous(active_runtime){active_runtime=&r;} ~ContextScope(){active_runtime=previous;} };
auto& registered_plugins(){return state().plugins;}
auto& reset_callbacks(){return state().resets;}
auto& frame_callbacks(){return state().frames;}

void set_error(std::string* out, const std::string& value) {
    if (out) *out = value;
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string strip_comment(const std::string& line) {
    bool quoted = false;
    bool escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (quoted && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (c == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

bool parse_string(const std::string& text, std::string& out) {
    const std::string value = trim(text);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;
    out.clear();
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i + 1 >= value.size()) return false;
        switch (value[i]) {
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return false;
        }
    }
    return true;
}

bool parse_bool(const std::string& text, bool& out) {
    const std::string value = trim(text);
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string& text, int64_t& out) {
    const std::string value = trim(text);
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno || !end || *end != '\0') return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 96 ||
        value.front() == '.' || value.back() == '.')
        return false;
    for (unsigned char c : value) {
        if (!(std::islower(c) || std::isdigit(c) ||
              c == '.' || c == '-' || c == '_'))
            return false;
    }
    const auto stem = value.substr(0, value.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0)
        && stem[3] >= '1' && stem[3] <= '9') return false;
    return true;
}

bool valid_sha256(const std::string& value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) || (c >= 'a' && c <= 'f');
        });
}


const Feature* find_feature(const Package& package,
                            const std::string& feature_id) {
    const auto it = std::find_if(
        package.features.begin(), package.features.end(),
        [&](const Feature& feature) { return feature.id == feature_id; });
    return it == package.features.end() ? nullptr : &*it;
}

const Option* find_option(const Package& package,
                          const std::string& feature_id,
                          const std::string& option_id) {
    const auto it = std::find_if(
        package.options.begin(), package.options.end(),
        [&](const Option& option) {
            return option.feature_id == feature_id && option.id == option_id;
        });
    return it == package.options.end() ? nullptr : &*it;
}

const Resource* find_resource(const Package& package,
                              const std::string& feature_id,
                              const std::string& resource_id) {
    const auto it = std::find_if(
        package.resources.begin(), package.resources.end(),
        [&](const Resource& resource) {
            return resource.feature_id == feature_id &&
                   resource.id == resource_id;
        });
    return it == package.resources.end() ? nullptr : &*it;
}

bool validate_option_value(const Option& option, const std::string& value,
                           std::string* error) {
    if (option.type == OptionType::Boolean) {
        if (value == "true" || value == "false") return true;
        set_error(error, option.label + " must be true or false");
        return false;
    }
    if (option.type == OptionType::Choice) {
        const bool found = std::any_of(
            option.choices.begin(), option.choices.end(),
            [&](const Choice& choice) { return choice.value == value; });
        if (found) return true;
        set_error(error, option.label + " has an unknown choice");
        return false;
    }
    int64_t parsed = 0;
    if (!parse_int(value, parsed) ||
        parsed < option.min_value || parsed > option.max_value ||
        option.step <= 0 ||
        (uint64_t(parsed) - uint64_t(option.min_value)) % uint64_t(option.step) != 0) {
        set_error(error, option.label + " is outside its allowed range");
        return false;
    }
    return true;
}

bool resource_is_directory(const Resource& resource) {
    return resource.format == "directory" || resource.format == "folder";
}

bool parse_requirements(const std::string& text, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(start, end - start);
        const size_t first = item.find_first_not_of(" \t");
        const size_t last = item.find_last_not_of(" \t");
        item = first == std::string::npos ? std::string() : item.substr(first, last - first + 1);
        const size_t slash = item.find('/');
        if (slash == std::string::npos || !slash || slash + 1 >= item.size() ||
            item.find('/', slash + 1) != std::string::npos)
            return false;
        out.push_back(item);
        start = end + 1;
    }
    return !out.empty();
}

bool read_manifest(const fs::path& path, Package& out, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        set_error(error, "cannot open manifest: " + path.string());
        return false;
    }

    enum class Section {
        Package,
        Target,
        Feature,
        Option,
        Choice,
        Plugin,
        Resource,
    };
    Section section = Section::Package;
    Target* target = nullptr;
    Feature* feature = nullptr;
    Option* option = nullptr;
    Choice* choice = nullptr;
    Resource* resource = nullptr;
    std::string plugin_feature;
    std::string plugin_id;

    auto finish_plugin = [&]() -> bool {
        if (plugin_feature.empty() && plugin_id.empty()) return true;
        Feature* owner = nullptr;
        for (Feature& candidate : out.features) {
            if (candidate.id == plugin_feature) {
                owner = &candidate;
                break;
            }
        }
        if (!owner || !valid_id(plugin_id)) {
            set_error(error, "plugin has an invalid feature or id");
            return false;
        }
        owner->plugins.push_back(plugin_id);
        plugin_feature.clear();
        plugin_id.clear();
        return true;
    };

    std::string raw;
    size_t line_number = 0;
    while (std::getline(file, raw)) {
        ++line_number;
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.rfind("[[", 0) == 0 &&
            line.size() >= 4 &&
            line.substr(line.size() - 2) == "]]") {
            if (section == Section::Plugin && !finish_plugin()) return false;
            const std::string name = trim(line.substr(2, line.size() - 4));
            target = nullptr;
            feature = nullptr;
            option = nullptr;
            choice = nullptr;
            resource = nullptr;
            if (name == "target") {
                section = Section::Target;
                out.targets.emplace_back();
                target = &out.targets.back();
            } else if (name == "feature") {
                section = Section::Feature;
                out.features.emplace_back();
                feature = &out.features.back();
            } else if (name == "option") {
                section = Section::Option;
                out.options.emplace_back();
                option = &out.options.back();
            } else if (name == "option.choice") {
                if (out.options.empty()) {
                    set_error(error, "option.choice appears before option");
                    return false;
                }
                section = Section::Choice;
                out.options.back().choices.emplace_back();
                option = &out.options.back();
                choice = &option->choices.back();
            } else if (name == "plugin") {
                section = Section::Plugin;
            } else if (name == "resource" || name == "external_rom") {
                section = Section::Resource;
                out.resources.emplace_back();
                resource = &out.resources.back();
            } else {
                set_error(error, "unsupported manifest section [[" + name + "]]");
                return false;
            }
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            set_error(error, "invalid manifest line " +
                             std::to_string(line_number));
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string string_value;
        int64_t int_value = 0;
        bool bool_value = false;

        auto string_field = [&](std::string& field) -> bool {
            if (!parse_string(value, string_value)) return false;
            field = string_value;
            return true;
        };

        bool known = true;
        bool parsed = true;
        switch (section) {
            case Section::Package:
                if (key == "format_version") {
                    parsed = parse_int(value, int_value) &&
                             int_value >= 1 && int_value <= 1;
                    if (parsed) out.format_version = (uint32_t)int_value;
                } else if (key == "id") parsed = string_field(out.id);
                else if (key == "version") parsed = string_field(out.version);
                else if (key == "name") parsed = string_field(out.name);
                else if (key == "author") parsed = string_field(out.author);
                else if (key == "description") parsed = string_field(out.description);
                else if (key == "license") parsed = string_field(out.license);
                else if (key == "resolver") {
                    parsed = parse_string(value, string_value) &&
                             string_value == "declarative";
                } else if (key == "save_compatibility") {
                    parsed = parse_string(value, string_value);
                } else known = false;
                break;
            case Section::Target:
                if (!target) parsed = false;
                else if (key == "game_id") parsed = string_field(target->game_id);
                else if (key == "rom_sha256") parsed = string_field(target->rom_sha256);
                else known = false;
                break;
            case Section::Feature:
                feature = out.features.empty() ? nullptr : &out.features.back();
                if (!feature) parsed = false;
                else if (key == "id") parsed = string_field(feature->id);
                else if (key == "name") parsed = string_field(feature->name);
                else if (key == "description") parsed = string_field(feature->description);
                else if (key == "group") parsed = string_field(feature->group);
                else if (key == "requires") parsed = parse_string(value, string_value) && parse_requirements(string_value, feature->requirements);
                else if (key == "channel") {
                    parsed = string_field(feature->channel) &&
                        (feature->channel == "stable" || feature->channel == "experimental" || feature->channel == "developer");
                }
                else if (key == "default_enabled") {
                    parsed = parse_bool(value, bool_value);
                    if (parsed) feature->default_enabled = bool_value;
                } else known = false;
                break;
            case Section::Option:
                option = out.options.empty() ? nullptr : &out.options.back();
                if (!option) parsed = false;
                else if (key == "feature") parsed = string_field(option->feature_id);
                else if (key == "id") parsed = string_field(option->id);
                else if (key == "label") parsed = string_field(option->label);
                else if (key == "description") parsed = string_field(option->description);
                else if (key == "group") parsed = string_field(option->group);
                else if (key == "type") {
                    parsed = parse_string(value, string_value);
                    if (parsed && string_value == "boolean")
                        option->type = OptionType::Boolean;
                    else if (parsed && string_value == "choice")
                        option->type = OptionType::Choice;
                    else if (parsed && string_value == "integer")
                        option->type = OptionType::Integer;
                    else parsed = false;
                } else if (key == "default") {
                    if (parse_string(value, string_value))
                        option->default_value = string_value;
                    else if (parse_bool(value, bool_value))
                        option->default_value = bool_value ? "true" : "false";
                    else if (parse_int(value, int_value))
                        option->default_value = std::to_string(int_value);
                    else parsed = false;
                } else if (key == "min") {
                    parsed = parse_int(value, option->min_value);
                } else if (key == "max") {
                    parsed = parse_int(value, option->max_value);
                } else if (key == "step") {
                    parsed = parse_int(value, option->step);
                } else known = false;
                break;
            case Section::Choice:
                option = out.options.empty() ? nullptr : &out.options.back();
                choice = (!option || option->choices.empty())
                    ? nullptr : &option->choices.back();
                if (!choice) parsed = false;
                else if (key == "value") parsed = string_field(choice->value);
                else if (key == "label") parsed = string_field(choice->label);
                else known = false;
                break;
            case Section::Plugin:
                if (key == "feature") parsed = string_field(plugin_feature);
                else if (key == "id") parsed = string_field(plugin_id);
                else known = false;
                break;
            case Section::Resource:
                resource = out.resources.empty() ? nullptr : &out.resources.back();
                if (!resource) parsed = false;
                else if (key == "feature") parsed = string_field(resource->feature_id);
                else if (key == "id") parsed = string_field(resource->id);
                else if (key == "label") parsed = string_field(resource->label);
                else if (key == "description") parsed = string_field(resource->description);
                else if (key == "format") parsed = string_field(resource->format);
                else if (key == "sha256") parsed = string_field(resource->sha256);
                else if (key == "file_patterns") parsed = string_field(resource->file_patterns);
                else if (key == "file_description") parsed = string_field(resource->file_description);
                else if (key == "size") {
                    parsed = parse_int(value, int_value) && int_value > 0;
                    if (parsed) resource->size = (uint64_t)int_value;
                } else if (key == "required") {
                    parsed = parse_bool(value, bool_value);
                    if (parsed) resource->required = bool_value;
                } else known = false;
                break;
        }
        if (!known || !parsed) {
            set_error(error, "invalid or unsupported manifest field '" + key +
                             "' on line " + std::to_string(line_number));
            return false;
        }
    }
    if (section == Section::Plugin && !finish_plugin()) return false;

    if (out.format_version != 1 || !valid_id(out.id) ||
        !valid_id(out.version) || out.name.empty() || out.targets.empty() ||
        out.features.empty()) {
        set_error(error, "manifest is missing required package fields");
        return false;
    }
    std::set<std::string> feature_ids;
    for (const Target& item : out.targets) {
        if (item.game_id.empty() || !valid_sha256(item.rom_sha256)) {
            set_error(error, "manifest has an invalid target");
            return false;
        }
    }
    for (const Feature& item : out.features) {
        if (!valid_id(item.id) || item.name.empty() ||
            !feature_ids.insert(item.id).second) {
            set_error(error, "manifest has an invalid or duplicate feature");
            return false;
        }
    }
    std::set<std::pair<std::string, std::string>> option_ids;
    for (const Option& item : out.options) {
        if (!find_feature(out, item.feature_id) || !valid_id(item.id) ||
            item.label.empty() ||
            !option_ids.insert({item.feature_id, item.id}).second ||
            !validate_option_value(item, item.default_value, error)) {
            if (error && error->empty())
                *error = "manifest has an invalid option";
            return false;
        }
        if (item.type == OptionType::Choice &&
            (item.choices.empty() ||
             std::any_of(item.choices.begin(), item.choices.end(),
                         [](const Choice& c) {
                             return c.value.empty() || c.label.empty();
                         }))) {
            set_error(error, "choice option has no valid choices");
            return false;
        }
    }
    std::set<std::pair<std::string, std::string>> resource_ids;
    for (Resource& item : out.resources) {
        if (!find_feature(out, item.feature_id) || !valid_id(item.id) ||
            item.label.empty() ||
            !resource_ids.insert({item.feature_id, item.id}).second) {
            set_error(error, "manifest has an invalid resource");
            return false;
        }
        if (!item.sha256.empty() &&
            !valid_sha256(item.sha256)) {
            set_error(error, "manifest has an invalid resource sha256");
            return false;
        }
        if (!item.format.empty() && item.format != "file" && !resource_is_directory(item)) {
            set_error(error, "resource format must be file or directory"); return false;
        }
        if (resource_is_directory(item) && !item.sha256.empty()) {
            set_error(error, "directory resources cannot declare a file hash"); return false;
        }
        if (item.file_patterns.empty() && !resource_is_directory(item))
            item.file_patterns = "*";
        if (item.file_description.empty() && !resource_is_directory(item))
            item.file_description = "Owner resource";
    }
    return true;
}

const Package* selected_package(const Runtime& runtime,
                                const std::string& id) {
    const auto packages = runtime.packages.find(id);
    if (packages == runtime.packages.end() || packages->second.empty())
        return nullptr;
    const auto selection = runtime.selections.find(id);
    if (selection != runtime.selections.end() &&
        !selection->second.version.empty()) {
        const auto version =
            packages->second.find(selection->second.version);
        if (version != packages->second.end()) return &version->second;
    }
    return &packages->second.rbegin()->second;
}

PackageSelection& package_selection(Runtime& runtime,
                                    const Package& package) {
    PackageSelection& selection = runtime.selections[package.id];
    if (selection.version.empty()) selection.version = package.version;
    return selection;
}

bool feature_enabled(Runtime& runtime, const Package& package,
                     const Feature& feature) {
    PackageSelection& package_state = package_selection(runtime, package);
    FeatureSelection& selection = package_state.features[feature.id];
    if (!selection.has_enabled)
        selection.enabled = feature.default_enabled;
    return selection.enabled;
}

std::string option_value(Runtime& runtime, const Package& package,
                         const Feature& feature, const Option& option) {
    PackageSelection& package_state = package_selection(runtime, package);
    FeatureSelection& selection = package_state.features[feature.id];
    const auto value = selection.values.find(option.id);
    return value == selection.values.end() ? option.default_value :
                                             value->second;
}

std::string resource_path(Runtime& runtime, const Package& package,
                          const Feature& feature,
                          const Resource& resource) {
    PackageSelection& package_state = package_selection(runtime, package);
    FeatureSelection& selection = package_state.features[feature.id];
    const auto value = selection.resources.find(resource.id);
    return value == selection.resources.end() ? std::string() :
                                                value->second;
}

bool sha256_file(const fs::path& path, std::string& out, std::string* error);

bool validate_resource_file(const Resource& resource,
                            const std::string& path_text,
                            std::string* status) {
    if (path_text.empty()) {
        if (status) {
            *status = resource.required
                ? (resource_is_directory(resource) ? "Required: select a folder"
                                                   : "Required: select a file")
                : "Not selected";
        }
        return !resource.required;
    }
    const fs::path path=fs::u8path(path_text);
    std::error_code ec;
    if (resource_is_directory(resource)) {
        if (!fs::is_directory(path, ec)) {
            if (status) *status = "Folder not found";
            return false;
        }
    } else if (!fs::is_regular_file(path, ec)) {
        if (status) *status = "File not found";
        return false;
    }
    if (resource.size != 0 && !resource_is_directory(resource)) {
        const uint64_t actual = (uint64_t)fs::file_size(path, ec);
        if (ec || actual != resource.size) {
            if (status) *status = "File size does not match";
            return false;
        }
    }
    if (!resource.sha256.empty()) {
        std::string digest;
        if (!sha256_file(path, digest, status) || digest != resource.sha256) {
            if (status) *status = "Resource SHA-256 does not match";
            return false;
        }
    }
    if (status) *status = resource.sha256.empty() ? "Selected" : "SHA-256 verified";
    return true;
}

bool target_matches(const Package& package, const Runtime& runtime) {
    return std::any_of(
        package.targets.begin(), package.targets.end(),
        [&](const Target& target) {
            return target.game_id == runtime.game_id &&
                   target.rom_sha256 == runtime.rom_sha256;
        });
}

Validation validate(Runtime& runtime) {
    Validation result;
    std::map<std::string, ResolvedPlugin> claimed;
    for (const auto& [id, versions] : runtime.packages) {
        (void)versions;
        const Package* package = selected_package(runtime, id);
        if (!package) continue;
        for (const Feature& feature : package->features) {
            if (!feature_enabled(runtime, *package, feature)) continue;
            if (!target_matches(*package, runtime)) {
                result.ok = false;
                result.diagnostics.push_back({
                    package->id, feature.id, {}, {},
                    "target:" + runtime.game_id,
                    "This feature does not support the selected stock ROM."
                });
                continue;
            }
            for (const auto& requirement : feature.requirements) {
                const auto slash=requirement.find('/');
                const auto* dependency=selected_package(runtime,requirement.substr(0,slash));
                const auto* required=dependency?find_feature(*dependency,requirement.substr(slash+1)):nullptr;
                if(!required || !feature_enabled(runtime,*dependency,*required)) {
                    result.ok=false; result.diagnostics.push_back({package->id,feature.id,{},{},"requires:"+requirement,"Required feature is not enabled."});
                }
            }
            for (const Resource& resource : package->resources) {
                if (resource.feature_id != feature.id) continue;
                std::string status;
                const std::string path =
                    resource_path(runtime, *package, feature, resource);
                if (!validate_resource_file(resource, path, &status)) {
                    result.ok = false;
                    result.diagnostics.push_back({
                        package->id, feature.id, {}, {},
                        "resource:" + resource.id,
                        status
                    });
                }
            }
            for (const std::string& plugin_id : feature.plugins) {
                const auto registered = registered_plugins().find(plugin_id);
                if (registered == registered_plugins().end() ||
                    !registered->second.activation) {
                    result.ok = false;
                    result.diagnostics.push_back({
                        package->id, feature.id, {}, {},
                        "plugin:" + plugin_id,
                        "The executable does not provide this trusted plugin."
                    });
                    continue;
                }
                ResolvedPlugin candidate{
                    plugin_id, package->id, feature.id,
                    registered->second.activation, {}, {}, {}
                };
                candidate.root = package->root;
                for (const Resource& resource : package->resources)
                    if (resource.feature_id == feature.id)
                        candidate.resources[resource.id] = resource_path(runtime, *package, feature, resource);
                for (const Option& option : package->options)
                    if (option.feature_id == feature.id)
                        candidate.options[option.id] = option_value(runtime, *package, feature, option);
                const std::string claim = registered->second.resource.empty()
                    ? "plugin:" + plugin_id : "exclusive:" + registered->second.resource;
                const auto prior = claimed.find(claim);
                if (prior != claimed.end()) {
                    result.ok = false;
                    result.diagnostics.push_back({
                        package->id, feature.id,
                        prior->second.package_id, prior->second.feature_id,
                        claim,
                        "Two enabled features claim the same plugin or exclusive resource."
                    });
                    result.diagnostics.push_back({
                        prior->second.package_id, prior->second.feature_id,
                        package->id, feature.id,
                        claim,
                        "Two enabled features claim the same plugin or exclusive resource."
                    });
                    continue;
                }
                claimed.emplace(claim, candidate);
            }
        }
    }
    for (const auto& [id, plugin] : claimed) {
        (void)id;
        result.plugins.push_back(plugin);
    }
    return result;
}

bool is_link(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(path, ec));
#endif
}

bool inside(const fs::path& parent, const fs::path& child) {
    std::error_code ec;
    const auto root = fs::canonical(parent, ec);
    if (ec) return false;
    const auto target = fs::weakly_canonical(child, ec);
    if (ec) return false;
    const auto relative = target.lexically_relative(root);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

bool scan(Runtime& runtime, std::string* error) {
    runtime.packages.clear();
    const fs::path packages_root = runtime.root / "packages";
    std::error_code ec;
    fs::create_directories(runtime.root, ec);
    if (ec) { set_error(error, "cannot create mods root: " + ec.message()); return false; }
    if (is_link(packages_root) || !inside(runtime.root, packages_root)) {
        set_error(error, "package catalog must be inside the mods directory"); return false;
    }
    fs::create_directories(packages_root, ec);
    if (ec) {
        set_error(error, "cannot create mods directory: " + ec.message());
        return false;
    }
    for (const fs::directory_entry& package_dir :
         fs::directory_iterator(packages_root, ec)) {
        if (ec) break;
        if (is_link(package_dir.path())) {
            set_error(error, "package directories cannot be links"); return false;
        }
        if (!package_dir.is_directory()) continue;
        for (const fs::directory_entry& version_dir :
             fs::directory_iterator(package_dir.path(), ec)) {
            if (ec) break;
            if (is_link(version_dir.path())) {
                set_error(error, "package versions cannot be links"); return false;
            }
            if (!version_dir.is_directory()) continue;
            const fs::path manifest = version_dir.path() / "manifest.toml";
            if (is_link(manifest)) { set_error(error, "manifest cannot be a link"); return false; }
            if (!fs::is_regular_file(manifest)) continue;
            Package package;
            if (!read_manifest(manifest, package, error)) return false;
            if (package.id != package_dir.path().filename().string() ||
                package.version != version_dir.path().filename().string()) {
                set_error(error, "package directory does not match manifest: " +
                                 manifest.string());
                return false;
            }
            package.root = version_dir.path();
            runtime.packages[package.id][package.version] = std::move(package);
        }
        if (ec) break;
    }
    if (ec) {
        set_error(error, "cannot scan mods directory: " + ec.message());
        return false;
    }
    return true;
}

bool load_state(Runtime& runtime, std::string* error) {
    runtime.selections.clear();
    const fs::path path = runtime.root / "state.toml";
    if (!fs::exists(path)) return true;
    std::ifstream file(path);
    if (!file) {
        set_error(error, "cannot read mod state");
        return false;
    }
    enum class Section { Root, Package, Feature, Values, Resource };
    Section section = Section::Root;
    std::string current_package;
    std::string current_feature;
    std::string current_resource;
    std::string raw;
    while (std::getline(file, raw)) {
        const std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line == "[[package]]") {
            section = Section::Package;
            current_package.clear();
            current_feature.clear();
            current_resource.clear();
            continue;
        }
        if (line == "[[feature]]") {
            section = Section::Feature;
            current_package.clear();
            current_feature.clear();
            current_resource.clear();
            continue;
        }
        if (line == "[[resource]]") {
            section = Section::Resource;
            current_package.clear();
            current_feature.clear();
            current_resource.clear();
            continue;
        }
        if (line == "[feature.values]") {
            section = Section::Values;
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string parsed;
        bool enabled = false;
        if (section == Section::Root) continue;
        if (section == Section::Package) {
            if (key == "id" && parse_string(value, current_package)) {
                runtime.selections[current_package];
            } else if (key == "version" && !current_package.empty() &&
                       parse_string(value, parsed)) {
                runtime.selections[current_package].version = parsed;
            }
            continue;
        }
        if (section == Section::Feature) {
            if (key == "package_id" && parse_string(value, current_package)) {
                runtime.selections[current_package];
            } else if (key == "id" && parse_string(value, current_feature)) {
                if (!current_package.empty())
                    runtime.selections[current_package].features[current_feature];
            } else if (key == "enabled" && !current_package.empty() &&
                       !current_feature.empty() &&
                       parse_bool(value, enabled)) {
                FeatureSelection& selection =
                    runtime.selections[current_package].features[current_feature];
                selection.enabled = enabled;
                selection.has_enabled = true;
            }
            continue;
        }
        if (section == Section::Values && !current_package.empty() &&
            !current_feature.empty() && parse_string(value, parsed)) {
            runtime.selections[current_package]
                .features[current_feature].values[key] = parsed;
            continue;
        }
        if (section == Section::Resource) {
            if (key == "package_id" && parse_string(value, current_package)) {
                runtime.selections[current_package];
            } else if (key == "feature_id" &&
                       parse_string(value, current_feature)) {
                if (!current_package.empty())
                    runtime.selections[current_package].features[current_feature];
            } else if (key == "id" && parse_string(value, parsed)) {
                current_resource = parsed;
                if (!current_package.empty() && current_feature.empty()) {
                    const Package* package =
                        selected_package(runtime, current_package);
                    if (package) {
                        for (const Feature& feature : package->features) {
                            if (find_resource(*package, feature.id, parsed)) {
                                current_feature = feature.id;
                                runtime.selections[current_package]
                                    .features[current_feature];
                                break;
                            }
                        }
                    }
                }
                if (!current_package.empty() && !current_feature.empty())
                    runtime.selections[current_package]
                        .features[current_feature].resources[current_resource];
            } else if (key == "path" && parse_string(value, parsed) &&
                       !current_package.empty() && !current_feature.empty() &&
                       !current_resource.empty()) {
                    runtime.selections[current_package]
                        .features[current_feature]
                        .resources[current_resource] = parsed;
            }
        }
    }
    return true;
}

std::string quote_toml(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool save_state(Runtime& runtime, std::string* error) {
    std::error_code ec;
    fs::create_directories(runtime.root, ec);
    const fs::path temp = runtime.root / "state.toml.tmp";
    const fs::path output = runtime.root / "state.toml";
    if(is_link(temp)||is_link(output)){set_error(error,"Mod state paths cannot be links");return false;}
    std::ofstream file(temp, std::ios::trunc);
    if (!file) {
        set_error(error, "cannot write mod state");
        return false;
    }
    file << "format_version = 1\n";
    for (const auto& [package_id, selection] : runtime.selections) {
        file << "\n[[package]]\nid = " << quote_toml(package_id)
             << "\nversion = " << quote_toml(selection.version) << "\n";
    }
    for (const auto& [package_id, selection] : runtime.selections) {
        for (const auto& [feature_id, feature] : selection.features) {
            file << "\n[[feature]]\npackage_id = " << quote_toml(package_id)
                 << "\nid = " << quote_toml(feature_id)
                 << "\nenabled = " << (feature.enabled ? "true" : "false")
                 << "\n";
            if (!feature.values.empty()) {
                file << "\n[feature.values]\n";
                for (const auto& [key, value] : feature.values)
                    file << key << " = " << quote_toml(value) << "\n";
            }
            for (const auto& [resource_id, path] : feature.resources) {
                if (path.empty()) continue;
                file << "\n[[resource]]\npackage_id = "
                     << quote_toml(package_id)
                     << "\nfeature_id = " << quote_toml(feature_id)
                     << "\nid = " << quote_toml(resource_id)
                     << "\npath = " << quote_toml(path) << "\n";
            }
        }
    }
    file.close();
    if (!file) {
        set_error(error, "cannot finish writing mod state");
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code((int)GetLastError(), std::system_category());
#else
    fs::rename(temp, output, ec);
#endif
    if (ec) {
        set_error(error, "cannot publish mod state: " + ec.message());
        return false;
    }
    return true;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out,
               std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open " + path.string());
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || (uint64_t)size > kMaxArchiveBytes) {
        set_error(error, "archive is too large");
        return false;
    }
    in.seekg(0);
    out.resize((size_t)size);
    if (!out.empty() && !in.read((char*)out.data(), size)) {
        set_error(error, "cannot read " + path.string());
        return false;
    }
    return true;
}

uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

using ZipEntry = recomp::mods::ZipEntry;

bool safe_archive_name(const std::string& name) {
    if (name.empty() || name.size() > 512 ||
        name[0] == '/' || name[0] == '\\')
        return false;
    if (name.size() >= 2 &&
        std::isalpha((unsigned char)name[0]) && name[1] == ':')
        return false;
    for (unsigned char c : name)
        if (c < 32 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    const std::string trimmed = name.back() == '/' ? name.substr(0, name.size() - 1) : name;
    if (trimmed.empty()) return false;
    const fs::path path(trimmed);
    for (const auto& part : path) {
        const std::string text = part.string();
        if (text == ".." || text == "." || text.empty()) return false;
        if (text.back() == '.' || text.back() == ' ') return false;
        auto stem = text.substr(0, text.find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
        if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0)
            && stem[3] >= '1' && stem[3] <= '9') return false;
    }
    return true;
}

bool parse_zip_impl(const std::vector<uint8_t>& bytes,
               std::vector<ZipEntry>& entries, std::string* error) {
    if (bytes.size() < 22) {
        set_error(error, "not a ZIP archive");
        return false;
    }
    size_t eocd = std::string::npos;
    const size_t floor = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (le32(bytes.data() + pos) == 0x06054b50u) {
            eocd = pos;
            break;
        }
        if (pos == floor) break;
    }
    if (eocd == std::string::npos) {
        set_error(error, "ZIP end record is missing");
        return false;
    }
    const uint16_t count = le16(bytes.data() + eocd + 10);
    const uint32_t central_size = le32(bytes.data() + eocd + 12);
    const uint32_t central_offset = le32(bytes.data() + eocd + 16);
    if (count > kMaxArchiveFiles ||
        (uint64_t)central_offset + central_size > bytes.size()) {
        set_error(error, "ZIP central directory is invalid");
        return false;
    }
    size_t at = central_offset;
    uint64_t expanded = 0;
    std::set<std::string> entry_names;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 46 > bytes.size() ||
            le32(bytes.data() + at) != 0x02014b50u) {
            set_error(error, "ZIP entry record is invalid");
            return false;
        }
        const uint16_t flags = le16(bytes.data() + at + 8);
        ZipEntry entry;
        entry.method = le16(bytes.data() + at + 10);
        entry.crc = le32(bytes.data() + at + 16);
        entry.compressed_size = le32(bytes.data() + at + 20);
        entry.size = le32(bytes.data() + at + 24);
        const uint16_t name_len = le16(bytes.data() + at + 28);
        const uint16_t extra_len = le16(bytes.data() + at + 30);
        const uint16_t comment_len = le16(bytes.data() + at + 32);
        entry.local_offset = le32(bytes.data() + at + 42);
        if ((flags & 1u) || (entry.method != 0 && entry.method != 8)) {
            set_error(error, (flags & 1u)
                ? "encrypted ZIP entries are not supported"
                : "ZIP compression method is not supported");
            return false;
        }
        if (at + 46ull + name_len + extra_len + comment_len > bytes.size()) {
            set_error(error, "ZIP entry name is truncated");
            return false;
        }
        entry.name.assign((const char*)bytes.data() + at + 46, name_len);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        entry.directory = !entry.name.empty() && entry.name.back() == '/';
        const std::string checked = entry.directory
            ? entry.name.substr(0, entry.name.size() - 1) : entry.name;
        if (!safe_archive_name(checked)) {
            set_error(error, "unsafe ZIP path: " + entry.name);
            return false;
        }
        std::string folded = checked;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (!entry_names.insert(folded).second) {
            set_error(error, "duplicate ZIP path: " + checked);
            return false;
        }
        expanded += entry.size;
        if (expanded > kMaxArchiveBytes) {
            set_error(error, "expanded archive exceeds the size limit");
            return false;
        }
        entries.push_back(std::move(entry));
        at += 46ull + name_len + extra_len + comment_len;
    }
    return true;
}

struct DeflateBits {
    const uint8_t* at = nullptr;
    const uint8_t* end = nullptr;
    uint64_t hold = 0;
    unsigned bits = 0;

    bool read(unsigned count, uint32_t& out) {
        while (bits < count) {
            if (at == end) return false;
            hold |= (uint64_t)*at++ << bits;
            bits += 8;
        }
        out = count == 32 ? (uint32_t)hold :
              (uint32_t)(hold & ((1ull << count) - 1));
        hold >>= count;
        bits -= count;
        return true;
    }
    void align_byte() {
        const unsigned drop = bits & 7u;
        hold >>= drop;
        bits -= drop;
    }
};

struct DeflateHuffman {
    std::array<uint16_t, 16> count{};
    std::vector<uint16_t> symbols;
};

bool build_huffman(const std::vector<uint8_t>& lengths,
                   DeflateHuffman& out) {
    out = {};
    for (uint8_t length : lengths) {
        if (length > 15) return false;
        out.count[length]++;
    }
    if (out.count[0] == lengths.size()) return false;
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= out.count[(size_t)length];
        if (left < 0) return false;
    }
    std::array<uint16_t, 16> offsets{};
    for (size_t length = 1; length < 15; ++length)
        offsets[length + 1] = offsets[length] + out.count[length];
    out.symbols.resize(lengths.size() - out.count[0]);
    for (uint16_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol])
            out.symbols[offsets[lengths[symbol]]++] = symbol;
    return true;
}

bool decode_symbol(DeflateBits& bits, const DeflateHuffman& table,
                   uint16_t& symbol) {
    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;
    for (uint32_t length = 1; length <= 15; ++length) {
        uint32_t bit = 0;
        if (!bits.read(1, bit)) return false;
        code |= bit;
        const uint32_t count = table.count[length];
        if (code < first + count) {
            const uint32_t slot = index + code - first;
            if (slot >= table.symbols.size()) return false;
            symbol = table.symbols[slot];
            return true;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return false;
}

bool inflate_deflate(const uint8_t* data, size_t size, size_t expected,
                     std::vector<uint8_t>& out) {
    static const uint16_t length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
        115,131,163,195,227,258};
    static const uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,
        12,12,13,13};
    DeflateBits input{data, data + size};
    out.clear();
    out.reserve(expected);
    bool final = false;
    while (!final) {
        uint32_t final_bit = 0;
        uint32_t type = 0;
        if (!input.read(1, final_bit) || !input.read(2, type)) return false;
        final = final_bit != 0;
        if (type == 0) {
            input.align_byte();
            uint32_t length = 0;
            uint32_t complement = 0;
            if (!input.read(16, length) || !input.read(16, complement) ||
                (length ^ 0xffffu) != complement ||
                out.size() + length > expected)
                return false;
            for (uint32_t i = 0; i < length; ++i) {
                uint32_t byte = 0;
                if (!input.read(8, byte)) return false;
                out.push_back((uint8_t)byte);
            }
            continue;
        }
        if (type == 3) return false;
        std::vector<uint8_t> literal_lengths;
        std::vector<uint8_t> distance_lengths;
        if (type == 1) {
            literal_lengths.resize(288);
            for (size_t i = 0; i <= 143; ++i) literal_lengths[i] = 8;
            for (size_t i = 144; i <= 255; ++i) literal_lengths[i] = 9;
            for (size_t i = 256; i <= 279; ++i) literal_lengths[i] = 7;
            for (size_t i = 280; i <= 287; ++i) literal_lengths[i] = 8;
            distance_lengths.assign(32, 5);
        } else {
            uint32_t hlit = 0;
            uint32_t hdist = 0;
            uint32_t hclen = 0;
            if (!input.read(5, hlit) || !input.read(5, hdist) ||
                !input.read(4, hclen))
                return false;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            static const uint8_t order[19] =
                {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<uint8_t> code_lengths(19, 0);
            for (uint32_t i = 0; i < hclen; ++i) {
                uint32_t value = 0;
                if (!input.read(3, value)) return false;
                code_lengths[order[i]] = (uint8_t)value;
            }
            DeflateHuffman code_table;
            if (!build_huffman(code_lengths, code_table)) return false;
            std::vector<uint8_t> lengths;
            lengths.reserve(hlit + hdist);
            while (lengths.size() < hlit + hdist) {
                uint16_t symbol = 0;
                if (!decode_symbol(input, code_table, symbol)) return false;
                if (symbol <= 15) {
                    lengths.push_back((uint8_t)symbol);
                    continue;
                }
                uint32_t repeat = 0;
                uint32_t extra = 0;
                uint8_t value = 0;
                if (symbol == 16) {
                    if (lengths.empty() || !input.read(2, extra)) return false;
                    repeat = extra + 3;
                    value = lengths.back();
                } else if (symbol == 17) {
                    if (!input.read(3, extra)) return false;
                    repeat = extra + 3;
                } else if (symbol == 18) {
                    if (!input.read(7, extra)) return false;
                    repeat = extra + 11;
                } else {
                    return false;
                }
                if (lengths.size() + repeat > hlit + hdist) return false;
                lengths.insert(lengths.end(), repeat, value);
            }
            literal_lengths.assign(lengths.begin(), lengths.begin() + hlit);
            distance_lengths.assign(lengths.begin() + hlit, lengths.end());
        }
        DeflateHuffman literals;
        DeflateHuffman distances;
        if (!build_huffman(literal_lengths, literals) ||
            !build_huffman(distance_lengths, distances))
            return false;
        for (;;) {
            uint16_t symbol = 0;
            if (!decode_symbol(input, literals, symbol)) return false;
            if (symbol < 256) {
                if (out.size() >= expected) return false;
                out.push_back((uint8_t)symbol);
                continue;
            }
            if (symbol == 256) break;
            if (symbol < 257 || symbol > 285) return false;
            const unsigned length_index = symbol - 257;
            uint32_t extra = 0;
            if (!input.read(length_extra[length_index], extra)) return false;
            const size_t length = length_base[length_index] + extra;
            uint16_t distance_symbol = 0;
            if (!decode_symbol(input, distances, distance_symbol) ||
                distance_symbol >= 30)
                return false;
            if (!input.read(distance_extra[distance_symbol], extra)) return false;
            const size_t distance = distance_base[distance_symbol] + extra;
            if (distance == 0 || distance > out.size() ||
                out.size() + length > expected)
                return false;
            for (size_t i = 0; i < length; ++i)
                out.push_back(out[out.size() - distance]);
        }
    }
    return out.size() == expected;
}

bool extract_zip_impl(const std::vector<uint8_t>& bytes,
                 const std::vector<ZipEntry>& entries,
                 const fs::path& target, std::string* error) {
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        set_error(error, "cannot create staging directory: " + ec.message());
        return false;
    }
    for (const ZipEntry& entry : entries) {
        const fs::path output = target / fs::u8path(entry.name);
        if (entry.directory) {
            fs::create_directories(output, ec);
            if (ec) {
                set_error(error, "cannot create archive directory: " +
                                 ec.message());
                return false;
            }
            continue;
        }
        if ((uint64_t)entry.local_offset + 30 > bytes.size() ||
            le32(bytes.data() + entry.local_offset) != 0x04034b50u) {
            set_error(error, "ZIP local entry is invalid");
            return false;
        }
        const uint16_t name_len =
            le16(bytes.data() + entry.local_offset + 26);
        const uint16_t extra_len =
            le16(bytes.data() + entry.local_offset + 28);
        const uint64_t data_at =
            (uint64_t)entry.local_offset + 30 + name_len + extra_len;
        if (data_at + entry.compressed_size > bytes.size()) {
            set_error(error, "ZIP entry payload is invalid");
            return false;
        }
        const uint8_t* compressed = bytes.data() + data_at;
        std::vector<uint8_t> expanded;
        const uint8_t* data = compressed;
        if (entry.method == 0) {
            if (entry.compressed_size != entry.size) {
                set_error(error, "stored ZIP entry has inconsistent size");
                return false;
            }
        } else {
            if (!inflate_deflate(compressed, entry.compressed_size,
                                 entry.size, expanded)) {
                set_error(error, "cannot inflate ZIP entry: " + entry.name);
                return false;
            }
            data = expanded.data();
        }
        if (rmod_crc32_compute(data, entry.size) != entry.crc) {
            set_error(error, "ZIP entry checksum failed: " + entry.name);
            return false;
        }
        fs::create_directories(output.parent_path(), ec);
        if (ec) {
            set_error(error, "cannot create archive parent directory: " +
                             ec.message());
            return false;
        }
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file || (entry.size &&
                      !file.write((const char*)data, entry.size))) {
            set_error(error, "cannot extract archive entry: " + entry.name);
            return false;
        }
    }
    return true;
}

bool install_archive(Runtime& runtime, const fs::path& archive,
                     std::string* installed_id,
                     std::string* installed_version,
                     std::string* error) {
    if (archive.extension() != ".rmod") {
        set_error(error, "Recomp mod archives must use the .rmod extension");
        return false;
    }
    std::vector<uint8_t> bytes;
    std::vector<ZipEntry> entries;
    if (!read_file(archive, bytes, error) ||
        !parse_zip_impl(bytes, entries, error))
        return false;
    const bool has_manifest = std::any_of(
        entries.begin(), entries.end(),
        [](const ZipEntry& entry) {
            return entry.name == "manifest.toml" && !entry.directory;
        });
    if (!has_manifest) {
        set_error(error, "archive has no root manifest.toml");
        return false;
    }
    const uint64_t nonce = (uint64_t)
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path staging =
        runtime.root / (".staging-" + std::to_string(nonce));
    std::error_code ec;
    if (!inside(runtime.root, staging) || !fs::create_directory(staging, ec) || ec) {
        set_error(error, "cannot create a private staging directory"); return false;
    }
    auto cleanup = [&]() {
        std::error_code ignored;
        if (!is_link(staging) && inside(runtime.root, staging)) fs::remove_all(staging, ignored);
    };
    if (!extract_zip_impl(bytes, entries, staging, error)) {
        cleanup();
        return false;
    }
    Package package;
    if (!read_manifest(staging / "manifest.toml", package, error)) {
        cleanup();
        return false;
    }
    const fs::path destination =
        runtime.root / "packages" / package.id / package.version;
    if (is_link(runtime.root / "packages") || is_link(destination.parent_path()) ||
        !inside(runtime.root / "packages", destination)) {
        cleanup(); set_error(error, "package destination escapes the catalog"); return false;
    }
    if (fs::exists(destination)) {
        cleanup();
        set_error(error, package.id + " " + package.version +
                         " is already installed");
        return false;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (!ec) fs::rename(staging, destination, ec);
    if (ec) {
        cleanup();
        set_error(error, "cannot publish installed package: " + ec.message());
        return false;
    }
    if (installed_id) *installed_id = package.id;
    if (installed_version) *installed_version = package.version;
    return true;
}

bool sha256_file(const fs::path& path, std::string& out,
                 std::string* error) {
    std::ifstream input(path,std::ios::binary);
    if(!input){set_error(error,"Cannot read original image or resource");return false;}
    rmod_sha256_state state;rmod_sha256_init(&state);
    std::array<uint8_t,64*1024> buffer;
    while(input){input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());
        const auto count=input.gcount();if(count>0)rmod_sha256_update(&state,buffer.data(),static_cast<size_t>(count));
    }
    if(!input.eof()){set_error(error,"Original image or resource read failed");return false;}
    uint8_t digest[32];rmod_sha256_final(&state,digest);
    static const char hex[]="0123456789abcdef";out.resize(64);
    for(size_t i=0;i<32;++i){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}
    return true;
}


}  // namespace

bool mod_runtime_initialize(const fs::path& root,
                            const std::string& game_id,
                            const std::string& rom_sha256,
                            std::string* error) {
    Runtime& runtime = state();
    internal_mod_runtime_deactivate();
    auto plugins=std::move(runtime.plugins); auto resets=std::move(runtime.resets);
    runtime = {}; runtime.plugins=std::move(plugins); runtime.resets=std::move(resets);
    runtime.root = root;
    runtime.game_id = game_id;
    runtime.rom_sha256 = rom_sha256;
    if (game_id.empty() || !valid_sha256(rom_sha256)) {
        runtime.error = "Invalid mod target identity";
        set_error(error, runtime.error);
        return false;
    }
    if (!scan(runtime, &runtime.error) ||
        !load_state(runtime, &runtime.error)) {
        set_error(error, runtime.error);
        return false;
    }
    runtime.validation = validate(runtime);
    runtime.initialized = true;
    runtime.error.clear();
    return true;
}

#include "mod_identity.inc"

bool mod_runtime_commit(const fs::path& rom_path, std::string* error) {
    Runtime& runtime = state();
    if (!runtime.initialized || rom_path.empty()) {
        runtime.error = "A verified original ROM is required before activating mods.";
        set_error(error, runtime.error);
        return false;
    }
    if (!rom_path.empty()) {
        std::string digest;
        if (!sha256_file(rom_path, digest, &runtime.error)) {
            set_error(error, runtime.error);
            return false;
        }
        if (digest != runtime.rom_sha256) {
            runtime.error =
                "The selected ROM does not match this mod catalog target.";
            set_error(error, runtime.error);
            return false;
        }
    }
    runtime.validation = validate(runtime);
    if (!runtime.validation.ok) {
        runtime.error = "Resolve the highlighted mod conflicts before Play.";
        set_error(error, runtime.error);
        return false;
    }
    std::array<uint8_t,32> identity{};
    if(!committed_mod_identity(runtime,identity,&runtime.error)) {
        set_error(error,runtime.error);return false;
    }
    if (!save_state(runtime, &runtime.error)) {
        set_error(error, runtime.error);
        return false;
    }
    runtime.committed = runtime.validation;
    runtime.committed_identity=identity;
    runtime.committed_ready = true;
    runtime.error.clear();
    return true;
}

void mod_runtime_activate_plugins() {
    Runtime& runtime = state();
    if (!runtime.initialized || !runtime.committed_ready) return;
    frame_callbacks().clear();
    for (InternalModActivationCallback callback : reset_callbacks())
        if (callback) callback();
    for (const ResolvedPlugin& plugin : runtime.committed.plugins) {
        runtime.activating = &plugin;
        if (plugin.callback) plugin.callback();
    }
    runtime.activating = nullptr;
}


}  // namespace RecompMod

extern "C" int internal_mod_register_activation_plugin(
    const char* id, InternalModActivationCallback callback) {
    if (!id || !callback || !RecompMod::valid_id(id)) return 0;
    auto& plugins = RecompMod::registered_plugins();
    const auto prior = plugins.find(id);
    if (prior != plugins.end() && prior->second.activation != callback)
        return 0;
    plugins[id].activation = callback;
    return 1;
}

extern "C" int internal_mod_register_exclusive_plugin(const char* id, const char* resource,
                                                 InternalModActivationCallback callback) {
    if (!resource || !RecompMod::valid_id(resource)) return 0;
    if (!internal_mod_register_activation_plugin(id, callback)) return 0;
    auto& plugin = RecompMod::registered_plugins()[id];
    if (!plugin.resource.empty() && plugin.resource != resource) return 0;
    plugin.resource = resource;
    return 1;
}

extern "C" int internal_mod_register_reset_callback(
    InternalModActivationCallback callback) {
    if (!callback) return 0;
    auto& callbacks = RecompMod::reset_callbacks();
    if (std::find(callbacks.begin(), callbacks.end(), callback) ==
        callbacks.end())
        callbacks.push_back(callback);
    return 1;
}

extern "C" int internal_mod_register_frame_callback(InternalModFrameCallback callback) {
    if (!callback) return 0;
    auto& callbacks = RecompMod::frame_callbacks();
    if (std::find(callbacks.begin(), callbacks.end(), callback) ==
        callbacks.end())
        callbacks.push_back(callback);
    return 1;
}

extern "C" void internal_mod_runtime_frame_tick_c(void) {
    for (InternalModFrameCallback callback : RecompMod::frame_callbacks())
        if (callback) callback();
}

extern "C" int internal_mod_runtime_initialize_c(
    const char* root, const char* game_id, const char* rom_sha256) {
    std::string error;
    return RecompMod::mod_runtime_initialize(
        root ? fs::u8path(root) : fs::path("mods"),
        game_id ? game_id : "",
        rom_sha256 ? rom_sha256 : "", &error) ? 1 : 0;
}

extern "C" int internal_mod_runtime_commit_c(const char* rom_path) {
    std::string error;
    return RecompMod::mod_runtime_commit(
        rom_path ? fs::u8path(rom_path) : fs::path(), &error) ? 1 : 0;
}
extern "C" void internal_mod_runtime_identity_c(uint8_t out[32]) {
    if(out)std::memcpy(out,RecompMod::state().committed_identity.data(),32);
}

extern "C" void internal_mod_runtime_activate_plugins_c(void) {
    RecompMod::mod_runtime_activate_plugins();
}

extern "C" const char* internal_mod_runtime_last_error_c(void) {
    return RecompMod::state().error.c_str();
}

extern "C" int internal_mod_install_archive(const char* path) {
    using namespace RecompMod;
    if (!state().initialized || !path) return 0;
    std::string id, version;
    if (!install_archive(state(), fs::u8path(path), &id, &version, &state().error) ||
        !scan(state(), &state().error)) return 0;
    state().selections[id].version = version;
    state().validation = validate(state());
    state().error.clear();
    return 1;
}

extern "C" int internal_mod_enable(const char* package_id, const char* feature_id, int enabled) {
    using namespace RecompMod;
    if (!package_id || !feature_id) return 0;
    const Package* p = selected_package(state(), package_id);
    if (!p || !find_feature(*p, feature_id)) {
        state().error = "Unknown package feature.";
        return 0;
    }
    auto& selection = package_selection(state(), *p).features[feature_id];
    selection.enabled = enabled != 0;
    selection.has_enabled = true;
    state().validation = validate(state());
    state().error.clear();
    return 1;
}

extern "C" int internal_mod_set_option(const char* package_id, const char* feature_id,
                                  const char* option_id, const char* value) {
    using namespace RecompMod;
    if (!package_id || !feature_id || !option_id || !value) return 0;
    const Package* p = selected_package(state(), package_id);
    const Option* o = p ? find_option(*p, feature_id, option_id) : nullptr;
    if (!o) { state().error = "Unknown feature option."; return 0; }
    if (!validate_option_value(*o, value, &state().error)) return 0;
    package_selection(state(), *p).features[feature_id].values[option_id] = value;
    state().validation = validate(state());
    state().error.clear();
    return 1;
}

extern "C" int internal_mod_option(const char* id, char* out, unsigned capacity) {
    using namespace RecompMod;
    auto& rt = state();
    if (!rt.activating || !id || !out || !capacity) return 0;
    const auto found = rt.activating->options.find(id);
    if (found == rt.activating->options.end()) return 0;
    const auto& value = found->second;
    if (value.size() >= capacity) return 0;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return 1;
}

extern "C" int internal_mod_asset(const char* relative, char* out, unsigned capacity) {
    using namespace RecompMod;
    auto& rt = state();
    if (!rt.activating || !relative || !out || !capacity || !safe_archive_name(relative)) return 0;
    std::error_code ec;
    const auto root = fs::canonical(rt.activating->root, ec);
    if (ec) return 0;
    const auto path = fs::canonical(root / fs::u8path(relative), ec);
    if (ec || !fs::is_regular_file(path, ec)) return 0;
    const auto rel = path.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") return 0;
    const auto text = path.u8string();
    if (text.size() >= capacity) return 0;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return 1;
}

extern "C" int internal_mod_resource(const char* id, char* out, unsigned capacity) {
    using namespace RecompMod;
    const auto& rt = state();
    if (!rt.activating || !id || !out || !capacity) return 0;
    const auto value = rt.activating->resources.find(id);
    if (value == rt.activating->resources.end() || value->second.empty() || value->second.size() >= capacity) return 0;
    std::memcpy(out, value->second.c_str(), value->second.size() + 1);
    return 1;
}

extern "C" void internal_mod_runtime_deactivate(void) {
    using namespace RecompMod;
    frame_callbacks().clear();
    for (auto reset : reset_callbacks()) if (reset) reset();
    state().activating = nullptr;
}

struct rmod_context { RecompMod::Runtime runtime; };
namespace {
template<class F> int context_call(rmod_context* c, F&& operation) noexcept {
    if(!c) return 0;
    try { RecompMod::ContextScope scope(c->runtime); return operation(); }
    catch(const std::exception& e) { c->runtime.error=e.what(); return 0; }
    catch(...) { c->runtime.error="Mod core operation failed"; return 0; }
}
}
extern "C" int rmod_sha256_bytes(const void* data,size_t size,uint8_t out[32]) {
    if(!out||(!data&&size))return 0;
    rmod_sha256_compute(static_cast<const uint8_t*>(data),size,out);return 1;
}
extern "C" rmod_context* rmod_create(void) { try{return new rmod_context;}catch(...){return nullptr;} }
extern "C" void rmod_destroy(rmod_context* c) { delete c; }
extern "C" int rmod_initialize(rmod_context* c,const char* root,const char* game,const char* hash) {
    return context_call(c,[&]{return internal_mod_runtime_initialize_c(root,game,hash);});
}
extern "C" int rmod_bind_plan(rmod_context* c,const char* game,const char* image,const char* plan) {
    return context_call(c,[&]{
        if(!game||!*game||!image||!plan||!RecompMod::valid_sha256(image)||
           !RecompMod::valid_sha256(plan)) {
            c->runtime.error="Invalid resolved-plan identity";return 0;
        }
        auto nibble=[](char value){return value<='9'?value-'0':value-'a'+10;};
        std::array<uint8_t,32> identity{};
        for(size_t i=0;i<identity.size();++i)
            identity[i]=(uint8_t)((nibble(plan[i*2])<<4)|nibble(plan[i*2+1]));
        c->runtime.game_id=game;c->runtime.rom_sha256=image;
        c->runtime.committed_identity=identity;c->runtime.committed_ready=true;
        c->runtime.error.clear();return 1;
    });
}
extern "C" int rmod_install(rmod_context* c,const char* path) {
    return context_call(c,[&]{return internal_mod_install_archive(path);});
}
extern "C" int rmod_enable(rmod_context* c,const char* p,const char* f,int enabled) {
    return context_call(c,[&]{return internal_mod_enable(p,f,enabled);});
}
extern "C" int rmod_option_set(rmod_context* c,const char* p,const char* f,const char* o,const char* v) {
    return context_call(c,[&]{return internal_mod_set_option(p,f,o,v);});
}
extern "C" int rmod_resource_set(rmod_context* c,const char* p,const char* f,const char* id,const char* path) {
    return context_call(c,[&]{
        if(!p||!f||!id||!path)return 0;
        const auto* package=RecompMod::selected_package(c->runtime,p);
        if(!package)return 0;
        const auto found=std::find_if(package->resources.begin(),package->resources.end(),[&](const auto& r){return r.feature_id==f&&r.id==id;});
        if(found==package->resources.end()){c->runtime.error="Unknown feature resource";return 0;}
        RecompMod::package_selection(c->runtime,*package).features[f].resources[id]=path;
        c->runtime.error.clear();return 1;
    });
}
extern "C" int rmod_commit(rmod_context* c,const char* path) {
    return context_call(c,[&]{return internal_mod_runtime_commit_c(path);});
}
extern "C" void rmod_identity(const rmod_context* c,uint8_t out[32]) {
    if(c&&out)std::memcpy(out,c->runtime.committed_identity.data(),32);
}
extern "C" const char* rmod_error(const rmod_context* c){return c?c->runtime.error.c_str():"Null mod context";}
extern "C" int rmod_register_plugin(rmod_context* c,const char* id,const char* resource,rmod_plugin_callback cb) {
    return context_call(c,[&]{return resource&&*resource?internal_mod_register_exclusive_plugin(id,resource,cb):internal_mod_register_activation_plugin(id,cb);});
}
extern "C" int rmod_activate(rmod_context* c) {
    return context_call(c,[&]{if(!c->runtime.committed_ready)return 0;internal_mod_runtime_activate_plugins_c();return 1;});
}
extern "C" int rmod_option_get(rmod_context* c,const char* id,char* out,unsigned n) {
    return context_call(c,[&]{return internal_mod_option(id,out,n);});
}
extern "C" int rmod_asset_get(rmod_context* c,const char* id,char* out,unsigned n) {
    return context_call(c,[&]{return internal_mod_asset(id,out,n);});
}
extern "C" int rmod_resource_get(rmod_context* c,const char* id,char* out,unsigned n) {
    return context_call(c,[&]{return internal_mod_resource(id,out,n);});
}
extern "C" size_t rmod_feature_count(rmod_context* c) {
    size_t count=0;context_call(c,[&]{for(const auto& item:c->runtime.packages){const auto* p=RecompMod::selected_package(c->runtime,item.first);if(p)count+=p->features.size();}return 1;});return count;
}
extern "C" int rmod_feature_at(rmod_context* c,size_t n,rmod_feature_info* out) {
    return context_call(c,[&]{
        if(!out)return 0;
        for(const auto& item:c->runtime.packages){const auto* p=RecompMod::selected_package(c->runtime,item.first);if(!p)continue;
            for(const auto& f:p->features){if(n--==0){*out={p->id.c_str(),f.id.c_str(),f.name.c_str(),f.description.c_str(),RecompMod::feature_enabled(c->runtime,*p,f)?1:0};return 1;}}
        }return 0;
    });
}
extern "C" size_t rmod_diagnostic_count(rmod_context* c) { return c?c->runtime.validation.diagnostics.size():0; }
extern "C" int rmod_diagnostic_at(rmod_context* c,size_t n,rmod_diagnostic_info* out) {
    if(!c||!out||n>=c->runtime.validation.diagnostics.size())return 0;
    const auto& d=c->runtime.validation.diagnostics[n];
    *out={d.package_id.c_str(),d.feature_id.c_str(),d.related_package_id.c_str(),d.related_feature_id.c_str(),d.resource.c_str(),d.message.c_str()};return 1;
}
extern "C" int rmod_apply(rmod_context* c,const rmod_patch* patches,size_t n,
                          const rmod_hook* hooks,size_t hn,const rmod_engine* e,uint8_t out[32]) {
    return context_call(c,[&]{
        if(!c->runtime.committed_ready||!e||!e->game_id||!e->original_image_sha256||
           c->runtime.game_id!=e->game_id||c->runtime.rom_sha256!=e->original_image_sha256||
           !e->begin||!e->read||!e->write||!e->finish||
           (n&&!patches)||(hn&&(!hooks||!e->hook))||n>4096||hn>4096){c->runtime.error="Invalid or uncommitted activation plan";return 0;}
        struct Patch {std::string space;uint64_t address;std::vector<uint8_t> expected,replacement,before;};
        std::vector<Patch> plan;std::vector<std::string> hook_plan;std::vector<uint8_t> identity;
        identity.insert(identity.end(),c->runtime.committed_identity.begin(),c->runtime.committed_identity.end());
        RecompMod::identity_text(identity,c->runtime.game_id);RecompMod::identity_text(identity,c->runtime.rom_sha256);
        size_t total=0;
        for(size_t i=0;i<n;++i){const auto& p=patches[i];
            if(!p.space||!RecompMod::valid_id(p.space)||!p.size||!p.expected||!p.replacement||
               p.size>RecompMod::kMaxArchiveBytes-total||p.address>UINT64_MAX-p.size){c->runtime.error="Invalid patch extent";return 0;}
            for(const auto& prior:plan)if(prior.space==p.space&&prior.address<p.address+p.size&&p.address<prior.address+prior.expected.size()){
                c->runtime.error="Overlapping patch claims";return 0;
            }
            total+=p.size;plan.push_back({p.space,p.address,{p.expected,p.expected+p.size},{p.replacement,p.replacement+p.size},std::vector<uint8_t>(p.size)});
            RecompMod::identity_text(identity,p.space);RecompMod::identity_text(identity,std::to_string(p.address));
            RecompMod::identity_text(identity,std::string(reinterpret_cast<const char*>(p.expected),p.size));
            RecompMod::identity_text(identity,std::string(reinterpret_cast<const char*>(p.replacement),p.size));
        }
        std::set<std::string> claimed;
        for(size_t i=0;i<hn;++i){if(!hooks[i].id||!RecompMod::valid_id(hooks[i].id)||!claimed.insert(hooks[i].id).second){c->runtime.error="Invalid or duplicate hook claim";return 0;}
            hook_plan.emplace_back(hooks[i].id);RecompMod::identity_text(identity,hooks[i].id);
        }
        uint8_t fingerprint[32];rmod_sha256_compute(identity.data(),identity.size(),fingerprint);
        if(!e->begin(e->user)){c->runtime.error="Engine transaction unavailable";return 0;}
        for(auto& p:plan)if(!e->read(e->user,p.space.c_str(),p.address,p.before.data(),p.before.size())||p.before!=p.expected){
            e->finish(e->user,0);c->runtime.error="Expected-byte guard failed";return 0;
        }
        size_t touched=0;bool ok=true;
        for(auto& p:plan){++touched;if(!e->write(e->user,p.space.c_str(),p.address,p.replacement.data(),p.replacement.size())){ok=false;break;}}
        if(ok)for(const auto& h:hook_plan)if(!e->hook(e->user,h.c_str())){ok=false;break;}
        if(!ok){bool restored=true;
            while(touched){const auto& p=plan[--touched];if(!e->write(e->user,p.space.c_str(),p.address,p.before.data(),p.before.size()))restored=false;}
            e->finish(e->user,0);c->runtime.error=restored?"Activation rolled back":"Byte restoration failed; stop guest execution";return restored?0:-1;
        }
        e->finish(e->user,1);if(out)std::memcpy(out,fingerprint,32);c->runtime.error.clear();return 1;
    });
}

namespace recomp::mods {
bool parse_zip(const std::vector<uint8_t>& bytes, std::vector<ZipEntry>& entries,
               std::string* error) {
    return RecompMod::parse_zip_impl(bytes, entries, error);
}
bool extract_zip(const std::vector<uint8_t>& bytes, const std::vector<ZipEntry>& entries,
                 const std::filesystem::path& target, std::string* error) {
    return RecompMod::extract_zip_impl(bytes, entries, target, error);
}
}
