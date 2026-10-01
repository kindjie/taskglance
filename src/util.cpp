#include "taskglance/util.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
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
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace taskglance {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

int stdout_fd() {
#ifdef _WIN32
  return _fileno(stdout);
#else
  return STDOUT_FILENO;
#endif
}

int process_id() {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}

int char_display_width(wchar_t wide) {
#ifdef _WIN32
  return wide == 0 ? 0 : 1;
#else
  int width = ::wcwidth(wide);
  return width > 0 ? width : 0;
#endif
}


// Flushes fd to stable storage. On macOS fsync() can leave data in the
// drive's volatile cache, so prefer F_FULLFSYNC where the filesystem has it.
bool sync_fd(int fd) {
#ifdef _WIN32
  return ::_commit(fd) == 0;
#else
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return true;
  }
#endif
  return ::fsync(fd) == 0;
#endif
}

void write_synced_file(const std::filesystem::path& file,
                       const std::string& content) {
#ifdef _WIN32
  int fd = ::_wopen(file.c_str(),
                    _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                    _S_IREAD | _S_IWRITE);
#else
  int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  0666);
#endif
  if (fd < 0) {
    throw std::runtime_error("Could not write " + file.string());
  }
  const char* data = content.data();
  std::size_t remaining = content.size();
  bool ok = true;
  while (ok && remaining > 0) {
#ifdef _WIN32
    auto chunk = static_cast<unsigned int>(
      std::min<std::size_t>(remaining, INT_MAX)
    );
    auto written = ::_write(fd, data, chunk);
#else
    auto written = ::write(fd, data, remaining);
    if (written < 0 && errno == EINTR) {
      continue;
    }
#endif
    ok = written > 0;
    if (ok) {
      data += written;
      remaining -= static_cast<std::size_t>(written);
    }
  }
  ok = ok && sync_fd(fd);
#ifdef _WIN32
  ok = ::_close(fd) == 0 && ok;
#else
  ok = ::close(fd) == 0 && ok;
#endif
  if (!ok) {
    throw std::runtime_error("Could not write " + file.string());
  }
}

// Replaces to with from and makes the rename durable. Failing to sync the
// directory is not fatal: the new content is already in place, so reporting
// failure would misstate what happened.
void durable_rename(const std::filesystem::path& from,
                    const std::filesystem::path& to) {
#ifdef _WIN32
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (::MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return;
    }
    auto error = ::GetLastError();
    if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) ||
        attempt == 9) {
      throw std::runtime_error("Could not replace " + to.string());
    }
    ::Sleep(10);
  }
#else
  std::filesystem::rename(from, to);
  auto parent = to.parent_path();
  int fd = ::open(parent.empty() ? "." : parent.c_str(),
                  O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif
}

}  // namespace

std::string trim(const std::string& value) {
  auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::stringstream stream(value);
  while (std::getline(stream, current, delimiter)) {
    parts.push_back(current);
  }
  if (!value.empty() && value.back() == delimiter) {
    parts.emplace_back();
  }
  return parts;
}

std::string join(const std::vector<std::string>& values,
                 const std::string& delimiter) {
  std::string output;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      output += delimiter;
    }
    output += values[i];
  }
  return output;
}

std::string to_lower(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool is_truthy_env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  auto normalized = to_lower(trim(value));
  return normalized == "1" || normalized == "true" ||
         normalized == "yes" || normalized == "on";
}

bool stdout_is_tty() {
  int fd = stdout_fd();
#ifdef _WIN32
  return fd >= 0 && _isatty(fd) == 1;
#else
  return fd >= 0 && ::isatty(fd) == 1;
#endif
}

int terminal_columns() {
#ifndef _WIN32
  struct winsize size {};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
      size.ws_col > 0) {
    return static_cast<int>(size.ws_col);
  }
#endif

  const char* columns = std::getenv("COLUMNS");
  if (columns != nullptr) {
    try {
      int parsed = std::stoi(columns);
      if (parsed > 0) {
        return parsed;
      }
    } catch (...) {
    }
  }
  return 80;
}

std::string time_to_iso(std::chrono::system_clock::time_point time) {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
    time.time_since_epoch()
  ).count();
  return std::to_string(seconds);
}

std::chrono::system_clock::time_point time_from_iso(const std::string& value) {
  long long seconds = 0;
  try {
    seconds = std::stoll(value);
  } catch (...) {
    seconds = 0;
  }
  return std::chrono::system_clock::time_point{
    std::chrono::seconds(seconds)
  };
}

void ensure_parent_dir(const std::filesystem::path& file) {
  auto parent = file.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
}

