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

// TMP

#include <execinfo.h>
#include <sys/stat.h>
#include <sstream>
std::string get_trace(void)
{
    void *array[10];
    const size_t size = backtrace(array, 10);
    char **strings = backtrace_symbols(array, size);
    if (!strings)
        return "unable to acquire backtrace symbols";
    std::unique_ptr<char*, void(*)(char**)> sguard(strings, [](char **s) { free(s); });

    std::stringstream s;
    // skip first, as it is this handler
    for (size_t i = 1; i < size; i++)
    {
        // extract the exe name
        std::string exe(strings[i]);
        {
            const size_t s = exe.find("(");
            if(std::string::npos != s)
                exe.erase(s, exe.length());

            struct stat tmp;
            if (stat(exe.c_str(), &tmp) != 0)
                exe.clear();
        }

        // extract the address
        std::string addr(strings[i]);
        {
            size_t s = addr.find("(");
            if (std::string::npos != s)
            {
                ++s;
                addr.erase(0, s);

                s = addr.find(")");
                if(std::string::npos != s)
                    addr.erase(s, addr.length());
                else
                    addr.clear();
            }
            else
            {
                addr.clear();
            }
        }

        s << '[' << i << "]: " << strings[i];
        if (!exe.empty() && !addr.empty())
        {
            char cmd[1024];
            sprintf(cmd, "addr2line -s -a -p -f -C -e %s %s", exe.c_str(), addr.c_str());
            std::array<char, 128> buffer;
            std::string placement;
            std::unique_ptr<FILE, void(*)(FILE*)> pipe(popen(cmd, "r"),
                [](FILE * f) -> void
                {
                    // wrapper to ignore the return value from pclose() is needed with newer versions of gnu g++
                    std::ignore = pclose(f);
                });
            if (pipe)
            {
                while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr)
                    placement += buffer.data();

                if (!placement.empty())
                    s << " -> " << placement;
                else
                    s << "\n";
            }
        }
        else
        {
            s << "\n";
        }
    }

    return s.str();
}
