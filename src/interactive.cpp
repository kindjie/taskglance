#include "taskglance/interactive.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <locale>
#include <stdexcept>
#include <utility>

#include "taskglance/util.hpp"
#include "taskglance/watch.hpp"

namespace taskglance {
namespace {

std::size_t previous_character(const std::string& text, std::size_t cursor) {
  if (cursor == 0) return 0;
  --cursor;
  while (cursor > 0 &&
         (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80) {
    --cursor;
  }
  return cursor;
}

std::size_t next_character(const std::string& text, std::size_t cursor) {
  if (cursor >= text.size()) return text.size();
  ++cursor;
  while (cursor < text.size() &&
         (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80) {
    ++cursor;
  }
  return cursor;
}

std::string lowercase_filter(const std::string& text) {
  static const std::locale locale = [] {
    try {
      return std::locale("");
    } catch (const std::runtime_error&) {
      return std::locale::classic();
    }
  }();
  const auto& casing = std::use_facet<std::ctype<wchar_t>>(locale);
  auto safe = terminal_safe_text(text);
  std::string output;
  for (std::size_t i = 0; i < safe.size();) {
    auto end = next_character(safe, i);
    auto length = end - i;
    auto lead = static_cast<unsigned char>(safe[i]);
    char32_t value = length == 1 ? lead : lead & (0x7f >> length);
    for (auto j = i + 1; j < end; ++j) {
      value = (value << 6) | (static_cast<unsigned char>(safe[j]) & 0x3f);
    }
    if (value <= static_cast<char32_t>(
                   std::numeric_limits<wchar_t>::max())) {
      value = static_cast<char32_t>(casing.tolower(
        static_cast<wchar_t>(value)));
    }
    if (value < 0x80) {
      output += static_cast<char>(value);
    } else {
      static constexpr unsigned char leads[] = {0, 0, 0xc0, 0xe0, 0xf0};
      int count = value < 0x800 ? 2 : (value < 0x10000 ? 3 : 4);
      output += static_cast<char>(leads[count] |
                                    (value >> (6 * (count - 1))));
      for (int shift = 6 * (count - 2); shift >= 0; shift -= 6) {
        output += static_cast<char>(0x80 | ((value >> shift) & 0x3f));
      }
    }
    i = end;
  }
  return output;
}

Key control_key(unsigned char byte) {
  switch (byte) {
    case 1: return {KeyType::CtrlA, {}};
    case 3: return {KeyType::CtrlC, {}};
    case 4: return {KeyType::CtrlD, {}};
    case 5: return {KeyType::CtrlE, {}};
    case 8: case 127: return {KeyType::Backspace, {}};
    case 10: case 13: return {KeyType::Enter, {}};
    case 21: return {KeyType::CtrlU, {}};
    case 23: return {KeyType::CtrlW, {}};
    default: return {};
  }
}

Key escape_key(const std::string& sequence) {
  if (sequence == "\033[A" || sequence == "\033OA") {
    return {KeyType::Up, {}};
  }
  if (sequence == "\033[B" || sequence == "\033OB") {
    return {KeyType::Down, {}};
  }
  if (sequence == "\033[C" || sequence == "\033OC") {
    return {KeyType::Right, {}};
  }
  if (sequence == "\033[D" || sequence == "\033OD") {
    return {KeyType::Left, {}};
  }
  if (sequence == "\033[H" || sequence == "\033OH" ||
      sequence == "\033[1~" || sequence == "\033[7~") {
    return {KeyType::Home, {}};
  }
  if (sequence == "\033[F" || sequence == "\033OF" ||
      sequence == "\033[4~" || sequence == "\033[8~") {
    return {KeyType::End, {}};
  }
  return {};
}

void select_row(InteractiveState& state, std::size_t row) {
  state.selected = state.visible.empty()
                     ? 0 : std::min(row, state.visible.size() - 1);
  state.selected_id = state.visible.empty()
                       ? "" : state.visible[state.selected].id;
}

bool same_task(const Task& a, const Task& b) {
  // Disk timestamps have second precision. A newly made task may not have
  // been round-tripped yet; compare the representation update_tasks saves.
  return a.id == b.id && a.text == b.text && a.status == b.status &&
         time_to_iso(a.created_at) == time_to_iso(b.created_at) &&
         a.completed_at.has_value() == b.completed_at.has_value() &&
         (!a.completed_at ||
          time_to_iso(*a.completed_at) == time_to_iso(*b.completed_at));
}

ChangeResult undo_change(std::vector<Task>& tasks, const UndoChange& undo) {
  const auto& expected = undo.after ? *undo.after : *undo.before;
  auto found = std::find_if(tasks.begin(), tasks.end(), [&](const Task& t) {
    return t.id == expected.id;
  });
  if (undo.after) {
    if (found == tasks.end()) {
      return {false, "Cannot undo: task vanished", {}, {}};
    }
    if (!same_task(*found, *undo.after)) {
      return {false, "Cannot undo: task changed externally", {}, {}};
    }
    if (undo.before) {
      *found = *undo.before;
    } else {
      tasks.erase(found);
    }
  } else {
    if (found != tasks.end()) {
      return {false, "Cannot undo: task id already exists", {}, {}};
    }
    tasks.insert(tasks.begin() + static_cast<std::ptrdiff_t>(
                   std::min(undo.position, tasks.size())), *undo.before);
  }
  return {true, "Undone", undo.before ? undo.before->id : "", {}};
}

}  // namespace

std::vector<Key> KeyDecoder::feed(std::string_view bytes) {
  pending_.append(bytes);
  std::vector<Key> keys;
  std::size_t offset = 0;
  while (offset < pending_.size()) {
    auto byte = static_cast<unsigned char>(pending_[offset]);
    std::size_t length = 1;
    Key key;
    if (byte == 0x1b) {
      if (offset + 1 == pending_.size()) break;
      char following = pending_[offset + 1];
      if (following == '[' || following == 'O') {
        auto end = offset + 2;
        while (end < pending_.size() &&
               pending_[end] >= 0x20 && pending_[end] <= 0x3f) {
          ++end;
        }
        if (end == pending_.size()) break;
        auto final = static_cast<unsigned char>(pending_[end]);
        if (final < 0x40 || final > 0x7e) {
          // A control interrupts an incomplete CSI (especially Ctrl-C).
          offset = end;
          continue;
        }
        length = end - offset + 1;
        key = escape_key(pending_.substr(offset, length));
      } else {
        key = {KeyType::Escape, {}};
      }
    } else if (byte < 0x20 || byte == 0x7f) {
      key = control_key(byte);
    } else if (byte < 0x80) {
      key = {KeyType::Text, pending_.substr(offset, 1)};
    } else {
      if (byte >= 0xc2 && byte <= 0xdf) length = 2;
      else if (byte >= 0xe0 && byte <= 0xef) length = 3;
      else if (byte >= 0xf0 && byte <= 0xf4) length = 4;
      auto end = std::min(offset + length, pending_.size());
      bool valid_tail = true;
      for (auto i = offset + 1; i < end; ++i) {
        if ((static_cast<unsigned char>(pending_[i]) & 0xc0) != 0x80) {
          valid_tail = false;
          break;
        }
      }
      if (valid_tail && offset + length > pending_.size()) break;
      auto text = pending_.substr(offset, length);
      if (valid_tail && terminal_safe_text(text) == text) {
        key = {KeyType::Text, text};
      } else {
        // Resynchronize at the next byte, including ASCII after bad UTF-8.
        length = 1;
      }
    }
    if (key.type != KeyType::Unknown) keys.push_back(std::move(key));
    offset += length;
  }
  pending_.erase(0, offset);
  // Escape parameters are bounded even if a terminal sends garbage.
  if (pending_escape() && pending_.size() > 64) pending_.clear();
  return keys;
}

bool KeyDecoder::pending_escape() const {
  return !pending_.empty() && pending_[0] == '\033';
}

std::vector<Key> KeyDecoder::expire_escape() {
  if (!pending_escape()) return {};
  pending_.clear();
  return {{KeyType::Escape, {}}};
}

EditorResult edit_line(LineEditor& editor, const Key& key) {
  switch (key.type) {
    case KeyType::Enter: return EditorResult::Commit;
    case KeyType::Escape: return EditorResult::Cancel;
    case KeyType::Text:
      if (terminal_safe_text(key.text) == key.text) {
        editor.text.insert(editor.cursor, key.text);
        editor.cursor += key.text.size();
      }
      break;
    case KeyType::Backspace: {
      auto before = previous_character(editor.text, editor.cursor);
      editor.text.erase(before, editor.cursor - before);
      editor.cursor = before;
      break;
    }
    case KeyType::CtrlW: {
      auto before = editor.cursor;
      while (before > 0 && editor.text[before - 1] == ' ') --before;
      while (before > 0 && editor.text[before - 1] != ' ') {
        before = previous_character(editor.text, before);
      }
      editor.text.erase(before, editor.cursor - before);
      editor.cursor = before;
      break;
    }
    case KeyType::CtrlU:
      editor.text.clear();
      editor.cursor = 0;
      break;
    case KeyType::Left:
      editor.cursor = previous_character(editor.text, editor.cursor);
      break;
    case KeyType::Right:
      editor.cursor = next_character(editor.text, editor.cursor);
      break;
    case KeyType::Home: case KeyType::CtrlA:
      editor.cursor = 0;
      break;
    case KeyType::End: case KeyType::CtrlE:
      editor.cursor = editor.text.size();
      break;
    default: break;
  }
  return EditorResult::Continue;
}

void reload_interactive(InteractiveState& state,
                        const std::vector<Task>& tasks, bool all) {
  auto filter = lowercase_filter(state.mode == InteractiveMode::Filter
                                   ? state.editor.text : state.filter);
  state.visible.clear();
  for (const auto& task : tasks) {
    if ((all || task.status == TaskStatus::Active) &&
        (filter.empty() ||
         lowercase_filter(task.text).find(filter) != std::string::npos)) {
      state.visible.push_back(task);
    }
  }
  std::stable_sort(state.visible.begin(), state.visible.end(),
                   [](const Task& a, const Task& b) {
    if (a.status != b.status) return a.status == TaskStatus::Active;
    return a.created_at < b.created_at;
  });
  auto found = std::find_if(state.visible.begin(), state.visible.end(),
                            [&](const Task& t) {
    return t.id == state.selected_id;
  });
  select_row(state, found == state.visible.end() ? state.selected
                     : static_cast<std::size_t>(found - state.visible.begin()));
}

void scroll_interactive(InteractiveState& state, std::size_t rows) {
  if (rows == 0 || state.visible.empty()) {
    state.first_row = 0;
    return;
  }
  if (state.selected < state.first_row) state.first_row = state.selected;
  if (state.selected - state.first_row >= rows) {
    state.first_row = state.selected - rows + 1;
  }
  state.first_row = std::min(state.first_row,
                            state.visible.size() > rows
                              ? state.visible.size() - rows : 0);
}

InteractiveAction handle_interactive_key(InteractiveState& state,
                                         const Key& key,
                                         std::size_t page_rows) {
  if (key.type == KeyType::CtrlC) return {ActionType::Quit, {}, {}};
  if (state.mode == InteractiveMode::Add ||
      state.mode == InteractiveMode::Edit ||
      state.mode == InteractiveMode::Filter) {
    auto result = edit_line(state.editor, key);
    if (result == EditorResult::Continue) return {};
    auto mode = state.mode;
    state.mode = InteractiveMode::Normal;
    if (result == EditorResult::Cancel) {
      if (mode == InteractiveMode::Filter) state.filter.clear();
      state.message = "Cancelled";
      return {};
    }
    if (mode == InteractiveMode::Filter) {
      state.filter = state.editor.text;
      state.message = state.filter.empty() ? "Filter cleared" : "Filter kept";
      return {};
    }
    return {mode == InteractiveMode::Add ? ActionType::Add : ActionType::Edit,
            state.target_id, state.editor.text};
  }
  auto text = key.type == KeyType::Text ? key.text : "";
  if (state.mode == InteractiveMode::Help) {
    auto sequence = std::exchange(state.pending, "") + text;
    if (text == "q" || sequence == "ZZ") return {ActionType::Quit, {}, {}};
    if (text == "?" || key.type == KeyType::Escape) {
      state.mode = InteractiveMode::Normal;
    } else if (text == "Z") {
      state.pending = text;
    }
    return {};
  }
  if (state.mode == InteractiveMode::Confirm) {
    if (text == "y") {
      state.mode = InteractiveMode::Normal;
      return {ActionType::Delete, state.target_id, {}};
    }
    if (text == "n" || key.type == KeyType::Escape) {
      state.mode = InteractiveMode::Normal;
      state.message = "Delete cancelled";
    }
    return {};
  }
  if (key.type == KeyType::Escape) {
    state.pending.clear();
    return {};
  }
  auto sequence = std::exchange(state.pending, "") + text;
  if (sequence == "gg") {
    select_row(state, 0);
    return {};
  }
  if (sequence == "ZZ" || text == "q") return {ActionType::Quit, {}, {}};
  if (sequence == "dd" && !state.selected_id.empty()) {
    state.mode = InteractiveMode::Confirm;
    state.target_id = state.selected_id;
    state.message = "Delete [" + state.target_id + "]? y/n";
    return {};
  }
  if (text == "g" || text == "d" || text == "c" || text == "Z") {
    state.pending = text;
    return {};
  }
  auto step = std::max<std::size_t>(1, page_rows / 2);
  if (text == "j" || key.type == KeyType::Down) {
    select_row(state, state.selected + 1);
  } else if (text == "k" || key.type == KeyType::Up) {
    select_row(state, state.selected > 0 ? state.selected - 1 : 0);
  } else if (text == "G") {
    select_row(state, state.visible.size());
  } else if (key.type == KeyType::CtrlD) {
    select_row(state, state.selected + step);
  } else if (key.type == KeyType::CtrlU) {
    select_row(state, state.selected > step ? state.selected - step : 0);
  } else if (text == "a" || text == "o") {
    state.mode = InteractiveMode::Add;
    state.target_id.clear();
    state.editor = {};
  } else if ((text == "e" || sequence == "cw") &&
             !state.selected_id.empty()) {
    state.mode = InteractiveMode::Edit;
    state.target_id = state.selected_id;
    state.editor.text = terminal_safe_text(state.visible[state.selected].text);
    state.editor.cursor = state.editor.text.size();
  } else if (text == "x" && !state.selected_id.empty()) {
    return {ActionType::Toggle, state.selected_id, {}};
  } else if (text == "u") {
    return {ActionType::Undo, {}, {}};
  } else if (text == "/") {
    state.mode = InteractiveMode::Filter;
    state.editor = {state.filter, state.filter.size()};
  } else if (text == "?") {
    state.mode = InteractiveMode::Help;
  }
  return {};
}

ChangeResult apply_interactive_action(
  std::vector<Task>& tasks, const InteractiveAction& action,
  const std::optional<UndoChange>& undo
) {
  if (action.type == ActionType::Undo) {
    if (!undo || (!undo->before && !undo->after)) {
      return {false, "Nothing to undo", {}, {}};
    }
    return undo_change(tasks, *undo);
  }
  if (action.type == ActionType::None || action.type == ActionType::Quit) {
    return {};
  }
  if (action.type == ActionType::Add) {
    auto text = sanitize_task_text(action.text);
    if (text.empty()) return {false, "Empty add ignored", {}, {}};
    auto task = make_task(text, tasks);
    auto position = tasks.size();
    tasks.push_back(task);
    return {true, "Added", task.id, UndoChange{{}, task, position}};
  }
  auto found = std::find_if(tasks.begin(), tasks.end(), [&](const Task& t) {
    return t.id == action.id;
  });
  if (found == tasks.end()) return {false, "Task vanished", {}, {}};
  UndoChange change{*found, {},
                    static_cast<std::size_t>(found - tasks.begin())};
  std::string message;
  if (action.type == ActionType::Delete) {
    tasks.erase(found);
    message = "Deleted";
  } else {
    if (action.type == ActionType::Edit) {
      auto text = sanitize_task_text(action.text);
      if (text == found->text) return {false, "Text unchanged", {}, {}};
      found->text = text;
      message = "Edited";
    } else if (action.type == ActionType::Toggle) {
      if (found->status == TaskStatus::Active) {
        found->status = TaskStatus::Done;
        found->completed_at = std::chrono::system_clock::now();
        message = "Marked done";
      } else {
        found->status = TaskStatus::Active;
        found->completed_at.reset();
        message = "Marked active";
      }
    }
    change.after = *found;
  }
  return {true, message, action.type == ActionType::Delete ? "" : action.id,
          change};
}

namespace {

// Clip by complete code points, without an ellipsis eating the cursor's
// cell. Call only on terminal_safe_text output.
std::string clip_line(const std::string& text, int width) {
  std::string output;
  int used = 0;
  for (std::size_t i = 0; i < text.size();) {
    auto end = next_character(text, i);
    auto character = text.substr(i, end - i);
    auto columns = display_width(character);
    if (used + columns > width) break;
    output += character;
    used += columns;
    i = end;
  }
  return output;
}

bool is_editing(InteractiveMode mode) {
  return mode == InteractiveMode::Add || mode == InteractiveMode::Edit ||
         mode == InteractiveMode::Filter;
}

std::string mode_label(InteractiveMode mode) {
  switch (mode) {
    case InteractiveMode::Add: return "ADD";
    case InteractiveMode::Edit: return "EDIT";
    case InteractiveMode::Filter: return "FILTER";
    case InteractiveMode::Confirm: return "CONFIRM";
    case InteractiveMode::Help: return "HELP";
    default: return "NORMAL";
  }
}

}  // namespace

InteractiveFrame build_interactive_frame(
  const std::vector<Task>& tasks, const InteractiveState& state,
  const std::vector<std::string>& changed_ids, int width, int height,
  std::chrono::system_clock::time_point last_change,
  std::chrono::duration<double> since_change, bool color
) {
  if (width <= 0 || height <= 0) return {};
  int columns = width - 1;
  std::vector<std::string> lines;
  if (state.mode == InteractiveMode::Help) {
    static constexpr std::array help = {
      "taskglance | Interactive watch keys",
      "j/k Up/Down: move | gg/G: first/last | Ctrl-d/u: half page",
      "a/o: add | e/cw: edit | x: toggle done/active",
      "dd then y/n: delete | u: undo last session change",
      "/: filter | Enter: keep | Esc: clear/cancel",
      "Editor: UTF-8, Backspace, Ctrl-w/u, arrows, Home/End, Ctrl-a/e",
      "q or ZZ: quit | ? or Esc: close help"
    };
    for (auto line : help) {
      if (lines.size() >= static_cast<std::size_t>(height - 1)) break;
      lines.push_back(clip_line(line, columns));
    }
  } else if (height > 1) {
    WatchOptions options;
    options.tty = true;
    options.all = true;
    options.color = color;
    WatchViewport viewport{state.visible, state.first_row, state.selected_id};
    auto frame = build_watch_frame(tasks, changed_ids, width, height - 1,
                                   last_change, since_change, options,
                                   &viewport);
    std::size_t start = 0;
    do {
      auto end = frame.find('\n', start);
      lines.push_back(frame.substr(start, end - start));
      if (end == std::string::npos) break;
      start = end + 1;
    } while (true);
    // With a one-column terminal the only usable width is zero.
    if (columns == 0) {
      for (auto& line : lines) line.clear();
    }
  }
  lines.resize(static_cast<std::size_t>(height - 1));
  InteractiveFrame result;
  auto prefix = clip_line("[" + mode_label(state.mode) + "] ", columns);
  std::string status;
  if (is_editing(state.mode)) {
    auto hint = state.mode == InteractiveMode::Filter
                  ? " | Enter keep Esc clear" : " | Enter save Esc cancel";
    if (columns < 45) hint = "";
    int room = std::max(0, columns - display_width(prefix) -
                           display_width(hint));
    // The editor is always terminal-safe; retain the byte cursor when
    // scrolling horizontally instead of slicing through a UTF-8 character.
    auto cursor = std::min(state.editor.cursor, state.editor.text.size());
    std::size_t start = 0;
    auto before_width = display_width(terminal_safe_text(
      state.editor.text.substr(0, cursor)));
    while (start < cursor && before_width >= room) {
      auto next = next_character(state.editor.text, start);
      before_width -= display_width(terminal_safe_text(
        state.editor.text.substr(start, next - start)));
      start = next;
    }
    auto text = terminal_safe_text(state.editor.text.substr(start));
    status = prefix + clip_line(text, room) + hint;
    auto before_cursor = terminal_safe_text(
      state.editor.text.substr(start, cursor - start));
    result.cursor_column = std::max(1, std::min(columns,
      display_width(prefix) + display_width(before_cursor) + 1));
  } else {
    std::string hint = state.mode == InteractiveMode::Confirm
                         ? " | y delete n cancel" : " | ? help q quit";
    auto message = state.pending.empty() ? state.message
                                         : "Pending " + state.pending;
    if (!state.filter.empty() && state.mode == InteractiveMode::Normal) {
      message = "/" + state.filter + " | " + message;
    }
    int room = std::max(0, columns - display_width(prefix) -
                           display_width(hint));
    status = prefix + clip_line(terminal_safe_text(message), room) + hint;
  }
  lines.push_back(clip_line(status, columns));
  result.text = join(lines, "\n");
  return result;
}

}  // namespace taskglance
