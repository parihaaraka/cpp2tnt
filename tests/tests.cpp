#include <map>
#include <optional>
#include "connection.h"
#include "ev4cpp2tnt.h"
#include "iproto.h"
#include "mp_reader.h"
#include "iproto_writer.h"
#include "tests/sync.h"
#include "ut.hpp"

using namespace std;

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
        string err(body[tnt::response_field::ERROR].to_string());
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
    using namespace boost; // nothrow is ambiguous
    using namespace boost::ut;
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
        expect(interval[0].read<int>() == 1);
        expect(interval[1].read<int>() == 200);
        expect(interval[3].read<int>() == -77_i);

        /* error stack item keys:
        0 - MP_ERROR_TYPE     (MP_STR)
        1 - MP_ERROR_FILE     (MP_STR)
        2 - MP_ERROR_LINE     (MP_UINT)
        3 - MP_ERROR_MESSAGE  (MP_STR)
        4 - MP_ERROR_ERRNO    (MP_UINT)
        5 - MP_ERROR_ERRCODE  (MP_UINT)
        6 - MP_ERROR_FIELDS   (MP_MAPs)
        * https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-error-type */
        auto error_stack = ret_items.read<mp_array_reader>();
        auto first_error = error_stack.read<mp_map_reader>();
        expect(first_error[0].read<string_view>() == "ClientError");
        expect(first_error[1].read<string_view>() == R"([string "return require('msgpack').encode({..."])");
        expect(first_error[2].read<uint32_t>() == 7);
        expect(first_error[3].read<string_view>() == "test");
        expect(first_error[5].read<uint32_t>() == 20001);
        auto tmp = first_error[6].to_string();
        expect(tmp == R"({"name": "UNKNOWN", "fields": {"var1": "payload"}})");

        expect(ret_items.read<bool>() == true);
    };

    scope s = run_loop();
    bool connected = open_tnt_connection(argc > 1 ? argv[1] : "localhost:3301");

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
                    auto ret_data = body[tnt::response_field::DATA];
                    //ut::log << "response content:\n" << hex_dump(ret_data.begin(), ret_data.end()) << "\n";
                    auto ret_items = ret_data.read<mp_array_reader>();
                    expect(ret_items.cardinality() == 12_ul);
                    return true;
                });
                w.encode_ping_request();
                set_handler(cn.last_request_id(), [](const mp_map_reader &header, const mp_map_reader &body)
                {
                    expect(ut::nothrow([&](){ throw_if_error(header, body); }));
                    return true;
                });
                cn.flush();
            });
        };
    };

    return EXIT_SUCCESS;
}
