#ifndef CS_PARSER_H
#define CS_PARSER_H

/** @file */

#include <string>

/// Tarantool connector scope
namespace tnt
{

/// Result of connection string parsing
struct cs_parts
{
    std::string unix_socket_path, user, password, host, port;
};

/** connection string parser according to
 *  https://www.tarantool.io/ru/doc/2.1/reference/configuration/#uri */
cs_parts parse_cs(std::string_view connection_string) noexcept;

} // namespace tnt

#endif // CS_PARSER_H
