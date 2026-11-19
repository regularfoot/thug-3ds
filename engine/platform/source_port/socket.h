#pragma once
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstddef>
using SOCKET = int;
int closesocket(SOCKET socket);
int ioctlsocket(SOCKET socket, unsigned long command, unsigned long* argument);
namespace SourcePort {
// Main-thread service ownership. Offline/local GameNet apps do not acquire
// SOC, need a Wi-Fi connection, or reserve network service memory.
bool AcquireSockets();
bool ReleaseSockets();
unsigned SocketOwners();
SOCKET OpenDatagramSocket(bool broadcast);
void CloseDatagramSocket(SOCKET& socket);
}
