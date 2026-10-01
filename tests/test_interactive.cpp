#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <sstream>

#include "taskglance/interactive.hpp"
#include "taskglance/util.hpp"
#include "taskglance/watch.hpp"

using namespace taskglance;
using namespace std::chrono_literals;

#define CHECK(condition)                                                \
  do {                                                                 \
    if (!(condition)) {                                                 \
      std::cerr << "CHECK failed: " #condition " at " << __FILE__         \
                << ':' << __LINE__ << '\n';                             \
      std::exit(1);                                                     \
    }                                                                  \
  } while (false)

static Task task(std::string id, std::string text, int age = 0) {
  return {id, TaskStatus::Active,
          std::chrono::system_clock::time_point{1s * age}, {}, text};
}

static InteractiveAction key(InteractiveState& state, std::string text) {
  return handle_interactive_key(state, {KeyType::Text, text}, 6);
}

static void special(InteractiveState& state, KeyType type) {
  handle_interactive_key(state, {type, {}}, 6);
}

static void test_normal_keys() {
  InteractiveState state;
  reload_interactive(state, {task("aa", "First"), task("ab", "Second", 1)},
                     true);
  CHECK(key(state, "j").type == ActionType::None);
  CHECK(state.selected_id == "ab");
  key(state, "j");
  CHECK(state.selected == 1);
  key(state, "g");
  CHECK(state.selected == 1 && state.pending == "g");
  key(state, "g");
  CHECK(state.selected == 0 && state.pending.empty());
  key(state, "G");
  CHECK(state.selected == 1);
  key(state, "k");
  key(state, "k");
  CHECK(state.selected == 0);
  special(state, KeyType::Down);
  CHECK(state.selected == 1);
  special(state, KeyType::Up);
  CHECK(state.selected == 0);
  CHECK(key(state, "x").type == ActionType::Toggle);
  CHECK(key(state, "u").type == ActionType::Undo);
  CHECK(key(state, "d").type == ActionType::None);
  CHECK(key(state, "d").type == ActionType::None);
  CHECK(state.mode == InteractiveMode::Confirm);
  CHECK(key(state, "n").type == ActionType::None);
  CHECK(state.mode == InteractiveMode::Normal);
  key(state, "d");
  key(state, "d");
  // Reload during confirmation must not retarget the deletion.
  reload_interactive(state, {task("ab", "Second")}, true);
  auto deletion = key(state, "y");
  CHECK(deletion.type == ActionType::Delete && deletion.id == "aa");
  key(state, "c");
  key(state, "w");
  CHECK(state.mode == InteractiveMode::Edit);
  CHECK(state.editor.text == "Second");
  special(state, KeyType::Escape);
  CHECK(state.mode == InteractiveMode::Normal);
  key(state, "e");
  CHECK(state.mode == InteractiveMode::Edit);
  special(state, KeyType::Escape);
  for (auto prefix : {"g", "d", "c", "Z"}) {
    key(state, prefix);
    special(state, KeyType::Escape);
    CHECK(state.pending.empty());
    CHECK(state.mode == InteractiveMode::Normal);
  }
  key(state, "!");
  CHECK(state.selected == 0 && state.mode == InteractiveMode::Normal);
  key(state, "?");
  CHECK(state.mode == InteractiveMode::Help);
  special(state, KeyType::Escape);
  CHECK(state.mode == InteractiveMode::Normal);
  for (auto add : {"a", "o"}) {
    key(state, add);
    CHECK(state.mode == InteractiveMode::Add && state.editor.text.empty());
    CHECK(key(state, "New").type == ActionType::None);
    auto action = handle_interactive_key(state, {KeyType::Enter, {}}, 6);
    CHECK(action.type == ActionType::Add && action.text == "New");
  }
  CHECK(key(state, "q").type == ActionType::Quit);
  CHECK(key(state, "Z").type == ActionType::None);
  CHECK(key(state, "Z").type == ActionType::Quit);
  CHECK(handle_interactive_key(state, {KeyType::CtrlC, {}}, 6).type ==
        ActionType::Quit);
}

