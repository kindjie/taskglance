#include <chrono>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "taskglance/config.hpp"
#include "taskglance/legacy.hpp"
#include "taskglance/prompt.hpp"
#include "taskglance/render.hpp"
#include "taskglance/store.hpp"
#include "taskglance/util.hpp"

namespace {

using taskglance::Config;
using taskglance::Paths;
using taskglance::TaskStatus;

std::string usage() {
  return R"(taskglance - terminal task reminders

Usage:
  taskglance add <text>
  taskglance list
  taskglance done <id-prefix>
  taskglance delete <id-prefix>
  taskglance edit <id-prefix> <text>
  taskglance clear --done
  taskglance prompt
  taskglance import zsh-todo-reminder [path]
  taskglance config get|set|list|reset
  taskglance completions zsh|bash|fish [--alias tg]
  taskglance hooks zsh|bash|fish [--alias tg] [--transient]
)";
}

std::string rest(int start, int argc, char** argv) {
  std::string text;
  for (int i = start; i < argc; ++i) {
    if (!text.empty()) {
      text += ' ';
    }
    text += argv[i];
  }
  return text;
}

bool has_alias_flag(int argc, char** argv) {
  for (int i = 0; i < argc; ++i) {
    if (std::string(argv[i]) == "--alias" && i + 1 < argc &&
        std::string(argv[i + 1]) == "tg") {
      return true;
    }
  }
  return false;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 0; i < argc; ++i) {
    if (std::string(argv[i]) == flag) {
      return true;
    }
  }
  return false;
}

void print_tasks(const std::vector<taskglance::Task>& tasks) {
  for (const auto& task : tasks) {
    std::cout << task.id << "  "
              << (task.status == TaskStatus::Done ? "done  " : "active")
              << "  " << task.text << '\n';
  }
}

std::filesystem::path default_legacy_path() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return ".todo.save";
  }
  auto config_path = std::filesystem::path(home) /
                     ".config" / "todo-reminder" / "data.save";
  if (std::filesystem::exists(config_path)) {
    return config_path;
  }
  return std::filesystem::path(home) / ".todo.save";
}

