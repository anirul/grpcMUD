#pragma once

#include <grpcpp/grpcpp.h>

#include <mutex>

#include "mud.grpc.pb.h"

namespace grpcmud::server
{
class ClientSession
{
public:
    explicit ClientSession(
        grpc::ServerReaderWriter<mud::v1::ServerMessage, mud::v1::ClientMessage>* stream);

    bool Write(mud::v1::ServerMessage message);

private:
    grpc::ServerReaderWriter<mud::v1::ServerMessage, mud::v1::ClientMessage>* stream_;
    std::mutex write_mutex_;
};
} // namespace grpcmud::server

