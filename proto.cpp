#include "msgpuck/msgpuck.h"
#include "proto.h"
#include "base64.h"

extern "C"
{
    #include "sha1.h"
}

namespace tnt
{

using namespace std;

static void scramble_prepare(void *out, const void *salt, const string &pass)
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

void encode_auth_request(wtf_buffer &buf,
                         const cs_parts &cs_parts,
                         string_view greeting,
                         uint64_t request_id)
{
    if (buf.capacity() - buf.size() < 1024)
        buf.reserve(static_cast<size_t>(buf.capacity() * 1.5));

    auto size_header = buf.end;
    buf.end += 5; // 5 bytes header with int32 size

    buf.end = mp_encode_map(buf.end, 2);
    buf.end = mp_encode_uint(mp_encode_uint(buf.end, header_type::CODE), request_type::AUTH);
    buf.end = mp_encode_uint(mp_encode_uint(buf.end, header_type::SYNC), request_id);

    buf.end = mp_encode_map(buf.end, 2);
    buf.end = mp_encode_strl(mp_encode_uint(buf.end, body_type::USERNAME),
                             static_cast<uint32_t>(cs_parts.user.size()));
    memcpy(buf.end, cs_parts.user.data(), cs_parts.user.size());
    buf.end += cs_parts.user.size();

    buf.end = mp_encode_uint(buf.end, body_type::TUPLE);
    string_view b64_salt = {
        greeting.data() + tnt::VERSION_SIZE,
        tnt::SCRAMBLE_SIZE + tnt::SALT_SIZE
    };
    char salt[64];
    buf.end = mp_encode_array(buf.end, 2);
    buf.end = mp_encode_str(buf.end, "chap-sha1", 9);
    buf.end = mp_encode_strl(buf.end, tnt::SCRAMBLE_SIZE);
    base64_decode(b64_salt.data(), tnt::SALT_SIZE, salt, 64);
    scramble_prepare(buf.end, salt, cs_parts.password);
    buf.end += tnt::SCRAMBLE_SIZE;

    uint32_t size = static_cast<uint32_t>(buf.size());
    char *size_place = mp_store_u8(size_header, 0xce); // 0xce -> unit32
    mp_store_u32(size_place, size - 5);
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
        case header_type::SYNC:
            h.sync = mp_decode_uint(p);
            break;
        case header_type::CODE:
            h.code = static_cast<uint32_t>(mp_decode_uint(p));
            break;
        case header_type::SCHEMA_ID:
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

} // namespace tnt
