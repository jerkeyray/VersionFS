#include "versionfs/control.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace versionfs {
namespace {

constexpr std::uint32_t kMaxMessage = 64U * 1024U * 1024U;

void writeAll(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t result = ::write(fd, bytes, size);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("control socket write failed: ") + std::strerror(errno));
        }
        bytes += result;
        size -= static_cast<std::size_t>(result);
    }
}

void readAll(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t result = ::read(fd, bytes, size);
        if (result == 0) throw std::runtime_error("control socket closed unexpectedly");
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("control socket read failed: ") + std::strerror(errno));
        }
        bytes += result;
        size -= static_cast<std::size_t>(result);
    }
}

void appendU32(std::string& output, std::uint32_t value) {
    value = htonl(value);
    output.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::uint32_t takeU32(const std::string& input, std::size_t& offset) {
    if (offset + sizeof(std::uint32_t) > input.size()) throw std::runtime_error("malformed control request");
    std::uint32_t value{};
    std::memcpy(&value, input.data() + offset, sizeof(value));
    offset += sizeof(value);
    return ntohl(value);
}

std::string encodeArguments(const std::vector<std::string>& arguments) {
    std::string payload;
    appendU32(payload, static_cast<std::uint32_t>(arguments.size()));
    for (const auto& argument : arguments) {
        appendU32(payload, static_cast<std::uint32_t>(argument.size()));
        payload.append(argument);
    }
    return payload;
}

std::vector<std::string> decodeArguments(const std::string& payload) {
    std::size_t offset = 0;
    const auto count = takeU32(payload, offset);
    std::vector<std::string> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto size = takeU32(payload, offset);
        if (offset + size > payload.size()) throw std::runtime_error("malformed control request");
        result.emplace_back(payload.data() + offset, size);
        offset += size;
    }
    if (offset != payload.size()) throw std::runtime_error("malformed control request");
    return result;
}

std::string readFrame(int fd) {
    std::uint32_t networkSize{};
    readAll(fd, &networkSize, sizeof(networkSize));
    const std::uint32_t size = ntohl(networkSize);
    if (size > kMaxMessage) throw std::runtime_error("control message is too large");
    std::string payload(size, '\0');
    if (size) readAll(fd, payload.data(), payload.size());
    return payload;
}

void writeFrame(int fd, const std::string& payload) {
    if (payload.size() > kMaxMessage) throw std::runtime_error("control response is too large");
    const std::uint32_t size = htonl(static_cast<std::uint32_t>(payload.size()));
    writeAll(fd, &size, sizeof(size));
    if (!payload.empty()) writeAll(fd, payload.data(), payload.size());
}

// Ask the kernel who connected rather than trusting anything in the request payload.
ControlPeer peerOf(int fd) {
    ControlPeer peer;
#ifdef SO_PEERCRED
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
        throw std::runtime_error("cannot read control peer credentials");
    peer.pid = credentials.pid;
    peer.uid = credentials.uid;
#else
    uid_t uid{};
    gid_t gid{};
    if (::getpeereid(fd, &uid, &gid) != 0) throw std::runtime_error("cannot read control peer credentials");
    peer.uid = uid;
#endif
    return peer;
}

sockaddr_un socketAddress(const std::filesystem::path& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string value = path.string();
    if (value.size() >= sizeof(address.sun_path)) throw std::runtime_error("control socket path is too long");
    std::strncpy(address.sun_path, value.c_str(), sizeof(address.sun_path) - 1);
    return address;
}

}  // namespace

ControlServer::ControlServer(std::filesystem::path socketPath, ControlHandler handler)
    : socketPath_(std::move(socketPath)), handler_(std::move(handler)) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    std::error_code ignored;
    std::filesystem::remove(socketPath_, ignored);
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) throw std::runtime_error("cannot create control socket");
    const sockaddr_un address = socketAddress(socketPath_);
    if (::bind(listenFd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listenFd_, 8) != 0) {
        ::close(listenFd_); listenFd_ = -1;
        throw std::runtime_error(std::string("cannot bind control socket: ") + std::strerror(errno));
    }
    ::chmod(socketPath_.c_str(), 0600);
    thread_ = std::thread(&ControlServer::run, this);
}

void ControlServer::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    int wake = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (wake >= 0) {
        const sockaddr_un address = socketAddress(socketPath_);
        ::connect(wake, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::close(wake);
    }
    thread_.join();
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    std::error_code ignored;
    std::filesystem::remove(socketPath_, ignored);
}

void ControlServer::run() {
    while (!stopping_) {
        int client = ::accept(listenFd_, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stopping_) { ::close(client); break; }
        try {
            ControlResponse response;
            try {
                const ControlPeer peer = peerOf(client);
                if (peer.uid != ::geteuid() && peer.uid != 0)
                    throw std::runtime_error("control client is not the filesystem owner");
                response = handler_(peer, decodeArguments(readFrame(client)));
            }
            catch (const std::exception& error) { response = {false, error.what()}; }
            std::string frame(1, response.ok ? '\1' : '\0');
            frame += response.payload;
            writeFrame(client, frame);
        } catch (...) {}
        ::close(client);
    }
}

ControlResponse sendControlRequest(const std::filesystem::path& socketPath,
                                   const std::vector<std::string>& arguments) {
    int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket < 0) throw std::runtime_error("cannot create control client socket");
    const sockaddr_un address = socketAddress(socketPath);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string message = std::string("cannot connect to VersionFS daemon: ") + std::strerror(errno);
        ::close(socket); throw std::runtime_error(message);
    }
    try {
        writeFrame(socket, encodeArguments(arguments));
        std::string response = readFrame(socket);
        ::close(socket);
        if (response.empty()) throw std::runtime_error("empty response from VersionFS daemon");
        return {response.front() != '\0', response.substr(1)};
    } catch (...) { ::close(socket); throw; }
}

}  // namespace versionfs
