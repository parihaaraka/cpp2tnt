#ifndef WTF_BUFFER_H
#define WTF_BUFFER_H

#include <variant>
#include <vector>
#include "fu2/function2.hpp"

/// Lazy buffer (my bad :)
/// Proxy class over own internal vector (owning) or external vector/raw buffer (non-owning).
class wtf_buffer
{
public:
    explicit wtf_buffer(size_t size = 1024 * 1024);
    /// construct over existing vector, non-owning mode
    wtf_buffer(std::vector<char> &buf, size_t offset = 0);
    /// construct over existing vector, take ownership
    wtf_buffer(std::vector<char> &&buf, size_t offset = 0);
    /// Non-owning wrap around any external buffer with optional resize functionality.
    /// You may capture any container within realloc functinal object to own the container.
    wtf_buffer(char *data, size_t length, fu2::unique_function<char*(size_t)> realloc = {});

    size_t capacity() const noexcept;
    size_t size() const noexcept;
    size_t available() const noexcept;
    char* data() noexcept;
    const char* data() const noexcept;
    void reserve(size_t size);
    void resize(size_t size);
    void clear() noexcept;
    //void swap(wtf_buffer &other) noexcept;

    wtf_buffer(wtf_buffer &&) = default;
    wtf_buffer& operator= (wtf_buffer &&) = default;
    wtf_buffer(const wtf_buffer &) = delete;
    wtf_buffer& operator= (const wtf_buffer &) = delete;

    char *end; // let msgpuck to write directly

private:
    // to avoid teplating to preserve old code compatibility and stay simple
    // due to single type (two random buffers have the same type => simple move, swap, etc)
    std::variant<fu2::unique_function<char*(size_t)>, std::vector<char>*, std::vector<char>> target;
    char *head = nullptr;
    char *end_of_storage = nullptr;
};

#endif // WTF_BUFFER_H
