#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mud.grpc.pb.h"

namespace grpcmud::client
{
class GrpcSession
{
public:
    GrpcSession();
    ~GrpcSession();

    bool Connect(const std::string& server_address, const std::string& player_name,
                 std::string* error_message);
    bool SendCommand(const std::string& request_id, const std::string& command_text);
    bool SendPing();

    void StartReader(std::function<void(const mud::v1::ServerMessage&)> on_message,
                     std::function<void()> on_closed);
    grpc::Status Shutdown();

    bool IsOpen() const;

private:
    std::unique_ptr<mud::v1::MudService::Stub> stub_;
    std::unique_ptr<grpc::ClientReaderWriter<mud::v1::ClientMessage, mud::v1::ServerMessage>>
        stream_;
    grpc::ClientContext context_;

    std::thread reader_thread_;
    std::atomic<bool> open_;
    std::mutex write_mutex_;
};
} // namespace grpcmud::client

