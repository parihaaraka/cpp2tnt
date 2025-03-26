#include "mp_reader.h"
#include "wtf_buffer.h"
#include "msgpuck/msgpuck.h"

using namespace std;

struct mp_ext_initializer
{
    mp_ext_initializer()
    {
        mp_snprint_ext = mp_snprint_ext_tnt;
        mp_fprint_ext = mp_fprint_ext_tnt;
    }
};

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

mp_reader_error::mp_reader_error(const std::string &msg, const mp_reader &reader, const char *pos)
    : runtime_error(msg + '\n' + hex_dump(reader.begin(), reader.end(), pos ? pos : reader.pos()))
{
}

// ------------------------------------------------------------------------------------------------

mp_reader::mp_reader(const wtf_buffer &buf) : mp_reader(buf.data(), buf.end)
{
}

mp_reader::mp_reader(const std::vector<char> &buf) : mp_reader(buf.data(), buf.data() + buf.size())
{

}

mp_reader::mp_reader(const char *begin, const char *end)
{
    _current_pos = _begin = begin;
    _end = end;
    static mp_ext_initializer i;
}

void mp_reader::skip(const char **pos) const
{
    if (!pos || !*pos || *pos >= _end)
        throw mp_reader_error("read out of bounds", *this, _end);

    const char *prev = *pos;
    mp_next(pos);
    if (*pos > _end)
    {
        *pos = prev;
        throw mp_reader_error("invalid messagepack", *this, prev);
    }
}

void mp_reader::skip(mp_type type, bool nullable)
{
    auto actual_type = mp_typeof(*_current_pos);
    if (actual_type != type && (!nullable || actual_type != MP_NIL))
        throw mp_reader_error(mpuck_type_name(type) + " expected, got " + mpuck_type_name(actual_type), *this);
    skip();
}

mp_array_reader mp_reader::as_array(size_t ind) const
{
    const char *pos = _current_pos;
    for (size_t i = 0; i < ind; ++i)
        skip(&pos);

    const char *prev_pos = pos;
    auto type = mp_typeof(*pos);
    skip(&pos);
    size_t cardinality = 1;
    if (type == MP_ARRAY)
        cardinality = mp_decode_array(&prev_pos);
    else if (type == MP_MAP)
        cardinality = mp_decode_map(&prev_pos) * 2;

    return mp_array_reader(prev_pos, pos, cardinality);
}

mp_reader& mp_reader::operator>> (mp_map_reader &val)
{
    auto head = _current_pos;
    auto type = mp_typeof(*head);
    if (type == MP_MAP)
    {
        val._cardinality = mp_decode_map(&head);
    }
    else if (type == MP_EXT)
    {
        int8_t ext_type = 0;
        mp_decode_extl(&head, &ext_type);
        if (ext_type == MP_INTERVAL)
            // https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-interval-type
            val._cardinality = mp_decode_uint(&head);
        else
            throw mp_reader_error("unable to read map from ext type " + std::to_string(type), *this, head);
    }
    else
    {
        throw mp_reader_error("map expected, got " + mpuck_type_name(type), *this, head);
    }

    skip();
    val._begin = head;
    val._end = _current_pos;
    val._current_pos = head;

    return *this;
}

