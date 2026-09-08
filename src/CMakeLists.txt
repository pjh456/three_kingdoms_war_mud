add_library(tkw_io INTERFACE)

target_include_directories(tkw_io INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_io INTERFACE pjh_result pjh_platform)

add_library(tkw_config INTERFACE)

target_include_directories(tkw_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_config INTERFACE tkw_io pjh_json)

add_library(tkw_entity INTERFACE)

target_include_directories(tkw_entity INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_entity INTERFACE tkw_event pjh_result)

add_library(tkw_event INTERFACE)

target_include_directories(tkw_event INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(tkw_card INTERFACE)

target_include_directories(tkw_card INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_card INTERFACE tkw_config pjh_json)

# game 分层：core（值类型/原语）→ query（只读查询）→ resolve（效果引擎）
# → flow（流程/运行时）；ai 只读依赖前三层。目标仅作分层文档与聚合，头文件
# 经统一 src 根 include 目录引用。

add_library(tkw_game_core INTERFACE)

target_include_directories(tkw_game_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tkw_game_core INTERFACE tkw_entity tkw_card tkw_event pjh_result)

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
                                              tkw_event pjh_result)

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

add_executable(tkw
    main.cpp
)

target_link_libraries(tkw PRIVATE tkw_core pjh_json pjh_cli pjh_platform)
