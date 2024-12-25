#ifndef MP_READER_H
#define MP_READER_H

#include <cstddef>
#include <stdexcept>
#include <optional>
#include <vector>
#include <map>
#include <cmath>
#include "msgpuck/msgpuck.h"

#if defined _WIN32 || defined __CYGWIN__
  #ifdef BUILDING_DLL
    #ifdef __GNUC__
      #define DLL_PUBLIC __attribute__ ((dllexport))
    #else
      #define DLL_PUBLIC __declspec(dllexport)
    #endif
  #else
    #ifdef __GNUC__
      #define DLL_PUBLIC __attribute__ ((dllimport))
    #else
      #define DLL_PUBLIC __declspec(dllimport)
    #endif
  #endif
  #define DLL_LOCAL
#else
  #if __GNUC__ >= 4
    #define DLL_PUBLIC __attribute__ ((visibility ("default")))
    #define DLL_LOCAL  __attribute__ ((visibility ("hidden")))
  #else
    #define DLL_PUBLIC
    #define DLL_LOCAL
  #endif
#endif

class wtf_buffer;
class mp_map_reader;
class mp_array_reader;
class mp_reader;

std::string hex_dump(const char *begin, const char *end, const char *pos = nullptr);

/// messagepack parsing error
class DLL_PUBLIC mp_reader_error : public std::runtime_error
{
public:
    explicit mp_reader_error(const std::string &msg, const mp_reader &reader, const char *pos = nullptr);
};

#undef DLL_PUBLIC
#undef DLL_LOCAL

std::string mpuck_type_name(mp_type type);

/// messagepack reader
class mp_reader
{
public:
    mp_reader(const wtf_buffer &buf);
    mp_reader(const std::vector<char> &buf);
    mp_reader(const char *begin = nullptr, const char *end = nullptr);

    template <std::size_t N = 1>
    class none_t {
    private:
        none_t() = default;
    public:
        static none_t& instance()
        {
            static none_t tmp; return tmp;
        }
    };

    template <std::size_t N = 1>
    static none_t<N>& none()
    {
        return none_t<N>::instance();
    }

    inline const char* begin() const noexcept
    {
        return _begin;
    }

    inline const char* end() const noexcept
    {
        return _end;
    }

    inline const char* pos() const noexcept
    {
        return _current_pos;
    }

    /// Skip current encoded item (in case of array/map skips all its elements) and check content.
    inline void skip()
    {
        skip(&_current_pos);
    }

    /// Skip MsgPack item pointed by pos (within current buffer)
    void skip(const char **pos) const;
    /// Skip current encoded item and ensure it has expected type.
    void skip(mp_type type, bool nullable = false);

    /// Interpret any item as an array via mp_array_reader (?!).
    /// Bad try to simpify things... You'd better don't use it :)
    mp_array_reader as_array(size_t ind = 0) const;

    /// Return current encoded iproto message (header + body) within separate reader
    /// and move current position to next item.
    mp_reader iproto_message();

    /// Extract and serialize value to string (nil -> 'null') and move current position to next item.
    std::string to_string();

    /// Validate MsgPack within current buffer (all items)
    void check() const;

    /// Reset current reading position back to the beginning.
    inline void rewind() noexcept
    {
        _current_pos = _begin;
    }

    /// Return true if current value is nil.
    inline bool is_null() const
    {
        return mp_typeof(*_current_pos) == MP_NIL;
    }

    /// true if msgpack has more values to read
    inline bool has_next() const noexcept
    {
        return _current_pos && _end && _current_pos < _end;
    }

    /// true if not empty
    inline operator bool() const noexcept
    {
        return _begin && _end && _end > _begin;
    }

    /// Return reader for a value with specified index.
    /// Current parsing position stays unchanged. Throws if specified index out of bounds.
    mp_reader operator[](size_t ind) const;

    mp_reader& operator>> (std::string &val);
    mp_reader& operator>> (std::string_view &val);

    /// Use >> mp_reader::none() to skip a value or mp_reader::none<N>() to skip N items
    template<size_t N = 1>
    mp_reader& operator>> (none_t<N>&)
    {
        for (int i = 0; i < N; ++i)
            skip();
        return *this;
    }

    template <typename T>
    mp_reader& operator>> (std::optional<T> &val)
    {
        if (_current_pos >= _end)
        {
            val = std::nullopt;
        }
        else if (mp_typeof(*_current_pos) == MP_NIL)
        {
            skip();
            val = std::nullopt;
        }
        else
        {
            T non_opt;
            *this >> non_opt;
            val = std::move(non_opt);
        }
        return *this;
    }

