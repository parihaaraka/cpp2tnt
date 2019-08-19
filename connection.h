#ifndef TNT_NET_H
#define TNT_NET_H

/** @file */

#include <string_view>
#include <netdb.h>
#include <thread>
#include <mutex>
#include "wtf_buffer.h"
#include "unique_socket.h"
#include "fu2/function2.hpp"
#include "cs_parser.h"

/// Tarantool connector scope
namespace tnt
{

/// Internal error codes.
enum class error {
    invalid_parameter,    ///< invalid parameter (like incorrect connection string and so on)
    bad_call_sequence,    ///< api violation
    getaddr_in_progress,  ///< address resolving is still in progress
    system,               ///< system error
    getaddr,              ///< address resolving failed
    timeout,              ///< operation timeout
    auth,                 ///< authentication error
    closed_by_peer,       ///< connection closed by peer
    unexpected_data       ///< messagepack parse error and so on
};

/// Socket state to poll for.
enum socket_state {
    none = 0,      ///< disable watching
    read,          ///< ready read
    write,         ///< ready write
    read_write     ///< read or write
};

/// Tarantool connector's network layer.
class connection
{
private:
    unique_socket _socket;
    std::string _current_cs;
    /** Greeting may be used in subsequent authentication requests
     *  (to change current db user while stay connected) */
    std::string _greeting;
    cs_parts _cs_parts;
    int _idle_seconds_counter = -1;     ///< timeout counter

    /** The connection must be notified when _input_buffer was processed
     *  by caller completely. An external worker must not use _input_buffer
     *  after this notification until new responces acquired via
     *  on_response() handler. See input_processed() */
    wtf_buffer _input_buffer;           ///< ready to process buffer
    wtf_buffer _receive_buffer;         ///< recv destination (partial responce permitted)
    bool _caller_idle = true;           ///< true - connector may work with _input_buffer, false - caller
    size_t _last_received_head_offset = 0;
    size_t _detected_response_size = 0; ///< current response size (to detect it's being fetched en bloc)
    void process_receive_buffer();
    void pass_response_to_caller();

    wtf_buffer _output_buffer;          ///< actually corked buffer
    wtf_buffer _send_buffer;            ///< sending data
    // Send buffer never reallocates while sending in progress,
    // so no need to use offset instead of pointer.
    char *_next_to_send;
    uint64_t _request_id = 0;           ///< sync_id in terms of tnt
    bool _is_corked = false;
    size_t _uncorked_size = 0;          ///< size of data within output buffer

    enum class async_stage {
        none,
        address_resolving,
        connecting,
        auth,
        idle
    };
    async_stage _async_stage = async_stage::none;

    // move-only data must pass through this queue
    std::vector<fu2::unique_function<void()>> _notification_handlers, _tmp_notification_handlers;
    std::mutex _handlers_queue_guard;
    std::thread _address_resolver;
    void address_resolved(const addrinfo *addr_info);

    fu2::unique_function<void(std::string_view message,
                              error internal_error,
                              uint32_t db_error)> _error_cb = nullptr;
    void handle_error(std::string_view message = {},
                      error internal_error = error::system,
                      uint32_t db_error = 0) noexcept;

    // need to capture watcher, so movable functions preferred
    fu2::unique_function<void(int mode)> _socket_watcher_request_cb;
    fu2::unique_function<void()> _on_notify_request;
    // exact connection specific callbacks, so movable function preferred
    fu2::unique_function<void(wtf_buffer &buf)> _response_cb;
    fu2::unique_function<void()> _connected_cb;
    fu2::unique_function<void()> _disconnected_cb;

public:
    connection(std::string_view connection_string = {});
    ~connection();

    void open();
    void close(bool call_disconnect_handler = true);
    void set_connection_string(std::string_view connection_string);
    /** Thread-safe method to initiate a handler call in the connector's thread */
    void push_handler(fu2::unique_function<void()> &&handler);

    int socket_handle() const noexcept;
    std::string_view greeting() const noexcept;
    /** Get buffer to put requests in. A caller must take care of free space
     * availability by calling wtf_buffer::reserve() if needed. */
    wtf_buffer& output_buffer() noexcept;
    uint64_t last_request_id() const noexcept;
    uint64_t next_request_id() noexcept;
    const cs_parts& connection_string_parts() const noexcept;

    /** Prevent further requests from being sent immediately upon creation. */
    void cork() noexcept;
    /** flush() + allow to send further requests right away (if possible). */
    void uncork() noexcept;
    /**
     * Move accumulated requests to send buffer if possible.
     *
     * Use this method in corked mode to mark the end of accumulated
     * requests bunch. All these requests before the mark will go to send
     * buffer immediately or after the buffer run out.
     * It's assumed that we never have partial request in the _output_buffer.
     * \return true if any data was moved to send buffer
     */
    bool flush() noexcept;

    /** Notify connector that _input_buffer has been processed and
     *  it's safe to change it from connector's thread */
    void input_processed();

    /** Timeouts basis (low precision). Must be called every 1 second.
     *  Timer start/stop callbacks are not here yet, but a caller may
     *  use single timer to serve all timeout tasks and connections. */
    void tick_1sec() noexcept;

    /** Call this method from within connector's thread when asked via
     *  notify request handler. Use dedicated thread safe method of your
     *  event loop. */
    void acquire_notifications();

    /** Set sucessful connection handler. */
    connection& on_opened(decltype(_connected_cb) &&handler);

    /** Set disconnection handler. */
    connection& on_closed(decltype(_disconnected_cb) &&handler);

    /** Set error handler. */
    connection& on_error(decltype(_error_cb) &&handler);

    /** Set callback for asking external watcher to wait for specified socket state. */
    connection& on_socket_watcher_request(decltype(_socket_watcher_request_cb) &&handler);

    /** External socket watcher must call this function on ready read state detected. */
    void read();

    /** External socket watcher must call this function on ready write state detected. */
    void write();

    /** Set callback to pass reponses to. */
    connection& on_response(decltype(_response_cb) &&handler);

    /**
     * Cross-thread communication helper.
     *
     * Set callback to ask caller to excute acquire_notifications() method from
     * within connector thread. Only address resolver (separate thread) needs
     * this option (for a while).
     */
    connection& on_notify_request(decltype(_on_notify_request) &&handler);

};

} // namespace tnt

#endif // TNT_NET_H
