#include "ev4cpp2tnt.h"
#include "connection.h"

using namespace std;

static void timer_cb(struct ev_loop *, ev_timer *w, int)
{
    auto wrapper = static_cast<unordered_map<tnt::connection*, unique_ptr<pair<ev_async, ev_io>>>*>(w->data);
    for (auto &bundle_item: *wrapper)
        bundle_item.first->tick_1sec();
}

static void socket_event_cb(struct ev_loop *, ev_io *w, int revents)
{
    // man:
    // Libev will usually signal a few "dummy" events together with an error,
    // for example it might indicate that a fd is readable or writable, and
    // if your callbacks is well-written it can just attempt the operation
    // and cope with the error from read() or write().

    tnt::connection *cn = static_cast<tnt::connection*>(w->data);
    if (revents & EV_WRITE)
        cn->write();
    if (revents & EV_READ)
        cn->read();
}

static void connection_notifier_cb(struct ev_loop *, ev_async *w, int)
{
    tnt::connection *cn = static_cast<tnt::connection*>(w->data);
    cn->acquire_notifications();
}

void register_connection(ev4cpp2tnt::ev_data *data, tnt::connection *cn)
{
    auto res = data->per_connection_watchers.try_emplace(cn, make_unique<pair<ev_async, ev_io>>());
    if (!res.second)
        return;

    auto &evb = *res.first->second;
    ev_async *asyn_watcher = &evb.first;
    ev_io *socket_watcher = &evb.second;

    ev_async_init(asyn_watcher, connection_notifier_cb);
    asyn_watcher->data = cn;
    ev_async_start(data->loop, asyn_watcher);

    ev_init(socket_watcher, socket_event_cb);
    socket_watcher->data = cn;

    cn->on_notify_request(bind(ev_async_send, data->loop, asyn_watcher));
    cn->on_socket_watcher_request([data, socket_watcher](int mode) noexcept
    {
        int events = (mode & tnt::socket_state::read  ? EV_READ  : EV_NONE);
        events    |= (mode & tnt::socket_state::write ? EV_WRITE : EV_NONE);

        bool was_active = ev_is_active(socket_watcher);
        // ev_io_set() sets EV__IOFDSET flag internally,
        // so direct comparison does not work.
        if (!was_active || (socket_watcher->events & (EV_READ | EV_WRITE)) != events)
        {
            if (was_active)
                ev_io_stop(data->loop, socket_watcher);
            tnt::connection *cn = static_cast<tnt::connection*>(socket_watcher->data);
            ev_io_set(socket_watcher, cn->socket_handle(), events);
            if (events)
                ev_io_start(data->loop, socket_watcher);
        }
    });
}

void unregister_connection(ev4cpp2tnt::ev_data *data, tnt::connection *cn)
{
    auto node = data->per_connection_watchers.extract(cn);
    if (!node.empty())
        return;
    cn->on_socket_watcher_request({});
    auto &evb = *node.mapped();
    ev_async *asyn_watcher = &evb.first;
    ev_io *socket_watcher = &evb.second;

    if (ev_is_active(asyn_watcher))
        ev_async_stop(data->loop, asyn_watcher);
    if (ev_is_active(socket_watcher))
        ev_io_stop(data->loop, socket_watcher);
}

ev4cpp2tnt::ev4cpp2tnt(struct ev_loop *loop)
{
    _ev_data = make_unique<ev_data>();
    _ev_data->loop = loop;

    ev_timer *timer = &_ev_data->timer;
    ev_timer_init(timer, timer_cb, 1, 1);
    timer->data = &_ev_data->per_connection_watchers;
    ev_timer_start(_ev_data->loop, timer);
    ev_unref(_ev_data->loop);
}

ev4cpp2tnt::~ev4cpp2tnt()
{
    while (!_ev_data->per_connection_watchers.empty())
        unregister_connection(_ev_data->per_connection_watchers.begin()->first);
    if (ev_is_active(&_ev_data->timer))
        ev_timer_stop(_ev_data->loop, &_ev_data->timer);
}

void ev4cpp2tnt::take_care(tnt::connection *cn)
{
    register_connection(cn);
    cn->on_destruct(bind(::unregister_connection, _ev_data.get(), cn));
}

void ev4cpp2tnt::enable_globally()
{
    tnt::connection::on_construct_global(bind(::register_connection, _ev_data.get(), placeholders::_1));
    tnt::connection::on_destruct_global(bind(::unregister_connection, _ev_data.get(), placeholders::_1));
}

void ev4cpp2tnt::disable_globally()
{
    tnt::connection::on_construct_global(nullptr);
    tnt::connection::on_destruct_global(nullptr);
}

void ev4cpp2tnt::register_connection(tnt::connection *cn)
{
    ::register_connection(_ev_data.get(), cn);
}

void ev4cpp2tnt::unregister_connection(tnt::connection *cn)
{
    ::unregister_connection(_ev_data.get(), cn);
}
