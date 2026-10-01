#include "taskglance/prompt.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

#include "taskglance/util.hpp"

namespace taskglance {
namespace {

std::string task_signature(const std::vector<Task>& tasks) {
  std::ostringstream out;
  for (const auto& task : active_tasks(tasks)) {
    out << task.id << ':' << task.text << ';';
  }
  return hex_short(fnv1a64(out.str()), 16);
}

std::string command_name(bool alias_tg) {
  return alias_tg ? "tg" : "taskglance";
}

}  // namespace

bool should_render_prompt(const Paths& paths, const Config& config,
                          const std::vector<Task>& tasks) {
  if (!config.prompt_enabled) {
    return false;
  }
  if (config.prompt_mode == "manual") {
    return false;
  }
  if (active_tasks(tasks).empty()) {
    return false;
  }

  std::string current_signature = task_signature(tasks);
  std::ifstream in(paths.prompt_state_file);
  if (!in) {
    return true;
  }

  std::string previous_signature;
  std::string previous_time_raw;
  std::getline(in, previous_signature);
  std::getline(in, previous_time_raw);

  if (previous_signature != current_signature) {
    return true;
  }

  auto previous_time = time_from_iso(previous_time_raw);
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::system_clock::now() - previous_time
  ).count();
  return elapsed >= config.prompt_interval_seconds;
}

void update_prompt_state(const Paths& paths, const std::vector<Task>& tasks) {
  std::ostringstream out;
  out << task_signature(tasks) << '\n'
      << time_to_iso(std::chrono::system_clock::now()) << '\n';
  atomic_write_file(paths.prompt_state_file, out.str());
}

std::string hook_script(const std::string& shell, bool alias_tg,
                        bool transient) {
  auto shell_name = to_lower(shell);
  std::ostringstream out;
  if (shell_name == "zsh") {
    out << "# taskglance zsh hook\n";
    if (alias_tg) {
      out << "alias tg=taskglance\n";
    }
    out << "if (( $+functions[compdef] )); then\n";
    out << "  autoload -Uz _taskglance\n";
    out << "  compdef _taskglance taskglance\n";
    if (alias_tg) {
      out << "  compdef _taskglance tg\n";
    }
    out << "fi\n";
    out << "_taskglance_precmd() {\n";
    out << "  [[ \"$(taskglance config get prompt_mode 2>/dev/null)\" = "
           "\"transient\" ]] && return\n";
    out << "  taskglance prompt\n";
    out << "}\n";
    if (transient) {
      out << "_taskglance_transient_widget() {\n";
      out << "  if [[ \"$(taskglance config get prompt_mode 2>/dev/null)\" = "
             "\"transient\" ]]; then\n";
      out << "    local _tg_msg=\"$(taskglance prompt --force)\"\n";
      out << "    [[ -n \"$_tg_msg\" ]] && zle -M \"$_tg_msg\"\n";
      out << "  fi\n";
      out << "  zle .accept-line\n";
      out << "}\n";
      out << "zle -N accept-line _taskglance_transient_widget\n";
    }
    out << "autoload -Uz add-zsh-hook\n";
    out << "add-zsh-hook precmd _taskglance_precmd\n";
    out << "# Transient requires: taskglance hooks zsh --transient\n";
  } else if (shell_name == "bash") {
    out << "# taskglance bash hook\n";
    if (alias_tg) {
      out << "alias tg=taskglance\n";
    }
    out << "_taskglance_prompt() { taskglance prompt; }\n";
    out << "PROMPT_COMMAND=\"_taskglance_prompt"
           "${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"\n";
  } else if (shell_name == "fish") {
    out << "# taskglance fish hook\n";
    if (alias_tg) {
      out << "function tg; taskglance $argv; end\n";
    }
    out << "function _taskglance_prompt --on-event fish_prompt\n";
    out << "  taskglance prompt\n";
    out << "end\n";
  } else {
    out << "Unsupported shell: " << shell << '\n';
  }
  return out.str();
}

