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

add_executable(cli_tests test_main.cpp cli/cli_test.cpp)

target_link_libraries(cli_tests PRIVATE tkw_cli doctest)

target_compile_definitions(
    cli_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(cli_tests)

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

# 根帮助应中文化段标题并附用法示例；示例按批量一次性与 REPL 会话分组，会话流
# 命令不带 tkw 前缀（照抄进 shell 会失败）。
add_test(NAME cli_help_root_zh
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --help); echo \"$out\" | grep -q '用法:' && echo \"$out\" | grep -q '选项:' && echo \"$out\" | grep -q '子命令:' && echo \"$out\" | grep -q '示例:' && echo \"$out\" | grep -q 'REPL 会话' && echo \"$out\" | grep -q 'tkw deal 2 1' && echo \"$out\" | grep -q 'tkw audit' && ! echo \"$out\" | grep -q 'tkw new' && ! echo \"$out\" | grep -q 'tkw save'")
set_tests_properties(cli_help_root_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_help_subcommand_zh
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> new --help); echo \"$out\" | grep -q '用法:' && echo \"$out\" | grep -q '公共选项:' && ! echo \"$out\" | grep -q 'Inherited Options'")
set_tests_properties(cli_help_subcommand_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL ? 应列命令并带中文描述；help <命令> 应走同一中文用法渲染，继承选项段同样中文化。
add_test(NAME cli_repl_query_and_help_zh
         COMMAND sh -c
                 "out=$(printf '?\\nhelp new\\nquit\\n' | $<TARGET_FILE:tkw> repl); echo \"$out\" | grep -q '审计牌堆' && echo \"$out\" | grep -q '用法:' && echo \"$out\" | grep -q '公共选项:'")
set_tests_properties(cli_repl_query_and_help_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 内 `new --help` 走 App 帮助格式化器（与批量 --help 同一中文渲染路径）：
# 钉中文段标题齐全且无英文残留。与上面 `?`/`help` 的导航渲染链互为补充，两条链
# 分开验收；渲染改回英文即红，防子模块/渲染回退。
add_test(NAME cli_repl_subcommand_help_zh
         COMMAND sh -c
                 "out=$(printf 'new --help\\nquit\\n' | $<TARGET_FILE:tkw> repl 2>&1); printf '%s' \"$out\" | grep -q '用法:' && printf '%s' \"$out\" | grep -q '选项:' && printf '%s' \"$out\" | grep -q '公共选项:' && ! printf '%s' \"$out\" | grep -q 'Usage:'")
set_tests_properties(cli_repl_subcommand_help_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 无会话执行错误的打印路径（run() → print_error 默认 what()）只输出纯中文
# 消息、不带 Parse Error 前缀；cli_tests 直调 process_line 不经过该路径，E2E 补上。
add_test(NAME cli_repl_runtime_error_no_prefix
         COMMAND sh -c
                 "out=$(printf 'step\\nquit\\n' | $<TARGET_FILE:tkw> repl 2>&1); printf '%s' \"$out\" | grep -q '没有进行中的对局' && ! printf '%s' \"$out\" | grep -q 'Parse Error'")
set_tests_properties(cli_repl_runtime_error_no_prefix PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# verbose 日志可读性：卡牌事件用中文卡名，且补伤害/体力行（seed 固定，输出确定）。
add_test(NAME cli_verbose_log_zh
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --verbose deal 2 1 2>&1); printf '%s' \"$out\" | grep -q '\\[打出\\] P0 杀' && printf '%s' \"$out\" | grep -q '\\[伤害\\] P0 -> P1 1' && printf '%s' \"$out\" | grep -q '\\[体力\\] P1 4->3/4'")
set_tests_properties(cli_verbose_log_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 裸 tkw 与 REPL 各打印一行引导文案。
add_test(NAME cli_bare_root_banner
         COMMAND sh -c "$<TARGET_FILE:tkw> 2>&1 | grep -q '无子命令'")
set_tests_properties(cli_bare_root_banner PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_repl_banner
         COMMAND sh -c
                 "printf 'quit\\n' | $<TARGET_FILE:tkw> repl 2>&1 | grep -q 'quit 退出'")
set_tests_properties(cli_repl_banner PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# raw 提示符层冒烟（依赖 util-linux script 提供 pty；configure 时缺失则整条不注册）：
# statux\\x7fs 退格后应派发 status（会话: 无），证明手动回显与退格可用；
# ne\\x03 在 raw 态被行编辑器接管（echo ^C、清缓冲、重绘提示符、REPL 存活），
# cooked 下 0x03 是 SIGINT（进程死 rc=130、quit 不达），故 ^C 存在 + rc=0 兼作
# raw 态证明。输入必须逐段 sleep 节流——整段一次灌入时 0x03 可能落在启动期或
# action 的 cooked 窗口被内核 SIGINT（实测 flaky）。退出后宿主终端状态不归
# pty 可测范围，须真机手验（清单 V5/V6）。
find_program(TKW_SCRIPT_EXE script)
if(TKW_SCRIPT_EXE)
  add_test(NAME cli_repl_raw_mode_smoke
           COMMAND sh -c
                   "out=$({ sleep 0.3; printf 'statux\\177s\\n'; sleep 0.4; printf 'ne\\003\\n'; sleep 0.4; printf 'quit\\n'; sleep 0.3; } | script -qec '$<TARGET_FILE:tkw> repl' /dev/null 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '会话: 无' && printf '%s' \"$out\" | grep -q '\\^C' && [ $rc -eq 0 ]")
  set_tests_properties(cli_repl_raw_mode_smoke PROPERTIES
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      TIMEOUT 30)
endif()

# REPL 退出自动存档失败必须显式提示，不得静默丢档（父目录缺失 → NotExist，确定性失败）。
add_test(NAME cli_autosave_failure_message
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave /nonexistent-dir/x.json 2>&1); printf '%s' \"$out\" | grep -q '自动存档失败'")
set_tests_properties(cli_autosave_failure_message PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 解析错误保留框架统一前缀并退出 2（执行错误走另一路径，退出 1）。
add_test(NAME cli_parse_error_prefix_and_rc
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> new --bogus 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '^Parse Error: ' && [ $rc -eq 2 ]")
set_tests_properties(cli_parse_error_prefix_and_rc PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 未知子命令落到框架 unknown command 文案并退出 2；近距手误已在更早的分派处纠错或
# 报歧义，故该路径不会给出 did-you-mean 建议。文案由框架渲染，这里只钉关键词与退出码。
add_test(NAME cli_unknown_command_message
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> nosuchcmd 2>&1); rc=$?; printf '%s' \"$out\" | grep -q 'unknown command' && [ $rc -eq 2 ]")
set_tests_properties(cli_unknown_command_message PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
