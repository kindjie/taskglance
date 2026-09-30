#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "taskglance/store.hpp"

namespace taskglance {

enum class KeyType {
  Text, Escape, Enter, Backspace, Left, Right, Up, Down, Home, End,
  CtrlA, CtrlC, CtrlD, CtrlE, CtrlU, CtrlW, Unknown
};

struct Key {
  KeyType type = KeyType::Unknown;
  std::string text;
};

// Streaming decoder: feed arbitrary byte chunks. The caller expires an
// incomplete escape sequence after 25 ms without another byte.
class KeyDecoder {
 public:
  std::vector<Key> feed(std::string_view bytes);
  std::vector<Key> expire_escape();
  bool pending_escape() const;

 private:
  std::string pending_;
};

struct LineEditor {
  std::string text;
  std::size_t cursor = 0;  // UTF-8 byte boundary
};

enum class EditorResult { Continue, Commit, Cancel };
EditorResult edit_line(LineEditor& editor, const Key& key);

enum class InteractiveMode { Normal, Add, Edit, Filter, Confirm, Help };
enum class ActionType { None, Add, Edit, Toggle, Delete, Undo, Quit };

struct InteractiveAction {
  ActionType type = ActionType::None;
  std::string id;
  std::string text;
};

struct InteractiveState {
  InteractiveMode mode = InteractiveMode::Normal;
  std::vector<Task> visible;
  std::size_t selected = 0;
  std::size_t first_row = 0;
  std::string selected_id;
  std::string target_id;  // captured when starting edit/confirmation
  std::string filter;
  std::string pending;
  std::string message;
  LineEditor editor;
};

void reload_interactive(InteractiveState& state,
                        const std::vector<Task>& tasks, bool all);
void scroll_interactive(InteractiveState& state, std::size_t rows);
InteractiveAction handle_interactive_key(InteractiveState& state,
                                         const Key& key,
                                         std::size_t page_rows);

struct UndoChange {
  std::optional<Task> before;
  std::optional<Task> after;
  std::size_t position = 0;
};

struct ChangeResult {
  bool changed = false;
  std::string message;
  std::string selected_id;
  std::optional<UndoChange> undo;
};

// Call only inside update_tasks. This changes the freshly loaded vector,
// never the displayed snapshot. Keep result.undo only after save succeeds.
ChangeResult apply_interactive_action(
  std::vector<Task>& tasks, const InteractiveAction& action,
  const std::optional<UndoChange>& undo = std::nullopt
);

}  // namespace taskglance
