#pragma once

#include <ctime>
#include <string>

namespace vram::util {
/// Get current time with nanosecond precision
timespec time();

/// Split path/to/file.txt into "path/to" and "file.txt"
void split_file_path(const std::string& path, std::string& dir, std::string& file);
}