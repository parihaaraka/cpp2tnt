#include "wtf_buffer.h"
#include <stdexcept>

wtf_buffer::wtf_buffer(size_t size) : wtf_buffer(std::vector<char>(size), 0)
{
}

wtf_buffer::wtf_buffer(std::vector<char> &buf, size_t offset)
    : target(&buf)
{
    auto &v = *std::get<std::vector<char>*>(target);
    head = v.data();
    end_of_storage = v.data() + v.capacity();
    end = head + offset;
}

wtf_buffer::wtf_buffer(std::vector<char> &&buf, size_t offset)
    : target(buf)
{
    auto &v = std::get<std::vector<char>>(target);
    head = v.data();
    end_of_storage = v.data() + v.capacity();
    end = head + offset;
}

wtf_buffer::wtf_buffer(char *data, size_t length, fu2::unique_function<char*(size_t)> realloc)
    : target(std::move(realloc))
{
    if (!data)
        throw std::invalid_argument("nullptr data is not allowed");
    head = data;
    end_of_storage = data + length;
    end = head;
}

size_t wtf_buffer::capacity() const noexcept
{
    return end_of_storage - head;
}

size_t wtf_buffer::size() const noexcept
{
    return end - head;
}

size_t wtf_buffer::available() const noexcept
{
    return end_of_storage - end;
}

char *wtf_buffer::data() noexcept
{
    return head;
}

const char *wtf_buffer::data() const noexcept
{
    return head;
}

void wtf_buffer::reserve(size_t size)
{
    if (size <= capacity())
        return;
    size_t content_size = this->size();

    size_t ind = target.index();
    if (!ind)
    {
        auto &realloc = std::get<0>(target);
        if (!realloc)
            throw std::runtime_error("unable to resize raw buffer");
        head = realloc(size);
    }
    else
    {
        std::vector<char>* buf;
        if (ind == 1)
            buf = std::get<1>(target);
        else
            buf = &std::get<2>(target);

        buf->resize(size);
        head = buf->data();
    }
    end_of_storage = head + size;
    end = head + content_size;
}

void wtf_buffer::resize(size_t size)
{
    if (size > capacity())
        reserve(size);
    end = head + size;
}

void wtf_buffer::clear() noexcept
{
    end = head;
}
