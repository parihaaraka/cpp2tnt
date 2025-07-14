#include <array>
#include <execinfo.h>
#include <memory>
#include <sys/stat.h>
#include <sstream>
#include "misc.h"

std::string hex_dump(const char *begin, const char *end, const char *pos)
{
    std::string res;
    res.reserve(static_cast<size_t>((end - begin)) * 4);
    constexpr char hexmap[] = {"0123456789abcdef"};
    int cnt = 0;
    for (const char* c = begin; c < end; ++c)
    {
        ++cnt;
        char sep = ' ';
        if (pos)
        {
            if (c == pos - 1)
                sep = '>';
            else if (c == pos)
                sep = '<';
        }
        res += hexmap[(*c & 0xF0) >> 4];
        res += hexmap[*c & 0x0F];
        res += sep;
        if (cnt % 16 == 0)
            res += '\n';
        else if (cnt % 8 == 0)
            res += ' ';
    }
    return res;
}

std::string get_trace()
{
    void *array[10];
    const size_t size = backtrace(array, 10);
    char **strings = backtrace_symbols(array, size);
    if (!strings)
        return "unable to acquire backtrace symbols";
    std::unique_ptr<char*, void(*)(char**)> sguard(strings, [](char **s) { free(s); });

    std::string error;
    std::stringstream s;
    // skip this handler
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
        if (error.empty() && !exe.empty() && !addr.empty())
        {
            char cmd[1024];
            sprintf(cmd, "addr2line -s -a -p -f -C -e %s %s 2>&1", exe.c_str(), addr.c_str());
            std::array<char, 128> buffer;
            std::string placement;
            int stat = 0;
            int wstat = 0;
            auto deleter = [&error, &stat, &wstat](FILE * f)
            {
                stat = pclose(f);
                wstat = WEXITSTATUS(stat);
            };
            std::unique_ptr<FILE, decltype(deleter)> pipe(popen(cmd, "r"), deleter);
            if (pipe)
            {
                while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr)
                    placement += buffer.data();

                pipe.release();
                if (stat < 0 || (wstat != 0 && wstat != 128 + 13/*SIGPIPE*/))
                    error = placement;
                else if (!placement.empty())
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

    if (!error.empty())
        s << "\n" << error;

    return s.str();
}
