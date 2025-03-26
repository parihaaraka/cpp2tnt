#include "iproto.h"

/// Tarantool connector scope
namespace tnt
{

proto_id::proto_id(std::initializer_list<feature> features, uint64_t version, std::string auth)
    : version(version), auth(auth)
{
    for (const feature &f: features)
    {
        if (static_cast<uint8_t>(f) < this->features.size())
            this->features.set(static_cast<uint8_t>(f));
    }
}

bool proto_id::has_feature(feature f) const
{
    return features.test(static_cast<uint8_t>(f));
}

std::vector<uint8_t> proto_id::list_features() const
{
    std::vector<uint8_t> res;
    for (size_t i = 0; i < features.size(); ++i)
    {
        if (features.test(i) && i < static_cast<uint8_t>(feature::invalid))
            res.push_back(i);
    }
    return res;
}

} // namespace tnt
