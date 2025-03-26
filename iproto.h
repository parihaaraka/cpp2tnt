#ifndef IPROTO_H
#define IPROTO_H

#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

/// Tarantool connector scope
namespace tnt
{

constexpr int SCRAMBLE_SIZE =  20;
constexpr int GREETING_SIZE = 128;
constexpr int VERSION_SIZE  =  64;
constexpr int SALT_SIZE     =  44;

/// Request types
enum class request_type : uint8_t
{
    OK        = 0x00,
    SELECT    = 0x01,
    INSERT    = 0x02,
    REPLACE   = 0x03,
    UPDATE    = 0x04,
    DELETE    = 0x05,
    CALL_16   = 0x06,
    AUTH      = 0x07,
    EVAL      = 0x08,
    UPSERT    = 0x09,
    CALL      = 0x0a,
    EXECUTE   = 0x0b, // sql
    NOP       = 0x0c,
    PREPARE   = 0x0d, // sql
    PING      = 0x40,
    PROTO_ID  = 0x49, // https://www.tarantool.io/en/doc/latest/reference/internals/iproto/requests/#iproto-id
};

enum class feature : uint8_t
{
    STREAMS = 0,
    TRANSACTIONS,
    ERROR_EXTENSION,
    WATCHERS,
    PAGINATION,
    invalid
};

/// Request body field types (keys)
enum body_field
{
    SPACE         = 0x10,
    INDEX         = 0x11,
    LIMIT         = 0x12,
    OFFSET        = 0x13,
    ITERATOR      = 0x14,
    INDEX_BASE    = 0x15,
    KEY           = 0x20,
    TUPLE         = 0x21,
    FUNCTION_NAME = 0x22,
    USER_NAME     = 0x23,
    SERVER_UUID   = 0x24,
    CLUSTER_UUID  = 0x25,
    VCLOCK        = 0x26,
    EXPRESSION    = 0x27,
    OPS           = 0x28,
    SQL_TEXT      = 0x40,
    SQL_BIND      = 0x41,
    VERSION       = 0x54,
    FEATURES      = 0x55,
    AUTH_TYPE     = 0x5b,
};

/// Response body field types (keys)
enum response_field
{
    DATA      = 0x30,
    ERROR     = 0x31, // IPROTO_ERROR_24
    METADATA  = 0x32,
    SQL_INFO  = 0x42,
    ERROR_OBJ = 0x52, // IPROTO_ERROR
};

/// Request/response header field types (keys)
enum header_field
{
    CODE      = 0x00,
    SYNC      = 0x01,
    SERVER_ID = 0x02,
    LSN       = 0x03,
    TIMESTAMP = 0x04,
    SCHEMA_ID = 0x05, // IPROTO_SCHEMA_VERSION
};

/// Update operations
enum update_operation
{
    ADD       = '+',
    SUBSTRACT = '-',
    AND       = '&',
    XOR       = '^',
    OR        = '|',
    DELETE    = '#',
    INSERT    = '!',
    ASSIGN    = '=',
    SPLICE    = ':',
};

enum subscription_field
{
    EVENT_KEY  = 0x57,
    EVENT_DATA = 0x58,
};

// https://www.tarantool.io/en/doc/latest/reference/internals/iproto/requests/#iproto-id
struct proto_id
{
    proto_id(std::initializer_list<feature> features = {}, uint64_t version = 0, std::string auth = {});
    bool has_feature(feature f) const;
    /// make features ready to send
    std::vector<uint8_t> list_features() const;
    uint64_t version = 0;
    std::string auth;
    std::bitset<8> features{};
};

} // namespace tnt

#endif // IPROTO_H
