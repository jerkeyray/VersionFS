#include "versionfs/repository.hpp"

#include "versionfs/util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace versionfs {
namespace {

constexpr int kSchemaVersion = 2;

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    Statement(sqlite3* db, const std::string& sql) : Statement(db, sql.c_str()) {}
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() { return statement_; }
private:
    sqlite3_stmt* statement_{nullptr};
};

void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("cannot bind SQLite text value");
}

// Empty strings are stored as NULL so "no session" and "no file identity" stay distinguishable.
void bindOptional(sqlite3_stmt* statement, int index, const std::string& value) {
    if (value.empty()) sqlite3_bind_null(statement, index);
    else bindText(statement, index, value);
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

constexpr const char* kEventColumns =
    "e.id,e.time_ns,COALESCE(e.session_id,''),COALESCE(s.label,''),e.pid,e.process,e.op,"
    "COALESCE(e.file_id,''),e.path,COALESCE(e.path2,''),COALESCE(e.version_id,'')";

EventRecord readEvent(sqlite3_stmt* row) {
    EventRecord event;
    event.id = sqlite3_column_int64(row, 0);
    event.timeNs = sqlite3_column_int64(row, 1);
    event.sessionId = columnText(row, 2);
    event.sessionLabel = columnText(row, 3);
    event.pid = sqlite3_column_int(row, 4);
    event.process = columnText(row, 5);
    event.op = columnText(row, 6);
    event.fileId = columnText(row, 7);
    event.path = columnText(row, 8);
    event.path2 = columnText(row, 9);
    event.versionId = columnText(row, 10);
    return event;
}

// Copies the whole source descriptor, optionally hashing the bytes as they stream past.
void copyFd(int source, int destination, Sha256* hash = nullptr) {
    std::array<char, 128 * 1024> buffer{};
    off_t offset = 0;
    while (true) {
        ssize_t count = ::pread(source, buffer.data(), buffer.size(), offset);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("cannot read snapshot source: ") + std::strerror(errno));
        }
        if (hash) hash->update(buffer.data(), static_cast<std::size_t>(count));
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

bool isContentEvent(const std::string& op) {
    return op == "write" || op == "truncate" || op == "chmod" || op == "chown" || op == "utimens" ||
           op == "restore";
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

// Version records guard the "history is durable before the live file changes" invariant, so
// their commits fsync the WAL (FULL). Namespace and provenance rows use NORMAL: they survive a
// daemon crash, and the next FULL commit makes them durable against power loss as well.
void Repository::setDurable(bool durable) {
    if (durable == durable_) return;
    exec(durable ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL");
    durable_ = durable;
}

void Repository::initialize() {
    std::filesystem::create_directories(objectsRoot_);
    std::filesystem::create_directories(tempRoot_);
    if (sqlite3_open_v2((metadataRoot_ / "metadata.db").c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open VersionFS metadata database");
    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
    sqlite3_busy_timeout(db_, 5000);
    migrate();
    Statement check(db_, "PRAGMA quick_check");
    if (sqlite3_step(check.get()) != SQLITE_ROW || columnText(check.get(), 0) != "ok")
        throw std::runtime_error("VersionFS metadata integrity check failed");
    cleanup();
}

void Repository::migrate() {
    // Statements are scoped so they are finalized before DROP TABLE, which SQLite refuses
    // while any statement on the connection is still pending.
    bool legacy = false;
    {
        Statement current(db_, "PRAGMA user_version");
        if (sqlite3_step(current.get()) == SQLITE_ROW && sqlite3_column_int(current.get(), 0) >= kSchemaVersion)
            return;
        Statement legacyCheck(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='versions'");
        legacy = sqlite3_step(legacyCheck.get()) == SQLITE_ROW;
    }
    exec("BEGIN IMMEDIATE");
    try {
        // v1 stored one object per version and declared object_name UNIQUE. v2 names objects by
        // content hash so identical states share storage; rebuild the table without the constraint.
        if (legacy) exec("DROP INDEX IF EXISTS versions_file_sequence;"
                         "ALTER TABLE versions RENAME TO versions_v1;");
        exec("CREATE TABLE IF NOT EXISTS files("
             "id TEXT PRIMARY KEY,current_path TEXT UNIQUE,last_path TEXT NOT NULL,"
             "deleted INTEGER NOT NULL DEFAULT 0,created_ns INTEGER NOT NULL,deleted_ns INTEGER);"
             "CREATE TABLE IF NOT EXISTS versions("
             "id TEXT PRIMARY KEY,file_id TEXT NOT NULL REFERENCES files(id),sequence INTEGER NOT NULL,"
             "event TEXT NOT NULL,path TEXT NOT NULL,object_name TEXT NOT NULL,checksum TEXT NOT NULL,"
             "size INTEGER NOT NULL,mode INTEGER NOT NULL,uid INTEGER NOT NULL,gid INTEGER NOT NULL,"
             "mtime_sec INTEGER NOT NULL,mtime_nsec INTEGER NOT NULL,created_ns INTEGER NOT NULL,"
             "UNIQUE(file_id,sequence));"
             "CREATE TABLE IF NOT EXISTS sessions("
             "id TEXT PRIMARY KEY,label TEXT NOT NULL,status TEXT NOT NULL,root_pid INTEGER NOT NULL,"
             "root_start INTEGER NOT NULL,started_ns INTEGER NOT NULL,ended_ns INTEGER);"
             "CREATE TABLE IF NOT EXISTS events("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,time_ns INTEGER NOT NULL,session_id TEXT,"
             "pid INTEGER NOT NULL,uid INTEGER NOT NULL,process TEXT NOT NULL,op TEXT NOT NULL,"
             "file_id TEXT,path TEXT NOT NULL,path2 TEXT,version_id TEXT);");
        if (legacy) exec("INSERT INTO versions SELECT id,file_id,sequence,event,path,object_name,checksum,"
                         "size,mode,uid,gid,mtime_sec,mtime_nsec,created_ns FROM versions_v1;"
                         "DROP TABLE versions_v1;");
        exec("CREATE INDEX IF NOT EXISTS versions_file_sequence ON versions(file_id,sequence DESC);"
             "CREATE INDEX IF NOT EXISTS versions_object ON versions(object_name);"
             "CREATE INDEX IF NOT EXISTS files_last_path ON files(last_path);"
             "CREATE INDEX IF NOT EXISTS events_file ON events(file_id,id);"
             "CREATE INDEX IF NOT EXISTS events_session ON events(session_id,id);"
             "CREATE INDEX IF NOT EXISTS events_version ON events(version_id);"
             "PRAGMA user_version=2;");
        exec("COMMIT");
    } catch (...) { exec("ROLLBACK"); throw; }
}

void Repository::cleanup() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(tempRoot_, error))
        std::filesystem::remove_all(entry.path(), error);
    // A daemon that stopped without ending its sessions cannot attribute to them any more.
    setDurable(false);
    Statement stale(db_, "UPDATE sessions SET status='ended',ended_ns=?1 WHERE status='active'");
    sqlite3_bind_int64(stale.get(), 1, nowNanoseconds());
    requireDone(db_, stale.get());
}

std::string Repository::ensureFile(const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    if (isInternalPath(path)) throw std::invalid_argument("reserved VersionFS path");
    Statement find(db_, "SELECT id FROM files WHERE current_path=?1 AND deleted=0");
    bindText(find.get(), 1, path);
    if (sqlite3_step(find.get()) == SQLITE_ROW) return columnText(find.get(), 0);
    const std::string id = randomId();
    setDurable(false);
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
    setDurable(false);
    exec("BEGIN IMMEDIATE");
    try {
        Statement replace(db_, "UPDATE files SET current_path=NULL,last_path=?1,deleted=1,deleted_ns=?2 "
                               "WHERE current_path=?1 AND deleted=0");
        bindText(replace.get(), 1, to);
        sqlite3_bind_int64(replace.get(), 2, nowNanoseconds());
        requireDone(db_, replace.get());
        if (directory) {
            // Prefix match with substr, not LIKE: LIKE treats '_' and '%' in directory names as
            // wildcards and folds ASCII case, which moved unrelated files' history.
            Statement update(db_, "UPDATE files SET current_path=?1||substr(current_path,length(?2)+1),"
                                  "last_path=?1||substr(last_path,length(?2)+1) "
                                  "WHERE deleted=0 AND (current_path=?2 OR "
                                  "substr(current_path,1,length(?2)+1)=?2||'/')");
            bindText(update.get(), 1, to);
            bindText(update.get(), 2, from);
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
    setDurable(false);
    Statement statement(db_, "UPDATE files SET current_path=NULL,last_path=?1,deleted=1,deleted_ns=?2 WHERE id=?3");
    bindText(statement.get(), 1, normalizePath(rawPath));
    sqlite3_bind_int64(statement.get(), 2, nowNanoseconds());
    bindText(statement.get(), 3, fileId);
    requireDone(db_, statement.get());
}

void Repository::reactivate(const std::string& fileId, const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    setDurable(false);
    Statement statement(db_, "UPDATE files SET current_path=?1,last_path=?1,deleted=0,deleted_ns=NULL WHERE id=?2");
    bindText(statement.get(), 1, normalizePath(rawPath));
    bindText(statement.get(), 2, fileId);
    requireDone(db_, statement.get());
}

std::optional<VersionRecord> Repository::snapshotPath(const std::string& rawPath,
                                                      const std::string& event, const Actor* actor) {
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
        auto result = snapshotFromFdLocked(fd, fileId, path, event, actor);
        ::close(fd);
        return result;
    } catch (...) { ::close(fd); throw; }
}

std::optional<VersionRecord> Repository::snapshotFd(int fd, const std::string& fileId,
                                                    const std::string& event, const Actor* actor) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    struct stat info{};
    if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot stat open file");
    if (!S_ISREG(info.st_mode)) return std::nullopt;
    const std::string path = currentPathForId(fileId);
    if ((::fcntl(fd, F_GETFL) & O_ACCMODE) != O_WRONLY)
        return snapshotFromFdLocked(fd, fileId, path, event, actor);
    // An O_WRONLY handle cannot be read. Reopen the same open file read-only through
    // /proc/self/fd (works even if it was renamed or unlinked), falling back to its path.
    int readable = ::open(("/proc/self/fd/" + std::to_string(fd)).c_str(), O_RDONLY | O_CLOEXEC);
    if (readable < 0) readable = ::open(backingPath(sourceRoot_, path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (readable < 0) throw std::runtime_error(std::string("cannot reopen file for snapshot: ") + std::strerror(errno));
    try {
        auto result = snapshotFromFdLocked(readable, fileId, path, event, actor);
        ::close(readable);
        return result;
    } catch (...) { ::close(readable); throw; }
}

std::optional<VersionRecord> Repository::snapshotFromFdLocked(int fd, const std::string& fileId,
                                                              const std::string& path,
                                                              const std::string& event,
                                                              const Actor* actor) {
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
    const auto temporary = tempRoot_ / (version.id + ".tmp");
    int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) throw std::runtime_error("cannot create temporary snapshot object");
    std::string objectName;
    try {
        Sha256 hash;
        copyFd(fd, output, &hash);
        version.checksum = hash.hex();
        objectName = version.checksum + ".blob";
        const auto finalObject = objectsRoot_ / objectName;
        if (std::filesystem::exists(finalObject)) {
            // Content-addressed deduplication: an identical state is already durable.
            ::close(output);
            output = -1;
            ::unlink(temporary.c_str());
        } else {
            if (::fchmod(output, 0400) != 0 || ::fsync(output) != 0)
                throw std::runtime_error("cannot sync snapshot object");
            ::close(output);
            output = -1;
            crashIfRequested("after-temp-sync");
            if (::rename(temporary.c_str(), finalObject.c_str()) != 0)
                throw std::runtime_error("cannot publish snapshot object");
            syncDirectory(objectsRoot_);
            crashIfRequested("after-object-publish");
        }
    } catch (...) {
        if (output >= 0) ::close(output);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    setDurable(true);
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
        if (actor) insertEvent(*actor, event, fileId, path, {}, version.id);
        exec("COMMIT");
        crashIfRequested("after-metadata-commit");
    } catch (...) { exec("ROLLBACK"); throw; }
    version.objectPath = objectsRoot_ / objectName;
    return version;
}

void Repository::insertEvent(const Actor& actor, const std::string& op, const std::string& fileId,
                             const std::string& path, const std::string& path2,
                             const std::string& versionId) {
    Statement insert(db_, "INSERT INTO events(time_ns,session_id,pid,uid,process,op,file_id,path,path2,version_id)"
                          " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
    sqlite3_bind_int64(insert.get(), 1, nowNanoseconds());
    bindOptional(insert.get(), 2, actor.sessionId);
    sqlite3_bind_int(insert.get(), 3, actor.pid);
    sqlite3_bind_int64(insert.get(), 4, actor.uid);
    bindText(insert.get(), 5, actor.process.empty() ? "?" : actor.process);
    bindText(insert.get(), 6, op);
    bindOptional(insert.get(), 7, fileId);
    bindText(insert.get(), 8, path);
    bindOptional(insert.get(), 9, path2);
    bindOptional(insert.get(), 10, versionId);
    requireDone(db_, insert.get());
}

std::int64_t Repository::logEvent(const Actor& actor, const std::string& op, const std::string& fileId,
                                  const std::string& path, const std::string& path2) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    setDurable(false);
    // path2 is a rename destination or a symlink target, which may legitimately be relative.
    insertEvent(actor, op, fileId, normalizePath(path), path2, {});
    return sqlite3_last_insert_rowid(db_);
}

void Repository::cancelEvent(std::int64_t eventId) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    Statement remove(db_, "DELETE FROM events WHERE id=?1");
    sqlite3_bind_int64(remove.get(), 1, eventId);
    requireDone(db_, remove.get());
}

std::vector<VersionRecord> Repository::history(const std::string& rawPath) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string path = normalizePath(rawPath);
    auto fileId = findFileId(path, true);
    if (!fileId) throw std::runtime_error("no tracked file or deleted history at this path");
    // History is the file's own identity plus every state recorded at this path. Editors and
    // `sed -i` save by renaming a temp file over the original, which replaces the identity;
    // matching on path keeps those earlier versions visible.
    Statement statement(db_, std::string("SELECT ") + kVersionColumns +
        ",COALESCE(e.process||'('||e.pid||')',''),COALESCE(s.label,'') FROM versions v "
        "LEFT JOIN events e ON e.version_id=v.id LEFT JOIN sessions s ON s.id=e.session_id "
        "WHERE v.file_id=?1 OR v.path=?2 ORDER BY v.created_ns DESC,v.sequence DESC");
    bindText(statement.get(), 1, *fileId);
    bindText(statement.get(), 2, path);
    std::vector<VersionRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        auto version = readVersion(statement.get(), objectsRoot_);
        const auto process = columnText(statement.get(), 14), label = columnText(statement.get(), 15);
        version.actor = label.empty() ? process : label + ":" + process;
        result.push_back(std::move(version));
    }
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
    Statement statement(db_, std::string("SELECT ") + kVersionColumns +
        " FROM versions v JOIN files f ON f.id=v.file_id WHERE v.id=?1 AND "
        "(f.current_path=?2 OR f.last_path=?2 OR v.path=?2) LIMIT 1");
    bindText(statement.get(), 1, versionId); bindText(statement.get(), 2, path);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error("version not found for path");
    return readVersion(statement.get(), objectsRoot_);
}

// Materialise a version at `target` atomically: stage in tmp/, restore metadata, fsync, rename.
void Repository::writeRestored(const VersionRecord& version, const std::filesystem::path& target) {
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = tempRoot_ / ("restore-" + randomId() + ".tmp");
    int input = ::open(version.objectPath.c_str(), O_RDONLY | O_CLOEXEC);
    int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    auto closeBoth = [&] {
        if (input >= 0) ::close(input);
        if (output >= 0) ::close(output);
        input = output = -1;
    };
    if (input < 0 || output < 0) {
        closeBoth();
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
        closeBoth();
        if (::rename(temporary.c_str(), target.c_str()) != 0)
            throw std::runtime_error(std::string("cannot publish restored file: ") + std::strerror(errno));
        syncDirectory(target.parent_path());
    } catch (...) {
        closeBoth();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void Repository::restore(const std::string& rawPath, const std::string& versionId,
                         const std::optional<std::string>& rawDestination,
                         const std::function<bool(const std::string&)>& hasWriters, const Actor* actor) {
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
    // In place: the version's own identity lives here, or (after an atomic-save rename) another
    // identity now occupies the path the version was recorded at. Either way the live file is
    // snapshotted and then overwritten, keeping whichever identity is current.
    const auto liveId = rawDestination ? std::nullopt : findFileId(destination, false);
    const bool inPlace = !rawDestination && liveId &&
                         ((!deleted && currentPath == destination) || version.path == destination);
    if (targetExists && !inPlace) throw std::runtime_error("restore destination already exists");
    if (inPlace) {
        if (hasWriters(destination)) throw std::runtime_error("file has an active writable handle");
        snapshotPath(destination, "restore", actor);
        writeRestored(version, target);
        return;
    }
    writeRestored(version, target);
    if (!rawDestination) reactivate(version.fileId, destination);
    const std::string restoredId = rawDestination ? ensureFile(destination) : version.fileId;
    if (actor) logEvent(*actor, rawDestination ? "create" : "restore", restoredId, destination);
}

void Repository::purge(const std::string& rawPath, const std::optional<std::string>& versionId) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    // A single version is resolved like show/restore do, so any version `history` lists (including
    // ones recorded at this path by an identity replaced through an atomic save) can be purged.
    auto fileId = versionId ? std::optional<std::string>(getVersion(rawPath, *versionId).fileId)
                            : findFileId(rawPath, true);
    if (!fileId) throw std::runtime_error("path has no version history");
    std::set<std::string> objects;
    Statement find(db_, versionId
        ? "SELECT object_name FROM versions WHERE file_id=?1 AND id=?2"
        : "SELECT object_name FROM versions WHERE file_id=?1");
    bindText(find.get(), 1, *fileId); if (versionId) bindText(find.get(), 2, *versionId);
    while (sqlite3_step(find.get()) == SQLITE_ROW) objects.insert(columnText(find.get(), 0));
    if (objects.empty()) throw std::runtime_error("no matching version to purge");
    setDurable(true);
    exec("BEGIN IMMEDIATE");
    try {
        Statement detach(db_, versionId
            ? "UPDATE events SET version_id=NULL WHERE version_id=?2"
            : "UPDATE events SET version_id=NULL WHERE version_id IN (SELECT id FROM versions WHERE file_id=?1)");
        bindText(detach.get(), 1, *fileId); if (versionId) bindText(detach.get(), 2, *versionId);
        requireDone(db_, detach.get());
        Statement remove(db_, versionId
            ? "DELETE FROM versions WHERE file_id=?1 AND id=?2"
            : "DELETE FROM versions WHERE file_id=?1");
        bindText(remove.get(), 1, *fileId); if (versionId) bindText(remove.get(), 2, *versionId);
        requireDone(db_, remove.get()); exec("COMMIT");
    } catch (...) { exec("ROLLBACK"); throw; }
    // Objects are shared between identical versions; delete only those nothing references.
    std::error_code ignored;
    for (const auto& object : objects) {
        Statement used(db_, "SELECT 1 FROM versions WHERE object_name=?1 LIMIT 1");
        bindText(used.get(), 1, object);
        if (sqlite3_step(used.get()) != SQLITE_ROW) std::filesystem::remove(objectsRoot_ / object, ignored);
    }
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
    Statement statement(db_, sql); if (fileId) bindText(statement.get(), 1, *fileId);
    std::vector<std::string> errors;
    std::map<std::filesystem::path, std::string> hashed;  // shared objects are hashed once
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        VersionRecord version = readVersion(statement.get(), objectsRoot_);
        if (!std::filesystem::is_regular_file(version.objectPath)) {
            errors.push_back(version.id + ": object is missing");
            continue;
        }
        auto found = hashed.find(version.objectPath);
        if (found == hashed.end()) found = hashed.emplace(version.objectPath, sha256File(version.objectPath)).first;
        if (found->second != version.checksum) errors.push_back(version.id + ": checksum mismatch");
    }
    return errors;
}

RepositoryStatus Repository::status() {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    RepositoryStatus result;
    Statement counts(db_, "SELECT COUNT(*),COALESCE(SUM(CASE WHEN deleted=1 THEN 1 ELSE 0 END),0) FROM files");
    if (sqlite3_step(counts.get()) == SQLITE_ROW) {
        result.files = sqlite3_column_int64(counts.get(), 0);
        result.deletedFiles = sqlite3_column_int64(counts.get(), 1);
    }
    Statement versions(db_, "SELECT COUNT(*),COALESCE(SUM(size),0) FROM versions");
    if (sqlite3_step(versions.get()) == SQLITE_ROW) {
        result.versions = sqlite3_column_int64(versions.get(), 0);
        result.objectBytes = sqlite3_column_int64(versions.get(), 1);
    }
    Statement stored(db_, "SELECT COALESCE(SUM(size),0) FROM "
                          "(SELECT MAX(size) size FROM versions GROUP BY object_name)");
    if (sqlite3_step(stored.get()) == SQLITE_ROW) result.storedBytes = sqlite3_column_int64(stored.get(), 0);
    Statement events(db_, "SELECT (SELECT COUNT(*) FROM events),(SELECT COUNT(*) FROM sessions)");
    if (sqlite3_step(events.get()) == SQLITE_ROW) {
        result.events = sqlite3_column_int64(events.get(), 0);
        result.sessions = sqlite3_column_int64(events.get(), 1);
    }
    return result;
}

std::string Repository::beginSession(const std::string& label, int rootPid, std::uint64_t rootStartTime) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const std::string id = randomId();
    setDurable(false);
    Statement insert(db_, "INSERT INTO sessions(id,label,status,root_pid,root_start,started_ns)"
                          " VALUES(?1,?2,'active',?3,?4,?5)");
    bindText(insert.get(), 1, id);
    bindText(insert.get(), 2, label.empty() ? "session-" + id.substr(0, 8) : label);
    sqlite3_bind_int(insert.get(), 3, rootPid);
    sqlite3_bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(rootStartTime));
    sqlite3_bind_int64(insert.get(), 5, nowNanoseconds());
    requireDone(db_, insert.get());
    return id;
}

