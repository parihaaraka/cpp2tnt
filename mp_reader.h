#ifndef MP_READER_H
#define MP_READER_H

#include <cstddef>
#include <stdexcept>
#include <optional>
#include <vector>
#include <map>
#include "msgpuck/msgpuck.h"

class wtf_buffer;
class mp_map_reader;
class mp_array_reader;
class mp_reader;

std::string hex_dump(const char *begin, const char *end, const char *pos = nullptr);

/// messagepack parsing error
class mp_reader_error : public std::runtime_error
{
public:
    explicit mp_reader_error(const std::string &msg, const mp_reader &reader);
};

/// messagepack reader
class mp_reader
{
public:
    mp_reader(const wtf_buffer &buf);
    mp_reader(const char *begin, const char *end);
    mp_reader(const mp_reader &) = default;
    const char* begin() const noexcept;
    const char* end() const noexcept;
    const char* pos() const noexcept;
    /// Skip current encoded item (in case of array/map skips all its elements).
    void skip();
    /// Skip current encoded item and verify its type.
    void skip(mp_type type, bool nullable = false);
    /// Return current encoded map within separate reader and move current position to next item.
    mp_map_reader map();
    /// Return current encoded array within separate reader and move current position to next item.
    mp_array_reader array();
    /// Return current encoded iproto message (header + body) within separate reader
    /// and move current position to next item.
    mp_reader iproto_message();
    /// Extract string data from current encoded string and move current position to next item.
    std::string_view to_string();

    /// true if not empty
    operator bool() const noexcept;

    mp_reader& operator>> (std::string &val);
    mp_reader& operator>> (std::string_view &val);

    template <typename T>
    mp_reader& operator>> (std::optional<T> &val)
    {
        if (mp_typeof(*_current_pos) == MP_NIL)
        {
            mp_decode_nil(&_current_pos);
            val = std::nullopt;
            return *this;
        }
        T non_opt;
        operator>>(non_opt);
        val = std::move(non_opt);
        return *this;
    }

    // use external operator overload for 128 bit integers
    template <typename T, typename = std::enable_if_t<std::is_integral_v<T> && sizeof(T) < 16>>
    mp_reader& operator>> (T &val)
    {
        auto type = mp_typeof(*_current_pos);
        if constexpr (std::is_same_v<T, bool>)
        {
            if (type == MP_BOOL)
            {
                val = mp_decode_bool(&_current_pos);
                return *this;
            }
            throw mp_reader_error("boolean expected", *this);
        }
        else
        {
            if (type == MP_UINT)
            {
                uint64_t res = mp_decode_uint(&_current_pos);
                if (res <= std::numeric_limits<T>::max())
                {
                    val = static_cast<T>(res);
                    return *this;
                }
            }
            else if (type == MP_INT)
            {
                int64_t res = mp_decode_int(&_current_pos);
                if (res <= std::numeric_limits<T>::max() && res >= std::numeric_limits<T>::min())
                {
                    val = static_cast<T>(res);
                    return *this;
                }
            }
            else
            {
                throw mp_reader_error("integer expected", *this);
            }
            throw mp_reader_error("value overflow", *this);
        }
    }

    template <typename T>
    mp_reader& operator>> (std::vector<T> &val);

    template <typename KeyT, typename ValueT>
    mp_reader& operator>> (std::map<KeyT, ValueT> &val);

    template <typename... Args>
    mp_reader& operator>> (std::tuple<Args...> &val);

    template <typename... Args>
    mp_reader& operator>> (std::tuple<Args&...> val);

protected:
    const char *_begin, *_end, *_current_pos;
};

/// messagepack map reader
class mp_map_reader : public mp_reader
{
public:
    mp_map_reader(const mp_map_reader &) = default;
    /// Return reader for a value with specified key.
    /// Current parsing position stays unchanged. Throw if key is not found.
    mp_reader operator[](int64_t key) const;
    /// Return reader for a value with specified key.
    /// Current parsing position stays unchanged. Returns empty reader if key is not found.
    mp_reader find(int64_t key) const;
    /// The map's cardinality.
    size_t cardinality() const noexcept;
private:
    friend class mp_reader;
    mp_map_reader(const char *begin, const char *end, size_t cardinality);
    size_t _cardinality;
};

/// messagepack array reader
class mp_array_reader : public mp_reader
{
public:
    mp_array_reader(const mp_array_reader &) = default;
    /// Return reader for a value with specified index.
    /// Current parsing position stays unchanged. Throw if index out of bounds.
    mp_reader operator[](size_t ind) const;
    /// The array's cardinality.
    size_t cardinality() const noexcept;

    using mp_reader::operator>>;
    template <typename T>
    mp_array_reader& operator>> (std::optional<T> &val)
    {
        // try to interpret out of bounds items as trailing tuple fields with NULL values
        if (_current_pos >= _end)
            val = std::nullopt;
        else
            mp_reader::operator>>(val);
        return *this;
    }
private:
    friend class mp_reader;
    mp_array_reader(const char *begin, const char *end, size_t cardinality);
    size_t _cardinality;
};

template <typename T>
mp_reader& mp_reader::operator>> (std::vector<T> &val)
{
    mp_array_reader arr = array();
    val.resize(arr.cardinality());
    for (size_t i = 0; i < val.size(); ++i)
        arr >> val[i];
    return *this;
}

template <typename KeyT, typename ValueT>
mp_reader& mp_reader::operator>> (std::map<KeyT, ValueT> &val)
{
    mp_map_reader mp_map = map();
    for (size_t i = 0; i < mp_map.cardinality(); ++i)
    {
        KeyT k;
        ValueT v;
        mp_map >> k >> v;
        val[k] = std::move(v);
    }
    return *this;
}

template <typename... Args>
mp_reader& mp_reader::operator>> (std::tuple<Args...> &val)
{
    mp_array_reader arr = array();
    std::apply(
        [arr](const auto&... item)
        {
            ((arr >> item), ...);
        },
        val
    );
    return *this;
}

template <typename... Args>
mp_reader& mp_reader::operator>> (std::tuple<Args&...> val)
{
    mp_array_reader arr = array();
    std::apply(
        [arr](const auto&... item)
        {
            ((arr >> item), ...);
        },
        val
    );
    return *this;
}

#endif // MP_READER_H
