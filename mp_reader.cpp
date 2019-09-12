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
    auto size = mp_decode_map(&head);

    return mp_map_reader(head, _current_pos, size);
}

mp_array_reader mp_reader::array()
{
    auto type = mp_typeof(*_current_pos);
    if (type != MP_ARRAY)
        throw mp_reader_error("array expected, got " + mpuck_type_name(type), *this);

    auto head = _current_pos;
    if (mp_check(&_current_pos, _end))
        throw mp_reader_error("invalid messagepack", *this);
    auto size = mp_decode_array(&head);

    return mp_array_reader(head, _current_pos, size);
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

string_view mp_reader::to_string()
{
    // for the sake of convenience
    if (!*this)
        return {};

    auto type = mp_typeof(*_current_pos);
    if (type == MP_STR)
    {
        uint32_t len = 0;
        const char *value = mp_decode_str(&_current_pos, &len);
        return {value, len};
    }

    if (type == MP_NIL)
    {
        mp_decode_nil(&_current_pos);
        return {}; // data() == nullptr
    }

    throw mp_reader_error("string expected, got " + mpuck_type_name(type), *this);
}

void mp_reader::rewind() noexcept
{
    _current_pos = _begin;
}

mp_reader &mp_reader::operator>>(string &val)
{
    string_view tmp = to_string();
    if (!tmp.data())
        throw mp_reader_error("string expected, got no data", *this);
    val.assign(tmp.data(), tmp.size());
    return *this;
}

mp_reader &mp_reader::operator>>(string_view &val)
{
    val = to_string();
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

mp_reader mp_map_reader::operator[](int64_t key) const
{
    mp_reader res = find(key);
    if (!res)
        throw mp_reader_error("key not found", *this);
    return res;
}

mp_reader mp_map_reader::find(int64_t key) const
{
    const char *ptr = _begin;
    auto n = _cardinality;
    while (n-- > 0)
    {
        bool found = false;
        if (key >= 0 && mp_typeof(*ptr) == MP_UINT)
        {
            auto cur_key = mp_decode_uint(&ptr);
            found = (cur_key <= std::numeric_limits<int64_t>::max() && key == static_cast<int64_t>(cur_key));
        }
        else if (mp_typeof(*ptr) == MP_INT) // msgpuck has assert(num < 0) within mp_encode_int()
        {
            auto cur_key = mp_decode_int(&ptr);
            found = (key == cur_key);
        }
        else
        {
            mp_next(&ptr); // skip a key
        }

        auto begin = ptr;
        mp_next(&ptr);

        if (found)
            return {begin, ptr};
    }
    return {nullptr, nullptr};
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
