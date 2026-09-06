#include "versionfs/control.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cerr << "Usage:\n"
              << "  versionfsctl --source <backing-dir> history <path>\n"
              << "  versionfsctl --source <backing-dir> show <path> <version-id>\n"
              << "  versionfsctl --source <backing-dir> restore <path> <version-id> [--to <path>]\n"
              << "  versionfsctl --source <backing-dir> deleted|status|verify [path]\n"
              << "  versionfsctl --source <backing-dir> purge <path> <version-id|--all>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string source;
    std::vector<std::string> command;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--source" && i + 1 < argc) source = argv[++i];
        else command.push_back(argument);
    }
    if (source.empty() || command.empty()) { usage(); return 2; }
    if (command[0] == "restore" && command.size() == 5 && command[3] == "--to")
        command = {"restore", command[1], command[2], command[4]};
    try {
        const auto root = std::filesystem::weakly_canonical(source);
        const auto response = versionfs::sendControlRequest(root / ".versionfs" / "control.sock", command);
        if (!response.payload.empty()) std::cout.write(response.payload.data(), response.payload.size());
        if (!response.ok) {
            if (response.payload.empty() || response.payload.back() != '\n') std::cerr << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "versionfsctl: " << error.what() << '\n';
        return 1;
    }
}
