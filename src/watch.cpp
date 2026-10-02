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

// Clip terminal-safe UTF-8 without an ellipsis or a partial code point.
std::string clip_watch_text(const std::string& text, int width) {
  std::size_t end = 0;
  int used = 0;
  while (end < text.size()) {
    auto length = std::max<std::size_t>(1, utf8_length(
      static_cast<unsigned char>(text[end])));
    auto columns = display_width(text.substr(end, length));
    if (used + columns > width) break;
    used += columns;
    end += length;
  }
  return text.substr(0, end);
}

std::string truncate_watch_text(const std::string& text, int width) {
  // truncate_display's tiny-width path slices bytes; avoid that for UTF-8.
  return width <= 3 ? clip_watch_text(text, width)
                    : truncate_display(text, width);
}

std::vector<std::string> selected_row_lines(const Task& task,
                                           std::size_t id_width, int width) {
  auto prefix = terminal_safe_text(
    "[" + task.id.substr(0, id_width) + "] ");
  auto text = terminal_safe_text(sanitize_task_text(task.text));
  return wrap_watch_row(prefix, text, width);
}

}  // namespace

std::vector<std::string> wrap_watch_row(const std::string& prefix,
                                      const std::string& text, int width) {
  auto label = prefix + text;
  if (display_width(label) <= width) return {label};
  int room = width - display_width(prefix);
  if (room <= 0) return {truncate_watch_text(label, width)};
  std::vector<std::string> lines;
  auto indent = std::string(static_cast<std::size_t>(display_width(prefix)),
                            ' ');
  std::size_t start = 0;
  while (start < text.size()) {
    auto chunk = clip_watch_text(text.substr(start), room);
    if (chunk.empty()) {
      // Even a single wide character cannot fit beside the id column.
      lines.push_back(truncate_watch_text(
        (lines.empty() ? prefix : indent) + text.substr(start), width));
      break;
    }
    auto end = start + chunk.size();
    if (end < text.size() && text[end] != ' ') {
      auto space = chunk.rfind(' ');
      if (space != std::string::npos && space > 0) {
        chunk.resize(space);
        end = start + space;
      }
    }
    lines.push_back((lines.empty() ? prefix : indent) + chunk);
    start = end;
    while (start < text.size() && text[start] == ' ') ++start;
  }
  return lines;
}

WatchRowLayout layout_watch_rows(std::size_t count, std::size_t selected,
                                 std::size_t selected_height,
                                 std::size_t first_row, std::size_t slots) {
  if (count == 0 || slots == 0) return {};
  selected = std::min(selected, count - 1);
  selected_height = std::max<std::size_t>(1, selected_height);
  // A summary must not displace any part of a row that needs the whole pane.
  bool summary = slots > selected_height &&
                 count > slots - selected_height + 1;
  auto capacity = slots - (summary ? 1 : 0);
  auto selected_lines = std::min(selected_height, capacity);
  auto other_rows = capacity - selected_lines;
  first_row = std::min(first_row, selected);
  auto earliest = selected > other_rows ? selected - other_rows : 0;
  first_row = std::max(first_row, earliest);
  // Fill spare space above the selection when it is near the end.
  auto available = count - first_row;
  if (available < other_rows + 1) {
    auto spare = other_rows + 1 - available;
    first_row -= std::min(first_row, spare);
  }
  auto shown = std::min(count - first_row, other_rows + 1);
  return {first_row, shown, selected_lines,
          summary ? count - first_row - shown : 0};
}

WatchRowLayout layout_watch_viewport(const std::vector<Task>& tasks,
                                     const WatchViewport& viewport,
                                     int width, int height) {
  if (width <= 0 || height <= 1 || viewport.tasks.empty()) return {};
  auto selected = std::find_if(viewport.tasks.begin(), viewport.tasks.end(),
    [&](const Task& task) { return task.id == viewport.selected_id; });
  auto index = static_cast<std::size_t>(selected - viewport.tasks.begin());
  if (selected == viewport.tasks.end()) {
    index = std::min(viewport.first_row, viewport.tasks.size() - 1);
  }
  auto lines = selected == viewport.tasks.end() ? 1 : selected_row_lines(
    *selected, unique_id_width(tasks, 2, true), width - 1).size();
  return layout_watch_rows(viewport.tasks.size(), index, lines,
                            viewport.first_row,
                            static_cast<std::size_t>(height - 1));
}

