# 可选 TUI 目标：仅在此目标链入 FTXUI，引擎与 CLI 目标不感知该依赖。
add_executable(tkw-tui
    main.cpp
)

target_link_libraries(tkw-tui PRIVATE ftxui::ftxui pjh_platform)
