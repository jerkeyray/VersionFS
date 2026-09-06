#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "versionfs/fuse_state.hpp"

#include "versionfs/util.hpp"

#include <fuse3/fuse.h>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace versionfs {
namespace {

FuseState& state() { return *static_cast<FuseState*>(fuse_get_context()->private_data); }

bool hidden(const char* path) {
    try { return isInternalPath(path); } catch (...) { return true; }
}

std::filesystem::path fullPath(const char* path) {
    return backingPath(state().repository().sourceRoot(), path);
}

FileHandle* handle(fuse_file_info* info) {
    return reinterpret_cast<FileHandle*>(static_cast<std::uintptr_t>(info->fh));
}

int failure() { return -errno; }

int vfsGetattr(const char* path, struct stat* info, fuse_file_info*) {
    if (hidden(path)) return -ENOENT;
    return ::lstat(fullPath(path).c_str(), info) == 0 ? 0 : failure();
}

int vfsAccess(const char* path, int mask) {
    if (hidden(path)) return -ENOENT;
    return ::access(fullPath(path).c_str(), mask) == 0 ? 0 : failure();
}

int vfsReadlink(const char* path, char* buffer, std::size_t size) {
    if (hidden(path)) return -ENOENT;
    const ssize_t count = ::readlink(fullPath(path).c_str(), buffer, size - 1);
    if (count < 0) return failure();
    buffer[count] = '\0';
    return 0;
}

int vfsMknod(const char* path, mode_t mode, dev_t device) {
    if (hidden(path)) return -EACCES;
    auto lock = state().repository().mutationLock();
    int result;
    const auto full = fullPath(path);
    if (S_ISREG(mode)) {
        int fd = ::open(full.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
        result = fd < 0 ? -1 : (::close(fd), 0);
    } else if (S_ISFIFO(mode)) result = ::mkfifo(full.c_str(), mode);
    else result = ::mknod(full.c_str(), mode, device);
    if (result != 0) return failure();
    if (S_ISREG(mode)) state().repository().ensureFile(path);
    return 0;
}

int vfsMkdir(const char* path, mode_t mode) {
    if (hidden(path)) return -EACCES;
    auto lock = state().repository().mutationLock();
    return ::mkdir(fullPath(path).c_str(), mode) == 0 ? 0 : failure();
}

int vfsUnlink(const char* path) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        auto snapshot = state().repository().snapshotPath(path, "delete");
        if (::unlink(fullPath(path).c_str()) != 0) return failure();
        if (snapshot) state().repository().markDeleted(snapshot->fileId, path);
        return 0;
    } catch (...) { return -EIO; }
}

int vfsRmdir(const char* path) {
    if (hidden(path)) return -EACCES;
    auto lock = state().repository().mutationLock();
    return ::rmdir(fullPath(path).c_str()) == 0 ? 0 : failure();
}

int vfsSymlink(const char* target, const char* path) {
    if (hidden(path)) return -EACCES;
    auto lock = state().repository().mutationLock();
    return ::symlink(target, fullPath(path).c_str()) == 0 ? 0 : failure();
}

int vfsRename(const char* from, const char* to, unsigned int flags) {
    if (hidden(from) || hidden(to)) return -EACCES;
    // Exchange and whiteout rename flags require two-identity transactional metadata updates.
    // They are intentionally outside the v1 compatibility surface.
    if (flags != 0) return -EINVAL;
    try {
        auto lock = state().repository().mutationLock();
        struct stat sourceInfo{};
        if (::lstat(fullPath(from).c_str(), &sourceInfo) != 0) return failure();
        if (S_ISREG(sourceInfo.st_mode)) state().repository().ensureFile(from);
        state().repository().snapshotPath(to, "rename-overwrite");
        int result = ::rename(fullPath(from).c_str(), fullPath(to).c_str());
        if (result != 0) return failure();
        state().repository().renamePath(from, to, S_ISDIR(sourceInfo.st_mode));
        return 0;
    } catch (...) { return -EIO; }
}

