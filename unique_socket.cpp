#include "unique_socket.h"
#include <unistd.h>

unique_socket::unique_socket(int fd): _fd(fd) {}

unique_socket::unique_socket(unique_socket &&src)
{
    _fd = src._fd;
    src._fd = -1;
}

unique_socket &unique_socket::operator=(unique_socket &&src)
{
    if (this != &src)
    {
        close();
        _fd = src._fd;
        src._fd = -1;
    }
    return *this;
}

unique_socket::~unique_socket()
{
    close();
}

unique_socket &unique_socket::operator=(int fd)
{
    close();
    _fd = fd;
    return *this;
}

int unique_socket::handle() const noexcept
{
    return _fd;
}

unique_socket::operator bool() const noexcept
{
    return _fd >= 0;
}

void unique_socket::close() noexcept
{
    if (_fd < 0)
        return;
    ::close(_fd);
    _fd = -1;
}
