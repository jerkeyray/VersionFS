#pragma once

#include "versionfs/control.hpp"
#include "versionfs/repository.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

struct fuse;
struct fuse_operations;

namespace versionfs {

struct FileHandle {
    FileHandle(int descriptor, std::string id, bool canWrite, bool alreadySnapshotted, bool creator,
               std::shared_ptr<std::shared_mutex> fileLock)
        : fd(descriptor), fileId(std::move(id)), writable(canWrite), created(creator),
          snapshotted(alreadySnapshotted), lock(std::move(fileLock)) {}
    int fd{-1};
    std::string fileId;
    bool writable{false};
    bool created{false};  // this handle created the file; it is "under construction" until release
    std::atomic<bool> snapshotted{false};
    // Per-file reader-writer lock: data writes share it, pre-change snapshots take it exclusively.
    std::shared_ptr<std::shared_mutex> lock;
};

// Exclusive hold on one file's lock; empty when no handle has the file open.
struct FileGuard {
    std::shared_ptr<std::shared_mutex> mutex;
    std::unique_lock<std::shared_mutex> lock;
};

class FuseState {
public:
    explicit FuseState(std::filesystem::path source);
    ~FuseState();
    Repository& repository() { return repository_; }
    void startControl(struct fuse* instance);
    void stopControl();
    void addWriter(const std::string& fileId);
    void removeWriter(const std::string& fileId);
    bool hasWriters(const std::string& path);
    void beginConstruction(const std::string& fileId);
    void endConstruction(const std::string& fileId);
    bool underConstruction(const std::string& path);

    std::shared_ptr<std::shared_mutex> fileLock(const std::string& fileId);
    FileGuard exclusiveFile(const std::string& path);

    // Attribute an operation to the calling process and, through its ancestry, to a session.
    Actor actorFor(int pid, std::uint32_t uid);
    Actor currentActor();

private:
    struct SessionRoot {
        std::string id;
        std::uint64_t startTime{};
    };
    struct CachedSession {
        std::uint64_t startTime{};
        std::string sessionId;
    };

    std::string sessionFor(int pid);
    void invalidate(const std::vector<std::string>& paths);
    ControlResponse handleControl(const ControlPeer& peer, const std::vector<std::string>& arguments);

    Repository repository_;
    std::unique_ptr<ControlServer> control_;
    struct fuse* fuse_{nullptr};
    std::mutex writersMutex_;
    std::unordered_map<std::string, std::size_t> writers_;
    std::unordered_map<std::string, std::size_t> constructing_;  // guarded by writersMutex_
    std::mutex locksMutex_;
    std::unordered_map<std::string, std::weak_ptr<std::shared_mutex>> fileLocks_;
    std::mutex sessionsMutex_;
    std::unordered_map<int, SessionRoot> roots_;            // root pid -> session
    std::unordered_map<int, CachedSession> sessionCache_;   // pid -> resolved session
};

const fuse_operations& operations();

}  // namespace versionfs
