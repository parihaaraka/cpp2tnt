#include "iproto_writer.h"
#include "base64.h"
extern "C"
{
#include "sha1.h"
}

namespace tnt
{

static void scramble_prepare(void *out, const void *salt, std::string_view pass) noexcept
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

iproto_writer::iproto_writer(std::function<uint64_t ()> get_request_id, wtf_buffer &buf)
    : mp_writer(buf), get_request_id(get_request_id) {}

void iproto_writer::start_message()
{
    finalize_all();

    // ensure we have 1kb free (make prereserve manually if you need some more)
    if (_buf.capacity() - _buf.size() < 1024)
        _buf.reserve(static_cast<size_t>(_buf.capacity() + 1024));

    size_t head_offset = _buf.size();
    _opened_containers.push({head_offset, std::numeric_limits<uint32_t>::max()});
    mp_store_u8(_buf.end, 0xce); // 0xce -> unit32 (place now to distinguish request header and containers)
    _buf.end += 5;
}

void iproto_writer::finalize()
{
    if (_opened_containers.empty())
        throw std::range_error("no request to finalize");

    auto &c = _opened_containers.top();
    char *head = _buf.data() + c.head_offset;

    if (static_cast<uint8_t>(*head) == 0xce)  // request head
    {
        _opened_containers.pop();
        size_t size = static_cast<size_t>(_buf.end - head);
        if (!size)
            return;

        if (size > c.max_cardinality)
            throw std::overflow_error("request size exceeded");
        mp_store_u32(++head, static_cast<uint32_t>(size - 5));
        return;
    }

    mp_writer::finalize();
}

void iproto_writer::finalize_all()
{
    while (!_opened_containers.empty())
        finalize();
}

void iproto_writer::encode_request_header(request_type req_type)
{
    start_message();
    _buf.end = mp_encode_map(_buf.end, 2);
    _buf.end = mp_encode_uint(mp_encode_uint(_buf.end, tnt::header_field::CODE), static_cast<uint8_t>(req_type));
    _buf.end = mp_encode_uint(mp_encode_uint(_buf.end, tnt::header_field::SYNC), get_request_id());
}

void iproto_writer::encode_response_header(uint32_t error_code, uint64_t schema_version)
{
    uint32_t code = error_code ? 0x8000 | error_code : static_cast<uint32_t>(request_type::OK);
    start_message();
    _buf.end = mp_encode_map(_buf.end, 3);
    _buf.end = mp_encode_uint(mp_encode_uint(_buf.end, tnt::header_field::CODE), code);
    _buf.end = mp_encode_uint(mp_encode_uint(_buf.end, tnt::header_field::SYNC), get_request_id());
    _buf.end = mp_encode_uint(mp_encode_uint(_buf.end, tnt::header_field::SCHEMA_ID), schema_version);
}

void iproto_writer::encode_auth_request(const char* greeting, std::string_view user, std::string_view password, std::string_view auth_proto)
{
    encode_request_header(tnt::request_type::AUTH);

    _buf.end = mp_encode_map(_buf.end, 2);
    _buf.end = mp_encode_strl(mp_encode_uint(_buf.end, tnt::body_field::USER_NAME),
                              static_cast<uint32_t>(user.size()));
    memcpy(_buf.end, user.data(), user.size());
    _buf.end += user.size();

    _buf.end = mp_encode_uint(_buf.end, tnt::body_field::TUPLE);
    std::string_view b64_salt = {
        greeting + tnt::VERSION_SIZE,
        tnt::SCRAMBLE_SIZE + tnt::SALT_SIZE
    };
    char salt[64];
    _buf.end = mp_encode_array(_buf.end, 2);
    _buf.end = mp_encode_str(_buf.end, auth_proto.data(), auth_proto.size());
    _buf.end = mp_encode_strl(_buf.end, tnt::SCRAMBLE_SIZE);
    base64_decode(b64_salt.data(), tnt::SALT_SIZE, salt, 64);
    scramble_prepare(_buf.end, salt, password);
    _buf.end += tnt::SCRAMBLE_SIZE;

    finalize();
}

void iproto_writer::encode_id_request(proto_id proto)
{
    encode_request_header(tnt::request_type::PROTO_ID);
    _buf.end = mp_encode_map(_buf.end, 1 + (proto.version ? 1 : 0) + (proto.auth.empty() ? 0 : 1));

    if (proto.version)
    {
        _buf.end = mp_encode_uint(_buf.end, tnt::body_field::VERSION);
        _buf.end = mp_encode_uint(_buf.end, proto.version);
    }

    auto features = proto.list_features();
    _buf.end = mp_encode_uint(_buf.end, tnt::body_field::FEATURES);
    _buf.end = mp_encode_array(_buf.end, features.size());
    // actually we could memcpy features as is
    for (auto &f: features)
        _buf.end = mp_encode_uint(_buf.end, f);

    if (!proto.auth.empty())
    {
        _buf.end = mp_encode_uint(_buf.end, tnt::body_field::AUTH_TYPE);
        _buf.end = mp_encode_str(_buf.end, proto.auth.data(), proto.auth.size());
    }
    finalize();
}

void iproto_writer::encode_ping_request()
{
    encode_request_header(tnt::request_type::PING);
    _buf.end = mp_encode_map(_buf.end, 0);
    finalize();
}

void iproto_writer::begin_call(std::string_view fn_name)
{
    encode_request_header(tnt::request_type::CALL);

    _buf.end = mp_encode_map(_buf.end, 2);
    _buf.end = mp_encode_uint(_buf.end, tnt::body_field::FUNCTION_NAME);
    _buf.end = mp_encode_str(_buf.end, fn_name.data(), static_cast<uint32_t>(fn_name.size()));
    _buf.end = mp_encode_uint(_buf.end, tnt::body_field::TUPLE);
    // a caller must append an array of arguments (zero-length one if void)
}

void iproto_writer::begin_eval(std::string_view script)
{
    encode_request_header(tnt::request_type::EVAL);

    _buf.end = mp_encode_map(_buf.end, 2);
    _buf.end = mp_encode_uint(_buf.end, tnt::body_field::EXPRESSION);
    _buf.end = mp_encode_str(_buf.end, script.data(), static_cast<uint32_t>(script.size()));
    _buf.end = mp_encode_uint(_buf.end, tnt::body_field::TUPLE);
    // a caller must append an array of arguments (zero-length one if void)
}

} // namespace tnt
