#include "wtf_buffer.h"

wtf_buffer::wtf_buffer(size_t size) : _buf(size)
{
    end = _buf.data();
}

size_t wtf_buffer::capacity() const noexcept
{
    return _buf.size();
}

size_t wtf_buffer::size() const noexcept
{
    return static_cast<size_t>(end - _buf.data());
}

size_t wtf_buffer::available() const noexcept
{
    return _buf.size() - size();
}

char *wtf_buffer::data() noexcept
{
    return _buf.data();
}

void wtf_buffer::reserve(size_t size)
{
    size_t s = this->size();
    _buf.resize(size);
    end = _buf.data() + s;
}

void wtf_buffer::resize(size_t size)
{
    if (size > capacity())
        _buf.resize(size);
    end = _buf.data() + size;
}

void wtf_buffer::clear() noexcept
{
    end = _buf.data();
    if (on_clear)
        on_clear();
}

void wtf_buffer::swap(wtf_buffer &other) noexcept
{
    _buf.swap(other._buf);
    std::swap(end, other.end);
    // DO NOT swap on_clear!
}