static void test_selection_and_filter() {
  std::vector<Task> tasks;
  for (int i = 0; i < 10; ++i) {
    tasks.push_back(task(std::to_string(i), "Task " + std::to_string(i), i));
  }
  InteractiveState state;
  reload_interactive(state, tasks, false);
  special(state, KeyType::CtrlD);
  CHECK(state.selected == 3);
  scroll_interactive(state, 2);
  CHECK(state.first_row == 2);
  special(state, KeyType::CtrlU);
  CHECK(state.selected == 0);
  key(state, "G");
  scroll_interactive(state, 2);
  CHECK(state.selected == 9 && state.first_row == 8);
  tasks.insert(tasks.begin(), task("new", "Before", -1));
  reload_interactive(state, tasks, false);
  CHECK(state.selected == 10 && state.selected_id == "9");
  tasks.pop_back();
  reload_interactive(state, tasks, false);
  CHECK(state.selected == 9 && state.selected_id == "8");
  key(state, "/");
  key(state, "tAsK 3");
  // Live filtering uses the editor until Enter keeps it.
  reload_interactive(state, tasks, false);
  CHECK(state.visible.size() == 1 && state.selected_id == "3");
  special(state, KeyType::Enter);
  CHECK(state.filter == "tAsK 3");
  key(state, "/");
  special(state, KeyType::Escape);
  reload_interactive(state, tasks, false);
  CHECK(state.filter.empty() && state.visible.size() == 10);
  state.filter = "missing";
  reload_interactive(state, tasks, false);
  CHECK(state.visible.empty() && state.selected_id.empty());
  key(state, "j");
  key(state, "G");
  CHECK(state.selected == 0);
  CHECK(key(state, "x").type == ActionType::None);
  reload_interactive(state, {}, true);
  scroll_interactive(state, 0);
  CHECK(state.first_row == 0);
}

static void test_editor() {
  LineEditor editor;
  CHECK(edit_line(editor, {KeyType::Text, "caf\xc3\xa9 \xe7\x8c\xab"}) ==
        EditorResult::Continue);
  CHECK(editor.cursor == editor.text.size());
  edit_line(editor, {KeyType::Backspace, {}});
  CHECK(editor.text == "caf\xc3\xa9 ");
  edit_line(editor, {KeyType::CtrlW, {}});
  CHECK(editor.text.empty() && editor.cursor == 0);
  edit_line(editor, {KeyType::Text, "one two"});
  edit_line(editor, {KeyType::CtrlW, {}});
  CHECK(editor.text == "one ");
  edit_line(editor, {KeyType::CtrlU, {}});
  CHECK(editor.text.empty());
  edit_line(editor, {KeyType::Text, "\xc3\xa9\xe7\x8c\xab"});
  edit_line(editor, {KeyType::Left, {}});
  CHECK(editor.cursor == 2);
  edit_line(editor, {KeyType::Text, "!"});
  CHECK(editor.text == "\xc3\xa9!\xe7\x8c\xab");
  edit_line(editor, {KeyType::Right, {}});
  CHECK(editor.cursor == editor.text.size());
  edit_line(editor, {KeyType::Home, {}});
  edit_line(editor, {KeyType::Left, {}});
  edit_line(editor, {KeyType::Backspace, {}});
  CHECK(editor.cursor == 0);
  edit_line(editor, {KeyType::End, {}});
  CHECK(editor.cursor == editor.text.size());
  edit_line(editor, {KeyType::CtrlA, {}});
  CHECK(editor.cursor == 0);
  edit_line(editor, {KeyType::CtrlE, {}});
  CHECK(editor.cursor == editor.text.size());
  CHECK(edit_line(editor, {KeyType::Enter, {}}) == EditorResult::Commit);
  CHECK(edit_line(editor, {KeyType::Escape, {}}) == EditorResult::Cancel);
  auto text = editor.text;
  edit_line(editor, {KeyType::Text, "\033[2J"});
  edit_line(editor, {KeyType::Text, "\xff"});
  CHECK(editor.text == text);
}

static void test_decoder() {
  KeyDecoder decoder;
  CHECK(decoder.feed("\033").empty() && decoder.pending_escape());
  CHECK(decoder.feed("[").empty());
  auto keys = decoder.feed("A\033[B\033[C\033[D\033[H\033[F");
  CHECK(keys.size() == 6);
  CHECK(keys[0].type == KeyType::Up && keys[1].type == KeyType::Down);
  CHECK(keys[2].type == KeyType::Right && keys[3].type == KeyType::Left);
  CHECK(keys[4].type == KeyType::Home && keys[5].type == KeyType::End);
  keys = decoder.feed("\033OH\033OF\033[1~\033[4~");
  CHECK(keys.size() == 4 && keys[0].type == KeyType::Home &&
        keys[3].type == KeyType::End);
  CHECK(decoder.feed("\033").empty());
  keys = decoder.expire_escape();
  CHECK(keys.size() == 1 && keys[0].type == KeyType::Escape);
  CHECK(!decoder.pending_escape() && decoder.expire_escape().empty());
  keys = decoder.feed("\x01\x03\x04\x05\x15\x17\x7f\x08\r\n");
  CHECK(keys.size() == 10);
  CHECK(keys[0].type == KeyType::CtrlA && keys[1].type == KeyType::CtrlC);
  CHECK(keys[2].type == KeyType::CtrlD && keys[3].type == KeyType::CtrlE);
  CHECK(keys[4].type == KeyType::CtrlU && keys[5].type == KeyType::CtrlW);
  CHECK(keys[6].type == KeyType::Backspace);
  CHECK(keys[8].type == KeyType::Enter);
  CHECK(decoder.feed("\xc3").empty());
  keys = decoder.feed("\xa9\xe7\x8c\xab");
  CHECK(keys.size() == 2 && keys[0].text == "\xc3\xa9" &&
        keys[1].text == "\xe7\x8c\xab");
  CHECK(decoder.feed("\xff\xc2\x9b\033[99~").empty());
  keys = decoder.feed("\033j");
  CHECK(keys.size() == 2 && keys[0].type == KeyType::Escape &&
        keys[1].text == "j");
  CHECK(decoder.feed("\033[").empty());
  CHECK(decoder.expire_escape().size() == 1);
  keys = decoder.feed("\033[\x03");
  CHECK(keys.size() == 1 && keys[0].type == KeyType::CtrlC);
  CHECK(!decoder.pending_escape());
  CHECK(decoder.feed("\xc3").empty());
  keys = decoder.feed("\x03");
  CHECK(keys.size() == 1 && keys[0].type == KeyType::CtrlC);
}

