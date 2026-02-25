#pragma once

#include <cstdint>

#include "frame/json/proto.h"
#include "gameplay.pb.h"

namespace grpcmud::client::scene
{

inline constexpr float kCellSize = 2.0f;
inline constexpr float kCameraHeight = 0.92f;
inline constexpr float kCameraForwardDistance = 2.0f;
inline constexpr float kCameraNearClip = 0.12f;

struct SceneLevelBuildResult
{
    frame::proto::Level level_proto;
    std::uint64_t geometry_hash = 0;
};

std::uint64_t ComputeRelativeSceneSignature(const mud::v1::LocalViewUpdate& view);
SceneLevelBuildResult BuildBootstrapLevelProto();
SceneLevelBuildResult BuildLevelProtoFromView(const mud::v1::LocalViewUpdate& view);

} // namespace grpcmud::client::scene
