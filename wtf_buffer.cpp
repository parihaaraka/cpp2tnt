#include "wtf_buffer.h"

wtf_buffer::wtf_buffer(size_t size) : _local_buf(size)
{
    _buf = &_local_buf;
    end = _buf->data();
}

wtf_buffer::wtf_buffer(std::vector<char> &buf, size_t offset) : _buf(&buf)
{
    end = _buf->data() + offset;
}

wtf_buffer::wtf_buffer(std::vector<char> &&buf, size_t offset) : _local_buf(std::move(buf))
{
    _buf = &_local_buf;
    end = _buf->data() + offset;
}

size_t wtf_buffer::capacity() const noexcept
{
    return _buf->size();
}

size_t wtf_buffer::size() const noexcept
{
    return static_cast<size_t>(end - _buf->data());
}

size_t wtf_buffer::available() const noexcept
{
    return _buf->size() - size();
}

char *wtf_buffer::data() noexcept
{
    return _buf->data();
}

const char *wtf_buffer::data() const noexcept
{
    return _buf->data();
}

void wtf_buffer::reserve(size_t size)
{
	size_t s = this->size();
    _buf->resize(size);
    end = _buf->data() + s;
}

void wtf_buffer::resize(size_t size)
{
    if (size > capacity())
        _buf->resize(size);
    end = _buf->data() + size;
}

void wtf_buffer::clear() noexcept
{
    end = _buf->data();
    if (on_clear)
        on_clear();
}

void wtf_buffer::swap(wtf_buffer &other) noexcept
{
    bool this_owner = (_buf == &_local_buf);
	size_t this_size = size();
    bool other_owner = (other._buf == &other._local_buf);
    size_t other_size = other.size();
    _local_buf.swap(other._local_buf);
    std::swap(_buf, other._buf);
    if (this_owner)
		other._buf = &other._local_buf;
	if (other_owner)
		_buf = &_local_buf;
	end = _buf->data() + other_size;
	other.end = other.data() + this_size;
    // DO NOT swap on_clear!
}

wtf_buffer::wtf_buffer(wtf_buffer &&src)
{
    _local_buf = std::move(src._local_buf);
    _buf = src._buf;
    end = src.end;
}

wtf_buffer &wtf_buffer::operator=(wtf_buffer &&src)
{
    if (this != &src)
    {
        _local_buf = std::move(src._local_buf);
        _buf = src._buf;
        end = src.end;
    }
    return *this;
}
