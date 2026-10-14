#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace versionfs {

std::string normalizePath(const std::string& path);
bool isInternalPath(const std::string& path);
std::filesystem::path backingPath(const std::filesystem::path& root,
                                  const std::string& virtualPath);
std::string randomId();
std::int64_t nowNanoseconds();
std::string sha256File(const std::filesystem::path& path);

// Incremental SHA-256 so callers can hash data while copying it (one read pass, not two).
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, std::size_t size);
    std::string hex();

private:
    void* context_;
};
std::string formatTimestamp(std::int64_t nanoseconds);
void crashIfRequested(const char* point);

// A snapshot of one /proc/<pid>/stat entry. starttime disambiguates reused PIDs.
struct ProcessInfo {
    int pid{};
    int ppid{};
    std::uint64_t startTime{};
    std::string name;
};

// Reads /proc/<pid>/stat. Returns nullopt for exited processes and on non-Linux hosts.
std::optional<ProcessInfo> readProcess(int pid);

// Thread-group (process) id of a thread id, from /proc/<tid>/status; returns tid if unknown.
int threadGroupOf(int tid);

}  // namespace versionfs