template <typename Operation>
int metadataMutation(const char* path, const char* event, Operation operation) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        state().repository().snapshotPath(path, event);
        return operation() == 0 ? 0 : failure();
    } catch (...) { return -EIO; }
}

int vfsChmod(const char* path, mode_t mode, fuse_file_info*) {
    return metadataMutation(path, "chmod", [&] { return ::chmod(fullPath(path).c_str(), mode); });
}

int vfsChown(const char* path, uid_t uid, gid_t gid, fuse_file_info*) {
    return metadataMutation(path, "chown", [&] { return ::lchown(fullPath(path).c_str(), uid, gid); });
}

int vfsTruncate(const char* path, off_t size, fuse_file_info* info) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        if (info && info->fh) {
            auto* open = handle(info);
            if (!open->snapshotted) {
                state().repository().snapshotFd(open->fd, open->fileId, "truncate");
                open->snapshotted = true;
            }
            return ::ftruncate(open->fd, size) == 0 ? 0 : failure();
        }
        state().repository().snapshotPath(path, "truncate");
        return ::truncate(fullPath(path).c_str(), size) == 0 ? 0 : failure();
    } catch (...) { return -EIO; }
}

int vfsOpen(const char* path, fuse_file_info* info) {
    if (hidden(path)) return -ENOENT;
    try {
        auto lock = state().repository().mutationLock();
        const bool writable = (info->flags & O_ACCMODE) != O_RDONLY;
        struct stat before{};
        const bool existed = ::lstat(fullPath(path).c_str(), &before) == 0;
        bool snapshotted = !existed;
        std::string id;
        if (writable && (info->flags & O_TRUNC)) {
            auto snapshot = state().repository().snapshotPath(path, "truncate");
            snapshotted = snapshot.has_value();
        }
        int fd = ::open(fullPath(path).c_str(), info->flags | O_CLOEXEC);
        if (fd < 0) return failure();
        try { id = state().repository().ensureFile(path); }
        catch (...) { ::close(fd); throw; }
        auto* open = new FileHandle{fd, id, writable, snapshotted};
        info->fh = reinterpret_cast<std::uintptr_t>(open);
        if (writable) state().addWriter(id);
        return 0;
    } catch (...) { return -EIO; }
}

int vfsCreate(const char* path, mode_t mode, fuse_file_info* info) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        struct stat before{};
        const bool existed = ::lstat(fullPath(path).c_str(), &before) == 0;
        bool snapshotted = !existed;
        if (existed && (info->flags & O_TRUNC)) {
            snapshotted = state().repository().snapshotPath(path, "truncate").has_value();
        }
        int fd = ::open(fullPath(path).c_str(), info->flags | O_CREAT | O_CLOEXEC, mode);
        if (fd < 0) return failure();
        std::string id;
        try { id = state().repository().ensureFile(path); }
        catch (...) { ::close(fd); ::unlink(fullPath(path).c_str()); throw; }
        const bool writable = (info->flags & O_ACCMODE) != O_RDONLY;
        auto* open = new FileHandle{fd, id, writable, snapshotted};
        info->fh = reinterpret_cast<std::uintptr_t>(open);
        if (writable) state().addWriter(id);
        return 0;
    } catch (...) { return -EIO; }
}

int vfsRead(const char*, char* buffer, std::size_t size, off_t offset, fuse_file_info* info) {
    const ssize_t result = ::pread(handle(info)->fd, buffer, size, offset);
    return result < 0 ? failure() : static_cast<int>(result);
}