mp_reader& mp_reader::operator>> (mp_array_reader &val)
{
    auto head = _current_pos;
    auto type = mp_typeof(*head);
    if (type == MP_ARRAY)
    {
        val._cardinality = mp_decode_array(&head);
        skip();
        val._end = _current_pos;
    }
    else if (type == MP_EXT)
    {
        int8_t ext_type = 0;
        mp_decode_extl(&head, &ext_type);
        if (ext_type == MP_ERROR)
        {
            // https://www.tarantool.io/en/doc/2.11/dev_guide/internals/msgpack_extensions/#the-error-type
            // acquire key 0 value from the topmost map
            uint32_t i = mp_decode_map(&head);
            while (i--)
            {
                uint32_t key = mp_decode_uint(&head);
                auto value_pos = head;
                if (key == 0)
                {
                    val._cardinality = mp_decode_array(&head);
                    mp_next(&value_pos);
                    val._end = value_pos;
                    val._begin = head;
                    val._current_pos = head;
                    skip();
                    return *this;
                }
                else
                {
                    mp_next(&head);
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

    val._begin = head;
    val._current_pos = head;

    return *this;
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
    const char *data = _current_pos;
    skip();

    string res(256, '\0');
    int cnt = mp_snprint(res.data(), static_cast<int>(res.size()), data);
    if (cnt >= static_cast<int>(res.size()))
    {
        res.resize(static_cast<size_t>(cnt + 1));
        cnt = mp_snprint(res.data(), static_cast<int>(res.size()), data);
    }
    if (cnt < 0)
        throw mp_reader_error("mp_snprint error", *this, data);
    res.resize(static_cast<size_t>(cnt));
    return res;
}

void mp_reader::check() const
{
    auto pos = _begin;
    while (pos < _end)
    {
        const char *prev = pos;
        if (mp_check(&pos, _end))
            throw mp_reader_error("invalid messagepack", *this, prev);
    }
}

mp_reader& mp_reader::operator>>(string &val)
{
    if (mp_typeof(*_current_pos) == MP_EXT)
    {
        const char *ext_head = _current_pos;
        const char *pos = _current_pos;
        int8_t ext_type = 0;
        size_t len = mp_decode_extl(&pos, &ext_type);
        skip();

        if (ext_type == MP_UUID) // print uuid without quotes
        {
            val.resize(36, '\0');
            char *dst = val.data();
            hex_print(&dst, &pos, 4);
            *dst++ = '-';
            hex_print(&dst, &pos, 2);
            *dst++ = '-';
            hex_print(&dst, &pos, 2);
            *dst++ = '-';
            hex_print(&dst, &pos, 2);
            *dst++ = '-';
            hex_print(&dst, &pos, 6);
            return *this;
        }

        // regular print
        if (val.size() < 128)
            val.resize(128, '\0');
        len = mp_snprint(val.data(), val.size(), ext_head);
        if (len > val.size())
        {
            val.resize(len + 1, '\0');
            len = mp_snprint(val.data(), len + 1, ext_head);
        }
        if (len < 0)
            throw mp_reader_error("bad ext value content", *this, ext_head);
        val.resize(len);
    }
    else
    {
        string_view tmp;
        *this >> tmp;
        if (!tmp.data())
            throw mp_reader_error("string expected, got no data", *this);
        val.assign(tmp.data(), tmp.size());
    }
    return *this;
}

mp_reader& mp_reader::operator>>(string_view &val)
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
        throw mp_reader_error("string expected, got " + mpuck_type_name(type), *this);
    }
    return *this;
}

mp_reader& mp_reader::operator>> (bool &val)
{
    const char *data = _current_pos;
    auto type = mp_typeof(*data);
    if (type != MP_BOOL)
        throw mp_reader_error("boolean expected, got " + mpuck_type_name(type), *this, _current_pos);

    val = mp_decode_bool(&data);
    skip();
    return *this;
}

mp_reader mp_reader::operator[](size_t ind) const
{
    const char *ptr = _begin;
    for (size_t i = 0; i < ind; ++i)
        skip(&ptr);

    auto begin = ptr;
    skip(&ptr);
    return {begin, ptr};
}

// ------------------------------------------------------------------------------------------------

mp_map_reader::mp_map_reader(const char *begin, const char *end, size_t cardinality)
    : mp_reader(begin, end), _cardinality(cardinality)
{
}

// ------------------------------------------------------------------------------------------------

mp_array_reader::mp_array_reader(const char *begin, const char *end, size_t cardinality)
    : mp_reader(begin, end), _cardinality(cardinality)
{
}

mp_array_reader::mp_array_reader(const wtf_buffer &buf)
    : mp_array_reader(buf.data(), buf.end)
{
}

mp_array_reader::mp_array_reader(const char *begin, const char *end)
    : mp_reader(begin, end)
{
    mp_reader::operator>>(*this);
}
