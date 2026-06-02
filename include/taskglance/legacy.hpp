#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "taskglance/store.hpp"

namespace taskglance {

std::vector<Task> import_zsh_todo_file(const std::filesystem::path& path);

}  // namespace taskglance
