#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "versionfs/fuse_state.hpp"

#include "versionfs/util.hpp"

#include <fuse3/fuse.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace versionfs {
namespace {

FuseState& state() { return *static_cast<FuseState*>(fuse_get_context()->private_data); }

bool hidden(const char* path) {
    if (!path) return true;
    try { return isInternalPath(path); } catch (...) { return true; }
}

std::filesystem::path fullPath(const char* path) {
    return backingPath(state().repository().sourceRoot(), path);
}

FileHandle* handle(fuse_file_info* info) {
    return reinterpret_cast<FileHandle*>(static_cast<std::uintptr_t>(info->fh));
}

int failure() { return -errno; }

int vfsGetattr(const char* path, struct stat* info, fuse_file_info* file) {
    // With nullpath_ok, fstat() on an open file arrives with a NULL path and a file handle.
    // Answering ENOENT there broke tools such as git that fstat a file they just opened.
    if (file && file->fh) return ::fstat(handle(file)->fd, info) == 0 ? 0 : failure();
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
    try {
        auto lock = state().repository().mutationLock();
        int result;
        const auto full = fullPath(path);
        if (S_ISREG(mode)) {
            int fd = ::open(full.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
            result = fd < 0 ? -1 : (::close(fd), 0);
        } else if (S_ISFIFO(mode)) result = ::mkfifo(full.c_str(), mode);
        else result = ::mknod(full.c_str(), mode, device);
        if (result != 0) return failure();
        if (S_ISREG(mode)) {
            const auto id = state().repository().ensureFile(path);
            state().repository().logEvent(state().currentActor(), "create", id, path);
        }
        return 0;
    } catch (...) { return -EIO; }
}

// Namespace operations without a pre-change snapshot are logged after they succeed. If the
// daemon dies in between, the change is merely unattributed, so undo errs on keeping data.
template <typename Operation>
int namespaceMutation(const char* path, const char* op, const std::string& path2, Operation operation) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        if (operation() != 0) return failure();
        state().repository().logEvent(state().currentActor(), op, {}, path, path2);
        return 0;
    } catch (...) { return -EIO; }
}

int vfsMkdir(const char* path, mode_t mode) {
    return namespaceMutation(path, "mkdir", {}, [&] { return ::mkdir(fullPath(path).c_str(), mode); });
}

int vfsRmdir(const char* path) {
    return namespaceMutation(path, "rmdir", {}, [&] { return ::rmdir(fullPath(path).c_str()); });
}

int vfsSymlink(const char* target, const char* path) {
    return namespaceMutation(path, "symlink", target, [&] { return ::symlink(target, fullPath(path).c_str()); });
}

int vfsUnlink(const char* path) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        auto guard = state().exclusiveFile(path);
        const Actor actor = state().currentActor();
        auto snapshot = state().repository().snapshotPath(path, "delete", &actor);
        // Symlinks have no content object; remember the target so undo can recreate the link.
        char target[PATH_MAX];
        const ssize_t length = snapshot ? -1 : ::readlink(fullPath(path).c_str(), target, sizeof(target) - 1);
        if (::unlink(fullPath(path).c_str()) != 0) return failure();
        if (snapshot) state().repository().markDeleted(snapshot->fileId, path);
        else if (length >= 0)
            state().repository().logEvent(actor, "delete-symlink", {}, path, std::string(target, length));
        return 0;
    } catch (...) { return -EIO; }
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
        const Actor actor = state().currentActor();
        std::string id;
        if (S_ISREG(sourceInfo.st_mode)) id = state().repository().ensureFile(from);
        {
            auto guard = state().exclusiveFile(to);
            state().repository().snapshotPath(to, "rename-overwrite", &actor);
        }
        int result = ::rename(fullPath(from).c_str(), fullPath(to).c_str());
        if (result != 0) return failure();
        const bool directory = S_ISDIR(sourceInfo.st_mode);
        state().repository().renamePath(from, to, directory);
        state().repository().logEvent(actor, directory ? "rename-dir" : id.empty() ? "rename-other" : "rename",
                                      id, from, to);
        return 0;
    } catch (...) { return -EIO; }
}

