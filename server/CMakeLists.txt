add_executable(grpcmud_server main.cpp)

target_link_libraries(grpcmud_server PRIVATE gRPC::grpc++)