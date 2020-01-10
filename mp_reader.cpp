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

mp_reader& mp_reader::operator>> (mp_map_reader &val) {
    auto type = mp_typeof(*_current_pos);
    if (type != MP_MAP)
        throw mp_reader_error("map expected, got " + mpuck_type_name(type), *this);

    auto head = _current_pos;
    if (mp_check(&_current_pos, _end))
        throw mp_reader_error("invalid messagepack", *this);
    auto cardinality = mp_decode_map(&head);

    val._begin = head;
    val._end = _current_pos;
    val._current_pos = head;
    val._cardinality = cardinality;

    return *this;
}

mp_reader& mp_reader::operator>> (mp_array_reader &val) {
    auto type = mp_typeof(*_current_pos);
    if (type != MP_ARRAY)
        throw mp_reader_error("array expected, got " + mpuck_type_name(type), *this);

    auto head = _current_pos;
    if (mp_check(&_current_pos, _end))
        throw mp_reader_error("invalid messagepack", *this);
    auto cardinality = mp_decode_array(&head);

    val._begin = head;
    val._end = _current_pos;
    val._current_pos = head;
    val._cardinality = cardinality;

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
    string res(256, '\0');
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
        throw mp_reader_error("string expected, got nil", *this);
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

const char* to_string(mp_type type)
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
        return "binary";
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
    default:
        return "unkown";
    }
}
