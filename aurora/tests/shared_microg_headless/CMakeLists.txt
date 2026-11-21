cmake_minimum_required(VERSION 3.12)
project(SharedMicrogHeadlessProbe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt5 REQUIRED COMPONENTS Core DBus)

add_executable(acl-shared-microg-probe service.cpp)
target_link_libraries(acl-shared-microg-probe PRIVATE Qt5::Core Qt5::DBus)
add_executable(acl-shared-microg-probe-client client.cpp)
target_link_libraries(acl-shared-microg-probe-client PRIVATE Qt5::Core Qt5::DBus)
