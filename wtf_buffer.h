#ifndef WTF_BUFFER_H
#define WTF_BUFFER_H

#include <vector>
#include <functional>

/// lazy buffer (my bad :)
class wtf_buffer
{
public:
    explicit wtf_buffer(size_t size = 1024 * 1024);
    wtf_buffer(std::vector<char> buf);
    size_t capacity() const noexcept;
    size_t size() const noexcept;
    size_t available() const noexcept;
    char* data() noexcept;
    const char* data() const noexcept;
    void reserve(size_t size);
    void resize(size_t size);
    void clear() noexcept;
    void swap(wtf_buffer &other) noexcept;

    char *end; // let msgpuck to write directly

    std::function<void()> on_clear;
    wtf_buffer(wtf_buffer &&);
    wtf_buffer& operator= (wtf_buffer &&);
    wtf_buffer(const wtf_buffer &) = delete;
    wtf_buffer& operator= (const wtf_buffer &) = delete;
private:
    std::vector<char> _buf;
};

#endif // WTF_BUFFER_H
