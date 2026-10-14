#include "versionfs/util.hpp"

#include <openssl/evp.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace versionfs {

std::string normalizePath(const std::string& input) {
    if (input.empty() || input.front() != '/') {
        throw std::invalid_argument("path must be absolute within the mount");
    }
    std::filesystem::path normalized = std::filesystem::path(input).lexically_normal();
    std::string result = normalized.generic_string();
    if (result.empty()) result = "/";
    for (const auto& part : normalized) {
        if (part == "..") throw std::invalid_argument("path escapes the mount");
    }
    return result;
}

bool isInternalPath(const std::string& input) {
    const std::string path = normalizePath(input);
    return path == "/.versionfs" || path.rfind("/.versionfs/", 0) == 0;
}

std::filesystem::path backingPath(const std::filesystem::path& root,
                                  const std::string& virtualPath) {
    const std::string path = normalizePath(virtualPath);
    if (isInternalPath(path)) throw std::invalid_argument("reserved VersionFS path");
    if (path == "/") return root;
    return root / path.substr(1);
}

std::string randomId() {
    // Seed once per thread: a fresh std::random_device per id cost 16 getrandom() syscalls.
    thread_local std::mt19937_64 random{(std::uint64_t{std::random_device{}()} << 32) ^ std::random_device{}()};
    std::array<unsigned char, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        const std::uint64_t value = random();
        for (std::size_t j = 0; j < 8; ++j) bytes[i + j] = static_cast<unsigned char>(value >> (8 * j));
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

std::int64_t nowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Sha256::Sha256() : context_(EVP_MD_CTX_new()) {
    if (!context_ || EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(context_), EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(context_));
        throw std::runtime_error("cannot create SHA-256 context");
    }
}

Sha256::~Sha256() { EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(context_)); }

void Sha256::update(const void* data, std::size_t size) {
    if (EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(context_), data, size) != 1)
        throw std::runtime_error("cannot calculate SHA-256 checksum");
}

std::string Sha256::hex() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(context_), digest, &digestLength) != 1)
        throw std::runtime_error("cannot calculate SHA-256 checksum");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLength; ++i) out << std::setw(2) << unsigned(digest[i]);
    return out.str();
}

std::string sha256File(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open object for checksum");
    try {
        Sha256 hash;
        std::array<unsigned char, 64 * 1024> buffer{};
        while (true) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("cannot read object for checksum");
            }
            hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
        ::close(fd);
        return hash.hex();
    } catch (...) { ::close(fd); throw; }
}

std::string formatTimestamp(std::int64_t nanoseconds) {
    const std::time_t seconds = static_cast<std::time_t>(nanoseconds / 1000000000LL);
    std::tm value{};
    gmtime_r(&seconds, &value);
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << 'Z';
    return out.str();
}

void crashIfRequested(const char* point) {
#ifdef VERSIONFS_ENABLE_FAULT_INJECTION
    const char* requested = std::getenv("VERSIONFS_CRASH_AT");
    if (requested && std::string(requested) == point) _exit(86);
#else
    (void)point;
#endif
}

std::optional<ProcessInfo> readProcess(int pid) {
    if (pid <= 0) return std::nullopt;
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!input || !std::getline(input, line)) return std::nullopt;
    // Format: pid (comm) state ppid ... ; comm may itself contain spaces or ')'.
    const auto open = line.find('(');
    const auto close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return std::nullopt;
    ProcessInfo info;
    info.pid = pid;
    info.name = line.substr(open + 1, close - open - 1);
    std::istringstream fields(line.substr(close + 2));
    std::string field;
    for (int index = 3; fields >> field; ++index) {
        if (index == 4) info.ppid = std::stoi(field);
        if (index == 22) { info.startTime = std::stoull(field); break; }
    }
    return info;
}

int threadGroupOf(int tid) {
    std::ifstream input("/proc/" + std::to_string(tid) + "/status");
    std::string line;
    while (std::getline(input, line))
        if (line.rfind("Tgid:", 0) == 0) return std::stoi(line.substr(5));
    return tid;
}

}  // namespace versionfs
