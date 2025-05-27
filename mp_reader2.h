#pragma once

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <optional>
#include <vector>
#include <map>
#include <cmath>
#include <charconv>
#include "msgpuck/ext_tnt.h"
#include "msgpuck/msgpuck.h"
#include "wtf_buffer.h"
#include "misc.h"

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
class mp_reader_error;
struct mp_plain;

/// Initialize mp_reader environment. It overrides ext types print functions for now.
inline void mp_initialize()
{
    mp_snprint_ext = mp_snprint_ext_tnt;
    mp_fprint_ext = mp_fprint_ext_tnt;
}

inline std::string mpuck_type_name(mp_type type)
{
    switch (type)
    {
    case MP_NIL:
        return "nil";
    case MP_UINT:
        return "uint";
    case MP_INT:
        return "int";
    case MP_STR:
        return "string";
    case MP_BIN:
        return "bin";
    case MP_ARRAY:
        return "array";
    case MP_MAP:
        return "map";
    case MP_BOOL:
        return "bool";
    case MP_FLOAT:
        return "float";
    case MP_DOUBLE:
        return "double";
    case MP_EXT:
        return "ext";
    }
    return "MPUCK:" + std::to_string(static_cast<int>(type));
}

template <typename MP>
std::string hex_dump_mp(const MP &mp, const char *pos);

struct mp_plain
{
    inline mp_plain(const wtf_buffer &buf) : mp_plain(buf.data(), buf.end) {}
    inline mp_plain(const std::vector<char> &buf)
        : mp_plain(buf.data(), buf.data() + buf.size()){}
    inline mp_plain(const char *begin = nullptr, const char *end = nullptr)
    {
        this->begin = begin;
        this->end = end;
    }

    /// true if not empty
    inline operator bool() const noexcept
    {
        if (end)
            return begin && end > begin;
        return false;
    }

    const char *begin = nullptr;
    const char *end = nullptr;
};

/// msgpack parsing error
class DLL_PUBLIC mp_reader_error : public std::runtime_error
{
public:
    inline mp_reader_error(const std::string &msg, const mp_plain &reader, const char *pos = nullptr)
        : runtime_error(msg + '\n' + hex_dump_mp(reader, pos)) {}
};

#undef DLL_PUBLIC
#undef DLL_LOCAL

struct mp_array : public mp_plain
{
    mp_array() = default;
    inline mp_array(const wtf_buffer &buf) : mp_array(buf.data(), buf.end) {};
    inline mp_array(const char *begin, const char *end = nullptr) : mp_plain(begin, end)
    {
        if (!begin)
            return;

        auto head = begin;
        auto type = mp_typeof(*head);
        if (type == MP_ARRAY)
        {
            cardinality = mp_decode_array(&head);
        }
        else if (type == MP_EXT)
        {
            int8_t ext_type = 0;
            mp_decode_extl(&head, &ext_type);
            if (ext_type == MP_ERROR) // extract error stack
            {
                // https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-error-type
                // acquire key 0 value from the topmost map
                uint32_t i = mp_decode_map(&head);
                while (i--)
                {
                    uint32_t key = mp_decode_uint(&head);
                    auto value_pos = head;
                    if (key == 0x00) // stack
                    {
                        cardinality = mp_decode_array(&head);
                        mp_next(&value_pos);
                        this->end = value_pos;
                        this->begin = head;
                        return;
                    }
                }
                throw mp_reader_error("MP_ERROR_STACK not found within ext error", *this, head);
            }
            else
            {
                throw mp_reader_error("unable to read map from ext type " + std::to_string(type), *this, head);
            }
        }
        else
        {
            throw mp_reader_error("array expected, got " + mpuck_type_name(type), *this, head);
        }
        this->begin = head;
    }
    /// Interpret `begin` as the first item of the array with the specified cardinality.
    inline mp_array(const char *begin, size_t cardinality) : mp_plain(begin), cardinality(cardinality){}
    /// Interpret `begin` as the first item of the array with the specified cardinality.
    inline mp_array(const char *begin, const char *end, size_t cardinality) : mp_plain(begin, end), cardinality(cardinality){}

    /// true if not empty
    inline operator bool() const noexcept
    {
        if (end)
            return begin && end > begin;
        return begin && cardinality;
    }

