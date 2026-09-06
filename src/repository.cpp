#include "versionfs/repository.hpp"

#include "versionfs/util.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace versionfs {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() { return statement_; }
private:
    sqlite3_stmt* statement_{nullptr};
};

void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("cannot bind SQLite text value");
}

void requireDone(sqlite3* db, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}

std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::int64_t mtimeSeconds(const struct stat& value) {
#ifdef __APPLE__
    return value.st_mtimespec.tv_sec;
#else
    return value.st_mtim.tv_sec;
#endif
}

std::int64_t mtimeNanoseconds(const struct stat& value) {
#ifdef __APPLE__
    return value.st_mtimespec.tv_nsec;
#else
    return value.st_mtim.tv_nsec;
#endif
}

VersionRecord readVersion(sqlite3_stmt* row, const std::filesystem::path& objectsRoot) {
    VersionRecord version;
    version.id = columnText(row, 0);
    version.fileId = columnText(row, 1);
    version.sequence = sqlite3_column_int64(row, 2);
    version.event = columnText(row, 3);
    version.path = columnText(row, 4);
    version.objectPath = objectsRoot / columnText(row, 5);
    version.checksum = columnText(row, 6);
    version.size = sqlite3_column_int64(row, 7);
    version.mode = static_cast<std::uint32_t>(sqlite3_column_int64(row, 8));
    version.uid = static_cast<std::uint32_t>(sqlite3_column_int64(row, 9));
    version.gid = static_cast<std::uint32_t>(sqlite3_column_int64(row, 10));
    version.mtimeSec = sqlite3_column_int64(row, 11);
    version.mtimeNsec = sqlite3_column_int64(row, 12);
    version.createdNs = sqlite3_column_int64(row, 13);
    return version;
}

constexpr const char* kVersionColumns =
    "v.id,v.file_id,v.sequence,v.event,v.path,v.object_name,v.checksum,v.size,"
    "v.mode,v.uid,v.gid,v.mtime_sec,v.mtime_nsec,v.created_ns";

void copyFd(int source, int destination) {
    std::array<char, 128 * 1024> buffer{};
    off_t offset = 0;
    while (true) {
        ssize_t count = ::pread(source, buffer.data(), buffer.size(), offset);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("cannot read snapshot source: ") + std::strerror(errno));
        }
        ssize_t written = 0;
        while (written < count) {
            ssize_t result = ::write(destination, buffer.data() + written,
                                     static_cast<std::size_t>(count - written));
            if (result < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("cannot write snapshot object: ") + std::strerror(errno));
            }
            written += result;
        }
        offset += count;
    }
}

