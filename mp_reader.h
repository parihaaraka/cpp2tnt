#ifndef MP_READER_H
#define MP_READER_H

#include <cstddef>
#include <stdexcept>
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

class mp_reader
{
public:
    mp_reader(const wtf_buffer &buf);
    mp_reader(const char *begin, const char *end);
    mp_reader(const mp_reader &) = default;
    const char* begin() const noexcept;
    const char* end() const noexcept;
    void skip();
    void skip(mp_type type);
    mp_map_reader map();
    mp_array_reader array();
    mp_reader iproto_message();
    std::string_view to_string();

    operator bool() const noexcept;
    template <typename T>
    mp_reader& operator>> (T &val)
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

protected:
    const char *_begin, *_end, *_current_pos;
};

class mp_map_reader : public mp_reader
{
public:
    mp_map_reader(const mp_map_reader &) = default;
    mp_reader operator[](int64_t key) const;
    mp_reader find(int64_t key) const;
    size_t size() const noexcept;
private:
    friend class mp_reader;
    mp_map_reader(const char *begin, const char *end, size_t size);
    size_t _size;
};

class mp_array_reader : public mp_reader
{
public:
    mp_array_reader(const mp_array_reader &) = default;
    mp_reader operator[](size_t ind) const;
    size_t size() const noexcept;
private:
    friend class mp_reader;
    mp_array_reader(const char *begin, const char *end, size_t size);
    size_t _size;
};

#endif // MP_READER_H
