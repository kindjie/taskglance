#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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

// Deliberately not assert(): NDEBUG, which CMake defines for Release and
// MinSizeRel builds, expands assert() to nothing and would leave the suite
// passing unconditionally.
#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::cerr << "CHECK failed: " #condition "\n  at " << __FILE__  \
                << ':' << __LINE__ << '\n';                           \
      std::exit(1);                                                   \
    }                                                                 \
  } while (false)

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
  CHECK(decode_field(encode_field(input)) == input);
}

static void test_task_persistence() {
  auto root = temp_root();
  auto file = root / "tasks.tsv";
  std::vector<Task> tasks;
  tasks.push_back(make_task("Fix auth", tasks));
  tasks.push_back(make_task("Review PR", tasks));
  save_tasks(file, tasks);

  auto loaded = load_tasks(file);
  CHECK(loaded.size() == 2);
  CHECK(loaded[0].text == "Fix auth");
  CHECK(loaded[1].status == TaskStatus::Active);
  auto snapshot = read_task_file(file);
  auto parsed = parse_tasks(snapshot);
  CHECK(parsed.size() == loaded.size());
  CHECK(parsed[0].id == loaded[0].id);
  CHECK(parsed[0].text == loaded[0].text);
  CHECK(parsed[0].created_at == loaded[0].created_at);
  CHECK(parsed[1].status == loaded[1].status);
  CHECK(read_task_file(root / "missing.tsv").empty());
  CHECK(parse_tasks("").empty());
}

static void test_task_ids_differ_across_lifetimes() {
  const std::vector<Task> existing;
  auto created_at = time_from_iso("1234567890");
  auto first = make_task("Identical", existing, created_at);
  auto replacement = make_task("Identical", existing, created_at);
  CHECK(first.text == replacement.text);
  CHECK(first.created_at == replacement.created_at);
  CHECK(first.id != replacement.id);
  for (const auto& id : {first.id, replacement.id}) {
    CHECK(id.size() == 6);
    CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
  }
}

