cmake_minimum_required(VERSION 3.21)

project(three_kingdoms_war VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 项目源码与资源均为无 BOM 的 UTF-8。MSVC 默认按系统代码页解释源文件，会使中文
# 字面量与磁盘 JSON 解析出的字节不一致（doctest 断言与 CTest 正则失配）。FTXUI 对
# 自身及消费方已 PUBLIC 带 /utf-8，这里补齐项目其余目标。
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(TKW_ENABLE_TESTS "Compile three_kingdoms_war tests" ON)
option(TKW_ENABLE_TUI "构建基于 FTXUI 的可选终端界面 tkw-tui" OFF)

# 可选 ccache 编译缓存：默认 OFF 时零行为变化；CI 已在 configure 步以环境变量
# CMAKE_CXX_COMPILER_LAUNCHER 注入 ccache，与本开关互不冲突。
option(TKW_USE_CCACHE "使用 ccache 缓存编译结果" OFF)
if(TKW_USE_CCACHE)
    find_program(TKW_CCACHE_PROGRAM ccache)
    if(NOT TKW_CCACHE_PROGRAM)
        message(FATAL_ERROR "TKW_USE_CCACHE=ON 但未找到 ccache，请先安装并加入 PATH")
    endif()
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TKW_CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${TKW_CCACHE_PROGRAM}")
endif()

add_subdirectory(thirdparty/pjh_result)
add_subdirectory(thirdparty/pjh_json)
add_subdirectory(thirdparty/pjh_platform)
add_subdirectory(thirdparty/pjh_cli)

add_subdirectory(src)

if(TKW_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 可选 TUI 目标：FTXUI 只链进 tkw-tui，tkw / tkw_cli / 引擎各 tkw_* 静态库零新增依赖。
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
