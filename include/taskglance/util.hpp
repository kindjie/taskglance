#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace taskglance {

std::string trim(const std::string& value);
std::vector<std::string> split(const std::string& value, char delimiter);
std::string join(const std::vector<std::string>& values,
                 const std::string& delimiter);

std::string to_lower(std::string value);
bool is_truthy_env(const char* name);
bool stdout_is_tty();
int terminal_columns();

std::string time_to_iso(std::chrono::system_clock::time_point time);
std::chrono::system_clock::time_point time_from_iso(const std::string& value);

void ensure_parent_dir(const std::filesystem::path& file);
void atomic_write_file(const std::filesystem::path& file,
                       const std::string& content);

std::uint64_t fnv1a64(const std::string& value);
std::string hex_short(std::uint64_t value, int width);

int display_width(const std::string& value);
std::string truncate_display(const std::string& value, int width);

}  // namespace taskglance
