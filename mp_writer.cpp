#include "msgpuck/msgpuck.h"
#include "mp_writer.h"
#include "mp_reader.h"

using namespace std;

mp_writer::mp_writer(wtf_buffer &buf) : _buf(buf) {}

void mp_writer::begin_array(uint32_t max_cardinality)
{
    increment_container_counter();

    _opened_containers.push({_buf.size(), max_cardinality});
    _buf.end = mp_encode_array(_buf.end, max_cardinality);
}

void mp_writer::begin_map(uint32_t max_cardinality)
{
    increment_container_counter();
    _opened_containers.push({_buf.size(), max_cardinality});
    _buf.end = mp_encode_map(_buf.end, max_cardinality);
}

void mp_writer::finalize()
{
    if (_opened_containers.empty())
        throw range_error("no container to finalize");

    auto &c = _opened_containers.pop();
    char *head = _buf.data() + c.head_offset;

    // mp_encode_array() may reduce header's size if actual cardinality
    // is smaller than initial value, so we update the header directly

    uint32_t max_num_bytes = 0;
    uint32_t actual_cardinality = c.items_count;
    auto container_type = mp_typeof(*head);

    if (container_type == MP_ARRAY)
    {
        if (actual_cardinality == c.max_cardinality)
            return;

        // get current header size
        max_num_bytes = mp_sizeof_array(c.max_cardinality);

        if (actual_cardinality > c.max_cardinality && mp_sizeof_array(actual_cardinality) > max_num_bytes)
            throw overflow_error("array header size exceeded");
            //throw overflow_error(std::format("array header size exceeded ({} of {})\n{}\n{}",
            //             actual_cardinality, c.max_cardinality,
            //             get_trace(), hex_dump(_buf.data(), _buf.end, head)));

        if (max_num_bytes == 1)
        {
            // replace 1-byte header with the new size
            mp_encode_array(head, actual_cardinality);
            return;
        }
    }
    else if (container_type == MP_MAP)
    {
        if (c.items_count & 0x1)
            throw runtime_error("odd number of map items");

        actual_cardinality = c.items_count / 2; // map cardinality
        if (actual_cardinality == c.max_cardinality)
            return;

        // get current header size
        max_num_bytes = mp_sizeof_map(c.max_cardinality);

        if (actual_cardinality > c.max_cardinality && mp_sizeof_map(actual_cardinality) > max_num_bytes)
            throw overflow_error("map header size exceeded");

        if (max_num_bytes == 1)
        {
            // replace 1-byte header with new size
            mp_encode_map(head, actual_cardinality);
            return;
        }
    }
    else
    {
        throw runtime_error("unexpected container header");
    }

    switch (max_num_bytes)
    {
    case 3:
        mp_store_u16(++head, static_cast<uint16_t>(actual_cardinality));
        break;
    case 5:
        mp_store_u32(++head, actual_cardinality);
        break;
    default:
        throw runtime_error("previously not implemented container cardinality");
    }
}

void mp_writer::finalize_all()
{
    while (!_opened_containers.empty())
        finalize();
}

void mp_writer::increment_container_counter(size_t items_added)
{
    if (!_opened_containers.empty())
        _opened_containers.top().items_count += items_added;
}

void mp_writer::write(const char *begin, const char *end, size_t cardinality)
{
    // make sure the destination has free space
    auto dst = _buf.end;
    _buf.resize(_buf.size() + end - begin);
    std::copy(begin, end, dst);

    if (!_opened_containers.empty())
    {
        if (!cardinality)
        {
            mp_reader mp{begin, end};
            while (mp.has_next())
            {
                mp.skip();
                ++cardinality;
            }
        }
        _opened_containers.top().items_count += cardinality;
    }
}

mp_writer &mp_writer::fill(mp_raw_view items_to_fill, uint32_t target_items_count)
{
    if (_opened_containers.empty())
        throw runtime_error("no opened containers");
    auto &c = _opened_containers.top();
    while (c.items_count < target_items_count)
    {
        if (c.items_count + items_to_fill.cardinality() <= target_items_count)
            *this << items_to_fill;
        else
            break;  // if target_items_count % items_to_fill.cardinality != 0
    }
    return *this;
}

mp_writer &mp_writer::operator<<(nullptr_t)
{
    _buf.end = mp_encode_nil(_buf.end);
    increment_container_counter();
    return *this;
}

void mp_writer::set_state(const state &state)
{
    if (_buf.capacity() >= state.content_len)
        _buf.end = _buf.data() + state.content_len;
    else
        throw std::overflow_error("destination buffer was truncated");
    _opened_containers = state.opened_containers;
}

mp_writer::state mp_writer::get_state()
{
    return state{_buf.size(), _opened_containers};
}

mp_writer& mp_writer::operator<<(const string_view &val)
{
    if (val.data() == nullptr)
        _buf.end = mp_encode_nil(_buf.end);
    else if (val.size() > std::numeric_limits<uint32_t>::max())
        throw overflow_error("too long string");
    else
        _buf.end = mp_encode_str(_buf.end, val.data(), static_cast<uint32_t>(val.size()));

    increment_container_counter();
    return *this;
}

mp_writer& mp_writer::operator<<(const mp_plain &src)
{
    mp_reader r(src);
    while (r.has_next(true))
        *this << r;
    return *this;
}
