#include <map>
#include <optional>
#include "connection.h"
#include "ev4cpp2tnt.h"
#include "iproto.h"
#include "mp_reader.h"
#include "iproto_writer.h"
#include "tests/sync.h"
#include "ut.hpp"
#include "msgpuck/ext_tnt.h"

using namespace std;
using namespace boost; // nothrow is ambiguous
using namespace boost::ut;

extern std::string hex_dump(const char *begin, const char *end, const char *pos);

template <typename T>
typename std::enable_if_t<std::is_same_v<T, __int128_t>, mp_reader&>
operator>> (mp_reader& r, T &val)
{
    // dummy - just to test external operator overloading
    val = 0;
    r.skip();
    return r;
}

void throw_if_error(const mp_map_reader &header, const mp_map_reader &body)
{
    int32_t code;
    header[tnt::header_field::CODE] >> code;
    if (code)
    {
        string err(body[tnt::response_field::IPROTO_ERROR_24].to_string());
        boost::ut::log << ut::colors{}.fail << err << "\n" << ut::colors{}.none;
        throw runtime_error(err);
    }
};

std::vector<char> hex2bin(string_view hex)
{
    if (hex.size() & 0x1)
        throw std::invalid_argument("the number of characters in a hexadecimal string must be even");
    std::vector<char> res(hex.size() >> 1);
    for (size_t i = 0; i < hex.size(); ++i)
    {
        const char &c = hex[i];
        uint8_t half = c <= '9' ? c - '0' : ((c <= 'F' ? c - 'A' : c - 'a') + 10);
        if (half > 0xF)
            throw std::invalid_argument(std::format("unexpected hex digit: {}", c));
        res[i >> 1] |= i & 0x1 ? half : (half << 4);
    }
    return res;
}

