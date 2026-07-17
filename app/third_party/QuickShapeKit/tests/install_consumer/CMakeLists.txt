cmake_minimum_required(VERSION 3.21)
project(QuickShapeKitConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(QuickShapeKit CONFIG REQUIRED)
add_executable(QuickShapeKitConsumer main.cpp)
target_link_libraries(QuickShapeKitConsumer PRIVATE QuickShape::Kit)

