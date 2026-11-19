cmake_minimum_required(VERSION 3.16)
project(mc_forward LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network Widgets)

# ---------------- 版本信息（自动带 git 提交号） ----------------
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MCFW_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

if(NOT MCFW_GIT_COMMIT)
    set(MCFW_GIT_COMMIT "nogit")
else()
    # 工作区有未提交改动时加 -dirty，避免版本号骗人
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MCFW_GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(MCFW_GIT_DIRTY)
        string(APPEND MCFW_GIT_COMMIT "-dirty")
    endif()
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gitinfo.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/gitinfo.h
    @ONLY)

include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)
message(STATUS "版本: V1.00 (${MCFW_GIT_COMMIT})")

# ---------------- 通用层 ----------------
add_library(mc_common STATIC
    src/common/log.cpp
    src/common/log.h
    src/common/config.cpp
    src/common/config.h
    src/common/protocol.cpp
    src/common/protocol.h
    src/common/stats.h
    src/common/appinit.cpp
    src/common/appinit.h
)
target_include_directories(mc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mc_common PUBLIC Qt5::Core Qt5::Network)

# ---------------- 主机 A ----------------
add_executable(mc_forward_a
    src/client/main.cpp
    src/client/txqueue.cpp
    src/client/txqueue.h
    src/client/multicastreceiver.cpp
    src/client/multicastreceiver.h
    src/client/tunnelclient.cpp
    src/client/tunnelclient.h
)
target_link_libraries(mc_forward_a PRIVATE mc_common)

# ---------------- 主机 B ----------------
add_executable(mc_forward_b
    src/server/main.cpp
    src/server/multicastsender.cpp
    src/server/multicastsender.h
    src/server/tunnelserver.cpp
    src/server/tunnelserver.h
)
target_link_libraries(mc_forward_b PRIVATE mc_common)

# ---------------- 图形界面（主交付形态） ----------------
set(GUI_WIN_RESOURCES)
if(WIN32)
    list(APPEND GUI_WIN_RESOURCES src/gui/app.rc)
endif()

add_executable(multicastForward WIN32
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/mainwindow.h
    src/gui/updateclient.cpp
    src/gui/updateclient.h
    src/gui/resources.qrc
    ${GUI_WIN_RESOURCES}
    src/client/txqueue.cpp
    src/client/txqueue.h
    src/client/multicastreceiver.cpp
    src/client/multicastreceiver.h
    src/client/tunnelclient.cpp
    src/client/tunnelclient.h
    src/server/multicastsender.cpp
    src/server/multicastsender.h
    src/server/tunnelserver.cpp
    src/server/tunnelserver.h
)
target_link_libraries(multicastForward PRIVATE mc_common Qt5::Widgets)

# ---------------- 远端更新启动器（纯 Win32，不依赖 Qt） ----------------
if(WIN32)
    add_executable(multicastForwardLauncher WIN32
        src/launcher/launcher.cpp
        src/launcher/launcher.rc)
    target_link_libraries(multicastForwardLauncher PRIVATE shell32)
endif()

# ---------------- 测试工具 ----------------
add_executable(mc_sender src/tools/mcsender.cpp)
target_link_libraries(mc_sender PRIVATE mc_common)

if(MSVC)
    foreach(tgt mc_common mc_forward_a mc_forward_b multicastForward mc_sender)
        target_compile_options(${tgt} PRIVATE /utf-8 /W3)
    endforeach()
    if(WIN32)
        target_compile_options(multicastForwardLauncher PRIVATE /utf-8 /W3)
    endif()
endif()