void Repository::endSession(const std::string& sessionId) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    setDurable(true);
    Statement update(db_, "UPDATE sessions SET status='ended',ended_ns=?1 WHERE id=?2 AND status='active'");
    sqlite3_bind_int64(update.get(), 1, nowNanoseconds());
    bindText(update.get(), 2, sessionId);
    requireDone(db_, update.get());
}

namespace {
constexpr const char* kSessionColumns =
    "s.id,s.label,s.status,s.root_pid,s.root_start,s.started_ns,COALESCE(s.ended_ns,0),"
    "(SELECT COUNT(*) FROM events e WHERE e.session_id=s.id),"
    "(SELECT COUNT(DISTINCT COALESCE(e.file_id,e.path)) FROM events e WHERE e.session_id=s.id)";

SessionRecord readSession(sqlite3_stmt* row) {
    SessionRecord session;
    session.id = columnText(row, 0);
    session.label = columnText(row, 1);
    session.status = columnText(row, 2);
    session.rootPid = sqlite3_column_int(row, 3);
    session.rootStartTime = static_cast<std::uint64_t>(sqlite3_column_int64(row, 4));
    session.startedNs = sqlite3_column_int64(row, 5);
    session.endedNs = sqlite3_column_int64(row, 6);
    session.events = static_cast<std::uint64_t>(sqlite3_column_int64(row, 7));
    session.files = static_cast<std::uint64_t>(sqlite3_column_int64(row, 8));
    return session;
}
}  // namespace

