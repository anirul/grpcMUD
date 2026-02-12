set(GRPCMUD_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(GRPCMUD_PROTO_FILES
  "${GRPCMUD_PROTO_DIR}/gameplay.proto"
  "${GRPCMUD_PROTO_DIR}/world_storage.proto"
)
set(GRPCMUD_SERVICE_PROTO "${GRPCMUD_PROTO_DIR}/gameplay.proto")

add_library(grpcmud_proto STATIC ${GRPCMUD_PROTO_FILES})

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
  IMPORT_DIRS "${GRPCMUD_PROTO_DIR}"
)

protobuf_generate(
  LANGUAGE grpc
  PROTOS "${GRPCMUD_SERVICE_PROTO}"
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  IMPORT_DIRS "${GRPCMUD_PROTO_DIR}"
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  OUT_VAR GRPCMUD_GRPC_GENERATED
)

target_sources(grpcmud_proto PRIVATE ${GRPCMUD_GRPC_GENERATED})
