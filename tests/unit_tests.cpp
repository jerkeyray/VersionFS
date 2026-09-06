#include "versionfs/repository.hpp"
#include "versionfs/util.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
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

}  // namespace

int main() {
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
        repository.restore("/renamed.txt", first->id, std::nullopt, [](const std::string&) { return false; });
        assert(readFile(root / "renamed.txt") == "first version\n");
        assert(repository.verify(std::nullopt).empty());
        const auto status = repository.status();
        assert(status.versions == 2);
    }
    std::filesystem::remove_all(root);
    std::cout << "versionfs unit tests passed\n";
}
