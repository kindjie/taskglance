#include "watch_terminal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <sys/ioctl.h>
#include <unistd.h>
#endif

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

class WatchScreen {
 public:
  explicit WatchScreen(bool tty) : tty_(tty) {
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
    std::fflush(stdout);
  }

  ~WatchScreen() {
    if (tty_) {
      std::fputs("\033[0m\033[?25h\033[?1049l", stdout);
      std::fflush(stdout);
#ifdef _WIN32
      ::SetConsoleMode(handle_, mode_);
#endif
    }
  }

 private:
  bool tty_;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  DWORD mode_ = 0;
#endif
};

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
  WatchSignals signals;
  WatchScreen screen(options.tty);
  auto content = taskglance::read_task_file(file);
  auto tasks = taskglance::parse_tasks(content);
  auto last_change = std::chrono::system_clock::now();
  auto changed_at = Clock::now();
  auto polled_at = changed_at;
  std::vector<std::string> changed_ids;
  auto size = terminal_size(options.tty);
  bool redraw = true;
  bool first = true;
  bool highlighting = false;
  while (!stopped) {
    auto now = Clock::now();
    if (Seconds(now - polled_at).count() >= interval) {
      auto next = taskglance::read_task_file(file);
      polled_at = now;
      if (next != content) {
        auto updated = taskglance::parse_tasks(next);
        changed_ids = taskglance::detect_task_changes(updated, tasks)
                        .changed_ids;
        tasks = std::move(updated);
        content = std::move(next);
        last_change = std::chrono::system_clock::now();
        changed_at = now;
        highlighting = options.tty && !changed_ids.empty();
        redraw = true;
      }
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
    if (redraw) {
      auto frame = taskglance::build_watch_frame(
        tasks, changed_ids, size.width, size.height, last_change,
        Seconds(now - changed_at), options
      );
      if (options.tty) {
        std::cout << "\033[H\033[2J";
        for (char ch : frame) {
          if (ch == '\n') {
            std::cout << '\r';
          }
          std::cout << ch;
        }
      } else {
        if (!first) {
          std::cout << '\n';
        }
        std::cout << frame << '\n';
      }
      std::cout.flush();
      if (!std::cout) {
        return 1;
      }
      first = false;
      redraw = false;
    }
    // Bound signal/resize latency independently of the requested file poll.
    double delay = std::min(0.05, std::max(0.0, interval -
                                      Seconds(Clock::now() - polled_at)
                                        .count()));
    if (highlighting) {
      delay = std::min(delay, std::max(0.0, 10.0 -
                                 Seconds(Clock::now() - changed_at).count()));
    }
    std::this_thread::sleep_for(Seconds(delay));
  }
  return 0;
}
