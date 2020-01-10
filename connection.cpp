#include "connection.h"
#include <cstring>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "msgpuck/msgpuck.h"
#include "proto.h"
#include "mp_reader.h"
#include "mp_writer.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define GENERAL_TIMEOUT 10

std::function<void(tnt::connection*)> tnt::connection::_on_construct_global_cb;
std::function<void(tnt::connection*)> tnt::connection::_on_destruct_global_cb;

static std::string errno2str()
{
    char buf[128];
#if (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE
    strerror_r(errno, buf, sizeof(buf));
    return buf;
#else
    return strerror_r(errno, buf, sizeof(buf));
#endif
}

namespace tnt
{

using namespace std;

connection::connection(std::string_view connection_string)
    : _current_cs(connection_string)
{
    _next_to_send = _send_buffer.data();
    _receive_buffer.on_clear = [this]() noexcept {
        _last_received_head_offset = 0;
        _detected_response_size = 0;
    };

    if (_on_construct_global_cb)
        _on_construct_global_cb(this);
}

void connection::handle_error(string_view message, error internal_error, uint32_t db_error) noexcept
{
    if (_error_cb)
    {
        try
        {
            if (errno && message.empty())
                _error_cb(errno2str(), internal_error, db_error);
            else
                _error_cb(message, internal_error, db_error);
        }
        catch (...) {}
    }
}

void connection::process_receive_buffer()
{
    // detect response verges
    size_t orphaned_bytes;
    do
    {
        orphaned_bytes = _receive_buffer.size() - _last_received_head_offset;
        if (!_detected_response_size && orphaned_bytes >= 5) // length part of standard tnt header
        {
            const char *head = _receive_buffer.data() + _last_received_head_offset;
            if (mp_typeof(*head) == MP_UINT)
                _detected_response_size = mp_decode_uint(&head) + 5;
            else
            {
                handle_error("incorrect iproto message", error::unexpected_data);
                _receive_buffer.resize(_last_received_head_offset);
            }
        }

        if (_detected_response_size && orphaned_bytes >= _detected_response_size)
        {
            _last_received_head_offset += _detected_response_size;
            _detected_response_size = 0;
            continue;
        }
        break;
    }
    while (true);

    // there are full responses in the buffer
    if (_last_received_head_offset)
    {
        // automatic authentication must be processed in a special way
        // (in contradistinction to manual authentication request)
        if (_state == state::authentication)
        {
            try
            {
                mp_reader response = mp_reader(_receive_buffer).iproto_message();
                uint32_t code;
                response.read<mp_map_reader>()[header_field::CODE] >> code;
                if (!code)
                {
                    _receive_buffer.clear();
                    _state = state::connected;
                    _idle_seconds_counter = -1;
                    if (_connected_cb)
                    {
                        try
                        {
                            _connected_cb();
                        }
                        catch (const exception &e)
                        {
                            handle_error(e.what(), error::external);
                        }
                        catch (...) {}
                    }
                    return;
                }
                code &= 0x7fff;
                handle_error(response.read<mp_map_reader>()[response_field::ERROR].to_string(), error::auth, code);
            }
            catch (const mp_reader_error &e)
            {
                handle_error(e.what(), error::unexpected_data);
            }
            catch (const exception &e)
            {
                handle_error(e.what(), error::system);
            }

            _receive_buffer.clear();
            close(false);
            _idle_seconds_counter = 0; // reconnect soon
        }
        else if (_caller_idle)
        {
            pass_response_to_caller();
        }
    }
}

void connection::pass_response_to_caller()
{
    if (!_last_received_head_offset)
        return;

    size_t orphaned_bytes = _receive_buffer.size() - _last_received_head_offset;
    _input_buffer.clear();
    _input_buffer.swap(_receive_buffer);
    if (orphaned_bytes) // partial response
    {
        _receive_buffer.resize(orphaned_bytes);
        memcpy(_receive_buffer.data(), _input_buffer.data() + _last_received_head_offset, orphaned_bytes);
        _input_buffer.resize(_input_buffer.size() - orphaned_bytes);
    }
    _last_received_head_offset = 0;

    if (_response_cb)
    {
        _caller_idle = false;
        try
        {
            _response_cb(_input_buffer);
        }
        catch (const exception &e)
        {
            handle_error(e.what(), error::system);
            input_processed();  // !!!
        }
        // If a caller processes data synchronously, then we will never get
        // nested calls, because the loop is stuck - we do not receive data.
        // If a caller processes data asynchronously, then the loop is ok.
    }
    else
    {
        _input_buffer.clear(); // wipe data that is not going to be processed
    }
}

void connection::watch_socket(socket_state mode) noexcept
{
    _prev_watch_mode = mode;
    _socket_watcher_request_cb(mode);
}

connection::~connection()
{
    close();
    if (_address_resolver.joinable())
        _address_resolver.join();

    try
    {
        if (_on_destruct_cb)
            _on_destruct_cb();
        else if (_on_destruct_global_cb)
            _on_destruct_global_cb(this);
    }
    catch (const exception &e)
    {
        handle_error(e.what(), error::external);
    }
    catch (...) {}
}

void connection::address_resolved(const addrinfo *addr_info)
{
    // disconnect() during resolving prevents further connecting
    if (_state != state::address_resolving)
        return;

    for (const addrinfo *addr = addr_info; addr; addr = addr->ai_next)
    {
        unique_socket s{socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK, addr->ai_protocol)};
        if (!s)
        {
            handle_error();
            continue;
        }

        int opt = 1;
        setsockopt(s.handle(), IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // GENERAL_TIMEOUT seconds max to get transmitted data acknowledgement
        // man:
        //     This option can be set during any state of a TCP connection,
        //     but is effective only during the synchronized states of a
        //     connection (ESTABLISHED, FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT,
        //     CLOSING, and LAST-ACK).  Moreover, when used with the TCP
        //     keepalive (SO_KEEPALIVE) option, TCP_USER_TIMEOUT will
        //     override keepalive to determine when to close a connection due
        //     to keepalive failure.
        opt = GENERAL_TIMEOUT * 1000;
        setsockopt(s.handle(), SOL_TCP, TCP_USER_TIMEOUT, &opt, sizeof(opt));
        // bad luck to get errors here, but why would we stop connecting?

        _state = state::connecting;
        if (connect(s.handle(), addr->ai_addr, addr->ai_addrlen) != -1)
        {
            _socket = move(s);
            watch_socket(socket_state::read); //wait for greeting
            return;
        }

        if (errno == EINPROGRESS)
        {
            _socket = move(s);
            watch_socket(socket_state::write);
            _idle_seconds_counter = 0;
            return;
        }

        handle_error();
        close(false);
        break;
    }

    _state = state::disconnected;
    _idle_seconds_counter = 0; // reconnect soon
}

void connection::open(int delay)
{
    if (_state == state::connected)
        return;

    if (_state != state::disconnected)
    {
        handle_error("unable to connect, connection is busy", error::bad_call_sequence);
        return;
    }

    if (delay > 0)
    {
        _idle_seconds_counter = delay;
        return;
    }

    if (_address_resolver.joinable())
    {
        handle_error("address resolver is still in progress", error::getaddr_in_progress);
        return;
    }

    _idle_seconds_counter = -1;
    _cs_parts = parse_cs(_current_cs);
    if (!_cs_parts.host.empty())
    {
        // getaddrinfo is a piece of unstoppable shit:
        // https://www.stefanchrist.eu/blog/2016_06_03/Signals,%20pthreads%20and%20getaddrinfo.xhtml
        // * eventfd - linux only
        // so don't try to implement resolving timeout

        _state = state::address_resolving;
        _address_resolver = std::thread([this]()
        {
            addrinfo *addr_info = nullptr;
            addrinfo hints{0, 0, SOCK_STREAM, IPPROTO_TCP, 0, nullptr, nullptr, nullptr};

            int res;
            do
            {
                res = getaddrinfo(_cs_parts.host.c_str(), _cs_parts.port.c_str(), &hints, &addr_info);
            }
            while (res == EAI_AGAIN);
            unique_ptr<addrinfo, void(*)(addrinfo*)> ai{addr_info, freeaddrinfo};

            if (!res && ai)
            {
                lock_guard<mutex> lk(_handlers_queue_guard);
                _notification_handlers.push_back([ai = move(ai), this](){
                    if (_address_resolver.joinable())
                        _address_resolver.join();
                    address_resolved(ai.get());
                });
            }
            else
            {
                string message;
                if (res == EAI_SYSTEM)
                    message = errno2str();
                else
                    message = gai_strerror(res);
                lock_guard<mutex> lk(_handlers_queue_guard);
                _notification_handlers.push_back([message, this](){
                    if (_address_resolver.joinable())
                        _address_resolver.join();
                    _state = state::disconnected;
                    handle_error(message, error::getaddr);
                    // retry because we may fix dns
                    _idle_seconds_counter = 0;
                });
            }
            // ask to notify this engine within its thread
            if (_on_notify_request)
                _on_notify_request();
            // else ?
        });
    }
    else if (!_cs_parts.unix_socket_path.empty())
    {
        unique_socket s = socket(PF_UNIX, SOCK_STREAM, 0);
        if (!s)
        {
            handle_error();
            return;
        }

        sockaddr_un addr{AF_UNIX, {}};
        copy(_cs_parts.unix_socket_path.begin(),
             _cs_parts.unix_socket_path.end(),
             addr.sun_path);

        _state = state::connecting;
        if (connect(s.handle(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != -1)
        {
            _socket = move(s);
            watch_socket(socket_state::read); //wait for greeting
            return;
        }

        if (errno == EAGAIN)
        {
            _socket = move(s);
            watch_socket(socket_state::write);
            _idle_seconds_counter = 0;
            return;
        }

        handle_error();
        close(false);
        _idle_seconds_counter = 0; // reconnect soon
    }
    else
    {
        handle_error("incorrect connection string", error::invalid_parameter);
    }
}

void connection::close(bool call_disconnect_handler, bool reconnect_soon) noexcept
{
    auto prev_async_stage = _state;
    _greeting.clear();
    _state = state::disconnected;
    _idle_seconds_counter = reconnect_soon ? 0 : -1;
    _request_id = 0;

    if (!_socket)
        return;

    watch_socket(socket_state::none);
    _socket.close();

    // Clear all sending buffers. A caller must resume its work
    // according to application logic.
    _output_buffer.clear();
    _send_buffer.clear();
    _next_to_send = _send_buffer.data(); // otherwise flush() is broken on reconnect
    _uncorked_size = 0;

    // remove partial response
    _detected_response_size = 0;
    // do not throw because never expand here
    _receive_buffer.resize(_last_received_head_offset);

    if (prev_async_stage != state::connecting && _disconnected_cb && call_disconnect_handler)
    {
        try
        {
            _disconnected_cb();
        }
        catch (const exception &e)
        {
            handle_error(e.what(), error::external);
        }
        catch (...) {}
    }
}

void connection::set_connection_string(string_view connection_string)
{
    if (_state != state::disconnected)
        throw runtime_error("unable to reset connection string on busy connection");
    _current_cs = connection_string;
}

void connection::push_handler(fu2::unique_function<void()> &&handler)
{
    unique_lock<mutex> lk(_handlers_queue_guard);
    _notification_handlers.push_back(move(handler));
    lk.unlock();

    if (_on_notify_request)
        _on_notify_request();
}

int connection::socket_handle() const noexcept
{
    return _socket.handle();
}

string_view connection::greeting() const noexcept
{
    return _greeting;
}

wtf_buffer& connection::output_buffer() noexcept
{
    return _output_buffer;
}

uint64_t connection::last_request_id() const noexcept
{
    return _request_id - 1;
}

uint64_t connection::next_request_id() noexcept
{
    return _request_id++;
}

const cs_parts &connection::connection_string_parts() const noexcept
{
    return _cs_parts;
}

bool connection::is_opened() const noexcept
{
    return _state == state::connected;
}

bool connection::is_closed() const noexcept
{
    return _state == state::disconnected;
}

void connection::cork() noexcept
{
    _is_corked = true;
}

void connection::uncork() noexcept
{
    flush();
    _is_corked = false;
}

bool connection::flush() noexcept
{
    // nothing to send
    if (!_output_buffer.size())
        return true;

    size_t bytes_not_sent = static_cast<size_t>(_send_buffer.end - _next_to_send);
    if (!bytes_not_sent)
    {
        _send_buffer.clear();
        _send_buffer.swap(_output_buffer);
        _next_to_send = _send_buffer.data();
        _uncorked_size = 0;
        write();
        return true;
    }

    _uncorked_size = _output_buffer.size();
    return false;
}

void connection::input_processed()
{
    // not an atomic yet.. it depends on implementation of next abstraction layer
    _caller_idle = true;
    pass_response_to_caller();
}

void connection::tick_1sec() noexcept
{
    if (_idle_seconds_counter >= 0 && ++_idle_seconds_counter >= GENERAL_TIMEOUT)
    {
        if (_state == state::disconnected) // waiting for reconnect
        {
            open();
        }
        else // connecting
        {
            close();
            handle_error("timeout expired", error::timeout);
            _idle_seconds_counter = 0; // reconnect soon
        }
    }

    // TMP
    else if (is_opened() && _uncorked_size && std::time(nullptr) - _last_write_time > 10)
    {
        size_t bytes_to_send = static_cast<size_t>(_send_buffer.end - _next_to_send);
        handle_error("~~~~~ uncorked data is stuck! ~~~~~"
                     "\ncurrent socket watch mode: " +
                     std::to_string(_prev_watch_mode) +
                     "\nbytes_to_send: " + std::to_string(bytes_to_send) +
                     "\nuncorked_size: " + std::to_string(_uncorked_size));
        flush();
    }
}

void connection::acquire_notifications()
{
    unique_lock<mutex> lk(_handlers_queue_guard);
    _tmp_notification_handlers.swap(_notification_handlers);
    lk.unlock();

    for (auto &fn: _tmp_notification_handlers)
        fn();
    _tmp_notification_handlers.clear();
}

connection& connection::on_opened(decltype(_connected_cb) &&handler)
{
    _connected_cb = move(handler);
    return *this;
}

connection& connection::on_closed(decltype(_disconnected_cb) &&handler)
{
    _disconnected_cb = move(handler);
    return *this;
}

connection& connection::on_error(decltype(_error_cb) &&handler)
{
    _error_cb = move(handler);
    return *this;
}

connection& connection::on_socket_watcher_request(decltype(_socket_watcher_request_cb) &&handler)
{
    _socket_watcher_request_cb = move(handler);
    return *this;
}

void connection::read()
{
    // some pollers may return dummy event to handle bad socket
    if (!_socket)
        return;

    do
    {
        auto buf_capacity = _receive_buffer.capacity();
        size_t rest = buf_capacity - _receive_buffer.size();
        if (rest < 1024)
        {
            _receive_buffer.reserve(size_t(buf_capacity * 1.5));
            rest = _receive_buffer.capacity() - _receive_buffer.size();
        }

        ssize_t r = recv(_socket.handle(), _receive_buffer.end, rest, 0);
        if (r <= 0)
        {
            if (r == 0)
                handle_error("connection closed by peer", error::closed_by_peer);
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else if (errno == EINTR) // interrupted by signal
                continue;
            else
                handle_error();

            close();
            _idle_seconds_counter = 0; // reconnect soon
            return;
        }
        _receive_buffer.end += r;
    }
    while (true);

    if (_state == state::connecting) // greeting
    {
        if (_receive_buffer.size() < tnt::GREETING_SIZE)
            return; // continue to read

        _greeting.assign(_receive_buffer.data(), _receive_buffer.size());
        _receive_buffer.clear();

        if (!_cs_parts.user.empty() || _cs_parts.user != "guest" || !_cs_parts.password.empty())
        {
            _state = state::authentication;

            // just to be sure that the buffer is not littered by a caller who ignored on_closed event
            _send_buffer.clear();
            _next_to_send = _send_buffer.data();

            iproto_writer dst(*this, _send_buffer); // skip _output buffer
            dst.encode_auth_request();
            write();

            //iproto_writer dst(*this);
            //dst.encode_auth_request();
            //flush();
        }
        else
        {
            // no need to authenticate
            _state = state::connected;
            _idle_seconds_counter = -1;
            if (_connected_cb)
            {
                try
                {
                    _connected_cb();
                }
                catch (const exception &e)
                {
                    handle_error(e.what(), error::external);
                }
                catch (...) {}
            }
        }

        return;
    }

    process_receive_buffer();
}

void connection::write() noexcept
{
    if (_state == state::connecting)
    {
        int opt = 0;
        socklen_t len = sizeof(opt);
        if (getsockopt(_socket.handle(), SOL_SOCKET, SO_ERROR, &opt, &len) == -1 || opt)
        {
            if (opt)
                errno = opt;
            handle_error();
            close(false);
            _idle_seconds_counter = 0; // reconnect soon
            return;
        }
        watch_socket(socket_state::read);
        return;
    }

    assert(_send_buffer.end >= _next_to_send);
    size_t bytes_to_send = static_cast<size_t>(_send_buffer.end - _next_to_send);

    // TMP
    if (bytes_to_send <= 0 && _uncorked_size)
    {
        handle_error("~~~~~ wtf inside write() ?! ~~~~~"
                     "\ncurrent socket watch mode: " +
                     std::to_string(_prev_watch_mode) +
                     "\nbytes_to_send: " + std::to_string(bytes_to_send) +
                     "\nuncorked_size: " + std::to_string(_uncorked_size));
    }

    while (bytes_to_send > 0)
    {
        ssize_t r = send(_socket.handle(),
                         _next_to_send,
                         bytes_to_send,
                         MSG_NOSIGNAL);
        if (r <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR) // interrupted by signal (shouldn't happen)
                continue;
            handle_error();
            close();
            _idle_seconds_counter = 0; // reconnect soon
            return;
        }
        _last_write_time = std::time(nullptr);
        bytes_to_send -= static_cast<size_t>(r);
        _next_to_send += r;

        // _send_buffer is done, _output_buffer has data to send
        if (!bytes_to_send && _uncorked_size)
        {
            _send_buffer.clear();
            _send_buffer.swap(_output_buffer);
            _next_to_send = _send_buffer.data();
            bytes_to_send = _uncorked_size;
            _uncorked_size = 0;
            if (bytes_to_send < _send_buffer.size())
            {
                size_t corked_tail_size = _send_buffer.size() - bytes_to_send;
                memcpy(_output_buffer.data(), _send_buffer.data() + bytes_to_send, corked_tail_size);
                _output_buffer.resize(corked_tail_size);
                _send_buffer.resize(bytes_to_send);
            }
        }
    }

    watch_socket(bytes_to_send ?
                     socket_state::read_write :
                     socket_state::read);
}

connection &connection::on_response(decltype(_response_cb) &&handler)
{
    _response_cb = move(handler);
    return *this;
}

connection& connection::on_notify_request(decltype(_on_notify_request) &&handler)
{
    _on_notify_request = move(handler);
    return *this;
}

void connection::on_construct_global(const std::function<void(connection*)> &handler)
{
    _on_construct_global_cb = handler;
}

void connection::on_destruct_global(const std::function<void(connection*)> &handler)
{
    _on_destruct_global_cb = handler;
}

void connection::on_destruct(fu2::unique_function<void()> &&handler)
{
    _on_destruct_cb = move(handler);
}

} // namespace tnt
