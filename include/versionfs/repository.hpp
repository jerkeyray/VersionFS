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
    std::string actor;  // "process(pid)" plus session label, when known
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
    std::uint64_t objectBytes{};  // logical bytes referenced by all versions
    std::uint64_t storedBytes{};  // physical bytes after content-addressed deduplication
    std::uint64_t events{};
    std::uint64_t sessions{};
};

// Who caused a mutation. Filled from the FUSE request context and /proc.
struct Actor {
    int pid{};
    std::uint32_t uid{};
    std::string process;
    std::string sessionId;  // empty when the process is not inside a tracked session
};

struct SessionRecord {
    std::string id;
    std::string label;
    std::string status;  // active | ended | undone
    int rootPid{};
    std::uint64_t rootStartTime{};
    std::int64_t startedNs{};
    std::int64_t endedNs{};
    std::uint64_t events{};
    std::uint64_t files{};
};

struct EventRecord {
    std::int64_t id{};
    std::int64_t timeNs{};
    std::string sessionId;
    std::string sessionLabel;
    int pid{};
    std::string process;
    std::string op;
    std::string fileId;
    std::string path;
    std::string path2;
    std::string versionId;
};

// Result of planning or applying an undo. `paths` lists mount paths whose kernel
// caches must be invalidated after the backing store changed underneath FUSE.
struct UndoReport {
    std::vector<std::string> lines;
    std::vector<std::string> conflicts;
    std::vector<std::string> paths;
    std::size_t applied{};
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

    // Preserve the current state before a destructive change. When an actor is supplied the
    // version and its provenance event commit in the same SQLite transaction.
    std::optional<VersionRecord> snapshotPath(const std::string& path, const std::string& event,
                                              const Actor* actor = nullptr);
    std::optional<VersionRecord> snapshotFd(int fd, const std::string& fileId,
                                            const std::string& event, const Actor* actor = nullptr);

    // Append a provenance event that has no pre-change snapshot (create, rename, mkdir, ...).
    // Returns the event row so a failed live operation can cancel its write-ahead intent.
    std::int64_t logEvent(const Actor& actor, const std::string& op, const std::string& fileId,
                          const std::string& path, const std::string& path2 = {});
    void cancelEvent(std::int64_t eventId);

    std::vector<VersionRecord> history(const std::string& path);
    std::vector<DeletedRecord> deletedFiles();
    VersionRecord getVersion(const std::string& path, const std::string& versionId);
    void restore(const std::string& path, const std::string& versionId,
                 const std::optional<std::string>& destination,
                 const std::function<bool(const std::string&)>& hasWriters,
                 const Actor* actor = nullptr);
    void purge(const std::string& path, const std::optional<std::string>& versionId);
    std::vector<std::string> verify(const std::optional<std::string>& path);
    RepositoryStatus status();

    // Sessions: a labelled process tree whose filesystem effects can be inspected and undone.
    std::string beginSession(const std::string& label, int rootPid, std::uint64_t rootStartTime);
    void endSession(const std::string& sessionId);
    std::vector<SessionRecord> sessions(bool activeOnly = false);
    SessionRecord session(const std::string& idOrLabel);
    std::vector<EventRecord> events(const std::optional<std::string>& path,
                                    const std::optional<std::string>& sessionId, std::size_t limit);
    UndoReport undoSession(const std::string& idOrLabel, bool dryRun, bool force, const Actor& actor,
                           const std::function<bool(const std::string&)>& hasWriters);

    std::unique_lock<std::recursive_mutex> mutationLock() {
        return std::unique_lock<std::recursive_mutex>(mutationMutex_);
    }

private:
    void initialize();
    void migrate();
    void cleanup();
    void exec(const char* sql);
    void setDurable(bool durable);
    std::optional<VersionRecord> snapshotFromFdLocked(int fd, const std::string& fileId,
                                                      const std::string& path,
                                                      const std::string& event, const Actor* actor);
    void insertEvent(const Actor& actor, const std::string& op, const std::string& fileId,
                     const std::string& path, const std::string& path2,
                     const std::string& versionId);
    void writeRestored(const VersionRecord& version, const std::filesystem::path& target);
    std::string currentPathForId(const std::string& fileId);

    std::filesystem::path sourceRoot_;
    std::filesystem::path metadataRoot_;
    std::filesystem::path objectsRoot_;
    std::filesystem::path tempRoot_;
    sqlite3* db_{nullptr};
    bool durable_{true};
    std::recursive_mutex mutationMutex_;
};

}  // namespace versionfs
