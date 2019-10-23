#include "mp_reader.h"
#include "wtf_buffer.h"
#include "proto.h"

using namespace std;

std::string hex_dump(const char *begin, const char *end, const char *pos)
{
    std::string res;
    res.reserve(static_cast<size_t>((end - begin)) * 4);
    constexpr char hexmap[] = {"0123456789abcdef"};
    int cnt = 0;
    for (const char* c = begin; c < end; ++c)
    {
        ++cnt;
        char sep = ' ';
        if (pos)
        {
            if (c == pos - 1)
                sep = '>';
            else if (c == pos)
                sep = '<';
        }
        res += hexmap[(*c & 0xF0) >> 4];
        res += hexmap[*c & 0x0F];
        res += sep;
        if (cnt % 16 == 0)
            res += '\n';
        else if (cnt % 8 == 0)
            res += ' ';
    }
    return res;
}

string mpuck_type_name(mp_type type)
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

mp_reader_error::mp_reader_error(const std::string &msg, const mp_reader &reader)
    : runtime_error(msg + '\n' + hex_dump(reader.begin(), reader.end(), reader.pos()))
{
}

// ------------------------------------------------------------------------------------------------

mp_reader::mp_reader(const wtf_buffer &buf) : mp_reader(buf.data(), buf.end)
{
}

mp_reader::mp_reader(const char *begin, const char *end)
{
    _current_pos = _begin = begin;
    _end = end;
}

const char* mp_reader::begin() const noexcept
{
    return _begin;
}

const char* mp_reader::end() const noexcept
{
    return _end;
}

const char* mp_reader::pos() const noexcept
{
    return _current_pos;
}

void mp_reader::fast_skip()
{
    mp_next(&_current_pos);
}

void mp_reader::skip()
{
    // TODO
    // use mp_check/mp_next more selectively (does it make sense?)
    if (mp_check(&_current_pos, _end))
        throw mp_reader_error("invalid messagepack", *this);
}

void mp_reader::skip(mp_type type, bool nullable)
{
    auto actual_type = mp_typeof(*_current_pos);
    if (actual_type != type && (!nullable || actual_type != MP_NIL))
        throw mp_reader_error(mpuck_type_name(type) + " expected, got " + mpuck_type_name(actual_type), *this);
    skip();
}

mp_map_reader mp_reader::map()
{
    auto type = mp_typeof(*_current_pos);
    if (type != MP_MAP)
        throw mp_reader_error("map expected, got " + mpuck_type_name(type), *this);

    auto head = _current_pos;
    if (mp_check(&_current_pos, _end))
        throw mp_reader_error("invalid messagepack", *this);
    auto cardinality = mp_decode_map(&head);

    return mp_map_reader(head, _current_pos, cardinality);
}

mp_array_reader mp_reader::array()
{
    auto type = mp_typeof(*_current_pos);
    if (type != MP_ARRAY)
        throw mp_reader_error("array expected, got " + mpuck_type_name(type), *this);

    auto head = _current_pos;
    if (mp_check(&_current_pos, _end))
        throw mp_reader_error("invalid messagepack", *this);
    auto cardinality = mp_decode_array(&head);

    return mp_array_reader(head, _current_pos, cardinality);
}

mp_reader mp_reader::iproto_message()
{
    if (_end - _current_pos < 5)
        return {_current_pos, _current_pos}; // empty object

    if (static_cast<uint8_t>(*_current_pos) != 0xce)
        throw mp_reader_error("invalid iproto packet", *this);

    uint64_t response_size = mp_decode_uint(&_current_pos);
    if (static_cast<uint64_t>(_end - _current_pos) < response_size)
        throw mp_reader_error("partial iproto packet", *this);

    auto head = _current_pos;
    _current_pos += response_size;
    return mp_reader{head, _current_pos};
}