void syncDirectory(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

Repository::Repository(std::filesystem::path sourceRoot)
    : sourceRoot_(std::filesystem::weakly_canonical(std::move(sourceRoot))),
      metadataRoot_(sourceRoot_ / ".versionfs"),
      objectsRoot_(metadataRoot_ / "objects"),
      tempRoot_(metadataRoot_ / "tmp") {
    if (!std::filesystem::is_directory(sourceRoot_))
        throw std::runtime_error("source must be an existing directory");
    initialize();
}

Repository::~Repository() {
    if (db_) sqlite3_close(db_);
}

std::filesystem::path Repository::socketPath() const { return metadataRoot_ / "control.sock"; }

void Repository::exec(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "SQLite operation failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void Repository::initialize() {
    std::filesystem::create_directories(objectsRoot_);
    std::filesystem::create_directories(tempRoot_);
    if (sqlite3_open_v2((metadataRoot_ / "metadata.db").c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open VersionFS metadata database");
    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
    exec("CREATE TABLE IF NOT EXISTS files("
         "id TEXT PRIMARY KEY,current_path TEXT UNIQUE,last_path TEXT NOT NULL,"
         "deleted INTEGER NOT NULL DEFAULT 0,created_ns INTEGER NOT NULL,deleted_ns INTEGER);"
         "CREATE TABLE IF NOT EXISTS versions("
         "id TEXT PRIMARY KEY,file_id TEXT NOT NULL REFERENCES files(id),sequence INTEGER NOT NULL,"
         "event TEXT NOT NULL,path TEXT NOT NULL,object_name TEXT NOT NULL UNIQUE,checksum TEXT NOT NULL,"
         "size INTEGER NOT NULL,mode INTEGER NOT NULL,uid INTEGER NOT NULL,gid INTEGER NOT NULL,"
         "mtime_sec INTEGER NOT NULL,mtime_nsec INTEGER NOT NULL,created_ns INTEGER NOT NULL,"
         "UNIQUE(file_id,sequence));"
         "CREATE INDEX IF NOT EXISTS versions_file_sequence ON versions(file_id,sequence DESC);"
         "CREATE INDEX IF NOT EXISTS files_last_path ON files(last_path);");
    Statement check(db_, "PRAGMA quick_check");
    if (sqlite3_step(check.get()) != SQLITE_ROW || columnText(check.get(), 0) != "ok")
        throw std::runtime_error("VersionFS metadata integrity check failed");
    cleanup();
}

void Repository::cleanup() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(tempRoot_, error))
        std::filesystem::remove_all(entry.path(), error);
}

std::string Repository::ensureFile(const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    if (isInternalPath(path)) throw std::invalid_argument("reserved VersionFS path");
    Statement find(db_, "SELECT id FROM files WHERE current_path=?1 AND deleted=0");
    bindText(find.get(), 1, path);
    if (sqlite3_step(find.get()) == SQLITE_ROW) return columnText(find.get(), 0);
    const std::string id = randomId();
    Statement insert(db_, "INSERT INTO files(id,current_path,last_path,deleted,created_ns)"
                          " VALUES(?1,?2,?2,0,?3)");
    bindText(insert.get(), 1, id);
    bindText(insert.get(), 2, path);
    sqlite3_bind_int64(insert.get(), 3, nowNanoseconds());
    requireDone(db_, insert.get());
    return id;
}

std::optional<std::string> Repository::findFileId(const std::string& rawPath, bool includeDeleted) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    const char* sql = includeDeleted
        ? "SELECT id FROM files WHERE current_path=?1 OR (deleted=1 AND last_path=?1) "
          "ORDER BY deleted ASC,COALESCE(deleted_ns,created_ns) DESC LIMIT 1"
        : "SELECT id FROM files WHERE current_path=?1 AND deleted=0 LIMIT 1";
    Statement statement(db_, sql);
    bindText(statement.get(), 1, path);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) return columnText(statement.get(), 0);
    return std::nullopt;
}

std::string Repository::currentPathForId(const std::string& fileId) {
    Statement statement(db_, "SELECT COALESCE(current_path,last_path) FROM files WHERE id=?1");
    bindText(statement.get(), 1, fileId);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error("unknown file identity");
    return columnText(statement.get(), 0);
}

void Repository::renamePath(const std::string& rawFrom, const std::string& rawTo, bool directory) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string from = normalizePath(rawFrom), to = normalizePath(rawTo);
    if (from == to) return;
    exec("BEGIN IMMEDIATE");
    try {
        Statement replace(db_, "UPDATE files SET current_path=NULL,last_path=?1,deleted=1,deleted_ns=?2 "
                               "WHERE current_path=?1 AND deleted=0");
        bindText(replace.get(), 1, to);
        sqlite3_bind_int64(replace.get(), 2, nowNanoseconds());
        requireDone(db_, replace.get());
        if (directory) {
            Statement update(db_, "UPDATE files SET current_path=?1||substr(current_path,length(?2)+1),"
                                  "last_path=?1||substr(last_path,length(?2)+1) "
                                  "WHERE deleted=0 AND (current_path=?2 OR current_path LIKE ?3)");
            bindText(update.get(), 1, to);
            bindText(update.get(), 2, from);
            bindText(update.get(), 3, from + "/%");
            requireDone(db_, update.get());
        } else {
            Statement update(db_, "UPDATE files SET current_path=?1,last_path=?1 WHERE current_path=?2 AND deleted=0");
            bindText(update.get(), 1, to);
            bindText(update.get(), 2, from);
            requireDone(db_, update.get());
        }
        exec("COMMIT");
    } catch (...) { exec("ROLLBACK"); throw; }
}

