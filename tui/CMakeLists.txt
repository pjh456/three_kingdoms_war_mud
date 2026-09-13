# 可选 TUI 目标：FTXUI 只在此目标；tkw_tui 提供纯视图模型（传递 tkw_cli/引擎）。
add_executable(tkw-tui
    main.cpp
    app.cpp
    decision_panel.cpp
)

target_link_libraries(tkw-tui PRIVATE ftxui::ftxui tkw_tui pjh_platform)