    /// Return mp_plain for a value with the specified index.
    mp_plain operator[](size_t i) const
    {
        auto pos = begin;
        const char *prev_pos = pos;
        while (i-- >= 0)
        {
            prev_pos = pos;
            mp_next(&pos);
        }
        return {prev_pos, pos};
    }

    size_t cardinality = 0;
};

struct mp_map : public mp_plain
{
    mp_map() = default;
    inline mp_map(const char *begin, const char *end = nullptr) : mp_plain(begin, end)
    {
        auto head = begin;
        auto type = mp_typeof(*head);
        if (type == MP_MAP)
        {
            cardinality = mp_decode_map(&head);
        }
        else if (type == MP_EXT)
        {
            int8_t ext_type = 0;
            mp_decode_extl(&head, &ext_type);
            if (ext_type == MP_INTERVAL)
            {
                // https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-interval-type
                cardinality = mp_decode_uint(&head);
            }
            else if (ext_type == MP_ERROR)  // acquire error container (map with key 0x31 (PROTO_ERROR_24) and optional 0x52 (IPROTO_ERROR))
            {
                cardinality = mp_decode_map(&head);
            }
            else
            {
                throw mp_reader_error("unable to read map from ext type " + std::to_string(type), *this, head);
            }
        }
        else
        {
            throw mp_reader_error("map expected, got " + mpuck_type_name(type), *this, head);
        }
        this->begin = head;
    }
    // Interpret `begin` as the first pair of the map with the specified cardinality (items*2).
    inline mp_map(const char *begin, size_t cardinality) : mp_plain(begin), cardinality(cardinality){}
    inline mp_map(const char *begin, const char *end, size_t cardinality) : mp_plain(begin, end), cardinality(cardinality){}

    /// true if not empty
    inline operator bool() const noexcept
    {
        if (end)
            return begin && end > begin;
        return begin && cardinality;
    }

    /// Return mp_plain for a value with the specified key.
    /// Returns mp_plain reader if the key is not found.
    template <typename T>
    mp_plain find(const T &key) const;

    /// Return mp_plain for a value with the specified key.
    /// Throws if the key is not found.
    template <typename T>
    mp_plain operator[](const T &key) const
    {
        mp_plain res = find(key);
        if (!res)
            throw mp_reader_error("key not found", *this);
        return res;
    }

    size_t cardinality = 0;
};

template <size_t maxN>
struct mp_span : public mp_array
{
    template<typename T>
    friend class mp_reader;

    mp_span() = default;
    mp_span(const char *begin, const char *end);
    mp_span sub(size_t first_ind, size_t last_ind) const
    {
        if (first_ind > last_ind)
            throw mp_reader_error("first_ind > last_ind", *this);
        if (last_ind >= cardinality)
            throw mp_reader_error("read out of bounds", *this);
        return mp_span<maxN>(begin + (first_ind ? rbounds[first_ind - 1] : 0),
                             begin + rbounds[last_ind],
                             last_ind - first_ind + 1,
                             &rbounds[first_ind]);
    }

    /// Return mp_plain for a value with the specified index.
    /// Throws if the key is not found.
    mp_plain operator[](size_t i) const
    {
        return sub(i, i);
    }

    template <size_t maxArgN>
    bool operator== (const mp_span<maxArgN>& s) const
    {
        if (*this && s && (end - begin) == (s.end - s.begin))
            return memcmp(begin, s.begin(), s.end - s.begin) == 0;
        return false;
    }

protected:
    std::array<uint32_t, maxN> rbounds;
    mp_span(const char *begin, const char *end, size_t cardinality, const uint32_t *first_rbound)
        : mp_array(begin, end, cardinality)
    {
        std::copy(first_rbound, first_rbound + cardinality, (uint32_t*)rbounds.data());
    };
};

template <std::size_t N = 1>
struct mp_none {};

template <typename T>
struct mp_optional
{
    mp_optional(T &dst, const T &def) : dst(dst), def(def) {}
    T &dst;
    const T &def;
};

/// messagepack reader
template<typename MP = mp_plain>
class mp_reader
{
    template<typename T>
    friend class mp_reader;

