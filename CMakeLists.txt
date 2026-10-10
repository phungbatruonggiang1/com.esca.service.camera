cmake_minimum_required(VERSION 3.16)
project(esca-camera VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)

# --- libesca-cambuf: the frame plane, usable without D-Bus ------------------
add_library(esca-cambuf SHARED src/buffer/buffer.cpp)
target_include_directories(esca-cambuf PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(esca-cambuf PUBLIC Threads::Threads)
set_target_properties(esca-cambuf PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# --- libesca-camhal: HAL interface plus the libcamera implementation --------
# Built in, not dlopen-ed, until there is a second implementation to choose
# between; hal.hpp keeps the seam so that stays a packaging change.
add_library(esca-camhal SHARED src/hal/libcamera_hal.cpp)
target_include_directories(esca-camhal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(esca-camhal PUBLIC PkgConfig::LIBCAMERA Threads::Threads)
set_target_properties(esca-camhal PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# --- libesca-camclient: what applications link ------------------------------
add_library(esca-camclient SHARED src/client/client.cpp)
target_include_directories(esca-camclient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(esca-camclient PUBLIC PkgConfig::SDBUSCPP esca-cambuf)
set_target_properties(esca-camclient PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

# --- esca-camerad: the service ----------------------------------------------
add_executable(esca-camerad
    src/service/main.cpp
    src/service/device.cpp
    src/service/service.cpp)
target_include_directories(esca-camerad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(esca-camerad PRIVATE
    PkgConfig::SDBUSCPP esca-camhal esca-cambuf Threads::Threads)

# --- esca-camctl: first client, and the one that keeps this testable --------
add_executable(esca-camctl src/tools/camctl.cpp)
target_include_directories(esca-camctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(esca-camctl PRIVATE esca-camclient)

install(TARGETS esca-cambuf esca-camhal esca-camclient
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS esca-camerad RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS esca-camctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/esca DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES data/vn.sparc.esca.Camera1.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES data/esca-camerad.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
