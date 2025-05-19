#include <condition_variable>
#include <map>
#include <queue>
#include "sync.h"
#include "connection.h"
#include "ev4cpp2tnt.h"
#include <ev.h>
#include "ut.hpp"
#include "misc.h"

using namespace std::chrono_literals;

scope::scope(fu2::unique_function<void ()> dtor) : dtor(std::move(dtor))
{
}

scope::~scope()
{
    if (dtor)
        dtor();
}

// we may replace the queue with a single functional object for a while
std::queue<fu2::unique_function<void (tnt::connection &)>> tasks4ev;
// Responce processing functions to wrap and put into the tests_side_handlers.
std::map<uint64_t, fu2::unique_function<bool(const mp_map_reader&, const mp_map_reader&)>> loop_side_handlers;
// Handlers prepared in ev (tnt connector) thread to be executed in main thread.
std::queue<fu2::unique_function<bool()>> tests_side_handlers;
std::mutex m;
std::condition_variable cv;

std::jthread loop_thread;
tnt::connection cn;
struct ev_loop *loop = nullptr;
ev_async event_watcher;
ev4cpp2tnt ev_wrapper;

bool wait_and_exec_responce()
{
    std::unique_lock lk(m);
    cv.wait(lk, []{ return loop_side_handlers.empty() && !tests_side_handlers.empty(); });
    bool ok = true;
    while (!tests_side_handlers.empty())
    {
        auto res_fn = std::move(tests_side_handlers.front());
        tests_side_handlers.pop();
        if (tests_side_handlers.empty())
            return res_fn() && ok;
        ok = res_fn() && ok;
    }
    return false;
}

bool sync_tnt_request(fu2::unique_function<void(tnt::connection&)> fn)
{
    if (!loop)
        throw std::runtime_error("call run_loop() first");

    {
        std::lock_guard lk(m);
        tasks4ev.push(std::move(fn));
    }
    ev_async_send(loop, &event_watcher);
    return wait_and_exec_responce();
}

void set_handler(uint64_t request_id, fu2::unique_function<bool(const mp_map_reader &, const mp_map_reader &)> handler)
{
    std::lock_guard lk(m);
    loop_side_handlers[request_id] = std::move(handler);
}

void signal_cb(struct ev_loop *loop, ev_signal *, int)
{
    ev_break(loop);
}

void event_cb(struct ev_loop *, ev_async *, int)
{
    std::unique_lock lk(m);
    while (!tasks4ev.empty())
    {
        auto fn = std::move(tasks4ev.front());
        tasks4ev.pop();
        lk.unlock();
        try
        {
            fn(cn);
        }
        catch (...) {}
        lk.lock();
    }
}

scope run_loop()
{
    loop_thread = std::jthread([](){
        loop = EV_DEFAULT;

        ev_signal term_signal_watcher;
        ev_signal_init(&term_signal_watcher, signal_cb, SIGTERM);
        ev_signal_start(loop, &term_signal_watcher);
        ev_unref(loop);

        ev_signal int_signal_watcher;
        ev_signal_init (&int_signal_watcher, signal_cb, SIGINT);
        ev_signal_start(loop, &int_signal_watcher);
        ev_unref(loop);

        ev_async_init(&event_watcher, event_cb);
        ev_async_start(loop, &event_watcher);
        // keep this watcher referenced to avoid loop exit

        ev_wrapper = ev4cpp2tnt(loop);
        {
            std::lock_guard lk(m);
            tests_side_handlers.push([](){
                return true;
            });
        }
        cv.notify_one();
        ev_run(loop, 0);
    });
    wait_and_exec_responce();

    return scope([](){
        sync_tnt_request([](tnt::connection &){
            if (loop)
                ev_break(loop);
            loop = nullptr;

            std::lock_guard lk(m);
            tests_side_handlers.push([](){
                if (loop_thread.joinable())
                    loop_thread.join();
                return true;
            });
            cv.notify_one();
        });
    });
}

bool open_tnt_connection(const std::string &connection_string)
{
    std::unique_lock lk(m);
    tasks4ev.push([connection_string](tnt::connection &cn){
        cn.set_connection_string(connection_string);
        ev_wrapper.take_care(&cn);
        cn.on_error([&](std::string_view message, tnt::error code, uint32_t db_error)
        {
            std::lock_guard lk(m);
            tests_side_handlers.push([msg = std::format("error: {}\n  internal code: {}\n  db error code: {}\n", message, static_cast<int>(code), db_error)](){
                boost::ut::log << msg;
                return false;
            });
            for (auto &i: loop_side_handlers)
            {
                tests_side_handlers.push([fn = std::move(i.second)]() mutable {
                    fn(mp_map_reader(), mp_map_reader());
                    return false;
                });
            }
            loop_side_handlers.clear();
            cv.notify_one();
        });

        cn.on_opened([]()
        {
            std::lock_guard lk(m);
            tests_side_handlers.push([](){
                return true;
            });
            cv.notify_one();
        });

        cn.on_closed([](){
            std::lock_guard lk(m);
            for (auto &i: loop_side_handlers)
            {
                tests_side_handlers.push([fn = std::move(i.second)]() mutable {
                    fn(mp_map_reader(), mp_map_reader());
                    return false;
                });
            }
            loop_side_handlers.clear();
            cv.notify_one();
        });

        cn.on_response([&](wtf_buffer &buf)
        {
            mp_reader bunch{buf};
            while (mp_reader r = bunch.iproto_message())
            {
                // make a copy to capture it
                std::vector<char> src_copy(r.begin(), r.end());
                r = mp_reader(src_copy);

                try
                {
                    auto encoded_header = r.read<mp_map_reader>();
                    uint64_t sync;
                    encoded_header[tnt::header_field::SYNC] >> sync;
                    auto handler = loop_side_handlers.extract(sync);
                    if (handler.empty())
                    {
                        std::lock_guard lk(m);
                        tests_side_handlers.push([msg = std::format("orphaned response {} acquired\n", sync)](){
                            boost::ut::log << msg;
                            return false;
                        });
                    }
                    else
                    {
                        auto encoded_body = r.read<mp_map_reader>();
                        std::lock_guard lk(m);
                        // to be executed in the main thread
                        tests_side_handlers.push([src = std::move(src_copy), fn = std::move(handler.mapped()), encoded_header, encoded_body]() mutable {
                            return fn(encoded_header, encoded_body);
                        });
                    }
                }
                catch(const mp_reader_error &e)
                {
                    std::lock_guard lk(m);
                    tests_side_handlers.push([msg = std::string(e.what())](){
                        boost::ut::log << msg << "\n";
                        return false;
                    });
                    //ev_break(loop);
                }
                catch(const std::exception &e)
                {
                    tests_side_handlers.push([msg = std::format("{}\n{}\n", e.what(), hex_dump(r.begin(), r.end()))](){
                        boost::ut::log << msg;
                        return false;
                    });
                    //ev_break(loop);
                }
            }
            cn.input_processed();
            cv.notify_one();
        });

        cn.open();
    });
    lk.unlock();
    //std::this_thread::sleep_for(10ms);
    ev_async_send(loop, &event_watcher);
    return wait_and_exec_responce();;
}
