#include "taskglance/legacy.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "taskglance/util.hpp"

namespace taskglance {

std::vector<Task> import_zsh_todo_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Could not read legacy file: " + path.string());
  }

  std::string first_line;
  std::getline(in, first_line);
  std::vector<Task> tasks;
  auto parts = split(first_line, '\0');
  for (const auto& part : parts) {
    auto text = sanitize_task_text(part);
    if (!text.empty()) {
      tasks.push_back(make_task(text, tasks));
    }
  }
  return tasks;
}

}  // namespace taskglance
