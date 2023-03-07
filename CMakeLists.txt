cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 14)

project(rabbitmq)

include(amqpcpp.cmake)
find_package(PkgConfig REQUIRED)

pkg_search_module(LIBEVENT REQUIRED libevent)

include_directories(${LIBEVENT_INCLUDE_DIRS})

add_executable(test_app test_app.cpp)

target_link_libraries(test_app ${LIBEVENT_LIBRARIES} amqpcpp pthread)