    static_assert(std::derived_from<MP, mp_plain>, "mp_reader operates on mp_plain and its descendants");
protected:
    MP _mp;
    const char *_current_pos = nullptr;
    uint32_t _current_ind = 0;

public:
    mp_reader(MP mp) : _mp(mp), _current_pos(_mp.begin) {}

    mp_reader(const wtf_buffer &buf) : mp_reader(buf.data(), buf.end) {};
    mp_reader(const std::vector<char> &buf) : mp_reader(buf.data(), buf.data() + buf.size()) {};

    template<typename ...Args>
    mp_reader(Args... args) : _mp(args ...), _current_pos(_mp.begin)
    {
    }

    MP content() const noexcept
    {
        return _mp;
    }

    const char* begin() const noexcept
    {
        return _mp.begin;
    }

    const char* end() const noexcept
    {
        return _mp.end;
    }

    size_t size() const
    {
        return _mp.end - _mp.begin;
    }

    const char* pos() const noexcept
    {
        return _current_pos;
    }

    uint32_t ind() const noexcept
    {
        return _current_ind;
    }

    /// Cardinality of the array or map.
    size_t cardinality() const noexcept
        requires (std::is_same<MP, mp_array>::value || std::is_same<MP, mp_map>::value)
    {
        return _mp.cardinality;
    }

    /// Skip current encoded item (in case of array/map skips all its elements).
    mp_reader& skip()
    {
        if (!_current_pos || (_mp.end && _current_pos >= _mp.end))
            throw mp_reader_error("read out of bounds", _mp, _mp.end);

        if constexpr (requires {_mp.cardinality;})
        {
            size_t c = _mp.cardinality;
            if constexpr (std::is_same<MP, mp_map>::value)
                c = c * 2;
            if (_current_ind >= c)
                throw mp_reader_error("read out of bounds", _mp, _current_pos);
        }

        const char *prev = _current_pos;
        mp_next(&_current_pos);
        if (_mp.end && _current_pos > _mp.end)
        {
            _current_pos = prev;
            throw mp_reader_error("invalid messagepack", _mp, prev);
        }
        ++_current_ind;
        return *this;
    }

    /// Skip current encoded item and ensure it has expected type.
    mp_reader& skip(mp_type type, bool nullable = false)
    {
        auto actual_type = mp_typeof(*_current_pos);
        if (actual_type != type && (!nullable || actual_type != MP_NIL))
            throw mp_reader_error(mpuck_type_name(type) + " expected, got " + mpuck_type_name(actual_type), _mp, _current_pos);
        return skip();
    }

    /// Return current encoded iproto message (header + body) within separate reader
    /// and move current position to next item.
    mp_reader iproto_message()
    {
        if (_mp.end - _current_pos < 5)
            return {_current_pos, _current_pos}; // empty object

        if (static_cast<uint8_t>(*_current_pos) != 0xce)
            throw mp_reader_error("invalid iproto packet", _mp);

        uint64_t response_size = mp_decode_uint(&_current_pos);
        if (static_cast<uint64_t>(_mp.end - _current_pos) < response_size)
            throw mp_reader_error("partial iproto packet", _mp);

        auto head = _current_pos;
        _current_pos += response_size;
        return mp_reader{mp_plain{head, _current_pos}};
    }

    /// Extract and serialize value to string (nil -> 'null') and move current position to next item.
    std::string to_string(uint32_t flags = 0)
    {
        const char *data = _current_pos;
        skip();

        std::string res(256, '\0');
        int cnt = mp_snprint(res.data(), static_cast<int>(res.size()), data, flags);
        if (cnt >= static_cast<int>(res.size()))
        {
            res.resize(static_cast<size_t>(cnt + 1));
            cnt = mp_snprint(res.data(), static_cast<int>(res.size()), data, flags);
        }
        if (cnt < 0)
            throw mp_reader_error("mp_snprint error", _mp, data);
        res.resize(static_cast<size_t>(cnt));
        return res;
    }

    /// Validate next MsgPack item and check right bound if known
    inline void check_next() const
    {
        if (!_current_pos)
            return;
        if (!_mp.end)
            throw mp_reader_error("right bound is not specified", _mp);

        if (_current_pos >= _mp.end)
            throw mp_reader_error("read out of bounds", _mp, _current_pos);
        auto pos = _current_pos;
        if (mp_check(&pos, _mp.end))
            throw mp_reader_error("invalid messagepack", _mp, _current_pos);
    }

