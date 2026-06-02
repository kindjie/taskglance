#include "taskglance/util.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace taskglance {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

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
  return ::isatty(STDOUT_FILENO) == 1;
}

int terminal_columns() {
  struct winsize size {};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
      size.ws_col > 0) {
    return static_cast<int>(size.ws_col);
  }

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
  temp += ".tmp." + std::to_string(::getpid());
  {
    std::ofstream out(temp, std::ios::binary);
    if (!out) {
      throw std::runtime_error("Could not write " + temp.string());
    }
    out << content;
    out.flush();
    if (!out) {
      throw std::runtime_error("Could not flush " + temp.string());
    }
  }
  std::filesystem::rename(temp, file);
}

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
    int char_width = ::wcwidth(wide);
    width += char_width > 0 ? char_width : 0;
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
    int char_width = ::wcwidth(wide);
    if (char_width < 0) {
      char_width = 0;
    }
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