static void test_concurrent_updates_keep_every_change() {
  auto root = temp_root();
  auto file = root / "tasks.tsv";
  constexpr int kWriters = 8;
  constexpr int kAddsPerWriter = 25;

  // Each writer opens its own lock handle, so the lock excludes threads in
  // one process just as it excludes separate taskglance processes.
  std::vector<std::thread> writers;
  for (int writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([&file, writer] {
      for (int add = 0; add < kAddsPerWriter; ++add) {
        update_tasks(file, [&](std::vector<Task>& tasks) {
          auto text = "w" + std::to_string(writer) + " a" +
                      std::to_string(add);
          tasks.push_back(make_task(text, tasks));
          return true;
        });
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }

  CHECK(load_tasks(file).size() == kWriters * kAddsPerWriter);
}

static void test_cancellable_lock_updates() {
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  auto file = temp_root() / "tasks.tsv";
  update_tasks(file, [](auto& tasks) {
    tasks.push_back(make_task("Keep me", tasks));
    return true;
  });
  auto before = read_task_file(file);
  auto held = std::make_unique<FileLock>(file.string() + ".lock");
  bool called = false;
  auto start = Clock::now();
  int polls = 0;
  CHECK(!update_tasks(file, [&](auto& tasks) {
    called = true;
    tasks.clear();
    return true;
  }, [&] {
    ++polls;
    return Clock::now() - start >= 60ms;
  }));
  CHECK(Clock::now() - start < 1s);
  CHECK(polls > 1);
  CHECK(!called);
  CHECK(read_task_file(file) == before);
  {
    FileLock cancelled(file.string() + ".lock", [] { return true; });
    CHECK(!cancelled.acquired());
  }

  // Releasing a contended lock allows exactly one change to proceed.
  std::atomic<int> attempts{0};
  bool completed = false;
  std::thread writer([&] {
    auto deadline = Clock::now() + 2s;
    completed = update_tasks(file, [&](auto& tasks) {
      called = true;
      tasks.push_back(make_task("After release", tasks));
      return true;
    }, [&] {
      ++attempts;
      return Clock::now() >= deadline;
    });
  });
  auto deadline = Clock::now() + 1s;
  while (attempts < 3 && Clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(attempts >= 3);
  held.reset();
  writer.join();
  CHECK(completed && called);
  CHECK(load_tasks(file).size() == 2);

  // Cancellation is checked again after acquiring and loading, so a stop
  // requested as the lock becomes available cannot run the change.
  called = false;
  polls = 0;
  before = read_task_file(file);
  CHECK(!update_tasks(file, [&](auto&) {
    called = true;
    return true;
  }, [&] { return ++polls == 2; }));
  CHECK(!called);
  CHECK(read_task_file(file) == before);

  // A throwing caller predicate must release its open lock handle.
  bool threw = false;
  try {
    FileLock lock(file.string() + ".lock", []() -> bool {
      throw std::runtime_error("cancel failed");
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  FileLock lock(file.string() + ".lock", [] { return false; });
  CHECK(lock.acquired());
}

static void test_declined_update_leaves_file_untouched() {
  auto root = temp_root();
  auto file = root / "tasks.tsv";
  update_tasks(file, [](std::vector<Task>& tasks) {
    tasks.push_back(make_task("Keep me", tasks));
    return true;
  });
  auto before = fs::last_write_time(file);

  update_tasks(file, [](std::vector<Task>& tasks) {
    tasks.clear();
    return false;
  });

  CHECK(fs::last_write_time(file) == before);
  CHECK(load_tasks(file).size() == 1);
}

static void test_done_lookup_ambiguity() {
  std::vector<Task> tasks;
  tasks.push_back(make_task("Alpha", tasks));
  tasks.push_back(make_task("Beta", tasks));
  tasks[0].id = "abcd";
  tasks[1].id = "abef";

  std::string error;
  auto found = find_task_by_prefix(tasks, "ab", &error);
  CHECK(!found.has_value());
  CHECK(error.find("Ambiguous") != std::string::npos);
}

static void test_render_compact_limits_tasks() {
  Config config;
  config.max_prompt_tasks = 2;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  tasks.push_back(make_task("Two", tasks));
  tasks.push_back(make_task("Three", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  CHECK(output.find("One") != std::string::npos);
  CHECK(output.find("Two") != std::string::npos);
  CHECK(output.find("+1 more") != std::string::npos);
}

static void test_render_aligns_right_by_default() {
  Config config;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  CHECK(!output.empty());
  CHECK(output.front() == ' ');
  CHECK(output.size() == 80);
  CHECK(output.find("tasks: ") != std::string::npos);
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
  CHECK(output.size() == 120);
  CHECK(output.rfind(std::string(40, ' '), 0) == 0);
  CHECK(output.find("tasks: ") != std::string::npos);
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
  CHECK(output.find("┌") != std::string::npos);
  CHECK(output.find("└") != std::string::npos);
  CHECK(output.find("One") != std::string::npos);
  CHECK(output.find("Two") != std::string::npos);
  CHECK(output.find("Three") == std::string::npos);
  CHECK(output.find("+1 more") != std::string::npos);
}

static void test_render_colors_ids_with_muted_terminal_color() {
  Config config;
  config.color = true;
  config.prompt_align = "left";
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  tasks[0].id = "abc123";

  auto output = render_tasks(tasks, config, 80, true);
  CHECK(output.find("\033[90m[ab]\033[39m") != std::string::npos);
  CHECK(output.find("\033[90mOne") == std::string::npos);
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
  CHECK(output.find("[ab1]") != std::string::npos);
  CHECK(output.find("[ab2]") != std::string::npos);
  CHECK(output.find("[cd3]") != std::string::npos);
  CHECK(output.find("[ab]") == std::string::npos);
}

static void test_render_left_alignment_can_be_configured() {
  Config config;
  config.prompt_align = "left";
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));

  auto output = render_tasks(tasks, config, 80, true);
  CHECK(output.rfind("tasks: ", 0) == 0);
}

static void test_render_disabled_returns_nothing() {
  Config config;
  config.prompt_enabled = false;
  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));

  CHECK(render_tasks(tasks, config, 80, true).empty());
  config.prompt_enabled = true;
  CHECK(!render_tasks(tasks, config, 80, true).empty());
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
  CHECK(should_render_prompt(paths, config, tasks));
  update_prompt_state(paths, tasks);
  CHECK(!should_render_prompt(paths, config, tasks));
  tasks.push_back(make_task("Two", tasks));
  CHECK(should_render_prompt(paths, config, tasks));
}

static void test_prompt_disabled_skips_rendering() {
  auto root = temp_root();
  Paths paths;
  paths.state_dir = root;
  paths.prompt_state_file = root / "prompt.state";
  Config config;
  config.prompt_enabled = false;

  std::vector<Task> tasks;
  tasks.push_back(make_task("One", tasks));
  CHECK(!should_render_prompt(paths, config, tasks));
  config.prompt_enabled = true;
  CHECK(should_render_prompt(paths, config, tasks));
}

static void test_config_validation() {
  CHECK(!validate_config_key_value("display_style", "compact").has_value());
  CHECK(validate_config_key_value("display_style", "sparkles").has_value());
  CHECK(!validate_config_key_value("prompt_align", "right").has_value());
  CHECK(validate_config_key_value("prompt_align", "center").has_value());
  CHECK(!validate_config_key_value("prompt_enabled", "false").has_value());
  CHECK(validate_config_key_value("prompt_enabled", "maybe").has_value());
  CHECK(validate_config_key_value("unknown", "value").has_value());
}

static void test_prompt_enabled_round_trips_through_config() {
  auto root = temp_root();
  Paths paths;
  paths.config_dir = root;
  paths.config_file = root / "config";

  Config config;
  CHECK(config.prompt_enabled);
  CHECK(config_to_map(config).at("prompt_enabled") == "true");

  set_config_value(config, "prompt_enabled", "off");
  CHECK(!config.prompt_enabled);
  save_config(paths, config);
  CHECK(!load_config(paths).prompt_enabled);
}

static void test_legacy_import() {
  auto root = temp_root();
  auto file = root / "data.save";
  {
    std::ofstream out(file);
    constexpr char tasks[] = "First task\0Second task\n";
    constexpr char colors[] = "\033[38;5;167m\0\033[38;5;71m\n";
    out.write(tasks, sizeof(tasks) - 1);
    out.write(colors, sizeof(colors) - 1);
    out << "3\n";
  }

  auto imported = import_zsh_todo_file(file);
  CHECK(imported.size() == 2);
  CHECK(imported[0].text == "First task");
  CHECK(imported[1].text == "Second task");
}

static void test_hooks_include_alias() {
  auto zsh = hook_script("zsh", true, true);
  CHECK(zsh.find("alias tg=taskglance") != std::string::npos);
  CHECK(zsh.find("autoload -Uz _taskglance") != std::string::npos);
  CHECK(zsh.find("compdef _taskglance taskglance") != std::string::npos);
  CHECK(zsh.find("compdef _taskglance tg") != std::string::npos);
  CHECK(zsh.find("precmd") != std::string::npos);
  CHECK(zsh.find("prompt --force") != std::string::npos);
  auto normal = hook_script("zsh", false, false);
  CHECK(normal.find("zle -N accept-line") == std::string::npos);
}

static void test_zsh_completions_include_tg() {
  auto zsh = completion_script("zsh", true);
  CHECK(zsh.find("#compdef taskglance tg") != std::string::npos);
  CHECK(zsh.find("_arguments -C") != std::string::npos);
  CHECK(zsh.find("compdef _taskglance_completion") == std::string::npos);
  CHECK(zsh.find("'set:Set a configuration value'") != std::string::npos);
  CHECK(zsh.find("'prompt_align:Prompt alignment'") != std::string::npos);
  CHECK(zsh.find("prompt_aligns=(right left)") != std::string::npos);
  CHECK(zsh.find("'enable:") != std::string::npos);
  CHECK(zsh.find("'disable:") != std::string::npos);
  CHECK(zsh.find("'prompt_enabled:") != std::string::npos);
  CHECK(zsh.find("config:set:prompt_enabled") != std::string::npos);
  CHECK(zsh.find("'watch:") != std::string::npos);
  CHECK(zsh.find("--interval") != std::string::npos);
  CHECK(zsh.find("--all") != std::string::npos);
}

static void test_watch_completions_in_all_shells() {
  for (const auto* shell : {"zsh", "bash", "fish"}) {
    auto completion = completion_script(shell, false);
    CHECK(completion.find("watch") != std::string::npos);
    CHECK(completion.find("interval") != std::string::npos);
    CHECK(completion.find("all") != std::string::npos);
    CHECK(completion.find("interactive") != std::string::npos);
  }
  auto bash = completion_script("bash", true);
  CHECK(bash.find("_tg_complete") != std::string::npos);
  CHECK(bash.find("COMP_WORDS[COMP_CWORD]") != std::string::npos);
  CHECK(bash.find("COMP_WORDS[COMP_CWORD-1]") != std::string::npos);
  CHECK(bash.find("--interactive -i") != std::string::npos);
  auto zsh = completion_script("zsh", false);
  CHECK(zsh.find("(-i)--interactive") != std::string::npos);
  CHECK(zsh.find("(--interactive)-i") != std::string::npos);
  auto fish = completion_script("fish", false);
  CHECK(fish.find("__fish_seen_subcommand_from watch") != std::string::npos);
  CHECK(fish.find("-l interval -r") != std::string::npos);
  CHECK(fish.find("-l interactive -s i") != std::string::npos);
}

int main() {
  test_percent_encoding_round_trips();
  test_task_persistence();
  test_task_ids_differ_across_lifetimes();
  test_concurrent_updates_keep_every_change();
  test_cancellable_lock_updates();
  test_declined_update_leaves_file_untouched();
  test_done_lookup_ambiguity();
  test_render_compact_limits_tasks();
  test_render_aligns_right_by_default();
  test_render_aligns_long_content_to_right_block();
  test_render_box_limits_tasks();
  test_render_colors_ids_with_muted_terminal_color();
  test_render_ids_expand_until_unique();
  test_render_left_alignment_can_be_configured();
  test_render_disabled_returns_nothing();
  test_prompt_state_change_detection();
  test_prompt_disabled_skips_rendering();
  test_config_validation();
  test_prompt_enabled_round_trips_through_config();
  test_legacy_import();
  test_hooks_include_alias();
  test_zsh_completions_include_tg();
  test_watch_completions_in_all_shells();
  std::cout << "taskglance tests passed\n";
  return 0;
}
