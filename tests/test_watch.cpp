#include <cstdlib>
#include <iostream>
#include <sstream>

#include "taskglance/util.hpp"
#include "taskglance/watch.hpp"

using namespace taskglance;
using namespace std::chrono_literals;

#define CHECK(condition)                                               \
  do {                                                                 \
    if (!(condition)) {                                                \
      std::cerr << "CHECK failed: " #condition " at " << __FILE__        \
                << ':' << __LINE__ << '\n';                            \
      std::exit(1);                                                    \
    }                                                                  \
  } while (false)

static Task task(std::string id, std::string text, int age = 0) {
  return {id, TaskStatus::Active,
          std::chrono::system_clock::time_point{1s * age}, {}, text};
}

static std::string frame(const std::vector<Task>& tasks,
                         WatchOptions options = {}, int width = 80,
                         int height = 24, double seconds = 0) {
  return build_watch_frame(tasks, {"ab1111", "ab2222"}, width, height,
                           std::chrono::system_clock::time_point{3661s},
                           std::chrono::duration<double>{seconds}, options);
}

static void test_changes() {
  std::vector<Task> previous{task("ab1111", "First"),
                             task("ab2222", "Second")};
  auto changes = detect_task_changes(previous, previous);
  CHECK(changes.changed_ids.empty());
  CHECK(changes.deleted_ids.empty());
  auto tasks = previous;
  tasks.push_back(task("cd3333", "Third"));
  changes = detect_task_changes(tasks, previous);
  CHECK(changes.changed_ids == std::vector<std::string>{"cd3333"});
  tasks = previous;
  tasks[0].text = "Edited";
  tasks[1].status = TaskStatus::Done;
  changes = detect_task_changes(tasks, previous);
  CHECK((changes.changed_ids ==
         std::vector<std::string>{"ab1111", "ab2222"}));
  tasks = {previous[1]};
  changes = detect_task_changes(tasks, previous);
  CHECK(changes.changed_ids.empty());
  CHECK(changes.deleted_ids == std::vector<std::string>{"ab1111"});
  CHECK(detect_task_changes({}, {}).deleted_ids.empty());
}

static void test_frames() {
  std::vector<Task> tasks{task("ab2222", "Newer", 2),
                          task("ab1111", "Oldest", 1),
                          task("cd3333", "Finished", 0)};
  tasks[2].status = TaskStatus::Done;
  auto plain = frame(tasks);
  auto clock = format_local_clock(std::chrono::system_clock::time_point{
    3661s
  });
  CHECK(clock.size() == 8 && clock[2] == ':' && clock[5] == ':');
  CHECK(plain.find("taskglance | 2 active | changed " + clock) == 0);
  CHECK(plain.find("[ab1] Oldest") < plain.find("[ab2] Newer"));
  CHECK(plain.find("Finished") == std::string::npos);
  CHECK(plain.find('\033') == std::string::npos);
  CHECK(frame({}).find("0 active") != std::string::npos);

  WatchOptions options;
  options.all = true;
  options.tty = true;
  options.color = false;
  auto all = frame(tasks, options);
  CHECK(all.find("\033[2m[cd3] Finished\033[0m") != std::string::npos);
  CHECK(all.find("Finished") > all.find("Newer"));
  CHECK(all.find("\033[1m[ab1] Oldest") != std::string::npos);
  CHECK(frame(tasks, options, 80, 24, 9.999).find("\033[1m") !=
        std::string::npos);
  CHECK(frame(tasks, options, 80, 24, 10).find("\033[1m") ==
        std::string::npos);
  CHECK(frame(tasks, options, 80, 24, 11).find("\033[1m") ==
        std::string::npos);
  CHECK(all.find("\033[90m") == std::string::npos);
  options.color = true;
  CHECK(frame(tasks, options).find("\033[90m[ab1]\033[39m") !=
        std::string::npos);
  options.tty = false;
  CHECK(frame(tasks, options).find('\033') == std::string::npos);

  tasks.push_back(task("ef4444", std::string(100, 'x'), 3));
  for (int width : {1, 2, 3, 8, 20, 80}) {
    std::istringstream in(frame(tasks, {}, width));
    std::string line;
    while (std::getline(in, line)) {
      CHECK(display_width(line) <= width);
    }
  }
  auto limited = frame(tasks, {}, 80, 3);
  CHECK(limited.find("Oldest") != std::string::npos);
  CHECK(limited.find("Newer") == std::string::npos);
  CHECK(limited.find("+2 more") != std::string::npos);
  CHECK(frame(tasks, {}, 80, 2).find("+3 more") != std::string::npos);
  CHECK(frame(tasks, {}, 80, 1).find('\n') == std::string::npos);
  CHECK(frame(tasks, {}, 80, 0).empty());
  CHECK(frame(tasks, {}, 0).empty());
  tasks[0].text = "Bad\033[31m\nText";
  CHECK(frame(tasks).find('\033') == std::string::npos);

  // Completed tasks participate in prefix uniqueness when displayed.
  tasks = {task("ab1111", "Active"), task("ab1222", "Done")};
  tasks[1].status = TaskStatus::Done;
  CHECK(frame(tasks, options).find("[ab11] Active") != std::string::npos);
  CHECK(frame(tasks, options).find("[ab12] Done") != std::string::npos);
}

static void test_intervals() {
  CHECK(parse_watch_interval("1") == 1.0);
  CHECK(parse_watch_interval("0.25") == 0.25);
  CHECK(parse_watch_interval("1e-2") == 0.01);
  for (const auto* value : {"", "0", "-1", "no", "1x", "nan", "inf",
                             "1e999", " 1", "1 "}) {
    CHECK(!parse_watch_interval(value));
  }
}

int main() {
  test_changes();
  test_frames();
  test_intervals();
  std::cout << "watch tests passed\n";
}
