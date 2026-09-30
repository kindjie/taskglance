#include "taskglance/watch.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
#include <sstream>
#include <unordered_map>

#include "taskglance/render.hpp"
#include "taskglance/util.hpp"

namespace taskglance {
namespace {

std::size_t utf8_length(unsigned char lead) {
  if (lead >= 0xc2 && lead <= 0xdf) return 2;
  if (lead >= 0xe0 && lead <= 0xef) return 3;
  if (lead >= 0xf0 && lead <= 0xf4) return 4;
  return 0;
}

// Returns the code point of a well-formed UTF-8 sequence at offset, or
// nothing if it is malformed, overlong, a surrogate, or out of range.
std::optional<char32_t> decode_utf8(const std::string& text,
                                    std::size_t offset, std::size_t length) {
  if (length == 0 || offset + length > text.size()) {
    return std::nullopt;
  }
  auto lead = static_cast<unsigned char>(text[offset]);
  char32_t value = lead & (0x7f >> length);
  for (std::size_t i = 1; i < length; ++i) {
    auto next = static_cast<unsigned char>(text[offset + i]);
    if ((next & 0xc0) != 0x80) {
      return std::nullopt;
    }
    value = (value << 6) | (next & 0x3f);
  }
  static constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < minimum[length] || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) {
    return std::nullopt;
  }
  return value;
}

// Task text comes from any process that can write the task file. Keep
// well-formed printable UTF-8 and replace everything a terminal could act
// on (C0, DEL, C1 in raw or encoded form, malformed bytes) with '?'.
std::string terminal_safe(const std::string& text) {
  std::string output;
  for (std::size_t i = 0; i < text.size();) {
    auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80) {
      output.push_back(byte >= 0x20 && byte != 0x7f ? text[i] : '?');
      ++i;
      continue;
    }
    auto length = utf8_length(byte);
    auto value = decode_utf8(text, i, length);
    if (!value || (*value >= 0x80 && *value <= 0x9f)) {
      output.push_back('?');
      ++i;
      continue;
    }
    output.append(text, i, length);
    i += length;
  }
  return output;
}

}  // namespace


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
      !std::isfinite(seconds) || seconds < 0.1) {
    return std::nullopt;
  }
  return seconds;
}

TaskChanges detect_task_changes(const std::vector<Task>& tasks,
                                const std::vector<Task>& previous) {
  TaskChanges changes;
  std::unordered_map<std::string, const Task*> before;
  for (const auto& task : previous) {
    before.emplace(task.id, &task);
  }
  for (const auto& task : tasks) {
    auto old = before.find(task.id);
    if (old == before.end()) {
      changes.changed_ids.push_back(task.id);
      continue;
    }
    if (old->second->text != task.text ||
        old->second->status != task.status) {
      changes.changed_ids.push_back(task.id);
    }
    before.erase(old);
  }
  // Whatever was not matched above no longer exists; report it in the
  // previous order.
  for (const auto& task : previous) {
    if (before.count(task.id) != 0) {
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
  // Writing a terminal's final column leaves the cursor pending a wrap, so
  // the line erase that follows would delete that cell, and Windows
  // consoles wrap at once. Leave the column free.
  if (options.tty && width > 1) {
    --width;
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
      terminal_safe(sanitize_task_text(task_label(task, id_width))), width
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
