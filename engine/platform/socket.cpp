#include <source_port/socket.h>
#include <source_port/socket_os.h>
#include <source_port/thread_os.h>
#include <climits>
namespace { unsigned owners = 0; }
int closesocket(SOCKET socket) { return close(socket); }
int ioctlsocket(SOCKET socket, unsigned long command, unsigned long* argument) {
    if (!argument) { errno = EINVAL; return -1; }
    if (command != FIONBIO) { errno = ENOTSUP; return -1; }
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(socket, F_SETFL, *argument ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
namespace SourcePort {
bool AcquireSockets() {
    if (!source_thread_is_controller()) { errno = EPERM; return false; }
    if (owners == UINT_MAX) { errno = EOVERFLOW; return false; }
    if (!owners && source_soc_init()) return false;
    ++owners;
    return true;
}
bool ReleaseSockets() {
    if (!source_thread_is_controller()) { errno = EPERM; return false; }
    if (!owners) { errno = EINVAL; return false; }
    if (owners == 1 && source_soc_exit()) return false;
    --owners;
    return true;
}
unsigned SocketOwners() { return owners; }
SOCKET OpenDatagramSocket(bool broadcast) {
    if (!AcquireSockets()) return -1;
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        unsigned long nonblocking = 1;
        int enabled = 1; // BSD SO_BROADCAST takes int, not host64 unsigned long
        if (ioctlsocket(fd, FIONBIO, &nonblocking) == 0 &&
            (!broadcast || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0))
            return fd;
    }
    int error = errno;
    if (fd >= 0) close(fd);
    ReleaseSockets();
    errno = error;
    return -1;
}
void CloseDatagramSocket(SOCKET& fd) {
    if (!source_thread_is_controller()) { errno = EPERM; return; }
    if (fd < 0) return;
    close(fd);
    fd = -1; // descriptor is consumed even if close reports an OS error
    ReleaseSockets();
}
}
