#pragma once

#include <string>
#include <vector>

#include "taskglance/config.hpp"
#include "taskglance/store.hpp"

namespace taskglance {

std::string task_label(const Task& task, std::size_t id_width);
std::size_t unique_id_width(const std::vector<Task>& tasks,
                            std::size_t minimum, bool include_done = false);
std::string color_task_ids(const std::string& rendered,
                           const Config& config);

std::string render_tasks(const std::vector<Task>& tasks, const Config& config,
                         int terminal_width, bool tty);

}  // namespace taskglance
