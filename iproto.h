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
    OK         = 0x00,
    SELECT     = 0x01,
    INSERT     = 0x02,
    REPLACE    = 0x03,
    UPDATE     = 0x04,
    DELETE     = 0x05,
    CALL_16    = 0x06,
    AUTH       = 0x07,
    EVAL       = 0x08,
    UPSERT     = 0x09,
    CALL       = 0x0a,
    EXECUTE    = 0x0b, // sql
    NOP        = 0x0c,
    PREPARE    = 0x0d, // sql
    PING       = 0x40,
    PROTO_ID   = 0x49, // https://www.tarantool.io/en/doc/latest/reference/internals/iproto/requests/#iproto-id
    WATCH      = 0x4a, // https://www.tarantool.io/ru/doc/latest/reference/internals/iproto/events/#iproto-watch
    UNWATCH    = 0x4b,
    EVENT      = 0x4c,
    WATCH_ONCE = 0x4d, // Synchronous request to fetch the data that is currently attached to a notification key without subscribing to changes.
};

/// iproto features and protocol versions
/// https://github.com/tarantool/tarantool/blob/e2a6e2cd13b73cd3ace7b055489de47bc8ecd1a2/src/box/iproto_features.h
enum class feature : uint8_t
{
    STREAMS = 0,
    TRANSACTIONS,
    ERROR_EXTENSION, /// proto 2+
    WATCHERS,        /// proto 3+
    PAGINATION,
    invalid
};

/// error stack item keys:
/// https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-error-type
enum error_field
{
    MP_ERROR_TYPE     = 0x00,  // MP_STR
    MP_ERROR_FILE     = 0x01,  // MP_STR
    MP_ERROR_LINE     = 0x02,  // MP_UINT
    MP_ERROR_MESSAGE  = 0x03,  // MP_STR
    MP_ERROR_ERRNO    = 0x04,  // MP_UINT
    MP_ERROR_ERRCODE  = 0x05,  // MP_UINT
    MP_ERROR_FIELDS   = 0x06,  // MP_MAPs
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
    IPROTO_DATA     = 0x30, // used in all requests and responses
    IPROTO_ERROR_24 = 0x31, // old style error (string)
    IPROTO_METADATA = 0x32, // SQL transaction metadata
    IPROTO_SQL_INFO = 0x42, // additional SQL-related parameters
    IPROTO_ERROR    = 0x52, // new style error (map with error stack)
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
    std::bitset<32> features{}; // в tnt 3.3.1 уже есть 12 фич, и многих нет в документации
};

} // namespace tnt

#endif // IPROTO_H
