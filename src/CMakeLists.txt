# TUI 控制器使用 std::jthread / std::atomic：线程库在此探测，经 tkw_tui 传递到
# tkw-tui 与 tui_tests，默认配置的 tkw/引擎目标不链 Threads。
find_package(Threads REQUIRED)

# 通用基础域：随机源实现（`Rng` 接口与 `rng.hpp` 声明在头，定义在 rng.cpp）。
# 仅依赖标准库，故无第三方链接。
add_library(tkw_util STATIC
    util/rng.cpp
)

target_include_directories(tkw_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(tkw_io STATIC
    io/file.cpp
)

target_include_directories(tkw_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_io PUBLIC pjh_result pjh_platform)

add_library(tkw_config STATIC
    config/resource.cpp
    config/fields.cpp
)

target_include_directories(tkw_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_config PUBLIC tkw_io pjh_json)

add_library(tkw_entity STATIC
    entity/base.cpp
    entity/hp.cpp
    entity/manager.cpp
)

target_include_directories(tkw_entity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_entity PUBLIC tkw_event tkw_card pjh_result)

# 事件支撑域：`Event` 虚表锚点与类型标签定义在 event.cpp；总线的模板实体保持头内。
# 无第三方依赖，故无 target_link_libraries。
add_library(tkw_event STATIC
    event/event.cpp
)

target_include_directories(tkw_event PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(tkw_card STATIC
    card/card.cpp
    card/catalog.cpp
    card/manager.cpp
)

target_include_directories(tkw_card PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_card PUBLIC tkw_config pjh_json tkw_util)

# 武将支撑域：值类型/目录加载，复用 entity::Gender；game 层向下引用，武将域不引用 game。
add_library(tkw_hero STATIC
    hero/catalog.cpp
)

target_include_directories(tkw_hero PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_hero PUBLIC tkw_config tkw_entity pjh_json)

# game 分层：core（值类型/原语）→ query（只读查询）→ resolve（效果引擎）
# → flow（流程/运行时）；ai 只读依赖前三层。目标仅作分层文档与聚合，头文件
# 经统一 src 根 include 目录引用。

add_library(tkw_game_core INTERFACE)

target_include_directories(tkw_game_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game_core INTERFACE tkw_entity tkw_card tkw_hero
                                               tkw_event pjh_result tkw_util)

add_library(tkw_game_query INTERFACE)

target_include_directories(tkw_game_query INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game_query INTERFACE tkw_game_core tkw_card pjh_result)

add_library(tkw_game_resolve INTERFACE)

target_include_directories(tkw_game_resolve INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game_resolve INTERFACE tkw_game_core tkw_game_query
                                               tkw_card tkw_entity tkw_event
                                               pjh_result)

add_library(tkw_game_flow INTERFACE)

target_include_directories(tkw_game_flow INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game_flow INTERFACE tkw_game_core tkw_game_query
                                              tkw_game_resolve tkw_card tkw_entity
                                              tkw_event pjh_result tkw_util)

add_library(tkw_game_ai INTERFACE)

target_include_directories(tkw_game_ai INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game_ai INTERFACE tkw_game_core tkw_game_query
                                            tkw_game_resolve tkw_card tkw_entity
                                            tkw_event pjh_result)

add_library(tkw_game INTERFACE)

target_include_directories(tkw_game INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game INTERFACE tkw_game_core tkw_game_query
                                         tkw_game_resolve tkw_game_flow tkw_game_ai
                                         tkw_entity tkw_card tkw_event pjh_result)

add_library(tkw_core INTERFACE)

target_include_directories(tkw_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_core INTERFACE tkw_entity tkw_config tkw_card tkw_game tkw_event tkw_io pjh_result)

add_library(tkw_cli INTERFACE)

target_include_directories(tkw_cli INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_cli INTERFACE tkw_core pjh_cli)

# TUI 纯视图模型与控制器：无 FTXUI、无新外链，只读消费 cli/引擎只读接口；
# 控制器起后台引擎线程，故向消费目标传递 Threads::Threads。
add_library(tkw_tui INTERFACE)

target_include_directories(tkw_tui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_tui INTERFACE tkw_cli Threads::Threads)

add_executable(tkw
    main.cpp
)

target_link_libraries(tkw PRIVATE tkw_cli pjh_json pjh_platform)
