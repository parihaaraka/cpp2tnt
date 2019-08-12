#include "msgpuck/msgpuck.h"
#include "proto.h"
#include "base64.h"
#include "wtf_buffer.h"
#include "connection.h"
#include "cs_parser.h"

extern "C"
{
    #include "sha1.h"
}

namespace tnt
{

using namespace std;

static void scramble_prepare(void *out, const void *salt, string_view pass)
{
    unsigned char hash1[tnt::SCRAMBLE_SIZE];
    unsigned char hash2[tnt::SCRAMBLE_SIZE];
    SHA1_CTX ctx;

    SHA1Init(&ctx);
    SHA1Update(&ctx, reinterpret_cast<const unsigned char*>(pass.data()), static_cast<uint32_t>(pass.size()));
    SHA1Final(hash1, &ctx);

    SHA1Init(&ctx);
    SHA1Update(&ctx, hash1, tnt::SCRAMBLE_SIZE);
    SHA1Final(hash2, &ctx);

    SHA1Init(&ctx);
    SHA1Update(&ctx, reinterpret_cast<const unsigned char*>(salt), tnt::SCRAMBLE_SIZE);
    SHA1Update(&ctx, hash2, tnt::SCRAMBLE_SIZE);
    SHA1Final(hash2, &ctx);

    uint8_t *dst = reinterpret_cast<uint8_t*>(out);
    for (int i = 0; i < tnt::SCRAMBLE_SIZE; ++i)
        dst[i] = hash1[i] ^ hash2[i];
}

void encode_auth_request(connection &cn, string_view user, string_view password)
{
    auto &buf = cn.output_buffer();

    if (buf.capacity() - buf.size() < 1024)
        buf.reserve(static_cast<size_t>(buf.capacity() * 1.5));

    auto head_offset = buf.size();
    buf.end += 5;
    encode_header(cn, request_type::AUTH);

    buf.end = mp_encode_map(buf.end, 2);
    buf.end = mp_encode_strl(mp_encode_uint(buf.end, body_field::USER_NAME),
                             static_cast<uint32_t>(user.size()));
    memcpy(buf.end, user.data(), user.size());
    buf.end += user.size();

    buf.end = mp_encode_uint(buf.end, body_field::TUPLE);
    string_view b64_salt = {
        cn.greeting().data() + tnt::VERSION_SIZE,
        tnt::SCRAMBLE_SIZE + tnt::SALT_SIZE
    };
    char salt[64];
    buf.end = mp_encode_array(buf.end, 2);
    buf.end = mp_encode_str(buf.end, "chap-sha1", 9);
    buf.end = mp_encode_strl(buf.end, tnt::SCRAMBLE_SIZE);
    base64_decode(b64_salt.data(), tnt::SALT_SIZE, salt, 64);
    scramble_prepare(buf.end, salt, password);
    buf.end += tnt::SCRAMBLE_SIZE;

    finalize_request(cn, head_offset);
}

unified_header decode_unified_header(const char **p)
{
    if (mp_typeof(**p) != MP_MAP)
        return {};

    unified_header h {0, 0, 0};
    uint32_t n = mp_decode_map(p);
    while (n-- > 0)
    {
        if (mp_typeof(**p) != MP_UINT)
            return {};
        uint32_t key = static_cast<uint32_t>(mp_decode_uint(p));
        if (mp_typeof(**p) != MP_UINT)
            return {};

        switch (key)
        {
        case header_field::SYNC:
            h.sync = mp_decode_uint(p);
            break;
        case header_field::CODE:
            h.code = static_cast<uint32_t>(mp_decode_uint(p));
            break;
        case header_field::SCHEMA_ID:
            h.schema_id = static_cast<uint32_t>(mp_decode_uint(p));
            break;
        default:
            return {};
        }
    }
    return h;
}

string_view string_from_map(const char **p, uint32_t key)
{
    if (mp_typeof(**p) == MP_MAP)
    {
        uint32_t n = mp_decode_map(p);
        while (n-- > 0)
        {
            uint32_t k = static_cast<uint32_t>(mp_decode_uint(p));
            if (k == key)
            {
                if (mp_typeof(**p) != MP_STR)
                    break;
                uint32_t elen = 0;
                const char *value = mp_decode_str(p, &elen);
                return {value, elen};
            }
            mp_next(p);
        }
    }
    return {*p, 0};
}

void encode_header(connection &cn, request_type rtype) noexcept
{
    auto &buf = cn.output_buffer();
    buf.end = mp_encode_map(buf.end, 2);
    buf.end = mp_encode_uint(mp_encode_uint(buf.end, header_field::CODE), static_cast<uint8_t>(rtype));
    buf.end = mp_encode_uint(mp_encode_uint(buf.end, header_field::SYNC), cn.next_request_id());
}

size_t begin_call(connection &cn, std::string_view fn_name)
{
    auto &buf = cn.output_buffer();
    size_t pos = buf.size();
    buf.end += 5;
    encode_header(cn, request_type::CALL);

    buf.end = mp_encode_map(buf.end, 2);
    buf.end = mp_encode_uint(buf.end, body_field::FUNCTION_NAME);
    buf.end = mp_encode_str(buf.end, fn_name.data(), static_cast<uint32_t>(fn_name.size()));
    buf.end = mp_encode_uint(buf.end, body_field::TUPLE);
    return pos;
}

void finalize_request(connection &cn, size_t head_offset)
{
    auto &buf = cn.output_buffer();
    char *size_header = buf.data() + head_offset;

    size_t size = static_cast<size_t>(buf.end - size_header);
    if (!size)
        return;

    if (size > std::numeric_limits<uint32_t>::max())
        throw range_error("request size exceeded");

    char *size_place = mp_store_u8(size_header, 0xce); // 0xce -> unit32
    mp_store_u32(size_place, static_cast<uint32_t>(size - 5));
}

connection& operator<<(connection &cn, int64_t var)
{
    auto &buf = cn.output_buffer();
    buf.end = mp_encode_int(buf.data(), var);
    return cn;
}

connection &operator<<(connection &cn, uint64_t var)
{
    auto &buf = cn.output_buffer();
    buf.end = mp_encode_uint(buf.data(), var);
    return cn;
}

} // namespace tnt
