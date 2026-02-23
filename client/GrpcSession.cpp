#include "GrpcSession.hpp"

#include <utility>

namespace grpcmud::client
{
GrpcSession::GrpcSession() : open_(false)
{
}

GrpcSession::~GrpcSession()
{
    Shutdown();
}

bool GrpcSession::Connect(const std::string& server_address, const std::string& player_name,
                          std::string* error_message)
{
    const grpc::Status previous_status = Shutdown();
    if (!previous_status.ok())
    {
        if (error_message)
        {
            *error_message =
                "Previous stream shutdown failed: [" + std::to_string(previous_status.error_code()) +
                "] " + previous_status.error_message();
        }
        return false;
    }

    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    stub_ = mud::v1::MudService::NewStub(channel);
    context_ = std::make_unique<grpc::ClientContext>();
    stream_ = stub_->Play(context_.get());

    if (!stream_)
    {
        if (error_message)
        {
            *error_message = "Failed to open stream.";
        }
        return false;
    }

    mud::v1::ClientMessage join;
    join.mutable_join()->set_player_name(player_name);
    if (!stream_->Write(join))
    {
        if (error_message)
        {
            *error_message = "Server closed stream before JoinRequest was sent.";
        }
        stream_.reset();
        context_.reset();
        return false;
    }

    open_.store(true, std::memory_order_relaxed);
    return true;
}

bool GrpcSession::SendClientMessage(const mud::v1::ClientMessage& message)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!stream_)
    {
        return false;
    }
    return stream_->Write(message);
}

bool GrpcSession::SendPing()
{
    mud::v1::ClientMessage message;
    message.mutable_ping();
    return SendClientMessage(message);
}

void GrpcSession::StartReader(std::function<void(const mud::v1::ServerMessage&)> on_message,
                              std::function<void()> on_closed)
{
    reader_thread_ = std::thread(
        [this, on_message = std::move(on_message), on_closed = std::move(on_closed)]()
        {
            mud::v1::ServerMessage message;
            while (stream_ && stream_->Read(&message))
            {
                on_message(message);
            }
            open_.store(false, std::memory_order_relaxed);
            on_closed();
        });
}

grpc::Status GrpcSession::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (context_)
        {
            context_->TryCancel();
        }
    }

    if (reader_thread_.joinable())
    {
        if (reader_thread_.get_id() == std::this_thread::get_id())
        {
            reader_thread_.detach();
        }
        else
        {
            reader_thread_.join();
        }
    }

    grpc::Status status = grpc::Status::OK;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (stream_)
        {
            status = stream_->Finish();
            stream_.reset();
        }
        context_.reset();
    }

    open_.store(false, std::memory_order_relaxed);
    return status;
}

bool GrpcSession::IsOpen() const
{
    return open_.load(std::memory_order_relaxed);
}
} // namespace grpcmud::client