void atomic_write_file(const std::filesystem::path& file,
                       const std::string& content) {
  ensure_parent_dir(file);
  auto temp = file;
  temp += ".tmp." + std::to_string(process_id());
  write_synced_file(temp, content);
  durable_rename(temp, file);
}

FileLock::FileLock(const std::filesystem::path& path) : FileLock(path, {}) {}

#ifdef _WIN32
FileLock::FileLock(const std::filesystem::path& path,
                   const std::function<bool()>& cancel) {
  ensure_parent_dir(path);
  handle_ = ::CreateFileW(
    path.c_str(), GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
  );
  if (handle_ == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not open lock " + path.string());
  }
  try {
    OVERLAPPED overlapped {};
    auto flags = LOCKFILE_EXCLUSIVE_LOCK |
                 (cancel ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    while (!cancel || !cancel()) {
      if (::LockFileEx(handle_, flags, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        acquired_ = true;
        break;
      }
      if (!cancel || ::GetLastError() != ERROR_LOCK_VIOLATION) {
        throw std::runtime_error("Could not lock " + path.string());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  } catch (...) {
    ::CloseHandle(handle_);
    throw;
  }
}

FileLock::~FileLock() {
  // Closing the handle releases the lock.
  ::CloseHandle(handle_);
}
#else
FileLock::FileLock(const std::filesystem::path& path,
                   const std::function<bool()>& cancel) {
  ensure_parent_dir(path);
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    throw std::runtime_error("Could not open lock " + path.string() + ": " +
                             std::strerror(errno));
  }
  try {
    auto flags = LOCK_EX | (cancel ? LOCK_NB : 0);
    while (!cancel || !cancel()) {
      if (::flock(fd_, flags) == 0) {
        acquired_ = true;
        break;
      }
      auto error = errno;
      if (error != EINTR &&
          !(cancel && (error == EWOULDBLOCK || error == EAGAIN))) {
        throw std::runtime_error("Could not lock " + path.string() + ": " +
                                 std::strerror(error));
      }
      if (cancel) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
      }
    }
  } catch (...) {
    ::close(fd_);
    throw;
  }
}

FileLock::~FileLock() {
  // Closing the descriptor releases the lock.
  ::close(fd_);
}
#endif

std::uint64_t fnv1a64(const std::string& value) {
  std::uint64_t hash = kFnvOffset;
  for (unsigned char ch : value) {
    hash ^= ch;
    hash *= kFnvPrime;
  }
  return hash;
}

std::string hex_short(std::uint64_t value, int width) {
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setw(width)
      << std::setfill('0') << value;
  auto text = out.str();
  if (static_cast<int>(text.size()) > width) {
    return text.substr(text.size() - static_cast<std::size_t>(width));
  }
  return text;
}

int display_width(const std::string& value) {
  std::setlocale(LC_CTYPE, "");
  std::mbstate_t state{};
  const char* cursor = value.c_str();
  std::size_t remaining = value.size();
  int width = 0;

  while (remaining > 0) {
    wchar_t wide = 0;
    std::size_t consumed = std::mbrtowc(&wide, cursor, remaining, &state);
    if (consumed == static_cast<std::size_t>(-1) ||
        consumed == static_cast<std::size_t>(-2)) {
      ++cursor;
      --remaining;
      ++width;
      std::memset(&state, 0, sizeof(state));
      continue;
    }
    if (consumed == 0) {
      break;
    }
    width += char_display_width(wide);
    cursor += consumed;
    remaining -= consumed;
  }
  return width;
}

std::string truncate_display(const std::string& value, int width) {
  if (width <= 0) {
    return "";
  }
  if (display_width(value) <= width) {
    return value;
  }
  if (width <= 3) {
    return value.substr(0, static_cast<std::size_t>(width));
  }

  std::string output;
  std::setlocale(LC_CTYPE, "");
  std::mbstate_t state{};
  const char* cursor = value.c_str();
  std::size_t remaining = value.size();
  int used = 0;
  int limit = width - 3;

  while (remaining > 0) {
    wchar_t wide = 0;
    std::size_t consumed = std::mbrtowc(&wide, cursor, remaining, &state);
    if (consumed == static_cast<std::size_t>(-1) ||
        consumed == static_cast<std::size_t>(-2)) {
      consumed = 1;
      std::memset(&state, 0, sizeof(state));
    } else if (consumed == 0) {
      break;
    }
    int char_width = char_display_width(wide);
    if (used + char_width > limit) {
      break;
    }
    output.append(cursor, consumed);
    used += char_width;
    cursor += consumed;
    remaining -= consumed;
  }

  output += "...";
  return output;
}

}  // namespace taskglance