std::vector<SessionRecord> Repository::sessions(bool activeOnly) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    Statement statement(db_, std::string("SELECT ") + kSessionColumns + " FROM sessions s" +
                                 (activeOnly ? " WHERE s.status='active'" : "") + " ORDER BY s.started_ns DESC");
    std::vector<SessionRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) result.push_back(readSession(statement.get()));
    return result;
}

// Resolve a full id, a unique id prefix, or (most recent) label.
SessionRecord Repository::session(const std::string& key) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    Statement byId(db_, std::string("SELECT ") + kSessionColumns +
                            " FROM sessions s WHERE substr(s.id,1,length(?1))=?1 ORDER BY s.started_ns DESC LIMIT 2");
    bindText(byId.get(), 1, key);
    std::vector<SessionRecord> matches;
    while (sqlite3_step(byId.get()) == SQLITE_ROW) matches.push_back(readSession(byId.get()));
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) throw std::runtime_error("session id prefix is ambiguous");
    Statement byLabel(db_, std::string("SELECT ") + kSessionColumns +
                               " FROM sessions s WHERE s.label=?1 ORDER BY s.started_ns DESC LIMIT 1");
    bindText(byLabel.get(), 1, key);
    if (sqlite3_step(byLabel.get()) == SQLITE_ROW) return readSession(byLabel.get());
    throw std::runtime_error("no session with this id or label");
}