template <typename Operation>
int metadataMutation(const char* path, const char* event, Operation operation) {
    if (hidden(path)) return -EACCES;
    try {
        auto lock = state().repository().mutationLock();
        // A file whose creating handle is still open has no earlier state worth keeping (think of
        // the fchown/fchmod that `sed -i` or `cp -p` apply to their freshly written temp files).
        if (state().underConstruction(path)) return operation() == 0 ? 0 : failure();
        auto guard = state().exclusiveFile(path);
        const Actor actor = state().currentActor();
        state().repository().snapshotPath(path, event, &actor);
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
    if (info && info->fh) {
        try {
            auto lock = state().repository().mutationLock();
            auto* open = handle(info);
            std::unique_lock<std::shared_mutex> exclusive(*open->lock);
            if (!open->snapshotted.load()) {
                const Actor actor = state().currentActor();
                state().repository().snapshotFd(open->fd, open->fileId, "truncate", &actor);
                open->snapshotted.store(true);
            }
            return ::ftruncate(open->fd, size) == 0 ? 0 : failure();
        } catch (...) { return -EIO; }
    }
    return metadataMutation(path, "truncate", [&] { return ::truncate(fullPath(path).c_str(), size); });
}

int openHandle(const char* path, fuse_file_info* info, bool creating, mode_t mode) {
    try {
        auto lock = state().repository().mutationLock();
        const bool writable = (info->flags & O_ACCMODE) != O_RDONLY;
        struct stat before{};
        const bool existed = ::lstat(fullPath(path).c_str(), &before) == 0;
        bool snapshotted = !existed;
        if (existed && writable && (info->flags & O_TRUNC)) {
            auto guard = state().exclusiveFile(path);
            const Actor actor = state().currentActor();
            snapshotted = state().repository().snapshotPath(path, "truncate", &actor).has_value();
        }
        const int flags = info->flags | O_CLOEXEC | (creating ? O_CREAT : 0);
        int fd = creating ? ::open(fullPath(path).c_str(), flags, mode) : ::open(fullPath(path).c_str(), flags);
        if (fd < 0) return failure();
        std::string id;
        try {
            id = state().repository().ensureFile(path);
            if (!existed) state().repository().logEvent(state().currentActor(), "create", id, path);
        } catch (...) {
            ::close(fd);
            if (!existed) ::unlink(fullPath(path).c_str());
            throw;
        }
        const bool creator = !existed && writable;
        auto* open = new FileHandle(fd, id, writable, snapshotted, creator, state().fileLock(id));
        info->fh = reinterpret_cast<std::uintptr_t>(open);
        if (writable) state().addWriter(id);
        if (creator) state().beginConstruction(id);
        return 0;
    } catch (...) { return -EIO; }
}

int vfsOpen(const char* path, fuse_file_info* info) {
    if (hidden(path)) return -ENOENT;
    return openHandle(path, info, false, 0);
}

int vfsCreate(const char* path, mode_t mode, fuse_file_info* info) {
    if (hidden(path)) return -EACCES;
    return openHandle(path, info, true, mode);
}

int vfsRead(const char*, char* buffer, std::size_t size, off_t offset, fuse_file_info* info) {
    const ssize_t result = ::pread(handle(info)->fd, buffer, size, offset);
    return result < 0 ? failure() : static_cast<int>(result);
}

int vfsWrite(const char*, const char* buffer, std::size_t size, off_t offset, fuse_file_info* info) {
    try {
        auto* open = handle(info);
        if (!open->snapshotted.load(std::memory_order_acquire)) {
            // Slow path, once per handle: preserve the pre-change state before the first byte lands.
            auto lock = state().repository().mutationLock();
            std::unique_lock<std::shared_mutex> exclusive(*open->lock);
            if (!open->snapshotted.load(std::memory_order_relaxed)) {
                const Actor actor = state().currentActor();
                state().repository().snapshotFd(open->fd, open->fileId, "write", &actor);
                open->snapshotted.store(true, std::memory_order_release);
            }
            crashIfRequested("before-live-write");
            const ssize_t result = ::pwrite(open->fd, buffer, size, offset);
            return result < 0 ? failure() : static_cast<int>(result);
        }
        // Fast path: history is already safe, so writers to different files never contend and
        // writers to the same file only exclude snapshots, not each other.
        std::shared_lock<std::shared_mutex> shared(*open->lock);
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
    if (open->created) state().endConstruction(open->fileId);
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
        // libfuse < 3.16 has no FUSE_FILL_DIR_DEFAULTS; zero is the same "no flags" value.
        if (filler(buffer, entry->d_name, &statInfo, next, static_cast<fuse_fill_dir_flags>(0))) break;
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
    current.startControl(fuse_get_context()->fuse);
    return &current;
}

void vfsDestroy(void* privateData) { static_cast<FuseState*>(privateData)->stopControl(); }

std::string shortId(const std::string& id) { return id.substr(0, 8); }

std::string describeActor(const EventRecord& event) {
    std::string actor = event.process + "(" + std::to_string(event.pid) + ")";
    return event.sessionLabel.empty() ? actor : event.sessionLabel + ":" + actor;
}

// Commands that rewrite or discard history. A process inside a supervised session may not run
// them, so an agent cannot (accidentally) erase the trail that lets its work be undone.
const std::set<std::string> kPrivileged{"restore", "purge", "undo", "session-begin"};

}  // namespace

FuseState::FuseState(std::filesystem::path source) : repository_(std::move(source)) {}
FuseState::~FuseState() { stopControl(); }

void FuseState::startControl(struct fuse* instance) {
    fuse_ = instance;
    if (!control_) control_ = std::make_unique<ControlServer>(
        repository_.socketPath(),
        [this](const ControlPeer& peer, const auto& args) { return handleControl(peer, args); });
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

void FuseState::beginConstruction(const std::string& fileId) {
    std::lock_guard<std::mutex> guard(writersMutex_); ++constructing_[fileId];
}

void FuseState::endConstruction(const std::string& fileId) {
    std::lock_guard<std::mutex> guard(writersMutex_);
    auto found = constructing_.find(fileId);
    if (found != constructing_.end() && --found->second == 0) constructing_.erase(found);
}

// Lock order is repository -> writers everywhere, so resolve the identity before locking.
bool FuseState::underConstruction(const std::string& path) {
    {
        std::lock_guard<std::mutex> guard(writersMutex_);
        if (constructing_.empty()) return false;
    }
    auto id = repository_.findFileId(path, false);
    std::lock_guard<std::mutex> guard(writersMutex_);
    return id && constructing_.count(*id) != 0;
}

std::shared_ptr<std::shared_mutex> FuseState::fileLock(const std::string& fileId) {
    std::lock_guard<std::mutex> guard(locksMutex_);
    auto& slot = fileLocks_[fileId];
    auto lock = slot.lock();
    if (!lock) slot = lock = std::make_shared<std::shared_mutex>();
    if (fileLocks_.size() > 4096) {
        for (auto it = fileLocks_.begin(); it != fileLocks_.end();)
            it = it->second.expired() ? fileLocks_.erase(it) : std::next(it);
    }
    return lock;
}

// Callers hold the repository mutation lock, so no new handle (and no new fast-path writer)
// can appear for this file while the guard is being taken.
FileGuard FuseState::exclusiveFile(const std::string& path) {
    FileGuard guard;
    auto id = repository_.findFileId(path, false);
    if (!id) return guard;
    {
        std::lock_guard<std::mutex> locks(locksMutex_);
        auto found = fileLocks_.find(*id);
        if (found != fileLocks_.end()) guard.mutex = found->second.lock();
    }
    if (guard.mutex) guard.lock = std::unique_lock<std::shared_mutex>(*guard.mutex);
    return guard;
}

// Walk the caller's ancestry through /proc until a registered session root is found. The root's
// start time is compared too, so a recycled PID cannot inherit a finished session.
std::string FuseState::sessionFor(int pid) {
    std::lock_guard<std::mutex> guard(sessionsMutex_);
    if (roots_.empty()) return {};
    auto self = readProcess(pid);
    if (!self) return {};
    auto cached = sessionCache_.find(pid);
    if (cached != sessionCache_.end() && cached->second.startTime == self->startTime) return cached->second.sessionId;
    std::string result;
    auto current = self;
    for (int depth = 0; current && depth < 64; ++depth) {
        auto root = roots_.find(current->pid);
        if (root != roots_.end() && root->second.startTime == current->startTime) {
            result = root->second.id;
            break;
        }
        if (current->ppid <= 1) break;
        current = readProcess(current->ppid);
    }
    if (sessionCache_.size() > 4096) sessionCache_.clear();
    sessionCache_[pid] = {self->startTime, result};
    return result;
}

Actor FuseState::actorFor(int pid, std::uint32_t uid) {
    Actor actor;
    actor.pid = pid;
    actor.uid = uid;
    if (auto info = readProcess(pid)) actor.process = info->name;
    // io_uring (used by Node/libuv, among others) performs file I/O on kernel worker threads
    // named "iou-wrk-<n>". They belong to the submitting process's thread group, so the session
    // walk works unchanged, but the name should be that of the process that asked for the I/O.
    if (actor.process.rfind("iou-wrk-", 0) == 0) {
        if (auto leader = readProcess(threadGroupOf(pid))) actor.process = leader->name;
    }
    actor.sessionId = sessionFor(pid);
    return actor;
}

Actor FuseState::currentActor() {
    const fuse_context* context = fuse_get_context();
    return actorFor(context->pid, context->uid);
}

// The control channel edits the backing store behind the kernel's back; drop cached attributes
// and pages so the mount reflects the change immediately. Called without repository locks held.
void FuseState::invalidate(const std::vector<std::string>& paths) {
    if (!fuse_) return;
    std::set<std::string> unique(paths.begin(), paths.end());
    for (const auto& path : unique) {
        fuse_invalidate_path(fuse_, path.c_str());
        const auto parent = std::filesystem::path(path).parent_path().generic_string();
        if (!parent.empty()) fuse_invalidate_path(fuse_, parent.c_str());
    }
}

ControlResponse FuseState::handleControl(const ControlPeer& peer, const std::vector<std::string>& args) {
    if (args.empty()) return {false, "missing command"};
    const Actor actor = peer.pid > 0 ? actorFor(peer.pid, peer.uid) : Actor{0, peer.uid, "versionfsctl", {}};
    if (!actor.sessionId.empty() && kPrivileged.count(args[0]))
        return {false, "refused: '" + args[0] + "' is not allowed from inside a supervised session"};
    std::ostringstream out;
    const std::string& command = args[0];
    if (command == "history" && args.size() == 2) {
        // Rows are numbered oldest = 1; versions may span several identities (see history()).
        const auto versions = repository_.history(args[1]);
        std::size_t number = versions.size();
        out << "#\tversion\tevent\tsize\tcreated\tby\tpath\n";
        for (const auto& version : versions)
            out << number-- << '\t' << version.id << '\t' << version.event << '\t'
                << version.size << '\t' << formatTimestamp(version.createdNs) << '\t'
                << (version.actor.empty() ? "-" : version.actor) << '\t' << version.path << '\n';
    } else if (command == "show" && args.size() == 3) {
        auto version = repository_.getVersion(args[1], args[2]);
        std::ifstream input(version.objectPath, std::ios::binary);
        if (!input) return {false, "snapshot object is unavailable"};
        out << input.rdbuf();
    } else if (command == "restore" && (args.size() == 3 || args.size() == 4)) {
        const auto destination = args.size() == 4 ? std::optional<std::string>(args[3]) : std::nullopt;
        repository_.restore(args[1], args[2], destination, [this](const auto& path) { return hasWriters(path); },
                            &actor);
        invalidate({args[1], destination.value_or(args[1])});
        out << "restored " << args[2] << " to " << destination.value_or(args[1]) << '\n';
    } else if (command == "deleted" && args.size() == 1) {
        out << "deleted\tfile-id\tpath\n";
        for (const auto& file : repository_.deletedFiles())
            out << formatTimestamp(file.deletedNs) << '\t' << file.fileId << '\t' << file.path << '\n';
    } else if (command == "status" && args.size() == 1) {
        const auto value = repository_.status();
        out << "source\t" << repository_.sourceRoot() << "\nfiles\t" << value.files
            << "\ndeleted\t" << value.deletedFiles << "\nversions\t" << value.versions
            << "\nobject-bytes\t" << value.objectBytes << "\nstored-bytes\t" << value.storedBytes
            << "\nevents\t" << value.events << "\nsessions\t" << value.sessions << '\n';
    } else if (command == "purge" && args.size() == 3) {
        repository_.purge(args[1], args[2] == "--all" ? std::nullopt : std::optional<std::string>(args[2]));
        out << "history purged\n";
    } else if (command == "verify" && (args.size() == 1 || args.size() == 2)) {
        auto errors = repository_.verify(args.size() == 2 ? std::optional<std::string>(args[1]) : std::nullopt);
        if (errors.empty()) out << "ok\n";
        else { for (const auto& error : errors) out << error << '\n'; return {false, out.str()}; }
    } else if (command == "session-begin" && args.size() <= 2) {
        auto root = readProcess(peer.pid);
        if (!root) return {false, "cannot identify the session's root process"};
        const auto id = repository_.beginSession(args.size() == 2 ? args[1] : "", root->pid, root->startTime);
        {
            std::lock_guard<std::mutex> guard(sessionsMutex_);
            roots_[root->pid] = {id, root->startTime};
            sessionCache_.clear();
        }
        out << id << '\n';
    } else if (command == "session-end" && args.size() == 2) {
        const auto session = repository_.session(args[1]);
        if (!actor.sessionId.empty() && session.rootPid != peer.pid)
            return {false, "refused: only the session's root process may end it"};
        repository_.endSession(session.id);
        {
            std::lock_guard<std::mutex> guard(sessionsMutex_);
            roots_.erase(session.rootPid);
            sessionCache_.clear();
        }
        const auto ended = repository_.session(session.id);
        out << "session " << ended.label << " (" << shortId(ended.id) << ") recorded " << ended.events
            << " changes to " << ended.files << " paths\n";
    } else if (command == "sessions" && args.size() == 1) {
        out << "session\tlabel\tstatus\tstarted\tchanges\tpaths\n";
        for (const auto& session : repository_.sessions())
            out << shortId(session.id) << '\t' << session.label << '\t' << session.status << '\t'
                << formatTimestamp(session.startedNs) << '\t' << session.events << '\t' << session.files << '\n';
    } else if (command == "changes" && args.size() == 2) {
        // Per-path summary of one session: what an agent touched, in first-touch order.
        const auto session = repository_.session(args[1]);
        auto events = repository_.events(std::nullopt, session.id, 1000000);
        std::reverse(events.begin(), events.end());
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::string>> ops;
        std::map<std::string, std::string> finalPath;
        for (const auto& event : events) {
            const auto key = event.fileId.empty() ? event.path : event.fileId;
            if (!ops.count(key)) order.push_back(key);
            auto& list = ops[key];
            if (list.empty() || list.back() != event.op) list.push_back(event.op);
            finalPath[key] = event.path2.empty() || event.op == "symlink" ? event.path
                                                                           : event.path + " -> " + event.path2;
        }
        out << "session " << session.label << " (" << shortId(session.id) << ", " << session.status << "): "
            << order.size() << " paths\n";
        for (const auto& key : order) {
            std::string joined;
            for (const auto& op : ops[key]) joined += (joined.empty() ? "" : ",") + op;
            out << joined << '\t' << finalPath[key] << '\n';
        }
    } else if (command == "undo" && args.size() >= 2) {
        bool dryRun = false, force = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--dry-run") dryRun = true;
            else if (args[i] == "--force") force = true;
            else return {false, "unknown undo option: " + args[i]};
        }
        const auto report = repository_.undoSession(args[1], dryRun, force, actor,
                                                    [this](const auto& path) { return hasWriters(path); });
        if (!dryRun) invalidate(report.paths);
        for (const auto& line : report.lines) out << line << '\n';
        for (const auto& conflict : report.conflicts) out << "conflict: " << conflict << '\n';
        out << (dryRun ? "plan: " : "undone: ") << report.applied << " steps, " << report.conflicts.size()
            << " conflicts\n";
    } else if (command == "log" && args.size() >= 1) {
        std::optional<std::string> path, session;
        std::size_t limit = 50;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--session" && i + 1 < args.size()) session = repository_.session(args[++i]).id;
            else if (args[i] == "--limit" && i + 1 < args.size()) limit = std::stoul(args[++i]);
            else path = args[i];
        }
        out << "time\tby\top\tpath\n";
        for (const auto& event : repository_.events(path, session, limit))
            out << formatTimestamp(event.timeNs) << '\t' << describeActor(event) << '\t' << event.op << '\t'
                << event.path << (event.path2.empty() ? "" : " -> " + event.path2) << '\n';
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
