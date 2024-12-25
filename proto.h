#ifndef TNT_PROTO_H
#define TNT_PROTO_H

/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/** @file */

#include <cstdint>

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
    SELECT    =  1,
    INSERT    =  2,
    REPLACE   =  3,
    UPDATE    =  4,
    DELETE    =  5,
    CALL_16   =  6,
    AUTH      =  7,
    EVAL      =  8,
    UPSERT    =  9,
    CALL      = 10,
    EXECUTE   = 11,
    PING      = 64,
    JOIN      = 65,
    SUBSCRIBE = 66
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
};

/// Response body field types (keys)
enum response_field
{
    DATA      = 0x30,
    ERROR     = 0x31,
    METADATA  = 0x32,
    SQL_INFO  = 0x42,
};

/// Request/response header field types (keys)
enum header_field
{
    CODE      = 0x00,
    SYNC      = 0x01,
    SERVER_ID = 0x02,
    LSN       = 0x03,
    TIMESTAMP = 0x04,
    SCHEMA_ID = 0x05
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

} // namespace tnt

#endif // TNT_PROTO_H

/* greeting example:
Tarantool 2.2.0 (Binary) 12cd26b5-61c6-4bc8-acc0-3271392fea75
GiRhweF832d0cEKTPcXvjMM6q8L544Rj5EQvt1x3sfA=
*/

/* ok auth response:
ce 00 00 00 18 83 00 ce  00 00 00 00 01 cf 00 00
00 00 00 00 00 00 05 ce  00 00 00 9b 80

ce = uint32, 0x18 = 24 bytes total (+5 this header)
83 = map of 3 items => 6 objects further
    00   = int(0)              (CODE)
      ce = uint32, 0x00
    01   = int(1)              (SYNC)
      cf = uint64, 0x0000000000000000
    05   = int(5)              (SCHEMA_ID)
      ce = uint32, 0x9b = 155
80 = map of 0 items
*/

/* err auth response:
ce 00 00 00 4d 83 00 ce  00 00 80 2f 01 cf 00 00
00 00 00 00 00 00 05 ce  00 00 00 9b 81 31 db 00
00 00 2f 49 6e 63 6f 72  72 65 63 74 20 70 61 73
73 77 6f 72 64 20 73 75  70 70 6c 69 65 64 20 66
6f 72 20 75 73 65 72 20  27 68 69 67 68 6b 69 63
6b 27

ce = uint32, 0x49 = 77 bytes total (+5 this header)
83 = map of 3 items => 6 objects further
    00   = int(0)              (CODE)
      ce = uint32, 0x802f = error code 0x02f = 47
    01   = int(1)              (SYNC)
      cf = uint64, 0x0000000000000000
    05   = int(5)              (SCHEMA_ID)
      ce = uint32, 0x9b = 155
81 = map of 1 items
    31   = int(49)             (ERROR)
      db = str32, 0x0000002f = 47 bytes long
           49 6e .... (Incorrect password supplied for user 'highkick')
*/

/* box.info.memory():
ce 00 00 00 51 83 00 ce  00 00 00 00 01 cf 00 00
00 00 00 00 00 01 05 ce  00 00 00 9b 81 30 dd 00
00 00 01 86 a5 63 61 63  68 65 00 a4 64 61 74 61
ce 00 02 0f 90 a2 74 78  00 a3 6c 75 61 ce 00 1f
14 d9 a3 6e 65 74 ce 00  0e 40 00 a5 69 6e 64 65
78 ce 00 28 00 00

81 = 1 item map
    30  = DATA
    dd  = array
        00 00 00 01 - 1 item
        86 = map of 6 items
            a5 63 61 63 68 65 = 'cache'
              00
            a4 64 61 74 61    = 'data'
              ce  00 02 0f 90
            a2 74 78          = 'tx'
              00
            a3 6c 75 61       = 'lua'
              ce  00 1f 14 d9
            a3 6e 65 74       = 'net'
              ce  00 0e 40 00
            a5 69 6e 64 65 78 = 'index'
              ce  00 28 00 00

cache: 0
data: 135056
tx: 0
lua: 2247129
net: 933888
index: 2621440
*/
