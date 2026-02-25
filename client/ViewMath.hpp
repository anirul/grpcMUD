#pragma once

#include <utility>

#include "gameplay.pb.h"

namespace grpcmud::client::viewmath
{

struct FacingVec
{
    float x = 0.0f;
    float z = -1.0f;
};

FacingVec ToFacingVec(mud::v1::Direction direction);
mud::v1::Direction TurnLeftDirection(mud::v1::Direction direction);
mud::v1::Direction TurnRightDirection(mud::v1::Direction direction);
std::pair<int, int> DirectionToGridDelta(mud::v1::Direction direction);

float DirectionToYawRadians(mud::v1::Direction direction);
float NormalizeAngleRadians(float radians);
float LerpAngleRadians(float from, float to, float t);
float SmoothStep(float t);

bool IsSingleTickMoveOrTurn(
    const mud::v1::LocalViewUpdate& from,
    const mud::v1::LocalViewUpdate& to);
bool CanPredictMoveFromCenter(
    const mud::v1::LocalViewUpdate& view,
    mud::v1::StepKind kind);

} // namespace grpcmud::client::viewmath
