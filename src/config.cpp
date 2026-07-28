#include "taskglance/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "taskglance/util.hpp"

namespace taskglance {
namespace {

std::filesystem::path home_dir() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string(home).empty()) {
    return ".";
  }
  return home;
}

std::filesystem::path env_path(const char* name,
                               const std::filesystem::path& fallback) {
  const char* value = std::getenv(name);
  if (value != nullptr && std::string(value).size() > 0) {
    return value;
  }
  return fallback;
}

bool parse_bool(const std::string& value) {
  auto normalized = to_lower(trim(value));
  return normalized == "1" || normalized == "true" ||
         normalized == "yes" || normalized == "on";
}

std::string bool_string(bool value) {
  return value ? "true" : "false";
}

bool in_set(const std::string& value, std::initializer_list<const char*> set) {
  for (const char* item : set) {
    if (value == item) {
      return true;
    }
  }
  return false;
}

bool is_int_in_range(const std::string& value, int min, int max) {
  try {
    std::size_t consumed = 0;
    int parsed = std::stoi(value, &consumed);
    return consumed == value.size() && parsed >= min && parsed <= max;
  } catch (...) {
    return false;
  }
}

}  // namespace

Paths default_paths() {
  auto home = home_dir();
  auto config_home = env_path("XDG_CONFIG_HOME", home / ".config");
  auto data_home = env_path("XDG_DATA_HOME", home / ".local" / "share");
  auto state_home = env_path("XDG_STATE_HOME", home / ".local" / "state");

  Paths paths;
  paths.data_dir = data_home / "taskglance";
  paths.config_dir = config_home / "taskglance";
  paths.state_dir = state_home / "taskglance";
  paths.tasks_file = paths.data_dir / "tasks.tsv";
  paths.config_file = paths.config_dir / "config";
  paths.prompt_state_file = paths.state_dir / "prompt.state";
  return paths;
}

std::optional<std::string> validate_config_key_value(
  const std::string& key,
  const std::string& value
) {
  if (key == "display_style") {
    if (!in_set(value, {"compact", "box", "plain"})) {
      return "display_style must be compact, box, or plain";
    }
    return std::nullopt;
  }
  if (key == "prompt_mode") {
    if (!in_set(value, {"compact", "transient", "manual"})) {
      return "prompt_mode must be compact, transient, or manual";
    }
    return std::nullopt;
  }
  if (key == "prompt_align") {
    if (!in_set(value, {"left", "right"})) {
      return "prompt_align must be left or right";
    }
    return std::nullopt;
  }
  if (key == "prompt_interval_seconds") {
    if (!is_int_in_range(value, 0, 86400)) {
      return "prompt_interval_seconds must be 0..86400";
    }
    return std::nullopt;
  }
  if (key == "max_prompt_tasks") {
    if (!is_int_in_range(value, 1, 20)) {
      return "max_prompt_tasks must be 1..20";
    }
    return std::nullopt;
  }
  if (key == "max_prompt_width") {
    if (!is_int_in_range(value, 0, 500)) {
      return "max_prompt_width must be 0..500";
    }
    return std::nullopt;
  }
  if (key == "prompt_enabled" || key == "color" || key == "alias_tg") {
    auto normalized = to_lower(trim(value));
    if (!in_set(normalized, {"true", "false", "1", "0", "yes", "no",
                             "on", "off"})) {
      return key + " must be a boolean";
    }
    return std::nullopt;
  }
  return "Unknown config key: " + key;
}

std::map<std::string, std::string> config_to_map(const Config& config) {
  return {
    {"display_style", config.display_style},
    {"prompt_align", config.prompt_align},
    {"prompt_mode", config.prompt_mode},
    {"prompt_interval_seconds",
     std::to_string(config.prompt_interval_seconds)},
    {"max_prompt_tasks", std::to_string(config.max_prompt_tasks)},
    {"max_prompt_width", std::to_string(config.max_prompt_width)},
    {"prompt_enabled", bool_string(config.prompt_enabled)},
    {"color", bool_string(config.color)},
    {"alias_tg", bool_string(config.alias_tg)},
  };
}

void set_config_value(Config& config, const std::string& key,
                      const std::string& value) {
  if (auto error = validate_config_key_value(key, value)) {
    throw std::runtime_error(*error);
  }
  if (key == "display_style") {
    config.display_style = value;
  } else if (key == "prompt_align") {
    config.prompt_align = value;
  } else if (key == "prompt_mode") {
    config.prompt_mode = value;
  } else if (key == "prompt_interval_seconds") {
    config.prompt_interval_seconds = std::stoi(value);
  } else if (key == "max_prompt_tasks") {
    config.max_prompt_tasks = std::stoi(value);
  } else if (key == "max_prompt_width") {
    config.max_prompt_width = std::stoi(value);
  } else if (key == "prompt_enabled") {
    config.prompt_enabled = parse_bool(value);
  } else if (key == "color") {
    config.color = parse_bool(value);
  } else if (key == "alias_tg") {
    config.alias_tg = parse_bool(value);
  }
}

Config load_config(const Paths& paths) {
  Config config;
  std::ifstream in(paths.config_file);
  if (!in) {
    return config;
  }

  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    line = trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto equals = line.find('=');
    if (equals == std::string::npos) {
      throw std::runtime_error(
        "Invalid config line " + std::to_string(line_number)
      );
    }
    auto key = trim(line.substr(0, equals));
    auto value = trim(line.substr(equals + 1));
    set_config_value(config, key, value);
  }
  return config;
}

void save_config(const Paths& paths, const Config& config) {
  std::ostringstream out;
  out << "# taskglance config\n";
  for (const auto& [key, value] : config_to_map(config)) {
    out << key << '=' << value << '\n';
  }
  atomic_write_file(paths.config_file, out.str());
}

}  // namespace taskglance