std::string completion_script(const std::string& shell, bool alias_tg) {
  auto shell_name = to_lower(shell);
  std::string command = command_name(alias_tg);
  std::ostringstream out;
  if (shell_name == "zsh") {
    out << "#compdef taskglance";
    if (alias_tg) {
      out << " tg";
    }
    out << "\n";
    out << "  local -a commands config_actions config_keys bool_values "
           "display_styles\n";
    out << "  local -a prompt_aligns prompt_modes shells\n";
    out << "  commands=(\n";
    out << "    'add:Add a task'\n";
    out << "    'list:List tasks'\n";
    out << "    'watch:Watch tasks live'\n";
    out << "    'done:Mark a task done'\n";
    out << "    'delete:Delete a task'\n";
    out << "    'edit:Edit a task'\n";
    out << "    'clear:Clear done tasks'\n";
    out << "    'prompt:Render prompt reminder'\n";
    out << "    'enable:Enable prompt rendering'\n";
    out << "    'disable:Disable prompt rendering'\n";
    out << "    'import:Import legacy tasks'\n";
    out << "    'config:Manage configuration'\n";
    out << "    'completions:Generate completions'\n";
    out << "    'hooks:Generate shell hooks'\n";
    out << "    'help:Show help'\n";
    out << "  )\n";
    out << "  config_actions=(\n";
    out << "    'get:Get a configuration value'\n";
    out << "    'set:Set a configuration value'\n";
    out << "    'list:List configuration'\n";
    out << "    'reset:Reset configuration'\n";
    out << "  )\n";
    out << "  config_keys=(\n";
    out << "    'alias_tg:Enable the tg alias in generated hooks'\n";
    out << "    'color:Enable color output'\n";
    out << "    'display_style:Prompt display style'\n";
    out << "    'max_prompt_tasks:Maximum tasks shown in the prompt'\n";
    out << "    'max_prompt_width:Maximum prompt render width'\n";
    out << "    'prompt_align:Prompt alignment'\n";
    out << "    'prompt_enabled:Render tasks on prompt'\n";
    out << "    'prompt_interval_seconds:Seconds between prompt reminders'\n";
    out << "    'prompt_mode:Prompt rendering mode'\n";
    out << "  )\n";
    out << "  bool_values=(true false)\n";
    out << "  display_styles=(compact box plain)\n";
    out << "  prompt_aligns=(right left)\n";
    out << "  prompt_modes=(compact transient manual)\n";
    out << "  shells=(zsh bash fish)\n";
    out << "  if [[ ${words[2]} = watch ]]; then\n";
    out << "    _arguments -s \\\n";
    out << "      '1:command:(watch)' \\\n";
    out << "      '--all[Include done tasks]' \\\n";
    out << "      '(-i)--interactive[Enable vim-key interactive mode]' \\\n";
    out << "      '(--interactive)-i[Enable vim-key interactive mode]' \\\n";
    out << "      '--interval[Polling interval in seconds]:seconds:'\n";
    out << "    return\n";
    out << "  fi\n";
    out << "  _arguments -C \\\n";
    out << "    '1:command:->command' \\\n";
    out << "    '2:argument:->arg2' \\\n";
    out << "    '3:argument:->arg3' \\\n";
    out << "    '4:argument:->arg4' \\\n";
    out << "    '*::argument:->rest'\n";
    out << "  case \"$state\" in\n";
    out << "    command) _describe 'taskglance command' commands ;;\n";
    out << "    arg2)\n";
    out << "      case \"${words[2]}\" in\n";
    out << "        config) _describe 'config action' config_actions ;;\n";
    out << "        hooks|completions) _describe 'shell' shells ;;\n";
    out << "        clear) _values 'option' --done ;;\n";
    out << "        import) _values 'legacy source' zsh-todo-reminder ;;\n";
    out << "      esac\n";
    out << "      ;;\n";
    out << "    arg3)\n";
    out << "      case \"${words[2]}:${words[3]}\" in\n";
    out << "        config:get|config:set|config:reset)\n";
    out << "          _describe 'config key' config_keys\n";
    out << "          ;;\n";
    out << "      esac\n";
    out << "      ;;\n";
    out << "    arg4)\n";
    out << "      case \"${words[2]}:${words[3]}:${words[4]}\" in\n";
    out << "        config:set:alias_tg|config:set:color|"
           "config:set:prompt_enabled)\n";
    out << "          _values 'value' \"${bool_values[@]}\"\n";
    out << "          ;;\n";
    out << "        config:set:display_style)\n";
    out << "          _values 'value' \"${display_styles[@]}\"\n";
    out << "          ;;\n";
    out << "        config:set:prompt_align)\n";
    out << "          _values 'value' \"${prompt_aligns[@]}\"\n";
    out << "          ;;\n";
    out << "        config:set:prompt_mode)\n";
    out << "          _values 'value' \"${prompt_modes[@]}\"\n";
    out << "          ;;\n";
    out << "      esac\n";
    out << "      ;;\n";
    out << "  esac\n";
  } else if (shell_name == "bash") {
    out << "_" << command << "_complete() {\n";
    out << "  local current=\"${COMP_WORDS[COMP_CWORD]}\"\n";
    out << "  if (( COMP_CWORD > 1 )); then\n";
    out << "    if [[ ${COMP_WORDS[1]} = watch &&\n";
    out << "          ${COMP_WORDS[COMP_CWORD-1]} != --interval ]]; then\n";
    out << "      COMPREPLY=($(compgen -W "
           "'--all --interval --interactive -i' -- "
           "\"$current\"))\n";
    out << "    else\n";
    out << "      COMPREPLY=()\n";
    out << "    fi\n";
    out << "    return\n";
    out << "  fi\n";
    out << "  COMPREPLY=($(compgen -W 'add list done delete edit clear "
           "watch prompt enable disable import config completions "
           "hooks help' -- \"$current\"))\n";
    out << "}\n";
    out << "complete -F _" << command << "_complete " << command << "\n";
  } else if (shell_name == "fish") {
    out << "complete -c " << command
        << " -f -n '__fish_use_subcommand' "
           "-a 'add list done delete edit clear watch prompt enable disable "
           "import config completions hooks help'\n";
    out << "complete -c " << command
        << " -f -n '__fish_seen_subcommand_from watch' "
           "-l all -d 'Include done tasks'\n";
    out << "complete -c " << command
        << " -f -n '__fish_seen_subcommand_from watch' "
           "-l interval -r -d 'Polling interval in seconds'\n";
    out << "complete -c " << command
        << " -f -n '__fish_seen_subcommand_from watch' "
           "-l interactive -s i -d 'Enable vim-key interactive mode'\n";
  } else {
    out << "Unsupported shell: " << shell << '\n';
  }
  return out.str();
}

}  // namespace taskglance
