#include "versionfs/util.hpp"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
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
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
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

std::string sha256File(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open object for checksum");
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) {
        ::close(fd);
        throw std::runtime_error("cannot create SHA-256 context");
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    while (ok) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        ok = EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1;
    }
    ok = ok && EVP_DigestFinal_ex(context, digest, &digestLength) == 1;
    EVP_MD_CTX_free(context);
    ::close(fd);
    if (!ok) throw std::runtime_error("cannot calculate SHA-256 checksum");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLength; ++i) out << std::setw(2) << unsigned(digest[i]);
    return out.str();
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

}  // namespace versionfs
