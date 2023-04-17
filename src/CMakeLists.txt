if(BUILD_SHARED_LIBS)
  add_library(gptj SHARED gptj.cpp)
else()
  add_executable(gptj gptj.cpp)
endif()

target_link_libraries(gptj PRIVATE ggml)
