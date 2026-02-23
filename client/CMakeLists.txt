find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(
  grpcmud_client
  main.cpp
  ClientApp.cpp
  GrpcSession.cpp
)

target_link_libraries(grpcmud_client PRIVATE grpcmud_proto)
target_link_libraries(grpcmud_client PRIVATE absl::flags_parse)
target_link_libraries(grpcmud_client PRIVATE Frame)
target_link_libraries(grpcmud_client PRIVATE FrameFile)
target_link_libraries(grpcmud_client PRIVATE FrameJson)
target_link_libraries(grpcmud_client PRIVATE FrameOpenGL)
target_link_libraries(grpcmud_client PRIVATE FrameOpenGLFile)
target_link_libraries(grpcmud_client PRIVATE FrameOpenGLGui)
target_link_libraries(grpcmud_client PRIVATE FrameVulkan)
target_link_libraries(grpcmud_client PRIVATE FrameVulkanGui)
target_link_libraries(grpcmud_client PRIVATE FrameCommon)
target_link_libraries(grpcmud_client PRIVATE FrameGui)
target_link_libraries(grpcmud_client PRIVATE FrameProto)
target_link_libraries(grpcmud_client PRIVATE SDL3::SDL3)
target_link_libraries(grpcmud_client PRIVATE imgui::imgui)
target_include_directories(grpcmud_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
