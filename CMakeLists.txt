cmake_minimum_required(VERSION 3.21)

project(three_kingdoms_war VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TKW_ENABLE_TESTS "Compile three_kingdoms_war tests" ON)
option(TKW_ENABLE_TUI "构建基于 FTXUI 的可选终端界面 tkw-tui" OFF)

add_subdirectory(thirdparty/pjh_result)
add_subdirectory(thirdparty/pjh_json)
add_subdirectory(thirdparty/pjh_platform)
add_subdirectory(thirdparty/pjh_cli)

add_subdirectory(src)

if(TKW_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 可选 TUI 目标：FTXUI 只链进 tkw-tui，tkw / tkw_cli / header-only 引擎零新增依赖。
# 默认 OFF 时不探测、不拉取 FTXUI；本机已装优先，未装回退 FetchContent 钉版本。
if(TKW_ENABLE_TUI)
    find_package(ftxui CONFIG QUIET)
    if(NOT TARGET ftxui::ftxui)
        include(FetchContent)
        set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
        FetchContent_Declare(ftxui
            GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
            GIT_TAG v7.0.3)
        FetchContent_MakeAvailable(ftxui)
    endif()
    add_subdirectory(tui)
endif()
