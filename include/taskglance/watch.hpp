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
  bool interactive = false;
};

struct WatchViewport {
  const std::vector<Task>& tasks;  // already ordered and filtered
  std::size_t first_row = 0;
  std::string selected_id;
};

// Inputs are already terminal-safe. Width excludes the reserved last column.
// If the prefix leaves no room for text, fall back to one clipped line.
std::vector<std::string> wrap_watch_row(const std::string& prefix,
                                      const std::string& text, int width);

struct WatchRowLayout {
  std::size_t first_row = 0;
  std::size_t count = 0;
  std::size_t selected_lines = 0;
  std::size_t more = 0;
};

// Slots exclude header/status. Only the selected row has variable height.
WatchRowLayout layout_watch_rows(std::size_t count, std::size_t selected,
                                 std::size_t selected_height,
                                 std::size_t first_row, std::size_t slots);

// Terminal width includes the reserved column; height excludes status,
// but includes the header.
WatchRowLayout layout_watch_viewport(const std::vector<Task>& tasks,
                                     const WatchViewport& viewport,
                                     int width, int height);

// Formats time as HH:MM:SS in the local time zone.
std::string format_local_clock(std::chrono::system_clock::time_point time);
std::optional<double> parse_watch_interval(const std::string& value);
TaskChanges detect_task_changes(const std::vector<Task>& tasks,
                                const std::vector<Task>& previous);
// Keeps printable, well-formed UTF-8; replaces terminal controls and bad
// bytes. Apply before any styling, including to status/editor text.
std::string terminal_safe_text(const std::string& text);
std::string build_watch_frame(
  const std::vector<Task>& tasks,
  const std::vector<std::string>& changed_ids,
  int width, int height,
  std::chrono::system_clock::time_point last_change,
  std::chrono::duration<double> since_change,
  const WatchOptions& options,
  const WatchViewport* viewport = nullptr
);

}  // namespace taskglance
