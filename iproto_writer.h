#ifndef IPROTO_WRITER_H
#define IPROTO_WRITER_H

#include "mp_writer.h"
#include "iproto.h"

/// Tarantool connector scope
namespace tnt
{

/** Helper to compose iproto messages.
*
* A caller must ensure there is enough free space in the underlying buffer.
*/
class iproto_writer : public mp_writer
{
public:
    /// Wrap writer object around specified buffer.
    iproto_writer(std::function<uint64_t()> get_request_id, wtf_buffer &buf);

    // base functions
    /// Initiate request or response. A caller must compose header and body afterwards and call finalize()
    /// to finalize the message.
    void start_message();
    /// Finalize topmost container or finalize a message (by fixing its final size).
    void finalize();
    /// Finalize all non-finalized containers, call, eval, etc (if exists).
    void finalize_all();

    // higher level functions
    /// Initiate request of specified type. A caller must compose a body afterwards and call finalize()
    /// to finalize request.
    void encode_request_header(tnt::request_type req_type);
    void encode_response_header(uint32_t error_code = 0, uint64_t schema_version = 1);

    /// Put authentication request into the underlying buffer.
    void encode_auth_request(const char* greeting, std::string_view user, std::string_view password, std::string_view auth_proto = "chap-sha1");
    /// Put proto_id request into the underlying buffer.
    void encode_id_request(tnt::proto_id proto);
    /// Put ping request into the underlying buffer.
    void encode_ping_request();

    /// Initiate call request. A caller must pass an array of arguments afterwards
    /// and call finalize() to finalize request.
    void begin_call(std::string_view fn_name);

    void begin_eval(std::string_view script);

    /// Call request all-in-one wrapper.
    template <typename ...Ts>
    void call(std::string_view fn_name, Ts const&... args)
    {
        begin_call(fn_name);
        begin_array(sizeof...(args));
        ((*this << args), ...);
        finalize_all();
    }

    /// Call request all-in-one wrapper.
    template <typename ...Ts>
    void eval(std::string_view script, Ts const&... args)
    {
        begin_eval(script);
        begin_array(sizeof...(args));
        ((*this << args), ...);
        finalize_all();
    }

    using mp_writer::operator<<;
private:
    std::function<uint64_t()> get_request_id;
};

} // namespace tnt

#endif // IPROTO_WRITER_H


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
