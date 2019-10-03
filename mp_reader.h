#ifndef MP_READER_H
#define MP_READER_H

#include <cstddef>
#include <stdexcept>
#include <optional>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "msgpuck/msgpuck.h"

class wtf_buffer;
class mp_map_reader;
class mp_array_reader;
class mp_reader;

template <typename> struct is_tuple: std::false_type {};
template <typename ...T> struct is_tuple<std::tuple<T...>>: std::true_type {};

std::string hex_dump(const char *begin, const char *end, const char *pos = nullptr);

/// messagepack parsing error
class mp_reader_error : public std::runtime_error
{
public:
    explicit mp_reader_error(const std::string &msg, const mp_reader &reader);
};

std::string mpuck_type_name(mp_type type);

/// messagepack reader
class mp_reader
{
public:
    mp_reader(const wtf_buffer &buf);
    mp_reader(const char *begin = nullptr, const char *end = nullptr);
    const char* begin() const noexcept;
    const char* end() const noexcept;
    const char* pos() const noexcept;
    /// Skip current encoded item (in case of array/map skips all its elements).
    void fast_skip();
    /// Skip current encoded item (in case of array/map skips all its elements) and check content.
    void skip();
    /// Skip current encoded item, verify its type and check content.
    void skip(mp_type type, bool nullable = false);
    /// Return current encoded map within separate reader and move current position to next item.
    mp_map_reader map();
    /// Return current encoded array within separate reader and move current position to next item.
    mp_array_reader array();
    /// Return current encoded iproto message (header + body) within separate reader
    /// and move current position to next item.
    mp_reader iproto_message();
    /// Extract and serialize value to string (nil -> 'null') and move current position to next item.
    std::string to_string();
    /// Reset current reading position back to the beginning.
    void rewind() noexcept;
    /// Return true if current value is nil.
    bool is_null() const;

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
        *this >> non_opt;
        val = std::move(non_opt);
        return *this;
    }

    // use external operator overload for 128 bit integers
    template <typename T, typename = std::enable_if_t<std::is_integral_v<T> && sizeof(T) < 16>>
    mp_reader& operator>> (T &val)
    {
        if (_current_pos >= _end)
            throw mp_reader_error("read out of bounds", *this);

        auto type = mp_typeof(*_current_pos);
        if constexpr (std::is_same_v<T, bool>)
        {
            if (type == MP_BOOL)
            {
                val = mp_decode_bool(&_current_pos);
                return *this;
            }
            throw mp_reader_error("boolean expected, got " + mpuck_type_name(type), *this);
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
                throw mp_reader_error("integer expected, got " + mpuck_type_name(type), *this);
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

    template <typename T>
    T value()
    {
        T res;
        *this >> res;
        return res;
    }

    template <typename... Args>
    mp_reader& values(Args&... args)
    {
        ((*this) >> ... >> args);
        return *this;
    }

    template <typename T>
    bool equals(const T &val) const
    {
        if (_current_pos >= _end)
            throw mp_reader_error("read out of bounds", *this);

        auto begin = _current_pos;
        auto end = begin;
        mp_next(&end);
        mp_reader tmp(begin, end);

        auto type = mp_typeof(*_current_pos);
        if constexpr (std::is_same_v<T, bool>)
            return type == MP_BOOL && val == tmp.value<T>();

        else if constexpr (std::is_enum_v<T> || (std::is_integral_v<T> && sizeof(T) < 16))
        {
            if (type == MP_UINT && val >= 0)
            {
                uint64_t res = mp_decode_uint(&begin);
                return res == static_cast<uint64_t>(val);
            }
            else if (type == MP_INT)
            {
                int64_t res = mp_decode_int(&begin);
                if (res < 0 && val < 0)
                    return res == val;
                if (res >= 0 && val >= 0)
                    return static_cast<uint64_t>(res) == static_cast<uint64_t>(val);
            }
        }
        else if constexpr (std::is_same_v<T, std::string_view>)
        {
            if (val.data() == nullptr)
                return type == MP_NIL;
            else if (type == MP_STR)
                return val == tmp.value<std::string_view>();
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            if (type == MP_STR)
                return val == tmp.value<std::string_view>();
        }
        else if constexpr (std::is_same_v<typename std::decay_t<T>, char *>)
        {
            if (val == nullptr)
                return type == MP_NIL;
            else if (type == MP_STR)
                return val == tmp.value<std::string_view>();
        }
        else
        {
            throw mp_reader_error("unsupported key type to find within map", *this);
        }

        return false;
    }

protected:
    const char *_begin, *_end, *_current_pos;
};

/// messagepack map reader
class mp_map_reader : public mp_reader
{
public:
    mp_map_reader() = default;
    /// Return reader for the value with a specified key.
    /// Current parsing position stays unchanged. Throws if the key is not found.
    template <typename T>
    mp_reader operator[](const T &key) const
    {
        mp_reader res = find(key);
        if (!res)
            throw mp_reader_error("key not found", *this);
        return res;
    }
    /// Return reader for the value with a specified key.
    /// Current parsing position stays unchanged. Returns empty reader if the key is not found.
    template <typename T>
    mp_reader find(const T &key) const
    {
        mp_reader tmp(_begin, _end); // to keep it const
        auto n = _cardinality;
        while (n-- > 0)
        {
            bool found = tmp.equals(key);
            tmp.fast_skip(); // skip a key

            auto value_begin = tmp.pos();
            tmp.fast_skip();
            auto value_end = tmp.pos();

            if (found)
                return {value_begin, value_end};
        }
        return {nullptr, nullptr};
    }

    /// The map's cardinality.
    size_t cardinality() const noexcept;

private:
    friend class mp_reader;
    mp_map_reader(const char *begin, const char *end, size_t cardinality);
    size_t _cardinality = 0;
};

/// messagepack array reader
class mp_array_reader : public mp_reader
{
public:
    mp_array_reader() = default;
    /// Return reader for a value with specified index.
    /// Current parsing position stays unchanged. Throws if specified index out of bounds.
    mp_reader operator[](size_t ind) const;
    /// The array's cardinality.
    size_t cardinality() const noexcept;

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

    //  Do not do it this way!
    //  It breaks out of bounds reading logic (successful for optionals) by switching to mp_reader.
    //  We need to preserve mp_array_reader type after every reading operation.
    //using mp_reader::operator>>;

    template <typename T, typename = std::enable_if_t<
                 (std::is_integral_v<T> && (sizeof(T) < 16)) ||
                 std::is_same_v<T, std::string> ||
                 std::is_same_v<T, std::string_view> ||
                 std::is_same_v<T, std::vector<T>> ||
                 is_tuple<T>::value
                 >>
    mp_array_reader& operator>> (T &val)
    {
        mp_reader::operator>>(val);
        return *this;
    }

    template <typename KeyT, typename ValueT>
    mp_array_reader& operator>> (std::map<KeyT, ValueT> &val)
    {
        mp_reader::operator>>(val);
        return *this;
    }

    template <typename... Args>
    mp_array_reader& values(Args&... args)
    {
        ((*this) >> ... >> args);
        return *this;
    }

private:
    friend class mp_reader;
    mp_array_reader(const char *begin, const char *end, size_t cardinality);
    size_t _cardinality = 0;
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
        [&arr](auto&... item)
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
        [&arr](auto&... item)
        {
            ((arr >> item), ...);
        },
        val
    );
    return *this;
}

#endif // MP_READER_H
