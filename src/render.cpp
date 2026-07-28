#include "taskglance/render.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

#include "taskglance/util.hpp"

namespace taskglance {
namespace {

std::string task_label(const Task& task, std::size_t id_width) {
  return "[" + task.id.substr(0, id_width) + "] " + task.text;
}

std::size_t unique_id_width(const std::vector<Task>& tasks,
                            std::size_t minimum) {
  auto active = active_tasks(tasks);
  if (active.empty()) {
    return minimum;
  }

  std::size_t max_width = minimum;
  for (const auto& task : active) {
    max_width = std::max(max_width, task.id.size());
  }

  for (std::size_t width = std::min(minimum, max_width); width < max_width;
       ++width) {
    bool conflict = false;
    for (std::size_t i = 0; i < active.size(); ++i) {
      for (std::size_t j = i + 1; j < active.size(); ++j) {
        auto left = active[i].id.substr(0, width);
        auto right = active[j].id.substr(0, width);
        if (left == right) {
          conflict = true;
          break;
        }
      }
      if (conflict) {
        break;
      }
    }
    if (!conflict) {
      return width;
    }
  }
  return max_width;
}

std::optional<std::size_t> id_token_length(const std::string& value,
                                           std::size_t offset) {
  if (offset + 3 >= value.size() || value[offset] != '[') {
    return std::nullopt;
  }
  std::size_t cursor = offset + 1;
  for (; cursor < value.size() && value[cursor] != ']'; ++cursor) {
    if (!std::isxdigit(static_cast<unsigned char>(value[cursor]))) {
      return std::nullopt;
    }
  }
  if (cursor >= value.size() || cursor == offset + 1) {
    return std::nullopt;
  }
  return cursor - offset + 1;
}

std::string color_task_ids(const std::string& rendered,
                           const Config& config) {
  if (!config.color) {
    return rendered;
  }

  std::string output;
  for (std::size_t i = 0; i < rendered.size();) {
    if (auto length = id_token_length(rendered, i)) {
      output += "\033[90m";
      output += rendered.substr(i, *length);
      output += "\033[39m";
      i += *length;
      continue;
    }
    output += rendered[i];
    ++i;
  }
  return output;
}

std::string pad_display(std::string text, int width) {
  text = truncate_display(text, width);
  int padding = width - display_width(text);
  if (padding > 0) {
    text += std::string(static_cast<std::size_t>(padding), ' ');
  }
  return text;
}

std::string repeat_text(const std::string& text, int count) {
  std::string result;
  for (int i = 0; i < count; ++i) {
    result += text;
  }
  return result;
}

std::vector<std::string> limited_task_labels(const std::vector<Task>& tasks,
                                             const Config& config) {
  auto active = active_tasks(tasks);
  std::vector<std::string> labels;
  if (active.empty()) {
    return labels;
  }

  int limit = std::min<int>(config.max_prompt_tasks,
                            static_cast<int>(active.size()));
  auto id_width = unique_id_width(tasks, 2);
  for (int i = 0; i < limit; ++i) {
    labels.push_back(task_label(active[static_cast<std::size_t>(i)],
                                id_width));
  }
  if (static_cast<int>(active.size()) > limit) {
    labels.push_back("(+" +
                     std::to_string(static_cast<int>(active.size()) - limit) +
                     " more)");
  }
  return labels;
}

std::string render_compact(const std::vector<Task>& tasks,
                           const Config& config, int width) {
  auto chunks = limited_task_labels(tasks, config);
  if (chunks.empty()) {
    return "";
  }

  auto line = "tasks: " + join(chunks, "  ");
  return truncate_display(line, width);
}

std::string render_plain(const std::vector<Task>& tasks,
                         const Config& config, int width) {
  auto labels = limited_task_labels(tasks, config);
  std::ostringstream out;
  for (const auto& label : labels) {
    out << truncate_display(label, width) << '\n';
  }
  auto result = out.str();
  if (!result.empty()) {
    result.pop_back();
  }
  return result;
}

std::string render_box(const std::vector<Task>& tasks,
                       const Config& config, int width) {
  auto labels = limited_task_labels(tasks, config);
  if (labels.empty()) {
    return "";
  }
  int box_width = std::min(std::max(24, width), 80);
  int content_width = box_width - 4;
  std::ostringstream out;
  out << "┌" << repeat_text("─", box_width - 2) << "┐\n";
  out << "│ " << pad_display("TASKS", content_width) << " │\n";
  for (const auto& label : labels) {
    out << "│ " << pad_display(label, content_width) << " │\n";
  }
  out << "└" << repeat_text("─", box_width - 2) << "┘";
  return out.str();
}

std::string align_rendered(const std::string& rendered, const Config& config,
                           int terminal_width) {
  if (config.prompt_align != "right" || terminal_width <= 0 ||
      rendered.empty()) {
    return rendered;
  }

  std::istringstream in(rendered);
  std::ostringstream out;
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!first) {
      out << '\n';
    }
    first = false;
    int padding = terminal_width - display_width(line);
    if (padding > 0) {
      out << std::string(static_cast<std::size_t>(padding), ' ');
    }
    out << line;
  }
  return out.str();
}

}  // namespace

std::string render_tasks(const std::vector<Task>& tasks, const Config& config,
                         int terminal_width, bool tty) {
  if (!config.prompt_enabled || !tty) {
    return "";
  }
  if (tasks.empty() || active_tasks(tasks).empty()) {
    return "";
  }
  int width = terminal_width;
  if (config.max_prompt_width > 0) {
    width = std::min(config.max_prompt_width, terminal_width);
  } else if (config.prompt_align == "right") {
    width = std::min(80, terminal_width);
  }
  width = std::max(20, width);

  std::string rendered;
  if (config.display_style == "plain") {
    rendered = render_plain(tasks, config, width);
  } else if (config.display_style == "box") {
    rendered = render_box(tasks, config, width);
  } else {
    rendered = render_compact(tasks, config, width);
  }
  return color_task_ids(align_rendered(rendered, config, terminal_width),
                        config);
}

}  // namespace taskglance
