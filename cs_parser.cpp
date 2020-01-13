#include "cs_parser.h"
#if __has_include(<charconv>)
#include <charconv>
#endif

namespace tnt
{

using namespace std;

// TODO https://github.com/tarantool/tarantool-c/issues/120
//      > Support user:pass@unix/:/path/to/socket

cs_parts parse_cs(string_view connection_string) noexcept
{
    if (connection_string.empty())
        return {};
    cs_parts res;
    string_view tail = connection_string;

    auto get_chunk = [&tail](const char *sep = ":/@[") noexcept
    {
        auto tmp = tail;
        size_t pos = tail.find_first_of(sep);
        if (pos == string_view::npos)
        {
            tail.remove_prefix(tail.size());
            return tmp;
        }
        tail.remove_prefix(pos);
        return string_view{tmp.data(), pos};
    };

    auto chunk2port = [&res](const string_view &chunk) noexcept
    {
        if (chunk.size() > 5)
            return false;

        // ensure the chunk contains valid port number
#if __has_include(<charconv>)
        uint16_t tmp_port;
        if (auto [ptr, err] = from_chars(chunk.data(), chunk.data() + chunk.size(), &tmp_port);
                ptr == chunk.back() && tmp_port && tmp_port <= 0xffff)
            res.port = chunk;
#else
        // to guarantee terminating null character
        char tmp_chunk[6] = {0};
        copy(chunk.begin(), chunk.end(), begin(tmp_chunk));
        char *parse_end;
        auto tmp_port = strtoul(tmp_chunk, &parse_end, 10);
        if (!*parse_end && tmp_port && tmp_port <= 0xffff)
            res.port = chunk;
#endif

        return !res.port.empty();
    };

    string_view chunk;
    while (res.port.empty())
    {
        chunk = get_chunk();
        if (tail.empty()) // port only?
        {
            // not first chunk or incorrect port
            if (chunk.data() != connection_string.data() || !chunk2port(chunk))
                return {};
            break;
        }

        switch (tail.front())
        {
        case ':':
        {
            tail.remove_prefix(1);
            if (tail.front() == '/')
            {
                res.unix_socket_path = tail;
                return res;
            }

            string_view chunk2 = get_chunk();
            if (tail.empty())
            {
                // chunk2 is port?
                if (!chunk2port(chunk2))
                    return {};
                res.host = chunk;
                break;
            }

            if (tail.front() == '@')
            {
                // chunk2 is password
                res.user = chunk;
                res.password = chunk2;
                tail.remove_prefix(1);
                break;
            }
            return {};
        }
        case '/': // unix socket
            if (tail[1] == ':')
            {
                tail.remove_prefix(2);
                if (chunk == "env")
                {
                    string var{tail};
                    const char *cs = std::getenv(var.c_str());
                    if (!cs)
                        return {};
                    return parse_cs(cs);
                }
                if (chunk != "unix")
                    return {};
            }
            if (!res.user.empty())
                return {};
            res.unix_socket_path = tail;
            return res;
        case '[': // ipv6 address
            if (!chunk.empty())
                return {};
            tail.remove_prefix(1);
            chunk = get_chunk("]");
            if (chunk.empty() || tail.front() != ']')
                return {};
            tail.remove_prefix(1);
            if (tail.empty() || tail.front() != ':')
                return {};
            tail.remove_prefix(1);
            if (!chunk2port(tail))
                return {};
            res.host = chunk;
            break;
        default: // @
            res.user = chunk;
            tail.remove_prefix(1);
        }
    }

    if (res.user.empty())
        res.user = "guest";
    if (res.host.empty())
        res.host = "localhost";
    return res;
}

} // namespace tnt