    /// Validate MsgPack within current buffer (all items)
    void check() const
    {
        if (!_mp.begin)
            return;
        if (!_mp.end)
            throw mp_reader_error("right bound is not specified", *this);
        auto pos = _mp.begin;
        while (pos < _mp.end)
        {
            const char *prev = pos;
            if (mp_check(&pos, _mp.end))
                throw mp_reader_error("invalid messagepack", *this, prev);
        }
    }

    /// Reset the current reading position back to the beginning.
    void rewind() noexcept
    {
        _current_pos = _mp.begin;
        _current_ind = 0;
    }

    /// Return true if the current value is `nil`.
    bool is_null() const
    {
        if (!has_next())
            throw mp_reader_error("read out of bounds", _mp, _current_pos);

        return mp_typeof(*_current_pos) == MP_NIL;
    }

    /// Returns `true` if msgpack has more values to read. Pass `true` as an argument to throw an exception
    /// when the possibility is unknown.
    bool has_next(bool strict = false) const
    {
        if (_mp.end)
            return _current_pos && _current_pos < _mp.end;
        if constexpr (requires {_mp.cardinality;})
        {
            if constexpr (std::is_same<MP, mp_map>::value)
                return _current_pos && _current_ind < _mp.cardinality * 2;
            else
                return _current_pos && _current_ind < _mp.cardinality;
        }

        // allow to read at one's risk
        if (!strict)
            return true;
        throw mp_reader_error("unable to determine if next value exists - no right bound specified", _mp, _current_pos);
    }

    /// true if not empty
    operator bool() const noexcept
    {
        return _mp;
        /*if (_mp.end)
            return _mp.begin && _mp.end > _mp.begin;
        if constexpr (requires {_mp.cardinality;})
            return _mp.begin && _mp.cardinality;
        return false;*/
    }

    /// Returns reader for a value with the specified index.
    /// Current parsing position stays unchanged. Throws if the specified index is out of bounds.
    mp_reader<mp_plain> operator[](size_t ind) const
        requires (!std::is_same<MP, mp_map>::value)
    {
        if constexpr (requires {_mp.rbounds;})
            return {_mp[ind]};

        size_t i = 0;
        auto tmp = *this;
        if (_current_ind <= ind)
            i = _current_ind;
        else
            tmp.rewind();

        for (; i < ind; ++i)
            tmp.skip();

        auto begin = tmp._current_pos;
        tmp.skip();
        return {mp_plain{begin, tmp._current_pos}};
    }

    /// Return `mp_reader<mp_plain>` for a value with the specified key.
    /// Current parsing position stays unchanged. Throws if the key is not found.
    template <typename T>
    mp_reader<mp_plain> operator[](const T &key) const
        requires (std::is_same<MP, mp_map>::value)
    {
        return {_mp[key]};
    }

    template <typename T>
    mp_reader<mp_plain> find(const T &key) const
        requires (std::is_same<MP, mp_map>::value)
    {
        return {_mp.find(key)};
    }

    mp_reader& operator>> (std::string &val)
    {
        if (mp_typeof(*_current_pos) == MP_EXT)
        {
            // regular print
            if (val.size() < 128)
                val.resize(128, '\0');
            size_t len = mp_snprint(val.data(), val.size(), _current_pos, UNQUOTE_UUID);
            if (len > val.size())
            {
                val.resize(len + 1, '\0');
                len = mp_snprint(val.data(), len + 1, _current_pos, UNQUOTE_UUID);
            }
            if (len < 0)
                throw mp_reader_error("bad ext value content", _mp, _current_pos);
            val.resize(len);
            skip();
        }
        else
        {
            std::string_view tmp;
            *this >> tmp;
            if (!tmp.data())
                throw mp_reader_error("string expected, got no data", _mp);
            val.assign(tmp.data(), tmp.size());
        }
        return *this;
    }

