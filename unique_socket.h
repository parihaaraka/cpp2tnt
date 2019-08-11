#ifndef UNIQUE_SOCKET_H
#define UNIQUE_SOCKET_H

/// socket handle wrapper
class unique_socket
{
public:
    unique_socket(int fd = -1);
    unique_socket(unique_socket &&src);
    unique_socket& operator=(unique_socket &&src);
    unique_socket(const unique_socket &) = delete;
    unique_socket& operator=(const unique_socket &) = delete;
    ~unique_socket();
    operator bool() const noexcept;
    unique_socket& operator=(int fd);
    int handle() const noexcept;
    void close();
private:
    int _fd = -1;
};

#endif // UNIQUE_SOCKET_H
