#ifndef MP_WRITER_H
#define MP_WRITER_H

#include <string_view>
#include <array>

namespace tnt {
class connection;
enum class request_type : uint8_t;
}

class wtf_buffer;

class mp_writer
{
public:
    mp_writer(tnt::connection &cn);
    mp_writer(tnt::connection &cn, wtf_buffer &buf);
    void encode_header(tnt::request_type req_type);
    void encode_auth_request();
    void encode_auth_request(std::string_view user, std::string_view password);

    void begin_call(std::string_view fn_name);
    /// Put array header with <max_size> cardinality and move current position over it.
    /// Call end() to replace initial cardinality with actual value (if needed).
    void begin_array(uint32_t max_size);
    void begin_map(uint32_t max_size);
    /// Replace initial cardinality settled with begin_array(), begin_map() or begin_call()
    /// with actual value. DO NOT call this function if the actual number of items
    /// inserted into the container is equal to the initial cardinality.
    void end();

private:
    template <typename T, std::size_t N = 16>
    class small_stack
    {
    private:
        std::array<T, N> _items;
        size_t _size = 0;
    public:
        small_stack() = default;
        void push(T &&value)
        {
            _items[_size++] = std::move(value);
        }
        T& pop()  // undefined if empty
        {
            if (!_size)
                return _items[_size];
            return _items[--_size];
        }
        T& last() // undefined if empty
        {
            return  _items[_size ? _size - 1 : _size];
        }
        size_t size() const noexcept
        {
            return _size;
        }
        bool empty() const noexcept
        {
            return _size == 0;
        }
    };

    struct container_meta
    {
        size_t head_offset; // we'll detect container type by the first byte
        uint32_t max_cardinality;
        uint32_t items_count = 0; // map will have 2x items
    };

    small_stack<container_meta> _opened_containers;
    tnt::connection &_cn; ///< request id, initial user name and password source
    wtf_buffer &_buf;
};

#endif // MP_WRITER_H
