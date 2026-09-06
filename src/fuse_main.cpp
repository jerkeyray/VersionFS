#include "versionfs/fuse_state.hpp"

#include <fuse3/fuse.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cerr << "Usage: versionfs mount --source <backing-dir> --mount <mountpoint> [--foreground]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) != "mount") { usage(); return 2; }
    std::string source, mount;
    bool foreground = false;
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--source" && i + 1 < argc) source = argv[++i];
        else if (argument == "--mount" && i + 1 < argc) mount = argv[++i];
        else if (argument == "--foreground") foreground = true;
        else { usage(); return 2; }
    }
    if (source.empty() || mount.empty()) { usage(); return 2; }
    try {
        const auto canonicalSource = std::filesystem::weakly_canonical(source);
        const auto canonicalMount = std::filesystem::weakly_canonical(mount);
        const auto sourceText = canonicalSource.generic_string() + "/";
        const auto mountText = canonicalMount.generic_string() + "/";
        if (sourceText.rfind(mountText, 0) == 0 || mountText.rfind(sourceText, 0) == 0) {
            std::cerr << "source and mountpoint must not overlap\n";
            return 2;
        }
        auto state = std::make_unique<versionfs::FuseState>(canonicalSource);
        std::vector<std::string> arguments{argv[0], canonicalMount.string()};
        if (foreground) arguments.emplace_back("-f");
        std::vector<char*> fuseArguments;
        for (auto& argument : arguments) fuseArguments.push_back(argument.data());
        const int result = fuse_main(static_cast<int>(fuseArguments.size()), fuseArguments.data(),
                                     &versionfs::operations(), state.get());
        return result;
    } catch (const std::exception& error) {
        std::cerr << "versionfs: " << error.what() << '\n';
        return 1;
    }
}
