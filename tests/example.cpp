#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include "connection.h"
#include "ev4cpp2tnt.h"
#include "proto.h"
#include "mp_reader.h"
#include "mp_writer.h"

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

static void signal_cb(struct ev_loop *loop, ev_signal *w, int)
{
    cout << endl << "caught signal " << w->signum << endl;
    ev_break(loop);
}

template<typename T>
string vector2str(T &&vector)
{
    stringstream ss;
    ss << '{';
    for (size_t i = 0; i < vector.size(); ++i)
        ss << (i ? ",":"") << vector[i];
    ss << '}';
    return ss.str();
};

int main(int argc, char *argv[])
{
    struct ev_loop *loop = EV_DEFAULT;
    ev4cpp2tnt ev_wrapper(loop);

    tnt::connection cn;
    cn.set_connection_string(argc > 1 ? argv[1] : "localhost:3301");
    //cn.set_connection_string(argc > 1 ? argv[1] : "unix/:/home/dev/100/projects/kickex/accounting/data/accounting.control");

    ev_wrapper.take_care(&cn);

    ev_signal term_signal_watcher;
    ev_signal_init(&term_signal_watcher, signal_cb, SIGTERM);
    ev_signal_start(loop, &term_signal_watcher);
    ev_unref(loop);

    ev_signal int_signal_watcher;
    ev_signal_init (&int_signal_watcher, signal_cb, SIGINT);
    ev_signal_start(loop, &int_signal_watcher);
    ev_unref(loop);

    cn.on_error([&](string_view message, tnt::error code, uint32_t db_error)
    {
        cout << "error: " << message << endl
             << "  internal code: " << static_cast<int>(code) << endl
             << "  db error code: " << db_error << endl;
    });

    map<uint64_t, fu2::unique_function<void(const mp_map_reader&, const mp_map_reader&)>> handlers;

    cn.on_opened([&cn, &handlers, loop]()
    {
        auto throw_if_error = [](const mp_map_reader &header, const mp_map_reader &body)
        {
            int32_t code;
            header[tnt::header_field::CODE] >> code;
            if (code)
            {
                string err(body[tnt::response_field::ERROR].to_string());
                throw runtime_error(err);
            }
        };

        cout << "connected" << endl;
        iproto_client w(cn);
        w.eval("return 1,2,{3,4},{8,9,10},{a=5,b=6},7.123,require('decimal').new(100),{11,12,13}");
        handlers[cn.last_request_id()] = [&throw_if_error, loop](const mp_map_reader &header, const mp_map_reader &body)
        {
            throw_if_error(header, body);
            // response is an array of returned values with 32 bit length
            auto ret_data = body[tnt::response_field::DATA];
            cout << "response content:" << endl
                 << hex_dump(ret_data.begin(), ret_data.end()) << endl;
            auto ret_items = ret_data.read<mp_array_reader>();

            mp_array_reader tmp = ret_items.as_array(3); // fourth item (array {8,9,10})
            cout << endl;
            while (tmp.has_next())
                cout << tmp.to_string() << ',';
            cout << endl;

            tmp = ret_items.as_array(4); // map {a=5,b=6}
            while (tmp.has_next())
                cout << tmp.to_string() << ',';
            cout << endl;

            __int128_t a;
            long b;
            tuple<long, long, optional<long>> c;
            vector<int> d;
            map<string, int> e;
            double f;
            optional<long> g, h;
            long i;
            optional<string> dec;
            optional<vector<int>> tail;

            // preserve original state
            auto ret_items_bak = ret_items;
            // read 6 items and move current position
            ret_items >> a >> b >> c >> d >> e >> f;

            // shallow copy of reader acquires the position too
            auto ti2 = ret_items;
            auto s1 = ret_items.to_string();
            string s2;
            ti2 >> s2;
            cout << endl
                 << "------------" << endl
                 << s1 << endl
                 << s2 << endl
                 << "------------" << endl;

            // read the rest
            ret_items >> tail >> g >> h;
            // read if exists
            i = ret_items.read_or(-1);
            cout << endl << b << endl
                 << vector2str(d) << endl
                 << get<0>(c) << ',' << get<1>(c) << ',' << get<2>(c).value_or(0) << endl;
            for (auto &[k,v]: e)
                cout << k << ": " << v << endl;
            cout << f << endl
                 << dec.value_or("") << endl
                 << vector2str(tail.value_or(vector<int>{})) << endl
                 << g.value_or(0) << endl
                 << h.value_or(0) << endl;

            // skip 5 items and read the 6'th one
            ret_items_bak >> mp_reader::none() >> mp_reader::none<4>() >> f;
            cout << endl << "sixth value: " << f << endl;
            ev_break(loop);
        };
        w.encode_ping_request();
        handlers[cn.last_request_id()] = [&throw_if_error](const mp_map_reader &header, const mp_map_reader &body)
        {
            throw_if_error(header, body);
            cout << "pong received" << endl;
        };
        cn.flush();
    });

    cn.on_closed([]()
    {
        cout << "disconnected" << endl;
    });

    cn.on_response([&cn, &handlers, &loop](wtf_buffer &buf)
    {
        cout << buf.size() << " bytes acquired:" << endl;
        mp_reader bunch{buf};
        while (mp_reader r = bunch.iproto_message())
        {
            try
            {
                auto encoded_header = r.read<mp_map_reader>();
                uint64_t sync;
                encoded_header[tnt::header_field::SYNC] >> sync;
                auto handler = handlers.extract(sync);
                if (handler.empty())
                    cout << "orphaned response " << sync << " acquired" << endl;
                else
                {
                    auto encoded_body = r.read<mp_map_reader>();
                    handler.mapped()(encoded_header, encoded_body);
                }
            }
            catch(const mp_reader_error &e)
            {
                cout << e.what() << endl;
                ev_break(loop);
            }
            catch(const exception &e)
            {
                cout << e.what() << endl
                     << hex_dump(r.begin(), r.end()) << endl;
                ev_break(loop);
            }
        }
        cn.input_processed();
    });

    cn.open();
    ev_run (loop, 0);

    // the one may start/stop tarantool to check the connector's behaviour

    cout << "loop stopped" << endl;
    return 0;
}
