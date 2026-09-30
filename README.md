# taskglance

[![Build][build-badge]][build-workflow]

`taskglance` is a small terminal task reminder CLI written in C++.

It keeps short-lived tasks visible without printing a large reminder before
every prompt. The canonical command is `taskglance`; shell hooks can optionally
install a short alias, but `tg` commonly collides with TopGit.

## Requirements

- CMake 3.24 or newer
- A C++20 compiler
- Linux, macOS, or Windows

Prompt hooks and completions are available for zsh, bash, and fish.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local
```

Make sure the install prefix is on `PATH`. For the example above:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

The install step also writes shell completions to:

- `share/zsh/site-functions/_taskglance`
- `share/bash-completion/completions/taskglance`
- `share/fish/vendor_completions.d/taskglance.fish`

## Shell Setup

`taskglance` generates shell hooks from the installed binary. Add the lines for
your shell after `taskglance` is on `PATH`.

Zsh:

```sh
# Before compinit, if your prefix is not already in fpath.
fpath=("$HOME/.local/share/zsh/site-functions" $fpath)

# After taskglance is on PATH.
eval "$(taskglance hooks zsh)"
```

Bash:

```sh
eval "$(taskglance hooks bash)"
```

Fish:

```fish
taskglance hooks fish | source
```

The optional `tg` alias can be enabled in generated hooks:

```sh
eval "$(taskglance hooks zsh --alias tg)"
```

## Usage

```sh
taskglance add "Review deploy checklist"
taskglance list
taskglance done a1b2
taskglance prompt
taskglance prompt --force
```

## Watch

Keep `taskglance watch` open in a tmux pane to see tasks update as other
processes or agents change them through the CLI:

```sh
taskglance watch
taskglance watch --interval 0.5 --all
```

Watch is read-only. It polls file contents every second by default;
`--interval` accepts positive fractional seconds. Active tasks appear oldest
first, with completed tasks following when `--all` is set. Added or edited
tasks and status changes appear bold for 10 seconds. The header shows the
active count and the local time of the last observed change, initially the
start time. Long lines are truncated and tasks beyond the pane's height are
summarized as `+N more`.

Ctrl-C exits cleanly and restores the screen and cursor. When stdout is piped
or redirected, watch prints a plain frame at startup and after each file
change, separated by a blank line, without terminal escape sequences.

## Prompt Rendering

Prompt rendering can be turned off globally and back on again:

```sh
taskglance disable
taskglance enable
```

Prompt hooks:

```sh
taskglance hooks zsh
taskglance hooks bash
taskglance hooks fish
taskglance hooks zsh --transient
```

The default prompt mode is compact, right-aligned, and rate-limited. Right
alignment uses an 80-column content block by default; set `max_prompt_width` to
choose a different width. `prompt_align=left` restores left-aligned rendering.
All prompt display styles respect `max_prompt_tasks`; extra active tasks are
summarized so box rendering stays small enough for repeated prompts.
`prompt_mode=transient` is an opt-in zsh-first integration target. The
transient hook wraps zsh `accept-line`, so install it only when that tradeoff is
acceptable.

`prompt_enabled=false` suppresses prompt rendering everywhere, including
`taskglance prompt --force`, so installed shell hooks stay silent without being
uninstalled. `taskglance disable` and `taskglance enable` are shortcuts for
`taskglance config set prompt_enabled false|true`. This differs from
`prompt_mode=manual`, which only stops automatic rendering and still honors
`taskglance prompt --force`. Other commands, such as `taskglance list`, are
unaffected.

## Data

Tasks use XDG paths by default:

- data: `$XDG_DATA_HOME/taskglance/tasks.tsv`
- config: `$XDG_CONFIG_HOME/taskglance/config`
- prompt state: `$XDG_STATE_HOME/taskglance/prompt.state`

If the XDG variables are unset, standard home-directory fallbacks are used.

Commands that change tasks hold an exclusive lock on `tasks.tsv.lock` beside
the data file, so concurrent `taskglance` invocations cannot lose each other's
changes. Saves are synced to disk and replace the file atomically.

## Legacy Import

```sh
taskglance import zsh-todo-reminder
taskglance import zsh-todo-reminder ~/.config/todo-reminder/data.save
```

Import reads task text from the old save file and does not mutate it.

## Attribution

Inspired by `kindjie/zsh-todo-reminder`, itself forked from
`AlexisBRENON/oh-my-zsh-reminder`.

## License

MIT. See `LICENSE`.

[build-badge]:
  https://github.com/kindjie/taskglance/actions/workflows/build.yml/badge.svg
[build-workflow]:
  https://github.com/kindjie/taskglance/actions/workflows/build.yml