static void test_changes_and_undo() {
  std::vector<Task> tasks{task("aa", "First"), task("ab", "Second")};
  auto result = apply_interactive_action(tasks, {ActionType::Add, {}, ""});
  CHECK(!result.changed && !result.message.empty());
  result = apply_interactive_action(tasks,
                                    {ActionType::Add, {}, "  New\tTask  "});
  CHECK(result.changed && tasks.back().text == "New Task" && result.undo);
  auto added = tasks.back().id;
  tasks.push_back(task("external", "External"));
  result = apply_interactive_action(tasks, {ActionType::Undo, {}, {}},
                                     result.undo);
  CHECK(result.changed && tasks.size() == 3);
  CHECK(tasks.back().id == "external");
  for (auto action : {InteractiveAction{ActionType::Edit, "aa", "Edited"},
                       InteractiveAction{ActionType::Toggle, "aa", {}},
                       InteractiveAction{ActionType::Delete, "aa", {}}}) {
    auto change = apply_interactive_action(tasks, action);
    CHECK(change.changed && change.undo);
    auto undone = apply_interactive_action(tasks, {ActionType::Undo, {}, {}},
                                           change.undo);
    CHECK(undone.changed && tasks.front().text == "First");
    CHECK(tasks.front().status == TaskStatus::Active);
    CHECK(!tasks.front().completed_at);
  }
  auto change = apply_interactive_action(tasks, {ActionType::Toggle, "aa", {}});
  CHECK(tasks.front().status == TaskStatus::Done &&
        tasks.front().completed_at);
  auto active = apply_interactive_action(tasks, {ActionType::Toggle, "aa", {}});
  CHECK(tasks.front().status == TaskStatus::Active);
  apply_interactive_action(tasks, {ActionType::Undo, {}, {}}, active.undo);
  CHECK(tasks.front().status == TaskStatus::Done);
  apply_interactive_action(tasks, {ActionType::Undo, {}, {}}, change.undo);
  CHECK(tasks.front().status == TaskStatus::Active);
  change = apply_interactive_action(tasks, {ActionType::Edit, "aa", "Edit"});
  tasks.front().text = "External edit";
  result = apply_interactive_action(tasks, {ActionType::Undo, {}, {}}, change.undo);
  CHECK(!result.changed && result.message.find("changed") !=
        std::string::npos);
  CHECK(tasks.front().text == "External edit");
  tasks.erase(tasks.begin());
  result = apply_interactive_action(tasks, {ActionType::Undo, {}, {}}, change.undo);
  CHECK(!result.changed && result.message.find("vanished") !=
        std::string::npos);
  for (auto type : {ActionType::Edit, ActionType::Toggle, ActionType::Delete}) {
    result = apply_interactive_action(tasks, {type, "aa", "x"});
    CHECK(!result.changed && result.message.find("vanished") !=
          std::string::npos);
  }
  // Exact id, never an abbreviated prefix.
  CHECK(!apply_interactive_action(tasks, {ActionType::Delete, "a", {}}).changed);
  change = apply_interactive_action(tasks, {ActionType::Delete, "ab", {}});
  tasks.push_back(task("ab", "Reused id"));
  CHECK(!apply_interactive_action(tasks, {ActionType::Undo, {}, {}},
                                  change.undo).changed);
  CHECK(!apply_interactive_action(tasks, {ActionType::Undo, {}, {}}).changed);
  CHECK(!added.empty());
  // A timestamp-only external update also invalidates undo.
  tasks = {task("aa", "First")};
  change = apply_interactive_action(tasks,
                                    {ActionType::Edit, "aa", "Edited"});
  tasks[0].created_at += 1s;
  CHECK(!apply_interactive_action(tasks, {ActionType::Undo, {}, {}},
                                  change.undo).changed);
}