void Repository::markDeleted(const std::string& fileId, const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    Statement statement(db_, "UPDATE files SET current_path=NULL,last_path=?1,deleted=1,deleted_ns=?2 WHERE id=?3");
    bindText(statement.get(), 1, normalizePath(rawPath));
    sqlite3_bind_int64(statement.get(), 2, nowNanoseconds());
    bindText(statement.get(), 3, fileId);
    requireDone(db_, statement.get());
}

void Repository::reactivate(const std::string& fileId, const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    Statement statement(db_, "UPDATE files SET current_path=?1,last_path=?1,deleted=0,deleted_ns=NULL WHERE id=?2");
    bindText(statement.get(), 1, normalizePath(rawPath));
    bindText(statement.get(), 2, fileId);
    requireDone(db_, statement.get());
}

std::optional<VersionRecord> Repository::snapshotPath(const std::string& rawPath,
                                                      const std::string& event) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    const auto fullPath = backingPath(sourceRoot_, path);
    struct stat info{};
    if (::lstat(fullPath.c_str(), &info) != 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::runtime_error(std::string("cannot stat snapshot source: ") + std::strerror(errno));
    }
    if (!S_ISREG(info.st_mode)) return std::nullopt;
    const std::string fileId = ensureFile(path);
    int fd = ::open(fullPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error(std::string("cannot open snapshot source: ") + std::strerror(errno));
    try {
        auto result = snapshotFromFdLocked(fd, fileId, path, event);
        ::close(fd);
        return result;
    } catch (...) { ::close(fd); throw; }
}

std::optional<VersionRecord> Repository::snapshotFd(int fd, const std::string& fileId,
                                                    const std::string& event) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    struct stat info{};
    if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot stat open file");
    if (!S_ISREG(info.st_mode)) return std::nullopt;
    return snapshotFromFdLocked(fd, fileId, currentPathForId(fileId), event);
}

std::optional<VersionRecord> Repository::snapshotFromFdLocked(int fd, const std::string& fileId,
                                                              const std::string& path,
                                                              const std::string& event) {
    struct stat info{};
    if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot stat snapshot source");
    if (!S_ISREG(info.st_mode)) return std::nullopt;
    VersionRecord version;
    version.id = randomId();
    version.fileId = fileId;
    version.event = event;
    version.path = path;
    version.size = info.st_size;
    version.mode = static_cast<std::uint32_t>(info.st_mode);
    version.uid = info.st_uid;
    version.gid = info.st_gid;
    version.mtimeSec = mtimeSeconds(info);
    version.mtimeNsec = mtimeNanoseconds(info);
    version.createdNs = nowNanoseconds();
    const std::string objectName = version.id + ".blob";
    const auto temporary = tempRoot_ / (objectName + ".tmp");
    const auto finalObject = objectsRoot_ / objectName;
    int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) throw std::runtime_error("cannot create temporary snapshot object");
    try {
        copyFd(fd, output);
        if (::fchmod(output, 0400) != 0 || ::fsync(output) != 0)
            throw std::runtime_error("cannot sync snapshot object");
        ::close(output);
        output = -1;
        crashIfRequested("after-temp-sync");
        version.checksum = sha256File(temporary);
        if (::rename(temporary.c_str(), finalObject.c_str()) != 0)
            throw std::runtime_error("cannot publish snapshot object");
        syncDirectory(objectsRoot_);
        crashIfRequested("after-object-publish");
    } catch (...) {
        if (output >= 0) ::close(output);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    exec("BEGIN IMMEDIATE");
    try {
        Statement sequence(db_, "SELECT COALESCE(MAX(sequence),0)+1 FROM versions WHERE file_id=?1");
        bindText(sequence.get(), 1, fileId);
        if (sqlite3_step(sequence.get()) != SQLITE_ROW) throw std::runtime_error("cannot allocate version sequence");
        version.sequence = sqlite3_column_int64(sequence.get(), 0);
        Statement insert(db_, "INSERT INTO versions(id,file_id,sequence,event,path,object_name,checksum,size,"
                              "mode,uid,gid,mtime_sec,mtime_nsec,created_ns)"
                              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)");
        bindText(insert.get(), 1, version.id); bindText(insert.get(), 2, fileId);
        sqlite3_bind_int64(insert.get(), 3, version.sequence); bindText(insert.get(), 4, event);
        bindText(insert.get(), 5, path); bindText(insert.get(), 6, objectName);
        bindText(insert.get(), 7, version.checksum); sqlite3_bind_int64(insert.get(), 8, version.size);
        sqlite3_bind_int64(insert.get(), 9, version.mode); sqlite3_bind_int64(insert.get(), 10, version.uid);
        sqlite3_bind_int64(insert.get(), 11, version.gid); sqlite3_bind_int64(insert.get(), 12, version.mtimeSec);
        sqlite3_bind_int64(insert.get(), 13, version.mtimeNsec); sqlite3_bind_int64(insert.get(), 14, version.createdNs);
        requireDone(db_, insert.get());
        exec("COMMIT");
        crashIfRequested("after-metadata-commit");
    } catch (...) { exec("ROLLBACK"); throw; }
    version.objectPath = finalObject;
    return version;
}

