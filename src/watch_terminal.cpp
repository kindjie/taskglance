#include "watch_terminal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "taskglance/interactive.hpp"
#include "taskglance/util.hpp"

namespace {

#ifdef _WIN32
std::atomic<std::sig_atomic_t> stopped{0};
static_assert(decltype(stopped)::is_always_lock_free);
BOOL WINAPI console_stop(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
    stopped = 1;
    return TRUE;
  }
  return FALSE;
}
#else
volatile std::sig_atomic_t stopped = 0;
#endif
volatile std::sig_atomic_t resized = 0;

void stop_watch(int) { stopped = 1; }
#ifndef _WIN32
void resize_watch(int) { resized = 1; }
#endif

class WatchSignals {
 public:
  WatchSignals() {
    stopped = 0;
    resized = 0;
#ifdef _WIN32
    if (!::SetConsoleCtrlHandler(console_stop, TRUE)) {
      throw std::runtime_error("Could not install console watch handler");
    }
    interrupt_ = std::signal(SIGINT, stop_watch);
    terminate_ = std::signal(SIGTERM, stop_watch);
#else
    // Install before entering the alternate screen. Roll back a partial
    // installation if any sigaction fails.
    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    try {
      for (; installed_ < 4; ++installed_) {
        action.sa_handler = signals_[installed_] == SIGWINCH
                              ? resize_watch : stop_watch;
        if (::sigaction(signals_[installed_], &action,
                         &previous_[installed_]) != 0) {
          throw std::runtime_error("Could not install watch signal handler");
        }
      }
    } catch (...) {
      restore();
      throw;
    }
#endif
  }

  ~WatchSignals() {
#ifdef _WIN32
    std::signal(SIGINT, interrupt_);
    std::signal(SIGTERM, terminate_);
    ::SetConsoleCtrlHandler(console_stop, FALSE);
#else
    restore();
#endif
  }

 private:
#ifdef _WIN32
  using Handler = void (*)(int);
  Handler interrupt_;
  Handler terminate_;
#else
  void restore() {
    while (installed_ > 0) {
      --installed_;
      ::sigaction(signals_[installed_], &previous_[installed_], nullptr);
    }
  }
  int signals_[4] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};
  struct sigaction previous_[4] {};
  int installed_ = 0;
#endif
};

// The persisted list timestamp, not the time this view was opened.
std::chrono::system_clock::time_point task_file_updated(
    const std::filesystem::path& file) {
  std::error_code error;
  auto modified = std::filesystem::last_write_time(file, error);
  if (error) return {};
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
    std::chrono::file_clock::to_sys(modified));
}

class WatchScreen {
 public:
  explicit WatchScreen(bool tty, bool mouse) : tty_(tty), mouse_(mouse) {
    if (!tty_) {
      return;
    }
#ifdef _WIN32
    handle_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!::GetConsoleMode(handle_, &mode_) ||
        !::SetConsoleMode(handle_, mode_ |
                                    ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      throw std::runtime_error("Could not enable terminal watch display");
    }
#endif
    std::fputs("\033[?1049h\033[?25l", stdout);
    if (mouse_) std::fputs("\033[?1000h\033[?1006h", stdout);
    std::fflush(stdout);
  }

  ~WatchScreen() {
    if (tty_) {
      if (mouse_) std::fputs("\033[?1006l\033[?1000l", stdout);
      std::fputs("\033[0m\033[?25h\033[?1049l", stdout);
      std::fflush(stdout);
#ifdef _WIN32
      ::SetConsoleMode(handle_, mode_);
#endif
    }
  }

 private:
  bool tty_;
  bool mouse_;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  DWORD mode_ = 0;
#endif
};

#ifndef _WIN32
class WatchInput {
 public:
  explicit WatchInput(bool interactive) : interactive_(interactive) {
    if (!interactive_) return;
    if (::tcgetattr(STDIN_FILENO, &previous_) != 0) {
      throw std::runtime_error("Could not read terminal input settings");
    }
    auto raw = previous_;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                     ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      restore();
      throw std::runtime_error("Could not enable interactive input");
    }
  }

  ~WatchInput() {
    if (interactive_) restore();
  }

  WatchInput(const WatchInput&) = delete;
  WatchInput& operator=(const WatchInput&) = delete;

 private:
  void restore() {
    while (::tcsetattr(STDIN_FILENO, TCSANOW, &previous_) != 0 &&
           errno == EINTR) {}
  }
  bool interactive_;
  struct termios previous_ {};
};
#endif

struct Size {
  int width;
  int height;
  bool operator==(const Size&) const = default;
};

Size terminal_size(bool tty) {
  // A pipe has no viewport; preserve all tasks in each plain frame.
  if (!tty) {
    return {taskglance::terminal_columns(),
            std::numeric_limits<int>::max()};
  }
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info {};
  if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE),
                                   &info)) {
    return {info.srWindow.Right - info.srWindow.Left + 1,
            info.srWindow.Bottom - info.srWindow.Top + 1};
  }
