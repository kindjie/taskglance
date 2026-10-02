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
  CHECK(plain.find("My Tasks · 2 active · updated " + clock) == 0);
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
  CHECK(frame(tasks, options).find("\033[36m") != std::string::npos);
  CHECK(frame(tasks, options).find("\033[32m") != std::string::npos);
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

static void test_task_text_cannot_emit_controls() {
  WatchOptions options;
  options.tty = true;
  for (const auto* text : {"a\x9b" "2Jb", "a\xc2\x9b" "2Jb",
                           "a\x1b[2Jb", "a\x7f" "b", "a\xff\xfe" "b",
                           "a\xe2\x82" "b"}) {
    for (bool tty : {false, true}) {
      options.tty = tty;
      auto rendered = frame({task("ab1111", text)}, options);
      auto line = rendered.substr(rendered.find("[ab"));
      for (unsigned char ch : line) {
        // Only the frame's own SGR styling may use ESC.
        CHECK(ch != 0x9b && ch != 0x7f && ch != 0xff && ch != 0xfe);
      }
      CHECK(line.find("\xc2\x9b") == std::string::npos);
      CHECK(line.find("[2J") == std::string::npos ||
            line.find("\x1b[2J") == std::string::npos);
    }
  }
  auto utf8 = frame({task("ab1111", "caf\xc3\xa9 \xe2\x9c\x93")});
  CHECK(utf8.find("caf\xc3\xa9 \xe2\x9c\x93") != std::string::npos);
}

static void test_tty_frames_leave_last_column_free() {
  // Writing the final column leaves the cursor pending a wrap, where the
  // erase that follows would delete it (or Windows wraps immediately).
  WatchOptions options;
  options.tty = true;
  options.color = false;
  std::vector<Task> tasks{task("ab1111", std::string(100, 'x'))};
  for (int width : {2, 8, 20, 80}) {
    std::istringstream in(frame(tasks, options, width));
    std::string line;
    while (std::getline(in, line)) {
      std::string visible;
      for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\033') {
          i = line.find('m', i);
          continue;
        }
        visible += line[i];
      }
      CHECK(display_width(visible) <= width - 1);
    }
  }
}

static void test_intervals() {
  CHECK(parse_watch_interval("1") == 1.0);
  CHECK(parse_watch_interval("0.25") == 0.25);
  CHECK(parse_watch_interval("0.1") == 0.1);
  CHECK(parse_watch_interval("1e-1") == 0.1);
  // Below 0.1 s a poll can outlast the interval and spin a core.
  for (const auto* value : {"", "0", "-1", "no", "1x", "nan", "inf",
                             "1e999", " 1", "1 ", "0.05", "1e-9"}) {
    CHECK(!parse_watch_interval(value));
  }
}

int main() {
  test_changes();
  test_frames();
  test_intervals();
  test_task_text_cannot_emit_controls();
  test_tty_frames_leave_last_column_free();
  std::cout << "watch tests passed\n";
}
