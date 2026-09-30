#include "taskglance/watch.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <sstream>

#include "taskglance/render.hpp"
#include "taskglance/util.hpp"

namespace taskglance {

std::string format_local_clock(std::chrono::system_clock::time_point time) {
  auto seconds = std::chrono::system_clock::to_time_t(time);
  std::tm local {};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
  return buffer;
}

std::optional<double> parse_watch_interval(const std::string& value) {
  double seconds = 0;
  auto [end, error] = std::from_chars(value.data(),
                                     value.data() + value.size(), seconds);
  if (error != std::errc{} || end != value.data() + value.size() ||
      !std::isfinite(seconds) || seconds <= 0) {
    return std::nullopt;
  }
  return seconds;
}

TaskChanges detect_task_changes(const std::vector<Task>& tasks,
                                const std::vector<Task>& previous) {
  TaskChanges changes;
  for (const auto& task : tasks) {
    auto old = std::find_if(previous.begin(), previous.end(),
                            [&](const auto& item) {
      return item.id == task.id;
    });
    if (old == previous.end() || old->text != task.text ||
        old->status != task.status) {
      changes.changed_ids.push_back(task.id);
    }
  }
  for (const auto& task : previous) {
    if (std::none_of(tasks.begin(), tasks.end(), [&](const auto& item) {
      return item.id == task.id;
    })) {
      changes.deleted_ids.push_back(task.id);
    }
  }
  return changes;
}

std::string build_watch_frame(
  const std::vector<Task>& tasks,
  const std::vector<std::string>& changed_ids,
  int width, int height,
  std::chrono::system_clock::time_point last_change,
  std::chrono::duration<double> since_change,
  const WatchOptions& options
) {
  if (width <= 0 || height <= 0) {
    return "";
  }
  auto visible = options.all ? tasks : active_tasks(tasks);
  std::stable_sort(visible.begin(), visible.end(), [](const auto& a,
                                                     const auto& b) {
    if (a.status != b.status) {
      return a.status == TaskStatus::Active;
    }
    return a.created_at < b.created_at;
  });
  std::ostringstream header;
  header << "taskglance | " << active_tasks(tasks).size()
         << " active | changed " << format_local_clock(last_change);
  std::string frame = truncate_display(header.str(), width);
  auto slots = static_cast<std::size_t>(height - 1);
  bool overflow = visible.size() > slots;
  auto count = std::min(visible.size(), slots);
  if (overflow && slots > 0) {
    --count;
  }
  auto id_width = unique_id_width(tasks, 2, options.all);
  Config colors;
  colors.color = options.tty && options.color;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& task = visible[i];
    // Stored/imported data can contain controls; only our styles may emit
    // escape sequences, and truncation must happen before styling.
    auto line = truncate_display(
      sanitize_task_text(task_label(task, id_width)), width
    );
    line = color_task_ids(line, colors);
    if (options.tty) {
      bool highlight = since_change.count() >= 0 &&
                       since_change.count() < 10 &&
                       std::find(changed_ids.begin(), changed_ids.end(),
                                  task.id) != changed_ids.end();
      bool done = task.status == TaskStatus::Done;
      if (highlight) {
        line = "\033[1m" + line;
      }
      if (done) {
        line = "\033[2m" + line;
      }
      if (highlight || done) {
        line += "\033[0m";
      }
    }
    frame += '\n' + line;
  }
  if (overflow && slots > 0) {
    frame += '\n' + truncate_display(
      "+" + std::to_string(visible.size() - count) + " more", width
    );
  }
  return frame;
}

}  // namespace taskglance
