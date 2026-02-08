#include "ClientSession.hpp"

namespace grpcmud::server
{
ClientSession::ClientSession(
    grpc::ServerReaderWriter<mud::v1::ServerMessage, mud::v1::ClientMessage>* stream)
    : stream_(stream)
{
}

bool ClientSession::Write(mud::v1::ServerMessage message)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    return stream_->Write(message);
}
} // namespace grpcmud::server

