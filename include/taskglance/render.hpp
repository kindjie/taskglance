#pragma once

#include <string>
#include <vector>

#include "taskglance/config.hpp"
#include "taskglance/store.hpp"

namespace taskglance {

std::string render_tasks(const std::vector<Task>& tasks, const Config& config,
                         int terminal_width, bool tty);

}  // namespace taskglance