string mp_reader::to_string()
{
    string res(128, '\0');

    auto type = mp_typeof(*_current_pos);
    if (type == MP_EXT) // mp_snprint prints 'undefined'
    {
        int8_t ext_type;
        uint32_t val_bytes; // data size
        const char *val = mp_decode_ext(&_current_pos, &ext_type, &val_bytes);
        //auto hex_val = hex_dump(val, val + val_bytes);
        if (ext_type != 1)
        {
            res = "undefined";
            return res;
        }

        char *pos = res.data();
        auto sign_nibble = static_cast<uint8_t>(val[--val_bytes]) & 0xF;
        if (sign_nibble == 0xB || sign_nibble == 0xD)
            *pos++ = '-';
        char *first_dec_digit = pos;
        *first_dec_digit = 'N';  // no output flag

        int8_t nibbles_left = static_cast<int8_t>(val_bytes * 2 - 1);
        if (!((*(val + 1)) & 0xF0)) // first nibble is 0
            --nibbles_left;

        int8_t exp = *val;
        // TODO adjust destination buffer size
        if (exp > 0 && exp >= nibbles_left)
        {
            *pos++ = '0';
            *pos++ = '.';
            for (int i = 0; i < exp - nibbles_left; ++i)
                *pos++ = '0';
        }

        uint8_t nz_flag = 0;
        do
        {
            uint8_t c = static_cast<uint8_t>(*(++val));
            if (nibbles_left & 0x1)
            {
                *pos = static_cast<char>('0' + (c >> 4));
                nz_flag |= *pos++;
                --nibbles_left;
                if (exp && nibbles_left == exp)
                    *pos++ = '.';
            }

            if (nibbles_left)
            {
                *pos = static_cast<char>('0' + (c & 0xF));
                nz_flag |= *pos++;
                --nibbles_left;
                if (exp && nibbles_left == exp)
                    *pos++ = '.';
            }
        }
        while (nibbles_left);

        if (nz_flag) // non-zero digit exists
        {
            while (exp++ < 0)
                *pos++ = '0';
        }
        else if (*first_dec_digit == 'N') // no digits at all
        {
            *first_dec_digit = '0';
            pos = first_dec_digit + 1;
        }

        res.resize(static_cast<size_t>(pos - res.data()));
        return res;
    }

    // Сериализация map в json. Именно здесь (в отличие от msgpack в целом) ключи
    // обязаны быть строками.
    // * пока msgpuck не умеет decimal, map придется сериализовать в строку руками
    if (type == MP_MAP)
    {
        auto m = map();
        auto size = static_cast<size_t>(m.end() - m.begin()) * 2; // хватит же? :)
        if (res.size() < size)
            res.resize( size);

        char *pos = res.data();
        *pos++ = '{';
        for (size_t i = 0; i < m.cardinality(); i++)
        {
            if (i)
                *pos++ = ',';

            *pos++ = '"';
            auto tmp = m.value<string_view>();
            memcpy(pos, tmp.data(), tmp.size());
            pos += tmp.size();
            *pos++ = '"';

            *pos++ = ':';

            if (mp_typeof(*m.pos()) == MP_STR)
            {
                *pos++ = '"';
                tmp = m.value<string_view>();
                memcpy(pos, tmp.data(), tmp.size());
                pos += tmp.size();
                *pos++ = '"';
            }
            else
            {
                string tmp = m.to_string();
                memcpy(pos, tmp.data(), tmp.size());
                pos += tmp.size();
            }
        }
        *pos++ = '}';
        res.resize(static_cast<size_t>(pos - res.data()));
        return res;
    }

    int cnt = mp_snprint(res.data(), static_cast<int>(res.size()), _current_pos);
    if (cnt < 0)
    {
        throw mp_reader_error("mp_snprint error", *this);
    }
    else
    {
        if (cnt >= static_cast<int>(res.size()))
        {
            res.resize(static_cast<size_t>(cnt + 1));
            cnt = mp_snprint(res.data(), static_cast<int>(res.size()), _current_pos);
        }
        res.resize(static_cast<size_t>(cnt));
    }
    mp_next(&_current_pos);
    return res;
}

void mp_reader::rewind() noexcept
{
    _current_pos = _begin;
}

bool mp_reader::is_null() const
{
    return mp_typeof(*_current_pos) == MP_NIL;
}

bool mp_reader::has_next() const noexcept
{
    return _current_pos && _end && _current_pos < _end;
}

mp_reader &mp_reader::operator>>(string &val)
{
    string_view tmp;
    *this >> tmp;
    if (!tmp.data())
        throw mp_reader_error("string expected, got no data", *this);
    val.assign(tmp.data(), tmp.size());
    return *this;
}

mp_reader &mp_reader::operator>>(string_view &val)
{
    // for the sake of convenience
    if (!*this || _current_pos >= _end)
    {
        val = {};
        return *this;
    }

    auto type = mp_typeof(*_current_pos);
    if (type == MP_STR)
    {
        uint32_t len = 0;
        const char *value = mp_decode_str(&_current_pos, &len);
        val = {value, len};
    }
    else if (type == MP_NIL)
    {
        mp_decode_nil(&_current_pos);
        val = {}; // data() == nullptr
    }
    else
    {
        throw mp_reader_error("string expected, got " + mpuck_type_name(type), *this);
    }
    return *this;
}

mp_reader::operator bool() const noexcept
{
    return _begin && _end && _end > _begin;
}

// ------------------------------------------------------------------------------------------------

mp_map_reader::mp_map_reader(const char *begin, const char *end, size_t cardinality)
    : mp_reader(begin, end), _cardinality(cardinality)
{
}

size_t mp_map_reader::cardinality() const noexcept
{
    return _cardinality;
}

// ------------------------------------------------------------------------------------------------

mp_array_reader::mp_array_reader(const char *begin, const char *end, size_t cardinality)
    : mp_reader(begin, end), _cardinality(cardinality)
{
}

mp_reader mp_array_reader::operator[](size_t ind) const
{
    if (ind >= _cardinality)
        throw mp_reader_error("index out of range", *this);

    const char *ptr = _begin;
    for (size_t i = 0; i < ind; ++i)
        mp_next(&ptr);

    auto begin = ptr;
    mp_next(&ptr);
    return {begin, ptr};
}

size_t mp_array_reader::cardinality() const noexcept
{
    return _cardinality;
}
