include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.0)

set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(tkw_tests test_main.cpp test_smoke.cpp)

target_link_libraries(tkw_tests PRIVATE tkw_core doctest)

target_compile_definitions(tkw_tests PRIVATE TKW_BUILD_TESTS)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(tkw_tests)

add_executable(entity_tests test_main.cpp entity/hp_test.cpp entity/event_test.cpp
                        entity/effect_test.cpp entity/manager_test.cpp)

target_link_libraries(entity_tests PRIVATE tkw_entity doctest)

doctest_discover_tests(entity_tests)

add_executable(config_tests test_main.cpp config/config_test.cpp)

target_link_libraries(config_tests PRIVATE tkw_config doctest)

doctest_discover_tests(config_tests)

add_executable(io_tests test_main.cpp io/file_test.cpp)

target_link_libraries(io_tests PRIVATE tkw_io doctest)

doctest_discover_tests(io_tests)

add_executable(event_tests test_main.cpp event/event_test.cpp)

target_link_libraries(event_tests PRIVATE tkw_event doctest)

doctest_discover_tests(event_tests)

add_executable(handler_tests test_main.cpp event/handler_test.cpp)

target_link_libraries(handler_tests PRIVATE tkw_event doctest)

doctest_discover_tests(handler_tests)

add_executable(card_tests test_main.cpp card/catalog_test.cpp card/manager_test.cpp)

target_link_libraries(card_tests PRIVATE tkw_card doctest)

target_compile_definitions(
    card_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(card_tests)

add_executable(game_tests test_main.cpp game/game_test.cpp game/replay_test.cpp
                           game/ai_test.cpp)

target_link_libraries(game_tests PRIVATE tkw_game doctest)

target_compile_definitions(
    game_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(game_tests)

add_executable(util_tests test_main.cpp util/rng_test.cpp)

target_link_libraries(util_tests PRIVATE tkw_game doctest)

doctest_discover_tests(util_tests)

add_executable(save_tests test_main.cpp save/save_test.cpp)

target_link_libraries(save_tests PRIVATE tkw_game doctest)

target_compile_definitions(
    save_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(save_tests)

# CLI 端到端：选项可置于子命令名之后，前置写法与未知选项行为不变。
add_test(NAME cli_option_after_subcommand
         COMMAND tkw new --human P0 --players 2 --seed 1)
set_tests_properties(cli_option_after_subcommand PROPERTIES
    PASS_REGULAR_EXPRESSION "新对局已开始"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_option_before_subcommand
         COMMAND tkw --human P0 new)
set_tests_properties(cli_option_before_subcommand PROPERTIES
    PASS_REGULAR_EXPRESSION "新对局已开始"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# repeatable 选项在子命令名前后混写应累积到同一上下文，而非最近声明覆盖。
add_test(NAME cli_human_mixed_positions
         COMMAND tkw --human P0 new --human P1 --players 2 --seed 1)
set_tests_properties(cli_human_mixed_positions PROPERTIES
    PASS_REGULAR_EXPRESSION "真人座位: P0,P1"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# repl 不应被贪婪的 --human 吞掉；非 TTY 无输入时打印一次提示即 EOF 退出。
add_test(NAME cli_repl_human_not_swallowed
         COMMAND tkw --human P0 repl)
set_tests_properties(cli_repl_human_not_swallowed PROPERTIES
    PASS_REGULAR_EXPRESSION "tkw>"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_deal_option_after_positional
         COMMAND tkw deal 2 1 --verbose)
set_tests_properties(cli_deal_option_after_positional PROPERTIES
    PASS_REGULAR_EXPRESSION "胜者|平局"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_unknown_option_still_errors
         COMMAND tkw new --bogus)
set_tests_properties(cli_unknown_option_still_errors PROPERTIES
    WILL_FAIL TRUE
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 执行错误（无会话）应只打印纯中文消息并退出 1，不得带框架统一的 Parse Error: 前缀。
add_test(NAME cli_step_without_session_plain_error
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> step 2>&1); rc=$?; [ \"$out\" = 没有进行中的对局 ] && [ $rc -eq 1 ]")
set_tests_properties(cli_step_without_session_plain_error PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# audit：只钉退出码；标准牌堆的未实现卡集合随引擎演进而变，不钉输出内容。
add_test(NAME cli_audit_ok COMMAND tkw audit)
set_tests_properties(cli_audit_ok PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_audit_bad_deck COMMAND tkw audit --deck /nonexistent-deck-dir)
set_tests_properties(cli_audit_bad_deck PROPERTIES
    WILL_FAIL TRUE
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 批量入口模糊匹配：唯一近距手误自动纠错到 audit；只钉退出码，不钉审计输出内容。
add_test(NAME cli_fuzzy_typo_resolves COMMAND tkw audti)
set_tests_properties(cli_fuzzy_typo_resolves PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 根帮助应中文化段标题并附用法示例；子命令帮助同样用中文用法前缀。
add_test(NAME cli_help_root_zh
         COMMAND sh -c
                 "$<TARGET_FILE:tkw> --help | grep -q '用法:' && $<TARGET_FILE:tkw> --help | grep -q '示例:'")
set_tests_properties(cli_help_root_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_help_subcommand_zh
         COMMAND sh -c "$<TARGET_FILE:tkw> new --help | grep -q '用法:'")
set_tests_properties(cli_help_subcommand_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL ? 应列命令并带中文描述；help <命令> 应走同一中文用法渲染。
add_test(NAME cli_repl_query_and_help_zh
         COMMAND sh -c
                 "out=$(printf '?\\nhelp new\\nquit\\n' | $<TARGET_FILE:tkw> repl); echo \"$out\" | grep -q '审计牌堆' && echo \"$out\" | grep -q '用法:'")
set_tests_properties(cli_repl_query_and_help_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