    mp_reader& operator>> (std::string_view &val)
    {
        // for the sake of convenience
        if (!has_next())
        {
            val = {};
            return *this;
        }

        auto type = mp_typeof(*_current_pos);
        if (type == MP_STR)
        {
            const char *prev = _current_pos;
            uint32_t len = 0;
            skip();
            const char *value = mp_decode_str(&prev, &len);
            val = {value, len};
        }
        else if (type == MP_NIL)
        {
            skip();
            val = {}; // data() == nullptr
        }
        else
        {
            throw mp_reader_error("string expected, got " + mpuck_type_name(type), _mp, _current_pos);
        }
        return *this;
    }

    mp_reader& operator>> (bool &val)
    {
        const char *data = _current_pos;
        auto type = mp_typeof(*data);
        if (type != MP_BOOL)
            throw mp_reader_error("boolean expected, got " + mpuck_type_name(type), _mp, _current_pos);

        val = mp_decode_bool(&data);
        skip();
        return *this;
    }

    /// Use `>> mp_none()` to skip a value or `>> mp_none<N>()` to skip N items
    template<size_t N = 1>
    mp_reader& operator>> (mp_none<N>)
    {
        for (int i = 0; i < N; ++i)
            skip();
        return *this;
    }

    template<size_t maxN>
    mp_reader& operator>> (mp_span<maxN> &dst)
    {
        dst.begin = _current_pos;
        for (size_t i = 0; i < maxN && has_next(); ++i)
        {
            skip();
            dst.rbounds[i] = _current_pos - dst.begin;
            ++dst.cardinality;
        }
        dst.end = _current_pos;
        return *this;
    }

    template <typename C, typename D>
    mp_reader& operator>> (std::chrono::time_point<C, D> &val)
    {
        using namespace std::chrono;
        auto type = mp_typeof(*_current_pos);
        switch (type) {
        case MP_UINT:
        case MP_INT:
            val = time_point<C, D>(D(read<int64_t>()));
            break;
        case MP_FLOAT:
        case MP_DOUBLE:
            val = time_point<C, D>(D(read<double>()));
            break;
        case MP_EXT:
        {
            struct tmp {
                int32_t nsec;
                int16_t tzoffset;
                int16_t tzindex;
            } tail = {0, 0, 0};

            int8_t ext_type;
            const char *data = _current_pos;
            uint32_t len = mp_decode_extl(&data, &ext_type);

            if (ext_type != MP_DATETIME)
                throw mp_reader_error("unable to extract time_point from " + mpuck_type_name(type), _mp, data);
            if (len != sizeof(int64_t) && len != sizeof(int64_t) + sizeof(tail))
                throw mp_reader_error("unexpected MP_DATETIME value", _mp, data);

            int64_t epoch;
            memcpy(&epoch, data, sizeof(epoch));
            data += sizeof(epoch);

            if (len != sizeof(int64_t))
                memcpy(&tail, data, sizeof(tail));

            if (tail.nsec)
                val = time_point<C, D>(duration<int64_t, std::nano>(tail.nsec + epoch*1000000000));
            else
                val = time_point<C, D>(duration<int64_t>(epoch));
            skip();
            break;
        }
        default:
            throw mp_reader_error("unable to get time_point from " + mpuck_type_name(type), _mp, _current_pos);
        }

        return *this;
    }

