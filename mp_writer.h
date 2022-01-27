#ifndef MP_WRITER_H
#define MP_WRITER_H

#include <string_view>
#include <array>
#include <map>
#include <cmath>
#include "msgpuck/msgpuck.h"
#include "wtf_buffer.h"

namespace tnt {
class connection;
enum class request_type : uint8_t;
}

/** msgpuck wrapper.
 *
 * A caller must ensure there is enough free space in the buffer.
 */
class mp_writer
{
public:
	/// hack to serialize small unsigned integers into bigger messagepack values
	template<typename T>
	struct strict_uint
	{
		T value;
		strict_uint(const T &val) : value(val) {}
	};

    /// Wrap writer object around specified buffer.
    mp_writer(wtf_buffer &buf);
    /// Put array header with <max_size> cardinality and move current position over it.
    /// A caller must call finalize() to close the array and actualize its initial size.
    void begin_array(uint32_t max_cardinality);
    /// Put map header with <max_size> cardinality and move current position over it.
    /// A caller must call finalize() to close the map and actualize its initial size.
    void begin_map(uint32_t max_cardinality);
    /// Replace initial cardinality settled with begin_array(), begin_map() with actual
    /// value (counted untill now).
    void finalize();

    /// Append msgpack buffer.
    void write(const char *begin, const char *end, size_t cardinality = 0);
    /// Append msgpack buffer.
    inline void write(const wtf_buffer &data, size_t cardinality = 0)
    {
        write(data.data(), data.end, cardinality);
    }

    inline operator const wtf_buffer&() const noexcept { return _buf; }
    inline wtf_buffer& buf() const noexcept { return _buf; }

    mp_writer& operator<< (std::nullptr_t);
    mp_writer& operator<< (const std::string_view &val);
    inline mp_writer& operator<< (const wtf_buffer &data)
    {
        write(data);
        return *this;
    }

    template <typename T>
    mp_writer& operator<< (const std::optional<T> &val) noexcept
    {
        if (!val.has_value())
        {
            _buf.end = mp_encode_nil(_buf.end);

            if (!_opened_containers.empty())
                ++_opened_containers.top().items_count;
            return *this;
        }
        *this << val.value();
        return *this;
    }

    template <typename T, typename = std::enable_if_t<
                  (std::is_integral_v<T> && sizeof(T) < 16) ||
                   std::is_floating_point_v<T>
                   >>
	mp_writer& operator<< (const T &val)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            _buf.end = mp_encode_bool(_buf.end, val);
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            if constexpr (sizeof(T) <= 4)
                _buf.end = mp_encode_float(_buf.end, val);
            else if (!std::isfinite(val) || (val <= std::numeric_limits<double>::max() && val >= std::numeric_limits<double>::min()))
                _buf.end = mp_encode_double(_buf.end, static_cast<double>(val));
            else
                throw std::overflow_error("unable to fit floating point value into msgpack");
        }
        else
        {
            _buf.end = (val >= 0 ?
                        mp_encode_uint(_buf.end, static_cast<uint64_t>(val)) :
                        mp_encode_int(_buf.end, val));
        }

        if (!_opened_containers.empty())
            ++_opened_containers.top().items_count;
        return *this;
    }

	template <typename T>
	mp_writer& operator<< (const strict_uint<T> &val)
	{
		if constexpr (sizeof(T) == 2)
		{
			_buf.end = mp_store_u8(_buf.end, 0xcd);
			_buf.end = mp_store_u16(_buf.end, uint16_t(val.value));
		}
		else if constexpr (sizeof(T) == 4)
		{
			_buf.end = mp_store_u8(_buf.end, 0xce);
			_buf.end = mp_store_u32(_buf.end, uint32_t(val.value));
		}
		else if constexpr (sizeof(T) == 8)
		{
			_buf.end = mp_store_u8(_buf.end, 0xcf);
			_buf.end = mp_store_u64(_buf.end, uint64_t(val.value));
		}
		else
			static_assert(!sizeof(T), "unsupported size");

		if (!_opened_containers.empty())
			++_opened_containers.top().items_count;
		return *this;
	}

    template<typename... Args>
    mp_writer& operator<< (const std::tuple<Args...> &val)
    {
        begin_array(std::tuple_size_v<std::tuple<Args...>>);
        std::apply(
            [this](const auto&... item)
            {
                ((*this << item), ...);
            },
            val
        );
        finalize();
        return *this;
    }

    template <typename T>
    mp_writer& operator<< (const std::vector<T> &val)
    {
        begin_array(val.size());
        for (const auto& elem: val)
            *this << elem;
        finalize();
        return *this;
    }

    template <typename KeyT, typename ValueT>
    mp_writer& operator<< (const std::map<KeyT, ValueT> &val)
    {
        begin_map(val.size());
        for (const auto& elem: val)
        {
            *this << elem.first;
            *this << elem.second;
        }
        finalize();
        return *this;
    }

protected:
    template <typename T, std::size_t N = 16>
    class wtf_stack
    {
    private:
        std::array<T, N> _items;
        size_t _size = 0;
    public:
        wtf_stack() = default;
        void push(T &&value)
        {
            _items[_size++] = std::move(value);
        }
        T& pop() noexcept  // undefined if empty
        {
            if (!_size)
                return _items[_size];
            return _items[--_size];
        }
        T& top() noexcept  // undefined if empty
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

    wtf_stack<container_meta> _opened_containers;
    wtf_buffer &_buf;
};


/** Helper to compose iproto messages.
*
* A caller must ensure there is enough free space in the underlying buffer.
*/
class iproto_writer : public mp_writer
{
public:
	/// Wrap writer object around specified buffer.
	iproto_writer(wtf_buffer &buf);

	/// Initiate request or response. A caller must compose header and body afterwards and call finalize()
	/// to finalize the message.
	void start_message();

	/// Finalize topmost container or finalize a message (by fixing its final size).
	void finalize();
	/// Finalize all non-finalized containers, call, eval, etc (if exists).
	void finalize_all();

	using mp_writer::operator<<;
};

/** Helper to compose requests.
*
* A caller must ensure there is enough free space in the underlying buffer.
*/
class iproto_client : public iproto_writer
{
public:
	/// Wrap writer object around cn.output_buffer().
	iproto_client(tnt::connection &cn);
	iproto_client(tnt::connection &cn, wtf_buffer &buf);

	/// Initiate request of specified type. A caller must compose a body afterwards and call finalize()
	/// to finalize request.
	void encode_header(tnt::request_type req_type);
	/// Put authentication request into the underlying buffer. User and password
	/// from connection string are used.
	void encode_auth_request();
	/// Put authentication request into the underlying buffer.
	void encode_auth_request(std::string_view user, std::string_view password);
	/// Put ping request into the underlying buffer.
	void encode_ping_request();

	/// Initiate call request. A caller must pass an array of arguments afterwards
	/// and call finalize() to finalize request.
	void begin_call(std::string_view fn_name);

	void begin_eval(std::string_view script);

	/// Call request all-in-one wrapper.
	template <typename ...Ts>
	void call(std::string_view fn_name, Ts const&... args)
	{
		begin_call(fn_name);
		begin_array(sizeof...(args));
		((*this << args), ...);
		finalize_all();
	}

	/// Call request all-in-one wrapper.
	template <typename ...Ts>
	void eval(std::string_view script, Ts const&... args)
	{
		begin_eval(script);
		begin_array(sizeof...(args));
		((*this << args), ...);
		finalize_all();
	}

	using mp_writer::operator<<;

private:
	tnt::connection &_cn; ///< request id, initial user name and password source
};

#endif // MP_WRITER_H
