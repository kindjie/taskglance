#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "taskglance/config.hpp"
#include "taskglance/store.hpp"

namespace taskglance {

bool should_render_prompt(const Paths& paths, const Config& config,
                          const std::vector<Task>& tasks);
void update_prompt_state(const Paths& paths, const std::vector<Task>& tasks);

std::string hook_script(const std::string& shell, bool alias_tg,
                        bool transient);
std::string completion_script(const std::string& shell, bool alias_tg);

}  // namespace taskglance