int run(int argc, char** argv) {
  if (argc < 2) {
    std::cout << usage();
    return 0;
  }

  Paths paths = taskglance::default_paths();
  Config config = taskglance::load_config(paths);
  std::string command = argv[1];

  if (command == "help" || command == "--help" || command == "-h") {
    std::cout << usage();
    return 0;
  }

  if (command == "add") {
    auto text = rest(2, argc, argv);
    auto tasks = taskglance::load_tasks(paths.tasks_file);
    auto task = taskglance::make_task(text, tasks);
    tasks.push_back(task);
    taskglance::save_tasks(paths.tasks_file, tasks);
    std::cout << "Added [" << task.id << "] " << task.text << '\n';
    return 0;
  }

  if (command == "list") {
    print_tasks(taskglance::load_tasks(paths.tasks_file));
    return 0;
  }

  if (command == "done" || command == "delete") {
    if (argc < 3) {
      std::cerr << command << " requires an id prefix\n";
      return 2;
    }
    auto tasks = taskglance::load_tasks(paths.tasks_file);
    std::string error;
    auto index = taskglance::find_task_by_prefix(tasks, argv[2], &error);
    if (!index) {
      std::cerr << error << '\n';
      return 1;
    }
    auto& task = tasks[*index];
    if (command == "done") {
      task.status = TaskStatus::Done;
      task.completed_at = std::chrono::system_clock::now();
      std::cout << "Done [" << task.id << "] " << task.text << '\n';
    } else {
      std::cout << "Deleted [" << task.id << "] " << task.text << '\n';
      tasks.erase(tasks.begin() + static_cast<long>(*index));
    }
    taskglance::save_tasks(paths.tasks_file, tasks);
    return 0;
  }

  if (command == "edit") {
    if (argc < 4) {
      std::cerr << "edit requires an id prefix and text\n";
      return 2;
    }
    auto tasks = taskglance::load_tasks(paths.tasks_file);
    std::string error;
    auto index = taskglance::find_task_by_prefix(tasks, argv[2], &error);
    if (!index) {
      std::cerr << error << '\n';
      return 1;
    }
    tasks[*index].text = taskglance::sanitize_task_text(rest(3, argc, argv));
    taskglance::save_tasks(paths.tasks_file, tasks);
    std::cout << "Updated [" << tasks[*index].id << "] "
              << tasks[*index].text << '\n';
    return 0;
  }

  if (command == "clear") {
    if (argc < 3 || std::string(argv[2]) != "--done") {
      std::cerr << "clear currently supports only --done\n";
      return 2;
    }
    auto tasks = taskglance::load_tasks(paths.tasks_file);
    auto before = tasks.size();
    tasks.erase(
      std::remove_if(tasks.begin(), tasks.end(), [](const auto& task) {
        return task.status == TaskStatus::Done;
      }),
      tasks.end()
    );
    taskglance::save_tasks(paths.tasks_file, tasks);
    std::cout << "Cleared " << (before - tasks.size()) << " done tasks\n";
    return 0;
  }

  if (command == "prompt") {
    bool force = argc >= 3 && std::string(argv[2]) == "--force";
    auto tasks = taskglance::load_tasks(paths.tasks_file);
    if (!force && !taskglance::should_render_prompt(paths, config, tasks)) {
      return 0;
    }
    auto output = taskglance::render_tasks(
      tasks, config, taskglance::terminal_columns(),
      taskglance::stdout_is_tty()
    );
    if (!output.empty()) {
      std::cout << output << '\n';
      if (!force) {
        taskglance::update_prompt_state(paths, tasks);
      }
    }
    return 0;
  }

  if (command == "import") {
    if (argc < 3 || std::string(argv[2]) != "zsh-todo-reminder") {
      std::cerr << "Supported import: zsh-todo-reminder [path]\n";
      return 2;
    }
    auto path = argc >= 4 ? std::filesystem::path(argv[3])
                          : default_legacy_path();
    auto imported = taskglance::import_zsh_todo_file(path);
    auto tasks = taskglance::load_tasks(paths.tasks_file);
    for (auto& task : imported) {
      task.id = taskglance::make_task(task.text, tasks).id;
      tasks.push_back(task);
    }
    taskglance::save_tasks(paths.tasks_file, tasks);
    std::cout << "Imported " << imported.size() << " tasks from "
              << path << '\n';
    return 0;
  }

  if (command == "config") {
    if (argc < 3 || std::string(argv[2]) == "list") {
      for (const auto& [key, value] : taskglance::config_to_map(config)) {
        std::cout << key << '=' << value << '\n';
      }
      return 0;
    }
    std::string action = argv[2];
    if (action == "get") {
      if (argc < 4) {
        std::cerr << "config get requires a key\n";
        return 2;
      }
      auto values = taskglance::config_to_map(config);
      auto found = values.find(argv[3]);
      if (found == values.end()) {
        std::cerr << "Unknown config key: " << argv[3] << '\n';
        return 1;
      }
      std::cout << found->second << '\n';
      return 0;
    }
    if (action == "set") {
      if (argc < 5) {
        std::cerr << "config set requires a key and value\n";
        return 2;
      }
      taskglance::set_config_value(config, argv[3], argv[4]);
      taskglance::save_config(paths, config);
      return 0;
    }
    if (action == "reset") {
      taskglance::save_config(paths, Config{});
      return 0;
    }
    std::cerr << "Unknown config action: " << action << '\n';
    return 2;
  }

  if (command == "hooks") {
    if (argc < 3) {
      std::cerr << "hooks requires zsh, bash, or fish\n";
      return 2;
    }
    std::cout << taskglance::hook_script(
      argv[2], has_alias_flag(argc, argv), has_flag(argc, argv, "--transient")
    );
    return 0;
  }

  if (command == "completions") {
    if (argc < 3) {
      std::cerr << "completions requires zsh, bash, or fish\n";
      return 2;
    }
    std::cout << taskglance::completion_script(argv[2],
                                               has_alias_flag(argc, argv));
    return 0;
  }

  std::cerr << "Unknown command: " << command << "\n\n" << usage();
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "taskglance: " << error.what() << '\n';
    return 1;
  }
}