int vfsWrite(const char*, const char* buffer, std::size_t size, off_t offset, fuse_file_info* info) {
    try {
        auto lock = state().repository().mutationLock();
        auto* open = handle(info);
        if (!open->snapshotted) {
            state().repository().snapshotFd(open->fd, open->fileId, "write");
            open->snapshotted = true;
        }
        crashIfRequested("before-live-write");
        const ssize_t result = ::pwrite(open->fd, buffer, size, offset);
        return result < 0 ? failure() : static_cast<int>(result);
    } catch (...) { return -EIO; }
}

int vfsStatfs(const char* path, struct statvfs* info) {
    if (hidden(path)) return -ENOENT;
    return ::statvfs(fullPath(path).c_str(), info) == 0 ? 0 : failure();
}

int vfsFlush(const char*, fuse_file_info* info) {
    int duplicate = ::dup(handle(info)->fd);
    return duplicate < 0 ? failure() : (::close(duplicate), 0);
}

int vfsRelease(const char*, fuse_file_info* info) {
    auto* open = handle(info);
    if (open->writable) state().removeWriter(open->fileId);
    const int result = ::close(open->fd);
    delete open;
    info->fh = 0;
    return result == 0 ? 0 : failure();
}

int vfsFsync(const char*, int dataOnly, fuse_file_info* info) {
    int result = dataOnly ? ::fdatasync(handle(info)->fd) : ::fsync(handle(info)->fd);
    return result == 0 ? 0 : failure();
}

int vfsUtimens(const char* path, const struct timespec times[2], fuse_file_info*) {
    return metadataMutation(path, "utimens", [&] {
        return ::utimensat(AT_FDCWD, fullPath(path).c_str(), times, AT_SYMLINK_NOFOLLOW);
    });
}

int vfsOpendir(const char* path, fuse_file_info* info) {
    if (hidden(path)) return -ENOENT;
    DIR* directory = ::opendir(fullPath(path).c_str());
    if (!directory) return failure();
    info->fh = reinterpret_cast<std::uintptr_t>(directory);
    return 0;
}

int vfsReaddir(const char*, void* buffer, fuse_fill_dir_t filler, off_t offset,
               fuse_file_info* info, fuse_readdir_flags) {
    DIR* directory = reinterpret_cast<DIR*>(static_cast<std::uintptr_t>(info->fh));
    if (offset != 0) ::seekdir(directory, offset);
    errno = 0;
    while (dirent* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".versionfs") == 0) continue;
        struct stat statInfo{};
        statInfo.st_ino = entry->d_ino;
        statInfo.st_mode = static_cast<mode_t>(entry->d_type) << 12;
        const off_t next = ::telldir(directory);
        if (filler(buffer, entry->d_name, &statInfo, next, FUSE_FILL_DIR_DEFAULTS)) break;
    }
    return errno == 0 ? 0 : failure();
}

int vfsReleasedir(const char*, fuse_file_info* info) {
    DIR* directory = reinterpret_cast<DIR*>(static_cast<std::uintptr_t>(info->fh));
    return ::closedir(directory) == 0 ? 0 : failure();
}

void* vfsInit(fuse_conn_info*, fuse_config* config) {
    config->use_ino = 1;
    config->nullpath_ok = 1;
    auto& current = state();
    current.startControl();
    return &current;
}

void vfsDestroy(void* privateData) { static_cast<FuseState*>(privateData)->stopControl(); }

}  // namespace

FuseState::FuseState(std::filesystem::path source) : repository_(std::move(source)) {}
FuseState::~FuseState() { stopControl(); }

void FuseState::startControl() {
    if (!control_) control_ = std::make_unique<ControlServer>(
        repository_.socketPath(), [this](const auto& args) { return handleControl(args); });
    control_->start();
}

void FuseState::stopControl() { if (control_) control_->stop(); }

void FuseState::addWriter(const std::string& fileId) {
    std::lock_guard<std::mutex> guard(writersMutex_); ++writers_[fileId];
}

void FuseState::removeWriter(const std::string& fileId) {
    std::lock_guard<std::mutex> guard(writersMutex_);
    auto found = writers_.find(fileId);
    if (found != writers_.end() && --found->second == 0) writers_.erase(found);
}