std::vector<VersionRecord> Repository::history(const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    auto fileId = findFileId(rawPath, true);
    if (!fileId) throw std::runtime_error("no tracked file or deleted history at this path");
    Statement statement(db_, (std::string("SELECT ") + kVersionColumns +
        " FROM versions v WHERE v.file_id=?1 ORDER BY v.sequence DESC").c_str());
    bindText(statement.get(), 1, *fileId);
    std::vector<VersionRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) result.push_back(readVersion(statement.get(), objectsRoot_));
    return result;
}

std::vector<DeletedRecord> Repository::deletedFiles() {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    Statement statement(db_, "SELECT id,last_path,deleted_ns FROM files WHERE deleted=1 ORDER BY deleted_ns DESC");
    std::vector<DeletedRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW)
        result.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1),
                          sqlite3_column_int64(statement.get(), 2)});
    return result;
}

VersionRecord Repository::getVersion(const std::string& rawPath, const std::string& versionId) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    Statement statement(db_, (std::string("SELECT ") + kVersionColumns +
        " FROM versions v JOIN files f ON f.id=v.file_id WHERE v.id=?1 AND "
        "(f.current_path=?2 OR f.last_path=?2 OR v.path=?2) LIMIT 1").c_str());
    bindText(statement.get(), 1, versionId); bindText(statement.get(), 2, path);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error("version not found for path");
    return readVersion(statement.get(), objectsRoot_);
}

void Repository::restore(const std::string& rawPath, const std::string& versionId,
                         const std::optional<std::string>& rawDestination,
                         const std::function<bool(const std::string&)>& hasWriters) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    const std::string destination = rawDestination ? normalizePath(*rawDestination) : path;
    if (isInternalPath(destination)) throw std::runtime_error("cannot restore into the internal repository");
    VersionRecord version = getVersion(path, versionId);
    const auto target = backingPath(sourceRoot_, destination);
    const bool targetExists = std::filesystem::exists(target);
    Statement file(db_, "SELECT current_path,deleted FROM files WHERE id=?1");
    bindText(file.get(), 1, version.fileId);
    if (sqlite3_step(file.get()) != SQLITE_ROW) throw std::runtime_error("version has no file record");
    const std::string currentPath = columnText(file.get(), 0);
    const bool deleted = sqlite3_column_int(file.get(), 1) != 0;
    if (hasWriters(destination) || (!currentPath.empty() && hasWriters(currentPath)))
        throw std::runtime_error("file has an active writable handle");
    const bool restoringSameIdentity = !deleted && currentPath == destination && !rawDestination;
    if (targetExists && !restoringSameIdentity)
        throw std::runtime_error("restore destination already exists");
    if (restoringSameIdentity) snapshotPath(destination, "restore");
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = tempRoot_ / ("restore-" + randomId() + ".tmp");
    int input = ::open(version.objectPath.c_str(), O_RDONLY | O_CLOEXEC);
    int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (input < 0 || output < 0) {
        if (input >= 0) ::close(input); if (output >= 0) ::close(output);
        throw std::runtime_error("cannot create restored file");
    }
    try {
        copyFd(input, output);
        ::fchmod(output, version.mode & 07777U);
        if (::fchown(output, version.uid, version.gid) != 0 && errno != EPERM)
            throw std::runtime_error("cannot restore file ownership");
        struct timespec times[2]{{version.mtimeSec, static_cast<long>(version.mtimeNsec)},
                                {version.mtimeSec, static_cast<long>(version.mtimeNsec)}};
        ::futimens(output, times);
        if (::fsync(output) != 0) throw std::runtime_error("cannot sync restored file");
        ::close(input); input = -1; ::close(output); output = -1;
        if (::rename(temporary.c_str(), target.c_str()) != 0)
            throw std::runtime_error(std::string("cannot publish restored file: ") + std::strerror(errno));
        syncDirectory(target.parent_path());
    } catch (...) {
        if (input >= 0) ::close(input); if (output >= 0) ::close(output);
        std::error_code ignored; std::filesystem::remove(temporary, ignored); throw;
    }
    if (!rawDestination) reactivate(version.fileId, destination);
    else ensureFile(destination);
}

