#include "taskglance/store.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "taskglance/util.hpp"

namespace taskglance {
namespace {

std::string status_to_string(TaskStatus status) {
  return status == TaskStatus::Done ? "done" : "active";
}

TaskStatus status_from_string(const std::string& value) {
  return value == "done" ? TaskStatus::Done : TaskStatus::Active;
}

bool id_exists(const std::vector<Task>& tasks, const std::string& id) {
  return std::any_of(tasks.begin(), tasks.end(), [&](const Task& task) {
    return task.id == id;
  });
}

}  // namespace

std::string encode_field(const std::string& value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (unsigned char ch : value) {
    if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r') {
      out << '%' << std::setw(2) << std::setfill('0')
          << static_cast<int>(ch);
    } else {
      out << static_cast<char>(ch);
    }
  }
  return out.str();
}

std::string decode_field(const std::string& value) {
  std::string output;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      auto hex = value.substr(i + 1, 2);
      char* end = nullptr;
      long parsed = std::strtol(hex.c_str(), &end, 16);
      if (end != nullptr && *end == '\0') {
        output.push_back(static_cast<char>(parsed));
        i += 2;
        continue;
      }
    }
    output.push_back(value[i]);
  }
  return output;
}

std::string sanitize_task_text(const std::string& text) {
  std::string output;
  for (unsigned char ch : text) {
    if (ch == '\t' || ch == '\n' || ch == '\r') {
      output.push_back(' ');
    } else if (ch >= 0x20 || ch >= 0x80) {
      output.push_back(static_cast<char>(ch));
    }
  }
  output = trim(output);
  while (output.find("  ") != std::string::npos) {
    auto position = output.find("  ");
    output.replace(position, 2, " ");
  }
  if (output.size() > 500) {
    output = output.substr(0, 497) + "...";
  }
  return output;
}

std::vector<Task> load_tasks(const std::filesystem::path& file) {
  std::vector<Task> tasks;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return tasks;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (trim(line).empty()) {
      continue;
    }
    auto fields = split(line, '\t');
    if (fields.size() < 5) {
      continue;
    }
    Task task;
    task.id = fields[0];
    task.status = status_from_string(fields[1]);
    task.created_at = time_from_iso(fields[2]);
    if (!fields[3].empty() && fields[3] != "-") {
      task.completed_at = time_from_iso(fields[3]);
    }
    task.text = decode_field(fields[4]);
    tasks.push_back(task);
  }
  return tasks;
}

void save_tasks(const std::filesystem::path& file,
                const std::vector<Task>& tasks) {
  std::ostringstream out;
  for (const auto& task : tasks) {
    out << task.id << '\t'
        << status_to_string(task.status) << '\t'
        << time_to_iso(task.created_at) << '\t'
        << (task.completed_at ? time_to_iso(*task.completed_at) : "-")
        << '\t'
        << encode_field(task.text) << '\n';
  }
  atomic_write_file(file, out.str());
}

void update_tasks(
  const std::filesystem::path& file,
  const std::function<bool(std::vector<Task>&)>& change
) {
  auto lock_file = file;
  lock_file += ".lock";
  FileLock lock(lock_file);
  auto tasks = load_tasks(file);
  if (change(tasks)) {
    save_tasks(file, tasks);
  }
}

Task make_task(const std::string& text, const std::vector<Task>& existing) {
  Task task;
  task.status = TaskStatus::Active;
  task.created_at = std::chrono::system_clock::now();
  task.text = sanitize_task_text(text);
  if (task.text.empty()) {
    throw std::runtime_error("Task text cannot be empty");
  }

  auto seed = task.text + time_to_iso(task.created_at) +
              std::to_string(existing.size());
  auto hash = fnv1a64(seed);
  task.id = hex_short(hash, 6);
  int salt = 0;
  while (id_exists(existing, task.id)) {
    task.id = hex_short(hash + static_cast<std::uint64_t>(++salt), 6);
  }
  return task;
}

std::vector<Task> active_tasks(const std::vector<Task>& tasks) {
  std::vector<Task> active;
  for (const auto& task : tasks) {
    if (task.status == TaskStatus::Active) {
      active.push_back(task);
    }
  }
  return active;
}

std::optional<std::size_t> find_task_by_prefix(
  const std::vector<Task>& tasks,
  const std::string& id_or_prefix,
  std::string* error
) {
  std::vector<std::size_t> matches;
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (tasks[i].id.rfind(id_or_prefix, 0) == 0) {
      matches.push_back(i);
    }
  }
  if (matches.empty()) {
    if (error != nullptr) {
      *error = "No task matches id prefix: " + id_or_prefix;
    }
    return std::nullopt;
  }
  if (matches.size() > 1) {
    if (error != nullptr) {
      std::vector<std::string> ids;
      for (auto index : matches) {
        ids.push_back(tasks[index].id);
      }
      *error = "Ambiguous task id prefix: " + id_or_prefix +
               " matches " + join(ids, ", ");
    }
    return std::nullopt;
  }
  return matches[0];
}

}  // namespace taskglance