    // use external operator overload for 128 bit integers
    template <typename T, typename = std::enable_if_t<
                  (std::is_integral_v<T> && sizeof(T) < 16) ||
                   std::is_floating_point_v<T>
                   >>
    mp_reader& operator>> (T &val)
    {
        const char *data = _current_pos;
        const char *prev_pos = _current_pos;
        skip();

        auto type = mp_typeof(*data);
        if constexpr (std::is_same_v<T, bool>)
        {
            if (type == MP_BOOL)
            {
                val = mp_decode_bool(&data);
                return *this;
            }
            throw mp_reader_error("boolean expected, got " + mpuck_type_name(type), *this, prev_pos);
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            if (type == MP_FLOAT)
            {
                val = mp_decode_float(&data);
                return *this;
            }
            else if (type == MP_DOUBLE)
            {
                double res = mp_decode_double(&data);
                if constexpr (std::is_same_v<T, float>)
                {
                    if (std::isfinite(res) && (res > std::numeric_limits<T>::max() || res < std::numeric_limits<T>::min()))
                        throw mp_reader_error("value overflow", *this);
                }
                val = static_cast<T>(res);
                return *this;
            }
            throw mp_reader_error("float expected, got " + mpuck_type_name(type), *this, prev_pos);
        }
        else
        {
            if (type == MP_UINT)
            {
                uint64_t res = mp_decode_uint(&data);
                if (res <= std::numeric_limits<T>::max())
                {
                    val = static_cast<T>(res);
                    return *this;
                }
            }
            else if (type == MP_INT)
            {
                int64_t res = mp_decode_int(&data);
                if (res <= std::numeric_limits<T>::max() && res >= std::numeric_limits<T>::min())
                {
                    val = static_cast<T>(res);
                    return *this;
                }
            }
            else
            {
                throw mp_reader_error("integer expected, got " + mpuck_type_name(type), *this, prev_pos);
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

    mp_reader& operator>> (mp_reader &val)  = delete;

    mp_reader& operator>> (mp_map_reader &val);

    mp_reader& operator>> (mp_array_reader &val);

    template <typename T>
    T read()
    {
        T res;
        *this >> res;
        return res;
    }

    template <typename T>
    T read_or(T &&def)
    {
        if (_current_pos >= _end)
        {
            return def;
        }
        else if (mp_typeof(*_current_pos) == MP_NIL)
        {
            skip();
            return def;
        }

        return read<T>();
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
        auto begin = _current_pos;
        auto end = begin;
        skip(&end);
        mp_reader tmp(begin, end);

        auto type = mp_typeof(*_current_pos);
        if constexpr (std::is_same_v<T, bool>)
        {
            return type == MP_BOOL && val == tmp.read<T>();
        }
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
        else if constexpr (std::is_floating_point_v<T>)
        {
            //if (isnan(val))
            //    return type == MP_NIL;
            if (type == MP_FLOAT)
                return val == mp_decode_float(&begin);
            if (type == MP_DOUBLE)
                return val == mp_decode_double(&begin);
        }
        else if constexpr (std::is_same_v<T, std::string_view>)
        {
            if (val.data() == nullptr)
                return type == MP_NIL;
            else if (type == MP_STR)
                return val == tmp.read<std::string_view>();
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            if (type == MP_STR)
                return val == tmp.read<std::string_view>();
        }
        else if constexpr (std::is_same_v<typename std::decay_t<T>, char *>)
        {
            if (val == nullptr)
                return type == MP_NIL;
            else if (type == MP_STR)
                return val == tmp.read<std::string_view>();
        }
        else
        {
            throw mp_reader_error("unsupported value type to compare with", *this);
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
            tmp.skip(); // skip a key

            auto value_begin = tmp.pos();
            tmp.skip();
            auto value_end = tmp.pos();

            if (found)
                return {value_begin, value_end};
        }
        return {nullptr, nullptr};
    }

    /// The map's cardinality.
    inline size_t cardinality() const noexcept
    {
        return _cardinality;
    }

private:
    friend class mp_reader;
    mp_map_reader(const char *begin, const char *end, size_t cardinality);
    size_t _cardinality = 0;
};

/// messagepack array reader
class mp_array_reader : public mp_reader
{
public:
    mp_array_reader(const wtf_buffer &buf);
    mp_array_reader(const char *begin, const char *end);
    mp_array_reader() = default;

    /// The array's cardinality.
    inline size_t cardinality() const noexcept
    {
        return _cardinality;
    }

    using mp_reader::operator>>;

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
    mp_array_reader arr = read<mp_array_reader>();
    val.resize(arr.cardinality());
    for (size_t i = 0; i < val.size(); ++i)
        arr >> val[i];
    return *this;
}

template <typename KeyT, typename ValueT>
mp_reader& mp_reader::operator>> (std::map<KeyT, ValueT> &val)
{
    mp_map_reader mp_map = read<mp_map_reader>();
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
    mp_array_reader arr = read<mp_array_reader>();
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
    mp_array_reader arr = read<mp_array_reader>();
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
