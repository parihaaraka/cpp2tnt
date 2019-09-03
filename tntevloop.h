#pragma once


#include <ev.h>

#include "connection.h"

namespace tnt
{

    class TntEvLoop : public connection
    {
    public:
        TntEvLoop();
        void Attach(struct ev_loop* loop);

    protected:
        struct ev_loop* loop_;

        ev_io socketWatcher_;
        static void OnSocketEvent_(struct ev_loop* loop, ev_io* w, int revents);
        void OnSocketEvent(struct ev_loop* loop, ev_io* w, int revents);

        ev_async asyncNotifier_;
        static void OnAsyncNotifier_(struct ev_loop* loop, ev_async* w, int revents);
        void OnAsyncNotifier(struct ev_loop* loop, ev_async* w, int revents);

        ev_timer timer_;
        static void OnTimer_(struct ev_loop* loop, ev_timer* w, int revents);
        void OnTimer(struct ev_loop* loop, ev_timer* w, int revents);

        void OnSocketWatcherRequest(int mode);
        void OnConnected();

    };

}
