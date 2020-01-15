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
        iproto_writer w(cn);
        w.eval("return 1,2,{3,4},{8,9,10},{a=5,b=6},7,require('decimal').new(100),{11,12,13}");
        handlers[cn.last_request_id()] = [&throw_if_error, loop](const mp_map_reader &header, const mp_map_reader &body)
        {
            throw_if_error(header, body);
            // response is an array of returned values with 32 bit length
            auto ret_data = body[tnt::response_field::DATA];
            cout << "response content:" << endl
                 << hex_dump(ret_data.begin(), ret_data.end()) << endl;
            auto ret_items = ret_data.read<mp_array_reader>();
            long a, b, e;
            map<string, int> d;
            tuple<long, long, optional<long>> c;
            vector<int> vec;
            //array<int, 3> vec;
            optional<long> f, g;
            optional<string> dec;
            optional<vector<int>> tail;

            //ret_items.values(a, b, c, d, e, f, g);
            ret_items >> a >> b >> c >> vec >> d >> e >> dec >> tail >> f >> g;
            cout << a << endl << b << endl
                 << vector2str(vec) << endl
                 << get<0>(c) << ',' << get<1>(c) << ',' << get<2>(c).value_or(0) << endl;
            for (auto &[k,v]: d)
                cout << k << ": " << v << endl;
            cout << e << endl
                 << dec.value_or("") << endl
                 << vector2str(tail.value_or(vector<int>{})) << endl
                 << f.value_or(0) << endl
                 << g.value_or(0) << endl;
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
