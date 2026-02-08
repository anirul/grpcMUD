set(GRPCMUD_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mud.proto")

add_library(grpcmud_proto STATIC "${GRPCMUD_PROTO_FILE}")

target_link_libraries(grpcmud_proto
  PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)

target_include_directories(grpcmud_proto
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
)

protobuf_generate(
  TARGET grpcmud_proto
  LANGUAGE cpp
  IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
)

protobuf_generate(
  TARGET grpcmud_proto
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
)