int main(int argc, char *argv[])
{
    using namespace std::chrono;
    "wtf_buffer"_test = [] {
        std::vector<char> storage(1);
        wtf_buffer buf(storage.data(), storage.size());
        ++buf.end;
        expect(ut::nothrow([&buf] { buf.resize(1); })); // same size, no allocation
        expect(throws([&buf] { buf.resize(1024); }));   // realloc handler is not specified
        buf = wtf_buffer(
                    buf.data(),
                    buf.size(),
                    [storage = std::move(storage)](size_t size) mutable { storage.reserve(size); return storage.data(); }
        );
        expect(storage.data() == nullptr); // captured by buf
        expect(ut::nothrow([&buf] { buf.reserve(2048); }));
        expect(buf.end == buf.data());
        expect(buf.capacity() == 2048);
        auto data = buf.data();
        wtf_buffer buf2(std::move(buf));
        expect(buf2.end == buf2.data() && buf2.end == data);
        expect(buf2.capacity() == 2048);
    };

    "mp_reader"_test = [] {
        // tnt 3.3.1 response for request like one below (return 1, 2, ..., <error>)
        auto msgpack_tnt_331 = hex2bin("9c01029203049308090a82a16105a16206cb401c7df3b645a1cbc712011e123456789012345678901234567890123cd80264d22e4dac924a23899ae59f34af5479d80460c91f610000000015cd5b07b4000000c70b0604000101ccc803d0b30801"
                                       "c776038100918700ab436c69656e744572726f72020701d9305b737472696e67202272657475726e207265717569726528276d73677061636b27292e656e636f6465287b2e2e2e225d03a474657374040005cd4e210682a46e616d65a7554e4b4e4f574ea66669656c647381a476617231a77061796c6f6164"
                                       "c3");
        mp_reader r(msgpack_tnt_331);
        auto printed = r.to_string();
        expect(printed == R"([1, 2, [3, 4], [8, 9, 10], {"a": 5, "b": 6}, 7.1230000000000002, 123.456789012345678901234567890123, "64d22e4d-ac92-4a23-899a-e59f34af5479", 1629473120.123456789, {"year": 1, "month": 200, "day": -77}, {"stack": [{"type": "ClientError", "line": 7, "file": "[string \"return require('msgpack').encode({...\"]", "message": "test", "code": 20001, "fields": {"name": "UNKNOWN", "fields": {"var1": "payload"}}}]}, true])");
        //                    a  b  c       d           e                 f, ff               <decimal>                           <uuid>                                  <datetime>            <interval>                             <error>
        r.rewind();
        auto ret_items = r.read<mp_array_reader>();
        auto ret_items_bak = ret_items; // backup reader (shallow copy of reader acquires the position too)

        __int128_t a;
        long b;
        tuple<long, long, optional<long>> c;
        vector<int> d;
        map<string, int> e;
        double f, ff;

        // read 6 items and move current position
        ret_items >> a >> b >> c >> d >> e >> f;
        // skip 5 items and read the 6'th one
        ret_items_bak >> mp_reader::none() >> mp_reader::none<4>() >> ff;

        expect(b == 2);
        expect(c == std::tuple<long, long, optional<long>>{3,4,{}});
        expect(d == std::vector{8,9,10});
        expect(e == std::map<string,int>{{"a", 5},{ "b", 6}});
        expect(f == ff);

        ret_items_bak = ret_items;
        auto decimal2double = ret_items.read<double>();
        expect(decimal2double == 123.45678901234568_d) << "epsilon=0.0000000000001";
        auto decimal2string = ret_items_bak.read<std::string>();
        expect(decimal2string == "123.456789012345678901234567890123");
        auto uuid = ret_items.read<std::string>();
        expect(uuid == "64d22e4d-ac92-4a23-899a-e59f34af5479");
        auto timepoint = ret_items.read<time_point<system_clock, duration<double>>>();
        expect(timepoint.time_since_epoch().count() == 1629473120.123456789);

        /* interval keys:
        0 – year
        1 – month
        2 – week
        3 – day
        4 – hour
        5 – minute
        6 – second
        7 – nanosecond
        8 – adjust
        * https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-interval-type */
        auto interval = ret_items.read<mp_map_reader>();
        expect(interval[FIELD_YEAR].read<int>() == 1);
        expect(interval[FIELD_MONTH].read<int>() == 200);
        expect(interval[FIELD_DAY].read<int>() == -77_i);

        // https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-error-type
        auto error_stack = ret_items.read<mp_array_reader>();
        auto first_error = error_stack.read<mp_map_reader>();
        expect(first_error[tnt::MP_ERROR_TYPE].read<string_view>() == "ClientError");
        expect(first_error[tnt::MP_ERROR_FILE].read<string_view>() == R"([string "return require('msgpack').encode({..."])");
        expect(first_error[tnt::MP_ERROR_LINE].read<uint32_t>() == 7);
        expect(first_error[tnt::MP_ERROR_MESSAGE].read<string_view>() == "test");
        expect(first_error[tnt::MP_ERROR_ERRCODE].read<uint32_t>() == 20001);
        auto tmp = first_error[6].to_string();
        expect(tmp == R"({"name": "UNKNOWN", "fields": {"var1": "payload"}})");

        expect(ret_items.read<bool>() == true);
    };

    scope s = run_loop();
    bool connected = open_tnt_connection(argc > 1 ? argv[1] : "localhost:3301");
    if (!connected)
      ut::log << ut::colors{}.fail << "* pass tnt connection string as a first argument (default: \"localhost:3301\")\n" << ut::colors{}.none;

/*  tnt 2.11.5:
> box.error.new({code=20001, reason='test', fields={var1='payload'}}):unpack()
---
- code: 20001
  base_type: ClientError
  type: ClientError
  message: test
  trace:
  - file: '[string "return box.error.new({code=20001, reason=''tes..."]'
    line: 1
...

    tnt 3.3.1:
> box.error.new({code=20001, reason='test', fields={var1='payload'}}):unpack()
---
- fields: {'var1': 'payload'}
  name: UNKNOWN
  code: 20001
  base_type: ClientError
  type: ClientError
  message: test
  trace:
  - file: '[string "return box.error.new({code=20001, reason=''tes..."]'
    line: 1
...
*/

    cfg<override> = {.tag = {connected ? "connected" : "skip"}};
    tag("connected") / "integrational"_test = []() {
        should("eval") = []{
            sync_tnt_request([](tnt::connection &cn)
            {
                tnt::iproto_writer w([&cn](){ return cn.next_request_id();}, cn.output_buffer());
                // test error object content on explicit return
                w.eval(R"(
return
  1, 2, {3,4}, {8,9,10}, {a=5, b=6}, 7.123,
  require('decimal').new('123.456789012345678901234567890123'),
  require('uuid').fromstr('64d22e4d-ac92-4a23-899a-e59f34af5479'),
  require('datetime').new({nsec = 123456789, sec = 20, min = 25, hour = 18, day = 20, month = 8, year = 2021, tzoffset = 180}),
  require('datetime').interval.new({year=1, month=200, day=-77}),
  box.error.new({code=20001, reason='test', fields={var1='payload'}}),
  true
)");
                set_handler(cn.last_request_id(), [](const mp_map_reader &header, const mp_map_reader &body) {
                    expect(ut::nothrow([&](){ throw_if_error(header, body); }));
                    // response is an array of returned values with 32 bit length
                    auto ret_data = body[tnt::IPROTO_DATA];
                    //ut::log << "response content:\n" << hex_dump(ret_data.begin(), ret_data.end()) << "\n";
                    auto ret_items = ret_data.read<mp_array_reader>();
                    expect(ret_items.cardinality() == 12_ul);
                    mp_array_reader err_stack;
                    ret_items >> mp_reader::none<10>() >> err_stack;
                    //
                    // * The case with the explicitly serialized error object. *
                    //
                    // If ERROR_EXTENSION is disabled, then the error is just a string:
                    //  a4 74 65 73 74
                    // If ERROR_EXTENSION is enabled, then the error is a ext msgpack object with the new style error content (map with 0x00 key (stack) and array as a value):
                    //  c7 25 03 81 00 91 86 00 ab 43 6c 69 65 6e 74 45 72 72 6f 72 02 08 01 a4 65 76 61 6c 03 a4 74 65 73 74 04 00 05 cd 4e 21
                    // Tarantool 3.1.0+ with MP_ERROR_FIELDS:
                    //  c7 49 03 81 00 91 87 00 ab 43 6c 69 65 6e 74 45 72 72 6f 72 02 08 01 a4 65 76 61 6c 03 a4 74 65 73 74 04 00 05 cd 4e 21 06 82 a4 6e 61 6d 65 a7 55 4e 4b 4e 4f 57 4e a6 66 69 65 6c 64 73 81 a4 76 61 72 31 a7 70 61 79 6c 6f 61 64
                    // Here we assume the extension is enabled.
                    auto last_err = err_stack.read<mp_map_reader>();
                    expect(last_err[tnt::MP_ERROR_MESSAGE].read<std::string_view>() == "test");
                    expect(last_err[tnt::MP_ERROR_ERRCODE].read<int>() == 20001);
                    // MP_ERROR_FIELDS contains user fields ("fields" subkey) and tnt additional fields related to the error
                    if (auto fields = last_err.find(tnt::MP_ERROR_FIELDS))
                         expect(fields.to_string() == R"({"name": "UNKNOWN", "fields": {"var1": "payload"}})");
                    return true;
                });

                // just one more request to test synchro wrapper
                w.encode_ping_request();
                set_handler(cn.last_request_id(), [](const mp_map_reader &header, const mp_map_reader &body)
                {
                    expect(ut::nothrow([&](){ throw_if_error(header, body); }));
                    return true;
                });

                // test tarantool error content within regular response
                w.eval("box.error({code=20001, reason='error message', fields={var1='payload'}})");
                set_handler(cn.last_request_id(), [](const mp_map_reader &header, const mp_map_reader &body) {
                    expect(ut::throws([&](){ throw_if_error(header, body); }));
                    // body is a map with key 0x31 (PROTO_ERROR_24 - string) and optional(?) key 0x52 (IPROTO_ERROR - map)
                    // If ERROR_EXTENSION is enabled:
                    // 82
                    //   31 a4 74 65 73 74
                    //   52 81 00 91 86 00 ab 43 6c 69 65 6e 74 45 72 72 6f 72 02 01 01 a4 65 76 61 6c 03 ad 65 72 72 6f 72 20 6d 65 73 73 61 67 65 04 00 05 cd 4e 21
                    //     * Tarantool 3.1.0+ with MP_ERROR_FIELDS:
                    //   52 81 00 91 87 00 ab 43 6c 69 65 6e 74 45 72 72 6f 72 02 01 01 a4 65 76 61 6c 03 ad 65 72 72 6f 72 20 6d 65 73 73 61 67 65 04 00 05 cd 4e 21 06 82 a4 6e 61 6d 65 a7 55 4e 4b 4e 4f 57 4e a6 66 69 65 6c 64 73 81 a4 76 61 72 31 a7 70 61 79 6c 6f 61 64
                    //     * {0: "ClientError", 1: "eval", 2: 1, 3: "error message", 4: 0, 5: 20001, 6: {"name": "UNKNOWN", "fields": {"var1": "payload"}}}

                    int err_code_from_header = header[tnt::header_field::CODE].read<int>() & 0x7fff;
                    expect(err_code_from_header == 20001_i);
                    auto old_style_error = body[tnt::IPROTO_ERROR_24].read<std::string_view>();
                    expect(old_style_error == "error message");
                    auto stack = body[tnt::IPROTO_ERROR].read<mp_map_reader>()[0x00].read<mp_array_reader>();
                    auto last_err = stack.read<mp_map_reader>();
                    expect(last_err[tnt::MP_ERROR_MESSAGE].read<std::string_view>() == old_style_error);
                    expect(last_err[tnt::MP_ERROR_ERRCODE].read<int>() == err_code_from_header);
                    if (auto fields = last_err.find(tnt::MP_ERROR_FIELDS))
                         expect(fields.to_string() == R"({"name": "UNKNOWN", "fields": {"var1": "payload"}})");
                    return true;
                });

                // test plain lua error content within regular response (works like tnt one)
                w.eval("error('lua error')");
                set_handler(cn.last_request_id(), [](const mp_map_reader &header, const mp_map_reader &body) {
                    expect(ut::throws([&](){ throw_if_error(header, body); }));
                    // 82
                    //   31 b1 65 76 61 6c 3a 31 3a 20 6c 75 61 20 65 72 72 6f 72   // "eval:1: lua error"
                    //   52 81 00 91 86 00 ab 4c 75 61 6a 69 74 45 72 72 6f 72 02 cd 02 69 01 b1 2e 2f 73 72 63 2f 6c 75 61 2f 75 74 69 6c 73 2e 63 03 b1 65 76 61 6c 3a 31 3a 20 6c 75 61 20 65 72 72 6f 72 04 00 05 20
                    //
                    // Last error in the stack (and the only one): {0: "LuajitError", 1: "./src/lua/utils.c", 2: 617, 3: "eval:1: lua error", 4: 0, 5: 32}
                    int err_code_from_header = header[tnt::header_field::CODE].read<int>() & 0x7fff;
                    expect(err_code_from_header == 32_i);
                    auto old_style_error = body[tnt::IPROTO_ERROR_24].read<std::string_view>();
                    auto stack = body[tnt::IPROTO_ERROR].read<mp_map_reader>()[0x00].read<mp_array_reader>();
                    auto last_err = stack.read<mp_map_reader>();
                    expect(last_err[tnt::MP_ERROR_MESSAGE].read<std::string_view>() == old_style_error);
                    expect(last_err[tnt::MP_ERROR_ERRCODE].read<int>() == err_code_from_header);
                    return false;
                });

                cn.flush();
            });
        };
    };

    return EXIT_SUCCESS;
}
