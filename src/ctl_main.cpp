#include "versionfs/control.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <vector>

namespace {

void usage() {
    std::cerr << "Usage:\n"
              << "  versionfsctl --source <backing-dir> history <path>\n"
              << "  versionfsctl --source <backing-dir> show <path> <version-id>\n"
              << "  versionfsctl --source <backing-dir> restore <path> <version-id> [--to <path>]\n"
              << "  versionfsctl --source <backing-dir> deleted|status|verify [path]\n"
              << "  versionfsctl --source <backing-dir> purge <path> <version-id|--all>\n"
              << "  versionfsctl --source <backing-dir> log [path] [--session <id|label>] [--limit N]\n"
              << "\n"
              << "Supervised sessions (attribute and undo everything a process tree does):\n"
              << "  versionfsctl --source <backing-dir> run [--label <name>] -- <command> [args...]\n"
              << "  versionfsctl --source <backing-dir> sessions\n"
              << "  versionfsctl --source <backing-dir> changes <session>\n"
              << "  versionfsctl --source <backing-dir> undo <session> [--dry-run] [--force]\n";
}

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) value.pop_back();
    return value;
}

// Register this process as a session root, run the command as its child, then close the
// session. The daemon attributes every descendant of this PID to the session.
int runSession(const std::filesystem::path& socket, const std::vector<std::string>& command) {
    std::string label;
    std::size_t start = 1;
    while (start < command.size()) {
        if (command[start] == "--label" && start + 1 < command.size()) { label = command[start + 1]; start += 2; }
        else if (command[start] == "--") { ++start; break; }
        else break;
    }
    if (start >= command.size()) { usage(); return 2; }
    const auto begin = versionfs::sendControlRequest(socket, {"session-begin", label});
    if (!begin.ok) { std::cerr << "versionfsctl: " << begin.payload << '\n'; return 1; }
    const std::string id = trim(begin.payload);
    std::cerr << "versionfs: session " << (label.empty() ? id.substr(0, 8) : label) << " started (id "
              << id.substr(0, 8) << ")\n";

#ifdef __linux__
    // Orphaned descendants (double-forked daemons, background jobs) re-parent to this process
    // instead of init, so the daemon's ancestry walk still attributes them to the session.
    ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif
    // The child gets the terminal's signals; the supervisor must survive them to end the session.
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGQUIT, SIG_IGN);
    const pid_t child = ::fork();
    if (child < 0) { std::perror("fork"); return 1; }
    if (child == 0) {
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGQUIT, SIG_DFL);
        ::setenv("VERSIONFS_SESSION", id.c_str(), 1);
        std::vector<char*> argv;
        for (std::size_t i = start; i < command.size(); ++i) argv.push_back(const_cast<char*>(command[i].c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::perror(argv[0]);
        _exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {}  // reap orphans adopted as subreaper
    const auto end = versionfs::sendControlRequest(socket, {"session-end", id});
    std::cerr << "versionfs: " << trim(end.payload) << "\n"
              << "versionfs: review with `versionfsctl changes " << id.substr(0, 8) << "`, revert with `"
              << "versionfsctl undo " << id.substr(0, 8) << "`\n";
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string source;
    std::vector<std::string> command;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        // Everything after a `run` subcommand belongs to the supervised command.
        if (argument == "--source" && i + 1 < argc && (command.empty() || command[0] != "run")) source = argv[++i];
        else command.push_back(argument);
    }
    if (source.empty() || command.empty()) { usage(); return 2; }
    if (command[0] == "restore" && command.size() == 5 && command[3] == "--to")
        command = {"restore", command[1], command[2], command[4]};
    try {
        const auto root = std::filesystem::weakly_canonical(source);
        const auto socket = root / ".versionfs" / "control.sock";
        if (command[0] == "run") return runSession(socket, command);
        const auto response = versionfs::sendControlRequest(socket, command);
        std::ostream& stream = response.ok ? std::cout : std::cerr;
        if (!response.payload.empty()) stream.write(response.payload.data(), response.payload.size());
        if (!response.ok && (response.payload.empty() || response.payload.back() != '\n')) std::cerr << '\n';
        return response.ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "versionfsctl: " << error.what() << '\n';
        return 1;
    }
}
