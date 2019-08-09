#include <iostream>
#include "connection.h"
// ev++.h does not compile in C++17 mode
// It has a lack of noticeable advantages over plain C api so lets use the last one.
#include <ev.h>

using namespace std;

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

static void echo_buf(const char *begin, const char *end)
{
    constexpr char hexmap[] = {"0123456789abcdef"};
    int cnt = 0;
    for (const char* c = begin; c < end; ++c)
    {
        ++cnt;
        cout << hexmap[(*c & 0xF0) >> 4] << hexmap[*c & 0x0F] << ' ';
        if (cnt % 16 == 0)
            cout << endl;
        else if (cnt % 8 == 0)
            cout << ' ';
    }
    cout << endl;
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

    cn.on_opened([&cn](){
        cout << "connected" << endl;
        auto buf = cn.output_buffer();
        // TODO
    });

    cn.on_closed([](){
        cout << "disconnected" << endl;
    });

    cn.on_notify_request(bind(ev_async_send, loop, &async_notifier));

    cn.on_response([](wtf_buffer &buf){
        // TMP
        cout << buf.size() << " bytes acquired:" << endl;
        echo_buf(buf.data(), buf.end);
    });

    cn.open();
    ev_run (loop, 0);

    // the one may start/stop tarantool to check the connector's behaviour

    cout << "loop stopped" << endl;
    return 0;
}