    template <typename T>
    mp_reader& operator>> (std::optional<T> &val)
    {
        if (!has_next(true))
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

    template <typename T>
    mp_reader& operator>> (mp_optional<T> &&val)
    {
        if (!has_next(true))
        {
            val.dst = val.def;
        }
        else if (mp_typeof(*_current_pos) == MP_NIL)
        {
            skip();
            val.dst = val.def;
        }
        else
        {
            *this >> val.dst;
        }
        return *this;
    }

    template <typename T>
    T read_or(const T &def)
    {
        if (!has_next(true))
            return def;

        if (mp_typeof(*_current_pos) == MP_NIL)
        {
            skip();
            return def;
        }

        return read<T>();
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

        auto ext_via_string = [prev_pos, &val, this]() -> mp_reader& {
            char tmp[64];
            auto len = mp_snprint(tmp, 64, prev_pos, 0);
            auto [ptr, ec] = std::from_chars(tmp, tmp + len, val);
            if (ec == std::errc())
                return *this;

            if (ec == std::errc::invalid_argument)
                throw mp_reader_error("not a number", _mp, prev_pos);
            else if (ec == std::errc::result_out_of_range)
                throw mp_reader_error("out of range", _mp, prev_pos);
            throw mp_reader_error("error parsing number", _mp, prev_pos);
        };

        auto type = mp_typeof(*data);
        if constexpr (std::is_floating_point_v<T>)
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
                        throw mp_reader_error("value overflow", _mp);
                }
                val = static_cast<T>(res);
                return *this;
            }
            else if (type == MP_EXT)
            {
                return ext_via_string();
            }
            throw mp_reader_error("float expected, got " + mpuck_type_name(type), _mp, prev_pos);
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
            else if (type == MP_EXT)
            {
                return ext_via_string();
            }
            else
            {
                throw mp_reader_error("integer expected, got " + mpuck_type_name(type), _mp, prev_pos);
            }
            throw mp_reader_error("value overflow", _mp);
        }
    }

    template <typename T>
    mp_reader& operator>> (std::vector<T> &val)
    {
        auto arr = read<mp_reader<mp_array>>();
        val.resize(arr.cardinality());
        for (size_t i = 0; i < val.size(); ++i)
            arr >> val[i];
        return *this;
    }

    template <typename KeyT, typename ValueT>
    mp_reader& operator>> (std::map<KeyT, ValueT> &val)
    {
        auto map = read<mp_reader<mp_map>>();
        for (size_t i = 0; i < map.cardinality(); ++i)
        {
            KeyT k;
            ValueT v;
            map >> k >> v;
            val[k] = std::move(v);
        }
        return *this;
    }

    template <typename... Args>
    mp_reader& operator>> (std::tuple<Args...> &val)
    {
        auto arr = read<mp_reader<mp_array>>();
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
    mp_reader& operator>> (std::tuple<Args&...> val)
    {
        auto arr = read<mp_reader<mp_array>>();
        std::apply(
            [&arr](auto&... item)
            {
                ((arr >> item), ...);
            },
            val
            );
        return *this;
    }

    mp_reader& operator>> (mp_reader<mp_plain> &val)  = delete;

    mp_reader& operator>> (mp_reader<mp_array> &val)
    {
        val._mp = mp_array(_current_pos);
        val._current_pos = val._mp.begin;
        val._current_ind = 0;
        skip();
        if (!val._mp.end)
            val._mp.end = _current_pos;
        return *this;
    }

    mp_reader& operator>> (mp_reader<mp_map> &val)
    {
        val._mp = mp_map(_current_pos);
        val._current_pos = val._mp.begin;
        val._current_ind = 0;
        skip();
        val._mp.end = _current_pos;
        return *this;
    }

    template <typename T>
    T read()
    {
        T res;
        *this >> res;
        return res;
    }

    template <size_t maxN>
    mp_span<maxN> read()
    {
        mp_span<maxN> dst{};
        *this >> dst;
        return dst;
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
        if (!has_next())
            throw mp_reader_error("read out of bounds", _mp);
        auto begin = _current_pos;
        //auto end = begin;
        //mp_next(&end);
        //mp_reader tmp(begin, end);
        mp_reader tmp(_current_pos);

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
            throw mp_reader_error("unsupported value type to compare with", _mp);
        }

        return false;
    }

};

template <size_t maxN>
mp_span<maxN>::mp_span(const char *begin, const char *end) : mp_array(begin, end, 0)
{
    auto r = mp_reader{mp_plain{begin, end}};
    r >> *this;
}

template <typename T>
mp_plain mp_map::find(const T &key) const
{
    auto tmp = mp_reader{mp_plain(begin, end)};
    auto n = cardinality;
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

template <typename MP>
std::string hex_dump_mp(const MP &mp, const char *pos)
{
    if (mp.begin)
    {
        if (mp.end)
            return hex_dump(mp.begin, mp.end, pos);
        if constexpr (requires {mp.cardinality;})
        {
            mp_reader tmp(mp);
            for (size_t i = 0; i < mp.cardinality; ++i)
                tmp.skip();
            return hex_dump(mp.begin, tmp.end, pos);
        }
    }
    return {};
}

using mp_plain_reader = mp_reader<mp_plain>;
using mp_array_reader = mp_reader<mp_array>;
using mp_map_reader = mp_reader<mp_map>;
