#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace taskglance {

struct Paths {
  std::filesystem::path data_dir;
  std::filesystem::path config_dir;
  std::filesystem::path state_dir;
  std::filesystem::path tasks_file;
  std::filesystem::path config_file;
  std::filesystem::path prompt_state_file;
};

struct Config {
  std::string display_style = "compact";
  std::string prompt_mode = "compact";
  std::string prompt_align = "right";
  int prompt_interval_seconds = 900;
  int max_prompt_tasks = 2;
  int max_prompt_width = 0;
  bool prompt_enabled = true;
  bool color = false;
  bool alias_tg = false;
};

Paths default_paths();
Config load_config(const Paths& paths);
void save_config(const Paths& paths, const Config& config);

std::optional<std::string> validate_config_key_value(
  const std::string& key,
  const std::string& value
);

std::map<std::string, std::string> config_to_map(const Config& config);
void set_config_value(Config& config, const std::string& key,
                      const std::string& value);

}  // namespace taskglance
