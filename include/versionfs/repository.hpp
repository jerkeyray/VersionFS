#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

namespace versionfs {

struct VersionRecord {
    std::string id;
    std::string fileId;
    std::int64_t sequence{};
    std::string event;
    std::string path;
    std::filesystem::path objectPath;
    std::string checksum;
    std::int64_t size{};
    std::uint32_t mode{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::int64_t mtimeSec{};
    std::int64_t mtimeNsec{};
    std::int64_t createdNs{};
};

struct DeletedRecord {
    std::string fileId;
    std::string path;
    std::int64_t deletedNs{};
};

struct RepositoryStatus {
    std::uint64_t files{};
    std::uint64_t deletedFiles{};
    std::uint64_t versions{};
    std::uint64_t objectBytes{};
};

class Repository {
public:
    explicit Repository(std::filesystem::path sourceRoot);
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    const std::filesystem::path& sourceRoot() const { return sourceRoot_; }
    const std::filesystem::path& metadataRoot() const { return metadataRoot_; }
    std::filesystem::path socketPath() const;

    std::string ensureFile(const std::string& path);
    std::optional<std::string> findFileId(const std::string& path, bool includeDeleted = true);
    void renamePath(const std::string& from, const std::string& to, bool directory);
    void markDeleted(const std::string& fileId, const std::string& path);
    void reactivate(const std::string& fileId, const std::string& path);

    std::optional<VersionRecord> snapshotPath(const std::string& path, const std::string& event);
    std::optional<VersionRecord> snapshotFd(int fd, const std::string& fileId,
                                            const std::string& event);
    std::vector<VersionRecord> history(const std::string& path);
    std::vector<DeletedRecord> deletedFiles();
    VersionRecord getVersion(const std::string& path, const std::string& versionId);
    void restore(const std::string& path, const std::string& versionId,
                 const std::optional<std::string>& destination,
                 const std::function<bool(const std::string&)>& hasWriters);
    void purge(const std::string& path, const std::optional<std::string>& versionId);
    std::vector<std::string> verify(const std::optional<std::string>& path);
    RepositoryStatus status();

    std::unique_lock<std::recursive_mutex> mutationLock() {
        return std::unique_lock<std::recursive_mutex>(mutationMutex_);
    }

private:
    void initialize();
    void cleanup();
    void exec(const char* sql);
    std::optional<VersionRecord> snapshotFromFdLocked(int fd, const std::string& fileId,
                                                      const std::string& path,
                                                      const std::string& event);
    std::string currentPathForId(const std::string& fileId);

    std::filesystem::path sourceRoot_;
    std::filesystem::path metadataRoot_;
    std::filesystem::path objectsRoot_;
    std::filesystem::path tempRoot_;
    sqlite3* db_{nullptr};
    std::recursive_mutex mutationMutex_;
};

}  // namespace versionfs

