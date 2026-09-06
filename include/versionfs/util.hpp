#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace versionfs {

std::string normalizePath(const std::string& path);
bool isInternalPath(const std::string& path);
std::filesystem::path backingPath(const std::filesystem::path& root,
                                  const std::string& virtualPath);
std::string randomId();
std::int64_t nowNanoseconds();
std::string sha256File(const std::filesystem::path& path);
std::string formatTimestamp(std::int64_t nanoseconds);
void crashIfRequested(const char* point);

}  // namespace versionfs
