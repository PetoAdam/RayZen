#include "Renderer/WarmWindow.h"

#include "Logger.h"

#include <utility>

#ifdef __linux__
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool address(const std::string& scope, sockaddr_un& result, socklen_t& length) {
    result = {};
    result.sun_family = AF_UNIX;
    const std::string name = "rayzen-warm-" + std::to_string(geteuid()) +
                             (scope.empty() ? "" : "-" + scope);
    if (name.size() >= sizeof(result.sun_path) - 1) {
        Logger::error("Warm-window control address is too long");
        return false;
    }
    std::memcpy(result.sun_path + 1, name.data(), name.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return true;
}

void reportError(const char* operation) {
    Logger::error(std::string(operation) + ": " + std::strerror(errno));
}

} // namespace
#endif

namespace rayzen {

WarmWindow::WarmWindow(std::string scope) : scope_(std::move(scope)) {}

WarmWindow::~WarmWindow() {
#ifdef __linux__
    if (socket_ >= 0) close(socket_);
#endif
}

bool WarmWindow::open() {
#ifdef __linux__
    socket_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_ < 0) {
        reportError("Cannot create warm-window control socket");
        return false;
    }
    const int enabled = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
        reportError("Cannot enable warm-window credential checks");
        return false;
    }
    socklen_t length;
    sockaddr_un addr;
    if (!address(scope_, addr, length)) return false;
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&addr), length) != 0) {
        reportError("Cannot start warm-window control (another instance may already be running)");
        return false;
    }
    return true;
#else
    Logger::error("Warm-window control is available only on Linux");
    return false;
#endif
}

WarmCommand WarmWindow::poll() {
#ifdef __linux__
    char command = 0;
    iovec iov{&command, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(ucred))]{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t count = recvmsg(socket_, &message, 0);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WarmCommand::None;
        reportError("Cannot receive warm-window command");
        return WarmCommand::Error;
    }
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS &&
            c->cmsg_len >= CMSG_LEN(sizeof(ucred))) {
            ucred credentials{};
            std::memcpy(&credentials, CMSG_DATA(c), sizeof(credentials));
            if (credentials.uid == geteuid() && count == 1 &&
                !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
                if (command == 'r') return WarmCommand::Restore;
                if (command == 'q') return WarmCommand::Quit;
            }
        }
    }
    Logger::error("Rejected invalid or unauthorized warm-window command");
    return WarmCommand::None;
#else
    return WarmCommand::Error;
#endif
}

bool WarmWindow::send(WarmCommand command, const std::string& scope) {
#ifdef __linux__
    if (command != WarmCommand::Restore && command != WarmCommand::Quit) {
        Logger::error("Invalid warm-window command");
        return false;
    }
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        reportError("Cannot create warm-window command socket");
        return false;
    }
    socklen_t length;
    sockaddr_un addr;
    if (!address(scope, addr, length)) {
        close(fd);
        return false;
    }
    const char message = command == WarmCommand::Restore ? 'r' : 'q';
    const ssize_t sent = sendto(fd, &message, 1, 0, reinterpret_cast<const sockaddr*>(&addr), length);
    if (sent != 1) reportError("Cannot contact a warm RayZen instance");
    close(fd);
    return sent == 1;
#else
    Logger::error("Warm-window control is available only on Linux");
    return false;
#endif
}

} // namespace rayzen
