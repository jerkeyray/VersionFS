#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace versionfs {

struct ControlResponse {
    bool ok{};
    std::string payload;
};

// Kernel-reported identity of the process on the other end of the control socket.
struct ControlPeer {
    int pid{-1};
    std::uint32_t uid{};
};

using ControlHandler =
    std::function<ControlResponse(const ControlPeer&, const std::vector<std::string>&)>;

class ControlServer {
public:
    ControlServer(std::filesystem::path socketPath, ControlHandler handler);
    ~ControlServer();
    void start();
    void stop();

private:
    void run();
    std::filesystem::path socketPath_;
    ControlHandler handler_;
    std::atomic<bool> stopping_{false};
    int listenFd_{-1};
    std::thread thread_;
};

ControlResponse sendControlRequest(const std::filesystem::path& socketPath,
                                   const std::vector<std::string>& arguments);

}  // namespace versionfs
