#ifndef EV4CPP2TNT_H
#define EV4CPP2TNT_H

#include <unordered_map>
#include <memory>
// ev++.h does not compile in C++17 mode
// It has a lack of noticeable advantages over plain C api so lets use the last one.
#include <ev.h>

namespace tnt {
class connection;
}

class ev4cpp2tnt
{
public:
    ev4cpp2tnt(struct ev_loop *loop = nullptr);
    ~ev4cpp2tnt();
    ev4cpp2tnt(ev4cpp2tnt &&src) = default;
    ev4cpp2tnt& operator= (ev4cpp2tnt &&src) = default;

    void take_care(tnt::connection *cn);

    // Caution! Always destroy connections first - before ev4cpp2tnt.
    void enable_globally();
    void disable_globally();

private:
    // we need to dynamically allocate libev watchers and other stuff to keep ev4cpp2tnt movable
    // (libev is being feeded with watchers by pointers, watcher.data is a raw pointer too)
    struct ev_data
    {
        struct ev_loop *loop = nullptr;
        ev_timer timer;
        std::unordered_map<tnt::connection*, std::unique_ptr<std::pair<ev_async, ev_io>>> per_connection_watchers;
    };
    std::unique_ptr<ev_data> _ev_data;

    void register_connection(tnt::connection *cn);
    void unregister_connection(tnt::connection *cn);

    friend void register_connection(ev_data *data, tnt::connection *cn);
    friend void unregister_connection(ev_data *data, tnt::connection *cn);
};

#endif // EV4CPP2TNT_H
