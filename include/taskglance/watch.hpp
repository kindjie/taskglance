#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "taskglance/store.hpp"

namespace taskglance {

struct TaskChanges {
  // Only surviving added/edited tasks can be highlighted.
  std::vector<std::string> changed_ids;
  std::vector<std::string> deleted_ids;
};

struct WatchOptions {
  bool all = false;
  bool tty = false;
  bool color = true;
};

std::optional<double> parse_watch_interval(const std::string& value);
TaskChanges detect_task_changes(const std::vector<Task>& tasks,
                                const std::vector<Task>& previous);
std::string build_watch_frame(
  const std::vector<Task>& tasks,
  const std::vector<std::string>& changed_ids,
  int width, int height,
  std::chrono::system_clock::time_point last_change,
  std::chrono::duration<double> since_change,
  const WatchOptions& options
);

}  // namespace taskglance
