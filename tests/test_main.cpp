#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "taskglance/config.hpp"
#include "taskglance/legacy.hpp"
#include "taskglance/prompt.hpp"
#include "taskglance/render.hpp"
#include "taskglance/store.hpp"
#include "taskglance/util.hpp"

namespace fs = std::filesystem;
using namespace taskglance;

static int process_id() {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}

static fs::path temp_root() {
  auto root = fs::temp_directory_path() /
              ("taskglance-tests-" + std::to_string(process_id()));
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

static void test_percent_encoding_round_trips() {
  std::string input = "tab\tnewline\npercent% unicode \xE2\x9C\x93";
  assert(decode_field(encode_field(input)) == input);
}

static void test_task_persistence() {
  auto root = temp_root();
  auto file = root / "tasks.tsv";
  std::vector<Task> tasks;
  tasks.push_back(make_task("Fix auth", tasks));
  tasks.push_back(make_task("Review PR", tasks));
  save_tasks(file, tasks);

  auto loaded = load_tasks(file);
  assert(loaded.size() == 2);
  assert(loaded[0].text == "Fix auth");
  assert(loaded[1].status == TaskStatus::Active);
}

static void test_done_lookup_ambiguity() {
  std::vector<Task> tasks;
  tasks.push_back(make_task("Alpha", tasks));
  tasks.push_back(make_task("Beta", tasks));
  tasks[0].id = "abcd";
  tasks[1].id = "abef";

  std::string error;
  auto found = find_task_by_prefix(tasks, "ab", &error);
  assert(!found.has_value());
  assert(error.find("Ambiguous") != std::string::npos);
}

static void test_render_compact_limits_tasks() {
  Config config;
  config.max_prompt_tasks = 2;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  tasks.push_back(make_task("Two", tasks));
  tasks.push_back(make_task("Three", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  assert(output.find("One") != std::string::npos);
  assert(output.find("Two") != std::string::npos);
  assert(output.find("+1 more") != std::string::npos);
}

static void test_render_aligns_right_by_default() {
  Config config;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  assert(!output.empty());
  assert(output.front() == ' ');
  assert(output.size() == 80);
  assert(output.find("tasks: ") != std::string::npos);
}

static void test_render_aligns_long_content_to_right_block() {
  Config config;
  std::vector<Task> tasks;
  tasks.push_back(make_task(
    "This task is intentionally long enough to exceed the default block width",
    tasks
  ));
  tasks.push_back(make_task(
    "This second task keeps compact rendering longer than eighty columns",
    tasks
  ));

  auto output = render_tasks(tasks, config, 120, true);
  assert(output.size() == 120);
  assert(output.rfind(std::string(40, ' '), 0) == 0);
  assert(output.find("tasks: ") != std::string::npos);
}

static void test_render_box_limits_tasks() {
  Config config;
  config.display_style = "box";
  config.prompt_align = "left";
  config.max_prompt_tasks = 2;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  tasks.push_back(make_task("Two", tasks));
  tasks.push_back(make_task("Three", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  assert(output.find("┌") != std::string::npos);
  assert(output.find("└") != std::string::npos);
  assert(output.find("One") != std::string::npos);
  assert(output.find("Two") != std::string::npos);
  assert(output.find("Three") == std::string::npos);
  assert(output.find("+1 more") != std::string::npos);
}

static void test_render_colors_ids_with_muted_terminal_color() {
  Config config;
  config.color = true;
  config.prompt_align = "left";
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  tasks[0].id = "abc123";

  auto output = render_tasks(tasks, config, 80, true);
  assert(output.find("\033[90m[ab]\033[39m") != std::string::npos);
  assert(output.find("\033[90mOne") == std::string::npos);
}

static void test_render_ids_expand_until_unique() {
  Config config;
  config.prompt_align = "left";
  config.max_prompt_tasks = 3;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  tasks.push_back(make_task("Two", tasks));
  tasks.push_back(make_task("Three", tasks));
  tasks[0].id = "ab1111";
  tasks[1].id = "ab2222";
  tasks[2].id = "cd3333";

  auto output = render_tasks(tasks, config, 80, true);
  assert(output.find("[ab1]") != std::string::npos);
  assert(output.find("[ab2]") != std::string::npos);
  assert(output.find("[cd3]") != std::string::npos);
  assert(output.find("[ab]") == std::string::npos);
}

static void test_render_left_alignment_can_be_configured() {
  Config config;
  config.prompt_align = "left";
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  assert(output.rfind("tasks: ", 0) == 0);
}

static void test_prompt_state_change_detection() {
  auto root = temp_root();
  Paths paths;
  paths.state_dir = root;
  paths.prompt_state_file = root / "prompt.state";
  Config config;
  config.prompt_interval_seconds = 3600;

  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  assert(should_render_prompt(paths, config, tasks));
  update_prompt_state(paths, tasks);
  assert(!should_render_prompt(paths, config, tasks));
  tasks.push_back(make_task("Two", tasks));
  assert(should_render_prompt(paths, config, tasks));
}

static void test_config_validation() {
  assert(!validate_config_key_value("display_style", "compact").has_value());
  assert(validate_config_key_value("display_style", "sparkles").has_value());
  assert(!validate_config_key_value("prompt_align", "right").has_value());
  assert(validate_config_key_value("prompt_align", "center").has_value());
  assert(validate_config_key_value("unknown", "value").has_value());
}

static void test_legacy_import() {
  auto root = temp_root();
  auto file = root / "data.save";
  {
    std::ofstream out(file);
    const std::string tasks("First task\0Second task\n", 23);
    const std::string colors("\033[38;5;167m\0\033[38;5;71m\n", 27);
    out.write(tasks.data(), static_cast<std::streamsize>(tasks.size()));
    out.write(colors.data(), static_cast<std::streamsize>(colors.size()));
    out << "3\n";
  }

  auto imported = import_zsh_todo_file(file);
  assert(imported.size() == 2);
  assert(imported[0].text == "First task");
  assert(imported[1].text == "Second task");
}

static void test_hooks_include_alias() {
  auto zsh = hook_script("zsh", true, true);
  assert(zsh.find("alias tg=taskglance") != std::string::npos);
  assert(zsh.find("autoload -Uz _taskglance") != std::string::npos);
  assert(zsh.find("compdef _taskglance taskglance") != std::string::npos);
  assert(zsh.find("compdef _taskglance tg") != std::string::npos);
  assert(zsh.find("precmd") != std::string::npos);
  assert(zsh.find("prompt --force") != std::string::npos);
  auto normal = hook_script("zsh", false, false);
  assert(normal.find("zle -N accept-line") == std::string::npos);
}

static void test_zsh_completions_include_tg() {
  auto zsh = completion_script("zsh", true);
  assert(zsh.find("#compdef taskglance tg") != std::string::npos);
  assert(zsh.find("_arguments -C") != std::string::npos);
  assert(zsh.find("compdef _taskglance_completion") == std::string::npos);
  assert(zsh.find("'set:Set a configuration value'") != std::string::npos);
  assert(zsh.find("'prompt_align:Prompt alignment'") != std::string::npos);
  assert(zsh.find("prompt_aligns=(right left)") != std::string::npos);
}

int main() {
  test_percent_encoding_round_trips();
  test_task_persistence();
  test_done_lookup_ambiguity();
  test_render_compact_limits_tasks();
  test_render_aligns_right_by_default();
  test_render_aligns_long_content_to_right_block();
  test_render_box_limits_tasks();
  test_render_colors_ids_with_muted_terminal_color();
  test_render_ids_expand_until_unique();
  test_render_left_alignment_can_be_configured();
  test_prompt_state_change_detection();
  test_config_validation();
  test_legacy_import();
  test_hooks_include_alias();
  test_zsh_completions_include_tg();
  std::cout << "taskglance tests passed\n";
  return 0;
}
