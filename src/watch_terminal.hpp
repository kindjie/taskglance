#pragma once

#include <filesystem>

#include "taskglance/watch.hpp"

// Terminal and process state belong to the executable, not the core library.
int run_watch(const std::filesystem::path& file, double interval,
              taskglance::WatchOptions options);