std::vector<EventRecord> Repository::events(const std::optional<std::string>& rawPath,
                                            const std::optional<std::string>& sessionId, std::size_t limit) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    std::string sql = std::string("SELECT ") + kEventColumns +
                      " FROM events e LEFT JOIN sessions s ON s.id=e.session_id WHERE 1=1";
    std::string path, fileId;
    if (rawPath) {
        path = normalizePath(*rawPath);
        fileId = findFileId(path, true).value_or("");
        sql += " AND (e.file_id=?1 OR e.path=?2 OR e.path2=?2)";
    }
    if (sessionId) sql += " AND e.session_id=?3";
    sql += " ORDER BY e.id DESC LIMIT ?4";
    Statement statement(db_, sql);
    if (rawPath) { bindText(statement.get(), 1, fileId); bindText(statement.get(), 2, path); }
    if (sessionId) bindText(statement.get(), 3, *sessionId);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(limit));
    std::vector<EventRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) result.push_back(readEvent(statement.get()));
    return result;
}

// Selective undo: revert only what one session did, leaving everyone else's work in place.
//
//  0. Files the session created are preserved as an "undo" version, then removed.
//  1. Namespace events (delete, rename-overwrite, rename, mkdir, rmdir, symlinks) are inverted
//     newest-first, so paths are freed before older states are put back.
//  2. Each file the session modified gets its earliest pre-session version back.
//
// A file that anyone outside the session changed after the session first touched it is a
// conflict: it is reported and left alone unless --force. Every state the undo overwrites is
// snapshotted first, so the undo itself can be undone from ordinary history.
UndoReport Repository::undoSession(const std::string& key, bool dryRun, bool force, const Actor& actor,
                                   const std::function<bool(const std::string&)>& hasWriters) {
    std::lock_guard<std::recursive_mutex> guard(mutationMutex_);
    const SessionRecord target = session(key);
    if (target.status == "active") throw std::runtime_error("session is still running; end it before undoing");
    if (target.status == "undone") throw std::runtime_error("session has already been undone");

    std::vector<EventRecord> log;
    {
        Statement statement(db_, std::string("SELECT ") + kEventColumns +
            " FROM events e LEFT JOIN sessions s ON s.id=e.session_id WHERE e.session_id=?1 ORDER BY e.id ASC");
        bindText(statement.get(), 1, target.id);
        while (sqlite3_step(statement.get()) == SQLITE_ROW) log.push_back(readEvent(statement.get()));
    }
    UndoReport report;
    const std::string prefix = dryRun ? "would " : "";
    auto line = [&](const std::string& text) { report.lines.push_back(prefix + text); };
    auto touch = [&](const std::string& path) { report.paths.push_back(path); };

    std::map<std::string, std::int64_t> firstEvent;   // identity -> first session event
    std::set<std::string> created, skipped, keptPaths;  // keptPaths: where conflicted files live
    for (const auto& event : log) {
        if (event.fileId.empty()) continue;
        firstEvent.emplace(event.fileId, event.id);
        if (event.op == "create") created.insert(event.fileId);
    }
    for (const auto& [fileId, first] : firstEvent) {
        Statement later(db_, "SELECT e.op,e.process,e.pid,e.path FROM events e WHERE e.file_id=?1 AND e.id>?2 "
                             "AND (e.session_id IS NULL OR e.session_id<>?3) ORDER BY e.id LIMIT 1");
        bindText(later.get(), 1, fileId);
        sqlite3_bind_int64(later.get(), 2, first);
        bindText(later.get(), 3, target.id);
        const std::string path = currentPathForId(fileId);
        if (sqlite3_step(later.get()) == SQLITE_ROW) {
            report.conflicts.push_back(path + ": changed afterwards by " + columnText(later.get(), 1) + "(" +
                                       std::to_string(sqlite3_column_int(later.get(), 2)) + ") [" +
                                       columnText(later.get(), 0) + "]" + (force ? " - overriding" : " - kept"));
            if (!force) skipped.insert(fileId);
        }
        if (!skipped.count(fileId) && hasWriters(path)) {
            report.conflicts.push_back(path + ": open for writing - kept");
            skipped.insert(fileId);
        }
        if (skipped.count(fileId)) keptPaths.insert(path);
    }

    // A dry run changes nothing, so it tracks the namespace it *would* produce: later steps must
    // see paths freed or occupied by earlier ones, or the plan reports conflicts the real undo
    // never hits.
    std::map<std::string, bool> plannedExists;                             // path -> exists
    std::map<std::string, std::optional<std::string>> plannedLive;         // identity -> live path
    auto plan = [&](const std::string& path, bool present) { if (dryRun) plannedExists[path] = present; };
    auto planLive = [&](const std::string& fileId, std::optional<std::string> path) {
        if (dryRun && !fileId.empty()) plannedLive[fileId] = std::move(path);
    };

    auto liveCurrentPath = [&](const std::string& fileId) -> std::optional<std::string> {
        if (auto planned = plannedLive.find(fileId); planned != plannedLive.end()) return planned->second;
        Statement statement(db_, "SELECT current_path FROM files WHERE id=?1 AND deleted=0");
        bindText(statement.get(), 1, fileId);
        if (sqlite3_step(statement.get()) == SQLITE_ROW) return columnText(statement.get(), 0);
        return std::nullopt;
    };
    auto versionById = [&](const std::string& versionId) {
        Statement statement(db_, std::string("SELECT ") + kVersionColumns + " FROM versions v WHERE v.id=?1");
        bindText(statement.get(), 1, versionId);
        if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error("version was purged");
        return readVersion(statement.get(), objectsRoot_);
    };
    auto exists = [&](const std::string& path) {
        if (auto planned = plannedExists.find(path); planned != plannedExists.end()) return planned->second;
        struct stat info{};
        return ::lstat(backingPath(sourceRoot_, path).c_str(), &info) == 0;
    };

    // Phase 0: remove files the session created (their final content stays in history).
    for (const auto& fileId : created) {
        if (skipped.count(fileId)) continue;
        const auto path = liveCurrentPath(fileId);
        if (!path) continue;
        line("remove created " + *path);
        if (!dryRun) {
            snapshotPath(*path, "undo", &actor);
            if (::unlink(backingPath(sourceRoot_, *path).c_str()) != 0 && errno != ENOENT)
                throw std::runtime_error("cannot remove " + *path + ": " + std::strerror(errno));
            markDeleted(fileId, *path);
            logEvent(actor, "undo-create", fileId, *path);
        }
        plan(*path, false);
        planLive(fileId, std::nullopt);
        touch(*path);
        ++report.applied;
    }

    // Phase 1: invert namespace changes newest-first.
    for (auto event = log.rbegin(); event != log.rend(); ++event) {
        if (!event->fileId.empty() && (skipped.count(event->fileId) || created.count(event->fileId))) continue;
        const auto& op = event->op;
        if (op == "delete" || op == "rename-overwrite") {
            if (liveCurrentPath(event->fileId) || event->versionId.empty()) continue;
            if (exists(event->path)) {
                // Already reported when the file now at this path is itself a conflict.
                if (!keptPaths.count(event->path))
                    report.conflicts.push_back(event->path + ": cannot bring back deleted file, path is occupied - kept");
                continue;
            }
            line("restore deleted " + event->path);
            if (!dryRun) {
                writeRestored(versionById(event->versionId), backingPath(sourceRoot_, event->path));
                reactivate(event->fileId, event->path);
                logEvent(actor, "undo-delete", event->fileId, event->path);
            }
            plan(event->path, true);
            planLive(event->fileId, event->path);
            touch(event->path);
            ++report.applied;
        } else if (op == "rename" || op == "rename-dir" || op == "rename-other") {
            const bool directory = op == "rename-dir";
            if (!exists(event->path2) || exists(event->path)) {
                report.conflicts.push_back(event->path2 + ": cannot move back to " + event->path + " - kept");
                continue;
            }
            line("move " + event->path2 + " back to " + event->path);
            if (!dryRun) {
                const auto from = backingPath(sourceRoot_, event->path2), to = backingPath(sourceRoot_, event->path);
                std::filesystem::create_directories(to.parent_path());
                if (::rename(from.c_str(), to.c_str()) != 0)
                    throw std::runtime_error("cannot move back " + event->path2 + ": " + std::strerror(errno));
                renamePath(event->path2, event->path, directory);
                logEvent(actor, "undo-rename", event->fileId, event->path2, event->path);
            }
            plan(event->path2, false);
            plan(event->path, true);
            planLive(event->fileId, event->path);
            touch(event->path); touch(event->path2);
            ++report.applied;
        } else if (op == "mkdir") {
            line("remove directory " + event->path);
            if (!dryRun && ::rmdir(backingPath(sourceRoot_, event->path).c_str()) != 0 && errno != ENOENT) {
                report.conflicts.push_back(event->path + ": directory is not empty - kept");
                continue;
            }
            plan(event->path, false);
            touch(event->path);
            ++report.applied;
        } else if (op == "rmdir") {
            if (exists(event->path)) continue;
            line("recreate directory " + event->path);
            if (!dryRun) std::filesystem::create_directories(backingPath(sourceRoot_, event->path));
            plan(event->path, true);
            touch(event->path);
            ++report.applied;
        } else if (op == "delete-symlink") {
            if (exists(event->path)) continue;
            line("recreate symlink " + event->path);
            if (!dryRun && ::symlink(event->path2.c_str(), backingPath(sourceRoot_, event->path).c_str()) != 0)
                throw std::runtime_error("cannot recreate symlink " + event->path + ": " + std::strerror(errno));
            plan(event->path, true);
            touch(event->path);
            ++report.applied;
        } else if (op == "symlink") {
            struct stat info{};
            const auto full = backingPath(sourceRoot_, event->path);
            if (!exists(event->path) || ::lstat(full.c_str(), &info) != 0 || !S_ISLNK(info.st_mode)) continue;
            line("remove symlink " + event->path);
            if (!dryRun) ::unlink(full.c_str());
            plan(event->path, false);
            touch(event->path);
            ++report.applied;
        }
    }

    // Phase 2: put back each modified file's pre-session content and metadata.
    std::map<std::string, std::string> earliest;  // identity -> first content version
    for (const auto& event : log)
        if (!event.fileId.empty() && !event.versionId.empty() && isContentEvent(event.op))
            earliest.emplace(event.fileId, event.versionId);
    for (const auto& [fileId, versionId] : earliest) {
        if (skipped.count(fileId) || created.count(fileId)) continue;
        const auto path = liveCurrentPath(fileId);
        if (!path) continue;
        line("restore " + *path + " to its pre-session state");
        if (!dryRun) {
            snapshotPath(*path, "undo", &actor);
            writeRestored(versionById(versionId), backingPath(sourceRoot_, *path));
        }
        touch(*path);
        ++report.applied;
    }

    if (!dryRun) {
        setDurable(true);
        Statement update(db_, "UPDATE sessions SET status='undone' WHERE id=?1");
        bindText(update.get(), 1, target.id);
        requireDone(db_, update.get());
    }
    return report;
}

}  // namespace versionfs
