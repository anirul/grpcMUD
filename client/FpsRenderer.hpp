#pragma once

#include <string>

#include "gameplay.pb.h"

namespace grpcmud::client
{
class FpsRenderer
{
public:
    static std::string Render(const mud::v1::FirstPersonView& view, int terminal_columns,
                              int terminal_rows);
};
} // namespace grpcmud::client