#else
  struct winsize size {};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
      size.ws_col > 0 && size.ws_row > 0) {
    return {size.ws_col, size.ws_row};
  }
#endif
  return {taskglance::terminal_columns(), 24};
}

}  // namespace

int run_watch(const std::filesystem::path& file, double interval,
              taskglance::WatchOptions options) {
  using Clock = std::chrono::steady_clock;
  using Seconds = std::chrono::duration<double>;
  if (options.interactive) {
#ifdef _WIN32
    std::cerr << "interactive mode is not supported on Windows yet\n";
    return 2;
#else
    if (!options.tty || ::isatty(STDIN_FILENO) != 1) {
      std::cerr << "interactive watch requires stdin and stdout to be TTYs\n";
      return 2;
    }
#endif
  }
  WatchSignals signals;
#ifndef _WIN32
  WatchInput input(options.interactive);
#endif
  WatchScreen screen(options.tty, options.interactive);
  auto content = taskglance::read_task_file(file);
  auto tasks = taskglance::parse_tasks(content);
  auto last_change = task_file_updated(file);
  auto changed_at = Clock::now();
  auto polled_at = changed_at;
  std::vector<std::string> changed_ids;
  auto size = terminal_size(options.tty);
  bool redraw = true;
  bool first = true;
  bool highlighting = false;
  taskglance::InteractiveState interactive;
  if (options.interactive) {
    taskglance::reload_interactive(interactive, tasks, options.all);
  }
  std::vector<taskglance::UndoChange> undo;
  taskglance::KeyDecoder decoder;
  auto escape_at = Clock::now();
  auto page_rows = [&]() {
    auto rows = static_cast<std::size_t>(std::max(0, size.height - 2));
    if (rows > 1 && interactive.visible.size() > rows) --rows;
    return rows;
  };
  auto reload = [&]() {
    auto next = taskglance::read_task_file(file);
    polled_at = Clock::now();
    if (next != content) {
      auto updated = taskglance::parse_tasks(next);
      changed_ids = taskglance::detect_task_changes(updated, tasks)
                      .changed_ids;
      tasks = std::move(updated);
      content = std::move(next);
      last_change = task_file_updated(file);
      changed_at = polled_at;
      highlighting = options.tty && !changed_ids.empty();
      if (options.interactive) {
        taskglance::reload_interactive(interactive, tasks, options.all);
      }
      redraw = true;
    }
  };
  auto render_frame = [&]() {
    auto now = Clock::now();
    std::string frame;
    std::optional<int> cursor;
    if (options.interactive) {
      auto rendered = taskglance::build_interactive_frame(
        tasks, interactive, changed_ids, size.width, size.height,
        last_change, Seconds(now - changed_at), options.color
      );
      interactive.first_row = rendered.first_row;
      frame = std::move(rendered.text);
      cursor = rendered.cursor_column;
    } else {
      frame = taskglance::build_watch_frame(
        tasks, changed_ids, size.width, size.height, last_change,
        Seconds(now - changed_at), options
      );
    }
    if (options.tty) {
      // Overwrite in place and erase leftovers rather than clearing the
      // screen first, which flickers in tmux.
      std::cout << "\033[H";
      for (char ch : frame) {
        if (ch == '\n') {
          std::cout << "\033[K\r";
        }
        std::cout << ch;
      }
      std::cout << "\033[K\033[J";
      if (options.interactive) {
        if (cursor) {
          std::cout << "\033[" << size.height << ';' << *cursor
                    << "H\033[?25h";
        } else {
          std::cout << "\033[?25l";
        }
      }
    } else {
      if (!first) {
        std::cout << '\n';
      }
      std::cout << frame << '\n';
    }
    std::cout.flush();
    if (!std::cout) {
      return false;
    }
    first = false;
    redraw = false;
    return true;
  };
  auto handle_keys = [&](const std::vector<taskglance::Key>& keys) {
    for (std::size_t index = 0; index < keys.size(); ++index) {
      const auto& key = keys[index];
      if (stopped) break;
      if (key.type == taskglance::KeyType::MouseClick) {
        redraw |= taskglance::select_mouse_row(interactive, tasks, key.row,
                                               size.width, size.height);
        continue;
      }
      auto action = taskglance::handle_interactive_key(interactive, key,
                                                       page_rows());
      redraw = true;
      if (action.type == taskglance::ActionType::Quit) {
        stopped = 1;
        break;
      }
      if (action.type != taskglance::ActionType::None) {
        try {
          taskglance::ChangeResult result;
          auto last = undo.empty() ? std::optional<taskglance::UndoChange>{}
                                     : undo.back();
          auto waiting_at = Clock::now();
          bool waiting_shown = false;
          bool pending_z = false;
          auto check_quit = [&](const taskglance::Key& waiting_key) {
            auto text = waiting_key.type == taskglance::KeyType::Text
                          ? waiting_key.text : "";
            if (waiting_key.type == taskglance::KeyType::CtrlC ||
                text == "q" || (pending_z && text == "Z")) {
              stopped = 1;
            }
            pending_z = text == "Z";
          };
          // Quit input already decoded after this action must also prevent
          // a write, even if it arrived in the same read as the action.
          for (auto next = index + 1; next < keys.size(); ++next) {
            check_quit(keys[next]);
          }
          auto cancel = [&]() {
            if (stopped) return true;
#ifndef _WIN32
            struct pollfd descriptor {STDIN_FILENO, POLLIN, 0};
            int ready = ::poll(&descriptor, 1, 0);
            if (ready < 0 && errno != EINTR) {
              throw std::runtime_error("Could not poll interactive input");
            }
            if (ready > 0 && (descriptor.revents & POLLIN)) {
              char bytes[256];
              auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
              if (count > 0) {
                // While a write waits, accept quit keys only: do not start
                // another mutation or change the undo history.
                for (const auto& waiting_key : decoder.feed(std::string_view(
                       bytes, static_cast<std::size_t>(count)))) {
                  check_quit(waiting_key);
                }
                escape_at = Clock::now();
              } else if (count == 0) {
                stopped = 1;
              } else if (errno != EINTR && errno != EAGAIN) {
                throw std::runtime_error("Could not read interactive input");
              }
            }
            if (ready > 0 &&
                (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))) {
              stopped = 1;
            }
            if (decoder.pending_escape() &&
                Clock::now() - escape_at >= std::chrono::milliseconds(25)) {
              for (const auto& waiting_key : decoder.expire_escape()) {
                check_quit(waiting_key);
              }
            }
#endif
            if (stopped) return true;
            if (!waiting_shown && Clock::now() - waiting_at >=
                                    std::chrono::milliseconds(200)) {
              interactive.message = "waiting for the task file lock…";
              size = terminal_size(options.tty);
              if (!render_frame()) {
                throw std::runtime_error("Could not render lock wait");
              }
              waiting_shown = true;
            }
            return stopped != 0;
          };
          bool completed = taskglance::update_tasks(file, [&](auto& current) {
            result = taskglance::apply_interactive_action(current, action,
                                                           last);
            return result.changed;
          }, cancel);
          if (!completed) break;
          redraw = true;
          interactive.message = result.message;
          if (result.changed) {
            if (action.type == taskglance::ActionType::Undo) {
              undo.pop_back();
            } else if (result.undo) {
              undo.push_back(*result.undo);
            }
            if (!result.selected_id.empty()) {
              interactive.selected_id = result.selected_id;
            }
          }
          reload();
        } catch (const std::exception& error) {
          redraw = true;
          interactive.message = std::string("Change failed: ") + error.what();
        }
      }
      // Filtering changes the view even when no file mutation is requested.
      taskglance::reload_interactive(interactive, tasks, options.all);
      taskglance::scroll_interactive(interactive, page_rows());
    }
  };
  while (!stopped) {
    auto now = Clock::now();
    if (Seconds(now - polled_at).count() >= interval) {
      reload();
      now = Clock::now();
    }
    if (options.tty) {
      bool resize_requested = resized != 0;
      resized = 0;
      auto next_size = terminal_size(true);
      if (resize_requested || next_size != size) {
        size = next_size;
        redraw = true;
      }
      if (highlighting && now - changed_at >= std::chrono::seconds(10)) {
        highlighting = false;
        redraw = true;
      }
    }
    if (redraw && !render_frame()) return 1;
    // Bound signal/resize latency independently of the requested file poll.
    double delay = std::min(0.05, std::max(0.0, interval -
                                      Seconds(Clock::now() - polled_at)
                                        .count()));
    if (highlighting) {
      delay = std::min(delay, std::max(0.0, 10.0 -
                                 Seconds(Clock::now() - changed_at).count()));
    }
#ifndef _WIN32
    if (options.interactive) {
      if (decoder.pending_escape()) {
        delay = std::min(delay, std::max(0.0, 0.025 -
                         Seconds(Clock::now() - escape_at).count()));
      }
      struct pollfd descriptor {STDIN_FILENO, POLLIN, 0};
      int timeout = static_cast<int>(std::ceil(delay * 1000));
      int ready = ::poll(&descriptor, 1, timeout);
      if (stopped) break;
      if (ready < 0 && errno != EINTR) {
        throw std::runtime_error("Could not poll interactive input");
      }
      if (ready > 0 && (descriptor.revents & POLLIN)) {
        char bytes[256];
        auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count > 0) {
          // Timeout is measured from the last received byte, including a
          // partial CSI sequence split over reads.
          handle_keys(decoder.feed(std::string_view(
            bytes, static_cast<std::size_t>(count))));
          escape_at = Clock::now();
        } else if (count == 0) {
          stopped = 1;
        } else if (errno != EINTR && errno != EAGAIN) {
          throw std::runtime_error("Could not read interactive input");
        }
      }
      if (ready > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        stopped = 1;
      }
      if (decoder.pending_escape() &&
          Clock::now() - escape_at >= std::chrono::milliseconds(25)) {
        handle_keys(decoder.expire_escape());
      }
    } else
#endif
    {
      std::this_thread::sleep_for(Seconds(delay));
    }
  }
  return 0;
}
