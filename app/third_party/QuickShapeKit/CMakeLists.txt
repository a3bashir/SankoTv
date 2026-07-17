cmake_minimum_required(VERSION 3.21)

project(QuickShapeKit VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_library(QuickShapeKit STATIC
    include/QuickShape/quickshape_types.h
    include/QuickShape/quickshape_recognizer.h
    include/QuickShape/quickshape_session.h
    src/quickshape_recognizer.cpp
    src/quickshape_session.cpp
)
add_library(QuickShape::Kit ALIAS QuickShapeKit)
set_target_properties(QuickShapeKit PROPERTIES EXPORT_NAME Kit)

target_include_directories(QuickShapeKit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(QuickShapeKit PUBLIC Qt6::Core Qt6::Gui)

option(QUICKSHAPE_BUILD_TESTS "Build QuickShapeKit smoke tests" ON)
if(QUICKSHAPE_BUILD_TESTS)
    enable_testing()
    add_executable(QuickShapeKitSmoke tests/quickshape_smoke.cpp)
    target_link_libraries(QuickShapeKitSmoke PRIVATE QuickShape::Kit Qt6::Core Qt6::Gui)
    add_test(NAME QuickShapeKitSmoke COMMAND QuickShapeKitSmoke)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS QuickShapeKit
    EXPORT QuickShapeKitTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT QuickShapeKitTargets
    FILE QuickShapeKitTargets.cmake
    NAMESPACE QuickShape::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QuickShapeKit
)
configure_package_config_file(
    cmake/QuickShapeKitConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/QuickShapeKitConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QuickShapeKit
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/QuickShapeKitConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/QuickShapeKitConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/QuickShapeKitConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QuickShapeKit
)
