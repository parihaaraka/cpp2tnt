#ifndef MP_READER_H
#define MP_READER_H

#include <cstddef>
#include <stdexcept>
#include <optional>
#include "msgpuck/msgpuck.h"

class mp_error : public std::runtime_error
{
public:
    explicit mp_error(const std::string &msg, const char *pos = nullptr);
    const char* pos() const noexcept;
private:
    const char* _pos;
};

class wtf_buffer;
class mp_map_reader;
class mp_array_reader;

/// messagepack reader
class mp_reader
{
public:
    mp_reader(const wtf_buffer &buf);
    mp_reader(const char *begin, const char *end);
    mp_reader(const mp_reader &) = default;
    const char* begin() const noexcept;
    const char* end() const noexcept;
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

    template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    mp_reader& operator>> (T &val)
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            if (mp_typeof(*_current_pos) == MP_BOOL)
            {
                val = mp_decode_bool(&_current_pos);
                return *this;
            }
            throw mp_error("boolean expected", _current_pos);
        }
        else if constexpr (std::is_integral_v<T>)
        {
            if (mp_typeof(*_current_pos) == MP_UINT)
            {
                uint64_t res = mp_decode_uint(&_current_pos);
                if (res <= std::numeric_limits<T>::max())
                {
                    val = static_cast<T>(res);
                    return *this;
                }
            }
            else if (mp_typeof(*_current_pos) == MP_INT)
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
                throw mp_error("integer expected", _current_pos);
            }
            throw mp_error("value overflow", _current_pos);
        }
    }

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
    size_t size() const noexcept;
private:
    friend class mp_reader;
    mp_map_reader(const char *begin, const char *end, size_t size);
    size_t _size;
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
    size_t size() const noexcept;
private:
    friend class mp_reader;
    mp_array_reader(const char *begin, const char *end, size_t size);
    size_t _size;
};

#endif // MP_READER_H
