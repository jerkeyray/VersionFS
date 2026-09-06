#pragma once

#include "versionfs/control.hpp"
#include "versionfs/repository.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct fuse_operations;

namespace versionfs {

struct FileHandle {
    int fd{-1};
    std::string fileId;
    bool writable{false};
    bool snapshotted{false};
};

class FuseState {
public:
    explicit FuseState(std::filesystem::path source);
    ~FuseState();
    Repository& repository() { return repository_; }
    void startControl();
    void stopControl();
    void addWriter(const std::string& fileId);
    void removeWriter(const std::string& fileId);
    bool hasWriters(const std::string& path);

private:
    ControlResponse handleControl(const std::vector<std::string>& arguments);
    Repository repository_;
    std::unique_ptr<ControlServer> control_;
    std::mutex writersMutex_;
    std::unordered_map<std::string, std::size_t> writers_;
};

const fuse_operations& operations();

}  // namespace versionfs