void Repository::purge(const std::string& rawPath, const std::optional<std::string>& versionId) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    auto fileId = findFileId(rawPath, true);
    if (!fileId) throw std::runtime_error("path has no version history");
    std::vector<std::filesystem::path> objects;
    Statement find(db_, versionId
        ? "SELECT object_name FROM versions WHERE file_id=?1 AND id=?2"
        : "SELECT object_name FROM versions WHERE file_id=?1");
    bindText(find.get(), 1, *fileId); if (versionId) bindText(find.get(), 2, *versionId);
    while (sqlite3_step(find.get()) == SQLITE_ROW) objects.push_back(objectsRoot_ / columnText(find.get(), 0));
    if (objects.empty()) throw std::runtime_error("no matching version to purge");
    exec("BEGIN IMMEDIATE");
    try {
        Statement remove(db_, versionId
            ? "DELETE FROM versions WHERE file_id=?1 AND id=?2"
            : "DELETE FROM versions WHERE file_id=?1");
        bindText(remove.get(), 1, *fileId); if (versionId) bindText(remove.get(), 2, *versionId);
        requireDone(db_, remove.get()); exec("COMMIT");
    } catch (...) { exec("ROLLBACK"); throw; }
    std::error_code ignored;
    for (const auto& object : objects) std::filesystem::remove(object, ignored);
}

std::vector<std::string> Repository::verify(const std::optional<std::string>& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    std::string sql = std::string("SELECT ") + kVersionColumns + " FROM versions v";
    std::optional<std::string> fileId;
    if (rawPath) {
        fileId = findFileId(*rawPath, true);
        if (!fileId) throw std::runtime_error("path has no version history");
        sql += " WHERE v.file_id=?1";
    }
    Statement statement(db_, sql.c_str()); if (fileId) bindText(statement.get(), 1, *fileId);
    std::vector<std::string> errors;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        VersionRecord version = readVersion(statement.get(), objectsRoot_);
        if (!std::filesystem::is_regular_file(version.objectPath))
            errors.push_back(version.id + ": object is missing");
        else if (sha256File(version.objectPath) != version.checksum)
            errors.push_back(version.id + ": checksum mismatch");
    }
    return errors;
}

RepositoryStatus Repository::status() {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    RepositoryStatus result;
    Statement counts(db_, "SELECT COUNT(*),SUM(CASE WHEN deleted=1 THEN 1 ELSE 0 END) FROM files");
    if (sqlite3_step(counts.get()) == SQLITE_ROW) {
        result.files = sqlite3_column_int64(counts.get(), 0);
        result.deletedFiles = sqlite3_column_int64(counts.get(), 1);
    }
    Statement versions(db_, "SELECT COUNT(*),COALESCE(SUM(size),0) FROM versions");
    if (sqlite3_step(versions.get()) == SQLITE_ROW) {
        result.versions = sqlite3_column_int64(versions.get(), 0);
        result.objectBytes = sqlite3_column_int64(versions.get(), 1);
    }
    return result;
}

}  // namespace versionfs
