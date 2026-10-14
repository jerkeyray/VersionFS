#include "versionfs/repository.hpp"
#include "versionfs/util.hpp"

#include <cassert>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

namespace {

std::filesystem::path temporaryDirectory() {
    std::string pattern = "/tmp/versionfs-unit-XXXXXX";
    char* value = ::mkdtemp(pattern.data());
    assert(value != nullptr);
    return value;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file << content;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool noWriters(const std::string&) { return false; }

void testPathsAndHistory() {
    assert(versionfs::normalizePath("/a/../b") == "/b");
    assert(versionfs::isInternalPath("/.versionfs/metadata.db"));
    assert(!versionfs::isInternalPath("/.versionfs-not-internal"));
    bool rejected = false;
    try { (void)versionfs::backingPath("/tmp", "/.versionfs/metadata.db"); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    const auto root = temporaryDirectory();
    const auto file = root / "notes.txt";
    writeFile(file, "first version\n");
    {
        versionfs::Repository repository(root);
        const auto id = repository.ensureFile("/notes.txt");
        const auto first = repository.snapshotPath("/notes.txt", "write");
        assert(first && first->sequence == 1 && readFile(first->objectPath) == "first version\n");
        writeFile(file, "second version\n");
        const auto second = repository.snapshotPath("/notes.txt", "write");
        assert(second && second->sequence == 2 && readFile(second->objectPath) == "second version\n");
        repository.renamePath("/notes.txt", "/renamed.txt", false);
        const auto history = repository.history("/renamed.txt");
        assert(history.size() == 2 && history.front().fileId == id);
        repository.markDeleted(id, "/renamed.txt");
        assert(repository.deletedFiles().size() == 1);
        repository.restore("/renamed.txt", first->id, std::nullopt, noWriters);
        assert(readFile(root / "renamed.txt") == "first version\n");
        assert(repository.verify(std::nullopt).empty());
        assert(repository.status().versions == 2);
    }
    std::filesystem::remove_all(root);
}

// Identical states share one content-addressed object; purge keeps objects still referenced.
void testDeduplication() {
    const auto root = temporaryDirectory();
    writeFile(root / "a.txt", "same bytes\n");
    writeFile(root / "b.txt", "same bytes\n");
    {
        versionfs::Repository repository(root);
        const auto a = repository.snapshotPath("/a.txt", "write");
        const auto a2 = repository.snapshotPath("/a.txt", "chmod");
        const auto b = repository.snapshotPath("/b.txt", "write");
        assert(a->objectPath == a2->objectPath && a->objectPath == b->objectPath);
        const auto status = repository.status();
        assert(status.versions == 3 && status.objectBytes == 33 && status.storedBytes == 11);
        repository.purge("/a.txt", std::nullopt);
        assert(std::filesystem::exists(b->objectPath));
        assert(repository.verify(std::nullopt).empty());
        repository.purge("/b.txt", std::nullopt);
        assert(!std::filesystem::exists(b->objectPath));
    }
    std::filesystem::remove_all(root);
}

// A write-only handle (O_WRONLY without O_TRUNC, e.g. `>>`) must still be snapshotted.
void testWriteOnlySnapshot() {
    const auto root = temporaryDirectory();
    writeFile(root / "log.txt", "line one\n");
    {
        versionfs::Repository repository(root);
        const auto id = repository.ensureFile("/log.txt");
        int fd = ::open((root / "log.txt").c_str(), O_WRONLY | O_APPEND);
        assert(fd >= 0);
        const auto version = repository.snapshotFd(fd, id, "write");
        ::close(fd);
        assert(version && readFile(version->objectPath) == "line one\n");
    }
    std::filesystem::remove_all(root);
}

// Directory rename must match path prefixes literally: '_' is not a wildcard, case matters.
void testDirectoryRenameIsLiteral() {
    const auto root = temporaryDirectory();
    {
        versionfs::Repository repository(root);
        const auto lookalike = repository.ensureFile("/aXb/x");
        const auto otherCase = repository.ensureFile("/docs/y");
        const auto inside = repository.ensureFile("/a_b/z");
        repository.renamePath("/a_b", "/q", true);
        repository.renamePath("/Docs", "/elsewhere", true);
        assert(repository.findFileId("/aXb/x", false) == lookalike);
        assert(repository.findFileId("/docs/y", false) == otherCase);
        assert(repository.findFileId("/q/z", false) == inside);
        assert(!repository.findFileId("/q/x", false));
    }
    std::filesystem::remove_all(root);
}

// The core scenario: an agent session edits, deletes, creates, and renames files while a human
// edits one of the same files. Undo reverts only the agent and reports the overlap.
void testSelectiveUndo() {
    const auto root = temporaryDirectory();
    writeFile(root / "a.txt", "A0\n");
    writeFile(root / "b.txt", "B0\n");
    writeFile(root / "c.txt", "C0\n");
    {
        versionfs::Repository repository(root);
        for (const char* path : {"/a.txt", "/b.txt", "/c.txt"}) repository.ensureFile(path);
        const auto session = repository.beginSession("agent", 4242, 1);
        const versionfs::Actor agent{4242, 1000, "agent", session};
        const versionfs::Actor human{777, 1000, "vim", ""};

        repository.snapshotPath("/a.txt", "write", &agent);
        writeFile(root / "a.txt", "A1\n");
        const auto b = repository.snapshotPath("/b.txt", "delete", &agent);
        std::filesystem::remove(root / "b.txt");
        repository.markDeleted(b->fileId, "/b.txt");
        writeFile(root / "new.txt", "generated\n");
        repository.logEvent(agent, "create", repository.ensureFile("/new.txt"), "/new.txt");
        repository.snapshotPath("/c.txt", "write", &agent);
        writeFile(root / "c.txt", "C1\n");
        repository.snapshotPath("/c.txt", "write", &human);  // human keeps working meanwhile
        writeFile(root / "c.txt", "C2 (human)\n");
        writeFile(root / "human.txt", "mine\n");
        repository.logEvent(human, "create", repository.ensureFile("/human.txt"), "/human.txt");
        const auto a = repository.findFileId("/a.txt", false);
        std::filesystem::rename(root / "a.txt", root / "moved.txt");
        repository.renamePath("/a.txt", "/moved.txt", false);
        repository.logEvent(agent, "rename", *a, "/a.txt", "/moved.txt");

        bool refused = false;
        try { repository.undoSession("agent", false, false, human, noWriters); }
        catch (const std::runtime_error&) { refused = true; }
        assert(refused);  // still active
        repository.endSession(session);

        const versionfs::Actor operatorActor{1, 1000, "versionfsctl", ""};
        const auto plan = repository.undoSession("agent", true, false, operatorActor, noWriters);
        assert(plan.applied > 0 && plan.conflicts.size() == 1);
        assert(readFile(root / "moved.txt") == "A1\n");  // dry run changes nothing

        const auto report = repository.undoSession(session.substr(0, 8), false, false, operatorActor, noWriters);
        assert(report.conflicts.size() == 1);
        assert(readFile(root / "a.txt") == "A0\n");
        assert(!std::filesystem::exists(root / "moved.txt"));
        assert(readFile(root / "b.txt") == "B0\n");
        assert(!std::filesystem::exists(root / "new.txt"));
        assert(readFile(root / "c.txt") == "C2 (human)\n");  // conflict: human's work is kept
        assert(readFile(root / "human.txt") == "mine\n");
        assert(repository.session(session).status == "undone");
        assert(!repository.history("/new.txt").empty());  // undo preserved what it removed
        assert(repository.verify(std::nullopt).empty());

        refused = false;
        try { repository.undoSession(session, false, false, operatorActor, noWriters); }
        catch (const std::runtime_error&) { refused = true; }
        assert(refused);  // undo is not applied twice
    }
    std::filesystem::remove_all(root);
}

// `sed -i` replaces a file by renaming a new temp file over it. The dry-run plan has to account
// for the temp file being removed first, or it reports a conflict the real undo never hits.
void testDryRunMatchesUndo() {
    const auto root = temporaryDirectory();
    writeFile(root / "report.txt", "draft\n");
    {
        versionfs::Repository repository(root);
        const auto original = repository.ensureFile("/report.txt");
        const auto session = repository.beginSession("sed", 99, 1);
        const versionfs::Actor sed{99, 1000, "sed", session};
        writeFile(root / "sedTMP", "DRAFT\n");
        repository.logEvent(sed, "create", repository.ensureFile("/sedTMP"), "/sedTMP");
        repository.snapshotPath("/report.txt", "rename-overwrite", &sed);
        std::filesystem::rename(root / "sedTMP", root / "report.txt");
        repository.renamePath("/sedTMP", "/report.txt", false);
        repository.endSession(session);

        const versionfs::Actor operatorActor{1, 1000, "versionfsctl", ""};
        const auto plan = repository.undoSession("sed", true, false, operatorActor, noWriters);
        const auto done = repository.undoSession("sed", false, false, operatorActor, noWriters);
        assert(plan.conflicts.empty() && done.conflicts.empty());
        assert(plan.applied == done.applied);
        assert(readFile(root / "report.txt") == "draft\n");
        assert(repository.findFileId("/report.txt", false) == original);
        // Every version history lists for the path can be purged, whichever identity owns it.
        for (const auto& version : repository.history("/report.txt")) repository.purge("/report.txt", version.id);
        assert(repository.history("/report.txt").empty());
    }
    std::filesystem::remove_all(root);
}

// A metadata database written by v1 (object_name UNIQUE, no provenance tables) is upgraded.
void testSchemaMigration() {
    const auto root = temporaryDirectory();
    std::filesystem::create_directories(root / ".versionfs" / "objects");
    writeFile(root / ".versionfs" / "objects" / "legacy.blob", "old\n");
    writeFile(root / "f.txt", "now\n");
    sqlite3* db = nullptr;
    assert(sqlite3_open((root / ".versionfs" / "metadata.db").c_str(), &db) == SQLITE_OK);
    const std::string checksum = versionfs::sha256File(root / ".versionfs" / "objects" / "legacy.blob");
    const std::string sql =
        "CREATE TABLE files(id TEXT PRIMARY KEY,current_path TEXT UNIQUE,last_path TEXT NOT NULL,"
        "deleted INTEGER NOT NULL DEFAULT 0,created_ns INTEGER NOT NULL,deleted_ns INTEGER);"
        "CREATE TABLE versions(id TEXT PRIMARY KEY,file_id TEXT NOT NULL REFERENCES files(id),sequence INTEGER NOT NULL,"
        "event TEXT NOT NULL,path TEXT NOT NULL,object_name TEXT NOT NULL UNIQUE,checksum TEXT NOT NULL,"
        "size INTEGER NOT NULL,mode INTEGER NOT NULL,uid INTEGER NOT NULL,gid INTEGER NOT NULL,"
        "mtime_sec INTEGER NOT NULL,mtime_nsec INTEGER NOT NULL,created_ns INTEGER NOT NULL,UNIQUE(file_id,sequence));"
        "CREATE INDEX versions_file_sequence ON versions(file_id,sequence DESC);"
        "INSERT INTO files VALUES('f1','/f.txt','/f.txt',0,1,NULL);"
        "INSERT INTO versions VALUES('v1','f1',1,'write','/f.txt','legacy.blob','" + checksum +
        "',4,33188,0,0,0,0,1);";
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    {
        versionfs::Repository repository(root);
        const auto history = repository.history("/f.txt");
        assert(history.size() == 1 && readFile(history.front().objectPath) == "old\n");
        assert(repository.verify(std::nullopt).empty());
        assert(repository.snapshotPath("/f.txt", "write")->sequence == 2);
    }
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    testPathsAndHistory();
    testDeduplication();
    testWriteOnlySnapshot();
    testDirectoryRenameIsLiteral();
    testSelectiveUndo();
    testDryRunMatchesUndo();
    testSchemaMigration();
    std::cout << "versionfs unit tests passed\n";
}
