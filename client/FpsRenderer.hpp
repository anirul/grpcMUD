#pragma once

#include <string>

#include "mud.pb.h"

namespace grpcmud::client
{
class FpsRenderer
{
public:
    static std::string Render(const mud::v1::FirstPersonView& view);
};
} // namespace grpcmud::client