static void test_locked_changes_and_persisted_undo() {
  auto root = std::filesystem::temp_directory_path() /
              ("taskglance-interactive-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
  auto file = root / "tasks.tsv";
  auto run = [&](const InteractiveAction& action,
                  std::optional<UndoChange> undo = {}) {
    ChangeResult result;
    update_tasks(file, [&](auto& current) {
      result = apply_interactive_action(current, action, undo);
      return result.changed;
    });
    return result;
  };
  auto add = run({ActionType::Add, {}, "Original"});
  auto id = add.selected_id;
  CHECK(add.changed);
  // Take a stale view, then simulate another writer before acting on it.
  auto displayed = load_tasks(file);
  update_tasks(file, [&](auto& current) {
    current.push_back(make_task("External", current));
    return true;
  });
  auto edit = run({ActionType::Edit, displayed[0].id, "Edited"});
  CHECK(load_tasks(file).size() == 2 && edit.changed);
  CHECK(run({ActionType::Undo, {}, {}}, edit.undo).changed);
  CHECK(load_tasks(file)[0].text == "Original");
  auto toggle = run({ActionType::Toggle, id, {}});
  CHECK(run({ActionType::Undo, {}, {}}, toggle.undo).changed);
  CHECK(load_tasks(file)[0].status == TaskStatus::Active);
  auto deletion = run({ActionType::Delete, id, {}});
  CHECK(run({ActionType::Undo, {}, {}}, deletion.undo).changed);
  CHECK(load_tasks(file).size() == 2);
  CHECK(run({ActionType::Undo, {}, {}}, add.undo).changed);
  CHECK(load_tasks(file).size() == 1);
  CHECK(load_tasks(file)[0].text == "External");
  auto before = read_task_file(file);
  CHECK(!run({ActionType::Edit, id, "Vanished"}).changed);
  CHECK(!run({ActionType::Undo, {}, {}}, add.undo).changed);
  CHECK(read_task_file(file) == before);
  std::filesystem::remove_all(root);
}

static std::string unstyled(const std::string& text) {
  std::string result;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\033') {
      CHECK(text[i + 1] == '[');
      i = text.find('m', i);
      CHECK(i != std::string::npos);
    } else {
      result += text[i];
    }
  }
  return result;
}

static void test_interactive_frames() {
  InteractiveState state;
  std::vector<Task> tasks{task("aa", "First"), task("ab", "Second", 1),
                          task("ac", "Third", 2)};
  reload_interactive(state, tasks, true);
  auto frame = [&](int width = 80, int height = 6) {
    return build_interactive_frame(tasks, state, {"aa"}, width, height,
                                    {}, 0s, false);
  };
  auto rendered = frame();
  CHECK(rendered.text.find("\033[7m") != std::string::npos);
  auto plain = unstyled(rendered.text);
  CHECK(plain.find("[aa] First") != std::string::npos);
  CHECK(plain.find("[NORMAL]") > plain.find("Third"));
  CHECK(!rendered.cursor_column);
  CHECK(std::count(plain.begin(), plain.end(), '\n') == 5);
  state.message = "bad\033[2J\xc2\x9b\xff";
  CHECK(frame().text.find("\033[2J") == std::string::npos);
  key(state, "G");
  scroll_interactive(state, 1);
  CHECK(unstyled(frame(80, 3).text).find("Third") != std::string::npos);
  key(state, "e");
  state.editor.text = "caf\xc3\xa9 \xe7\x8c\xab";
  state.editor.cursor = state.editor.text.size();
  for (int width : {1, 2, 3, 8, 20, 80}) {
    for (int height : {1, 2, 3, 6}) {
      auto output = frame(width, height);
      std::istringstream in(unstyled(output.text));
      std::string line;
      while (std::getline(in, line)) {
        CHECK(display_width(line) <= width - 1);
        CHECK(terminal_safe_text(line) == line);
      }
      CHECK(output.cursor_column);
      CHECK(*output.cursor_column >= 1);
      CHECK(*output.cursor_column <= std::max(1, width - 1));
    }
  }
  CHECK(frame(80, 0).text.empty());
  CHECK(frame(0, 6).text.empty());
  special(state, KeyType::Escape);
  key(state, "?");
  CHECK(unstyled(frame().text).find("gg/G") != std::string::npos);
  special(state, KeyType::Escape);
  tasks[0].text = "Unsafe\033[2J\xff";
  reload_interactive(state, tasks, true);
  CHECK(frame().text.find("\033[2J") == std::string::npos);
}

int main() {
  test_normal_keys();
  test_selection_and_filter();
  test_editor();
  test_decoder();
  test_changes_and_undo();
  test_locked_changes_and_persisted_undo();
  test_interactive_frames();
  std::cout << "interactive tests passed\n";
}
