#pragma once

#include <string>

#include "gameplay.pb.h"

namespace grpcmud::client
{
class MapRenderer
{
public:
    static std::string Render(const mud::v1::LocalViewUpdate& view);
};
} // namespace grpcmud::client