bool FuseState::hasWriters(const std::string& path) {
    auto id = repository_.findFileId(path, false);
    if (!id) return false;
    std::lock_guard<std::mutex> guard(writersMutex_);
    return writers_.count(*id) != 0;
}

ControlResponse FuseState::handleControl(const std::vector<std::string>& args) {
    if (args.empty()) return {false, "missing command"};
    std::ostringstream out;
    if (args[0] == "history" && args.size() == 2) {
        out << "sequence\tversion\tevent\tsize\tcreated\tpath\n";
        for (const auto& version : repository_.history(args[1]))
            out << version.sequence << '\t' << version.id << '\t' << version.event << '\t'
                << version.size << '\t' << formatTimestamp(version.createdNs) << '\t'
                << version.path << '\n';
    } else if (args[0] == "show" && args.size() == 3) {
        auto version = repository_.getVersion(args[1], args[2]);
        std::ifstream input(version.objectPath, std::ios::binary);
        if (!input) return {false, "snapshot object is unavailable"};
        out << input.rdbuf();
    } else if (args[0] == "restore" && (args.size() == 3 || args.size() == 4)) {
        repository_.restore(args[1], args[2], args.size() == 4 ? std::optional<std::string>(args[3]) : std::nullopt,
                            [this](const auto& path) { return hasWriters(path); });
        out << "restored " << args[2] << " to " << (args.size() == 4 ? args[3] : args[1]) << '\n';
    } else if (args[0] == "deleted" && args.size() == 1) {
        out << "deleted\tfile-id\tpath\n";
        for (const auto& file : repository_.deletedFiles())
            out << formatTimestamp(file.deletedNs) << '\t' << file.fileId << '\t' << file.path << '\n';
    } else if (args[0] == "status" && args.size() == 1) {
        const auto value = repository_.status();
        out << "source\t" << repository_.sourceRoot() << "\nfiles\t" << value.files
            << "\ndeleted\t" << value.deletedFiles << "\nversions\t" << value.versions
            << "\nobject-bytes\t" << value.objectBytes << '\n';
    } else if (args[0] == "purge" && args.size() == 3) {
        repository_.purge(args[1], args[2] == "--all" ? std::nullopt : std::optional<std::string>(args[2]));
        out << "history purged\n";
    } else if (args[0] == "verify" && (args.size() == 1 || args.size() == 2)) {
        auto errors = repository_.verify(args.size() == 2 ? std::optional<std::string>(args[1]) : std::nullopt);
        if (errors.empty()) out << "ok\n";
        else { for (const auto& error : errors) out << error << '\n'; return {false, out.str()}; }
    } else return {false, "invalid command or arguments"};
    return {true, out.str()};
}

const fuse_operations& operations() {
    static const fuse_operations value = [] {
        fuse_operations ops{};
        ops.getattr = vfsGetattr; ops.readlink = vfsReadlink; ops.mknod = vfsMknod;
        ops.mkdir = vfsMkdir; ops.unlink = vfsUnlink; ops.rmdir = vfsRmdir;
        ops.symlink = vfsSymlink; ops.rename = vfsRename; ops.chmod = vfsChmod;
        ops.chown = vfsChown; ops.truncate = vfsTruncate; ops.open = vfsOpen;
        ops.read = vfsRead; ops.write = vfsWrite; ops.statfs = vfsStatfs;
        ops.flush = vfsFlush; ops.release = vfsRelease; ops.fsync = vfsFsync;
        ops.utimens = vfsUtimens; ops.access = vfsAccess; ops.create = vfsCreate;
        ops.opendir = vfsOpendir; ops.readdir = vfsReaddir; ops.releasedir = vfsReleasedir;
        ops.init = vfsInit; ops.destroy = vfsDestroy;
        return ops;
    }();
    return value;
}

}  // namespace versionfs
