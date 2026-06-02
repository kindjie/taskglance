#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace taskglance {

enum class TaskStatus {
  Active,
  Done,
};

struct Task {
  std::string id;
  TaskStatus status = TaskStatus::Active;
  std::chrono::system_clock::time_point created_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::string text;
};

std::vector<Task> load_tasks(const std::filesystem::path& file);
void save_tasks(const std::filesystem::path& file,
                const std::vector<Task>& tasks);

Task make_task(const std::string& text,
               const std::vector<Task>& existing_tasks);
std::vector<Task> active_tasks(const std::vector<Task>& tasks);

std::optional<std::size_t> find_task_by_prefix(
  const std::vector<Task>& tasks,
  const std::string& id_or_prefix,
  std::string* error
);

std::string encode_field(const std::string& value);
std::string decode_field(const std::string& value);
std::string sanitize_task_text(const std::string& text);

}  // namespace taskglance
