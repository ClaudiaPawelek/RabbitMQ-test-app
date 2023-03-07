cmake_minimum_required(VERSION 3.11 FATAL_ERROR)
include(FetchContent)

message("AMQP-CPP.. Starting")

FetchContent_Declare(
  amqp-cpp
  GIT_REPOSITORY "https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git"
  GIT_TAG "v4.3.17"
  SOURCE_DIR  ${CMAKE_BINARY_DIR}/external/amqp-cpp
)

set(AMQP-CPP_LINUX_TCP ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(amqp-cpp)

FetchContent_GetProperties(amqp-cpp)
if(NOT amqp-cpp_POPULATED)
  FetchContent_Populate(amqp-cpp)
  add_subdirectory(${amqp-cpp_SOURCE_DIR} ${amqp-cpp_BINARY_DIR})
endif()

message("AMQP-CPP.. Finished")