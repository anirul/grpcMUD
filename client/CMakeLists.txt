add_executable(grpcmud_client main.cpp)

target_link_libraries(grpcmud_client PRIVATE gRPC::grpc++)