#pragma once

#include <atomic>
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

using ControlHandler = std::function<ControlResponse(const std::vector<std::string>&)>;

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
