#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace taskglance {

std::string trim(const std::string& value);
std::vector<std::string> split(const std::string& value, char delimiter);
std::string join(const std::vector<std::string>& values,
                 const std::string& delimiter);

std::string to_lower(std::string value);
bool is_truthy_env(const char* name);
bool stdout_is_tty();
int terminal_columns();

std::string time_to_iso(std::chrono::system_clock::time_point time);
std::chrono::system_clock::time_point time_from_iso(const std::string& value);

void ensure_parent_dir(const std::filesystem::path& file);
// Replaces file with content via a synced temporary file and a rename, so
// readers see either the old or the new content and a crash cannot leave a
// truncated file.
void atomic_write_file(const std::filesystem::path& file,
                       const std::string& content);

// Holds an exclusive advisory lock on path, created if missing, until
// destroyed. Every holder must open its own FileLock: the lock belongs to
// the open handle, so it also excludes other threads in the same process.
class FileLock {
 public:
  explicit FileLock(const std::filesystem::path& path);
  // Polls every 15 ms when cancel is supplied. Cancellation leaves
  // acquired() false; exceptions from cancel propagate without leaking.
  FileLock(const std::filesystem::path& path,
           const std::function<bool()>& cancel);
  bool acquired() const { return acquired_; }
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  bool acquired_ = false;
#ifdef _WIN32
  void* handle_;
#else
  int fd_;
#endif
};

std::uint64_t fnv1a64(const std::string& value);
std::string hex_short(std::uint64_t value, int width);

int display_width(const std::string& value);
std::string truncate_display(const std::string& value, int width);

}  // namespace taskglance