// Task text comes from any process that can write the task file. Keep
// well-formed printable UTF-8 and replace everything a terminal could act
// on (C0, DEL, C1 in raw or encoded form, malformed bytes) with '?'.
std::string terminal_safe_text(const std::string& text) {
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
  const WatchOptions& options,
  const WatchViewport* viewport
) {
  if (width <= 0 || height <= 0) {
    return "";
  }
  auto terminal_width = width;
  // Writing a terminal's final column leaves the cursor pending a wrap, so
  // the line erase that follows would delete that cell, and Windows
  // consoles wrap at once. Leave the column free.
  if (options.tty && (width > 1 || viewport)) {
    --width;
  }
  auto visible = viewport ? viewport->tasks
                         : (options.all ? tasks : active_tasks(tasks));
  if (!viewport) {
    std::stable_sort(visible.begin(), visible.end(), [](const Task& a,
                                                      const Task& b) {
      if (a.status != b.status) {
        return a.status == TaskStatus::Active;
      }
      return a.created_at < b.created_at;
    });
  }
  std::ostringstream header;
  header << "taskglance | " << active_tasks(tasks).size()
         << " active | updated " << (last_change.time_since_epoch().count() == 0
           ? "unknown" : format_local_clock(last_change));
  std::string frame = truncate_display(header.str(), width);
  auto slots = static_cast<std::size_t>(height - 1);
  auto start = viewport ? std::min(viewport->first_row, visible.size()) : 0;
  auto remaining = visible.size() - start;
  bool summary = visible.size() > slots && slots > (viewport ? 1u : 0u);
  auto count = std::min(remaining, slots - (summary ? 1 : 0));
  WatchRowLayout layout;
  if (viewport) {
    // The viewport is interactive; height already excludes its status row.
    layout = layout_watch_viewport(tasks, *viewport,
                                   terminal_width, height);
    start = layout.first_row;
    count = layout.count;
    remaining = visible.size() - start;
    summary = layout.more > 0;
  }
  auto id_width = unique_id_width(tasks, 2, options.all);
  Config colors;
  colors.color = options.tty && options.color;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& task = visible[start + i];
    // Stored/imported data can contain controls; only our styles may emit
    // escape sequences, and truncation must happen before styling.
    bool selected = viewport && task.id == viewport->selected_id;
    auto label = terminal_safe_text(
      sanitize_task_text(task_label(task, id_width)));
    std::vector<std::string> lines{viewport
      ? truncate_watch_text(label, width) : truncate_display(label, width)};
    if (selected) {
      lines = selected_row_lines(task, id_width, width);
      if (lines.size() > layout.selected_lines) {
        lines.resize(layout.selected_lines);
        if (!lines.empty()) {
          // Measure the ellipsis: it is three columns where the locale is
          // not UTF-8 (each byte counts), so assuming one would overflow.
          static const std::string ellipsis = "…";
          auto room = width - display_width(ellipsis);
          lines.back() = room >= 0
            ? clip_watch_text(lines.back(), room) + ellipsis
            : clip_watch_text(lines.back(), width);
        }
      }
    }
    for (auto& line : lines) {
      line = color_task_ids(line, colors);
      if (colors.color) {
        // Colour the task text after the neutral ID; retain inverse selection.
        auto marker = line.find("\033[39m");
        auto tone = task.status == TaskStatus::Done ? "\033[32m" : "\033[36m";
        if (marker != std::string::npos) line.insert(marker + 5, tone);
        else line = std::string(tone) + line;
        line += "\033[0m";
      }
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
        if (selected) {
          line = "\033[7m" + line;
        }
        if (highlight || done || selected) {
          line += "\033[0m";
        }
      }
      frame += '\n' + line;
    }
  }
  if (summary && remaining > count) {
    frame += '\n' + truncate_display(
      "+" + std::to_string(remaining - count) + " more", width
    );
  }
  return frame;
}

}  // namespace taskglance
