#include <iostream>
#include <map>
#include <optional>
#include "connection.h"
// ev++.h does not compile in C++17 mode
// It has a lack of noticeable advantages over plain C api so lets use the last one.
#include <ev.h>
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

static void timer_cb(struct ev_loop *, ev_timer *w, int)
{
    tnt::connection *cn = static_cast<tnt::connection*>(w->data);
    cn->tick_1sec();
}

static void socket_event_cb(struct ev_loop *, ev_io *w, int revents)
{
    // man:
    // Libev will usually signal a few "dummy" events together with an error,
    // for example it might indicate that a fd is readable or writable, and
    // if your callbacks is well-written it can just attempt the operation
    // and cope with the error from read() or write().
    if (revents & EV_ERROR)
        cout << "EV_ERROR soket state received" << endl;

    tnt::connection *cn = static_cast<tnt::connection*>(w->data);
    if (revents & EV_WRITE)
        cn->write();
    if (revents & EV_READ)
        cn->read();
}

static void async_notifier_cb(struct ev_loop *, ev_async *w, int)
{
    tnt::connection *cn = static_cast<tnt::connection*>(w->data);
    cn->acquire_notifications();
}

int main(int argc, char *argv[])
{
    tnt::connection cn;
    cn.set_connection_string(argc > 1 ? argv[1] : "localhost:3301");
    // "unix/:/var/run/tarantool/tarantool.sock"

    struct ev_loop *loop = EV_DEFAULT;

    ev_signal term_signal_watcher;
    ev_signal_init(&term_signal_watcher, signal_cb, SIGTERM);
    ev_signal_start(loop, &term_signal_watcher);
    ev_unref(loop);

    ev_signal int_signal_watcher;
    ev_signal_init (&int_signal_watcher, signal_cb, SIGINT);
    ev_signal_start(loop, &int_signal_watcher);
    ev_unref(loop);

    ev_timer timer;
    ev_timer_init(&timer, timer_cb, 1, 1);
    timer.data = &cn;
    ev_timer_start(loop, &timer);
    ev_unref(loop);

    ev_async async_notifier;
    ev_async_init(&async_notifier, async_notifier_cb);
    async_notifier.data = &cn;
    ev_async_start(loop, &async_notifier);

    ev_io socket_watcher{};
    ev_init(&socket_watcher, socket_event_cb);
    socket_watcher.data = &cn;

    cn.on_error([&](string_view message, tnt::error code, uint32_t db_error)
    {
        cout << "error: " << message << endl
             << "  internal code: " << static_cast<int>(code) << endl
             << "  db error code: " << db_error << endl;
    });

    cn.on_socket_watcher_request([loop, &socket_watcher](int mode)
    {
        int events = (mode & tnt::socket_state::read  ? EV_READ  : EV_NONE);
        events    |= (mode & tnt::socket_state::write ? EV_WRITE : EV_NONE);

        // ev_io_set() sets EV__IOFDSET flag internally,
        // so direct comparison does not work.
        if ((socket_watcher.events & (EV_READ | EV_WRITE)) != events)
        {
            if (ev_is_active(&socket_watcher))
                ev_io_stop(loop, &socket_watcher);
            tnt::connection *cn = static_cast<tnt::connection*>(socket_watcher.data);
            ev_io_set(&socket_watcher, cn->socket_handle(), events);
            if (events)
                ev_io_start(loop, &socket_watcher);

            if (mode & tnt::socket_state::read)
                cout << "R";
            if (mode & tnt::socket_state::write)
                cout << "W";
            if (!mode)
                cout << "N";
            cout << endl;
        }
    });

    map<uint64_t, fu2::unique_function<void(const mp_map_reader&, const mp_map_reader&)>> handlers;

    cn.on_opened([&cn, &handlers]()
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
        w.begin_call("box.info.memory");
        w.begin_array(0); // no need to finalize zero-length array
        w.finalize();     // finalize call
        handlers[cn.last_request_id()] = [&throw_if_error](const mp_map_reader &header, const mp_map_reader &body)
        {
            throw_if_error(header, body);
            // response is an array of returned values with 32 bit length
            auto ret_data = body[tnt::response_field::DATA];
            cout << "box.info.memory() response content:" << endl
                 << hex_dump(ret_data.begin(), ret_data.end()) << endl;
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

    cn.on_notify_request(bind(ev_async_send, loop, &async_notifier));

    cn.on_response([&cn, &handlers](wtf_buffer &buf)
    {
        cout << buf.size() << " bytes acquired:" << endl;
        mp_reader bunch{buf};
        while (mp_reader r = bunch.iproto_message())
        {
            try
            {
                auto encoded_header = r.map();
                uint64_t sync;
                encoded_header[tnt::header_field::SYNC] >> sync;
                auto handler = handlers.extract(sync);
                if (handler.empty())
                    cout << "orphaned response " << sync << " acquired" << endl;
                else
                {
                    auto encoded_body = r.map();
                    handler.mapped()(encoded_header, encoded_body);
                }
            }
            catch(const mp_reader_error &e)
            {
                cout << e.what() << endl;
            }
            catch(const exception &e)
            {
                cout << e.what() << endl
                     << hex_dump(r.begin(), r.end()) << endl;
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
