#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
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

// A single snapshot for byte comparison and parsing; missing files are empty.
std::string read_task_file(const std::filesystem::path& file);
std::vector<Task> parse_tasks(const std::string& content);

std::vector<Task> load_tasks(const std::filesystem::path& file);
void save_tasks(const std::filesystem::path& file,
                const std::vector<Task>& tasks);

// Loads the tasks, applies change, and saves the result if change returns
// true, all under an exclusive lock so concurrent writers cannot lose each
// other's changes. Every read-modify-write of the task file goes through here.
void update_tasks(
  const std::filesystem::path& file,
  const std::function<bool(std::vector<Task>&)>& change
);

// Returns false if cancelled before change runs; true means change ran,
// even if it declined to save. Empty cancel preserves blocking acquisition.
// Cancellation is checked while waiting and immediately before change.
bool update_tasks(
  const std::filesystem::path& file,
  const std::function<bool(std::vector<Task>&)>& change,
  const std::function<bool()>& cancel
);

Task make_task(const std::string& text,
               const std::vector<Task>& existing_tasks,
               std::chrono::system_clock::time_point created_at =
                 std::chrono::system_clock::now());
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
