include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.0)

set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# doctest 2.5.0 默认前置声明 std::basic_ostream；MSVC 14.4x 的 <string_view> 在
# 该类型不完整时实例化流插入，报 C2027（goodbit/badbit 未声明）。切到标准头模式，
# 让 doctest 先完整包含 <ostream>，无需升级版本。
target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

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

add_executable(hero_tests test_main.cpp hero/catalog_test.cpp)

target_link_libraries(hero_tests PRIVATE tkw_hero tkw_game doctest)

target_compile_definitions(
    hero_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(hero_tests)

add_executable(game_tests test_main.cpp game/game_test.cpp game/replay_test.cpp
                       game/ai_test.cpp game/aggressive_test.cpp
                       game/roles_test.cpp)

target_link_libraries(game_tests PRIVATE tkw_game doctest)

target_compile_definitions(
    game_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(game_tests)

add_executable(util_tests test_main.cpp util/rng_test.cpp)

target_link_libraries(util_tests PRIVATE tkw_game doctest)

doctest_discover_tests(util_tests)

add_executable(save_tests test_main.cpp save/save_test.cpp)

target_link_libraries(save_tests PRIVATE tkw_save tkw_game doctest)

target_compile_definitions(
    save_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(save_tests)

add_executable(cli_tests test_main.cpp cli/cli_test.cpp)

target_link_libraries(cli_tests PRIVATE tkw_cli doctest)

target_compile_definitions(
    cli_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(cli_tests)

add_executable(tui_tests test_main.cpp tui/visibility_test.cpp
                        tui/snapshot_test.cpp tui/log_lines_test.cpp
                        tui/log_scroll_test.cpp tui/command_test.cpp
                        tui/decision_test.cpp tui/controller_test.cpp)

target_link_libraries(tui_tests PRIVATE tkw_tui doctest)

target_compile_definitions(
    tui_tests PRIVATE TKW_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources")

doctest_discover_tests(tui_tests)

# CLI 端到端（跨平台）：以下用例直跑二进制、不含 shell，Windows 侧亦注册。

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

# audit：钉退出码与结果关键词；未实现卡集合随引擎演进而变，不钉具体卡名。
add_test(NAME cli_audit_ok COMMAND tkw audit)
set_tests_properties(cli_audit_ok PROPERTIES
    PASS_REGULAR_EXPRESSION "牌堆全部可结算|未实现卡"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 批量入口模糊匹配：唯一近距手误自动纠错到 audit；钉纠错后真跑到 audit。
add_test(NAME cli_fuzzy_typo_resolves COMMAND tkw audti)
set_tests_properties(cli_fuzzy_typo_resolves PROPERTIES
    PASS_REGULAR_EXPRESSION "牌堆全部可结算|未实现卡"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ── 以下 E2E 依赖 POSIX shell（sh/grep/awk/printf/mktemp/ln -s）与 util-linux
#    script；Windows 无这些工具，整段仅在 Unix 注册。Windows 侧由上方直跑二进制
#    的 E2E 与全部 doctest 目标覆盖。
if(UNIX)

# 执行错误（无会话）应只打印纯中文消息并退出 1，不得带解析错误前缀（中文或英文）。
add_test(NAME cli_step_without_session_plain_error
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> step 2>&1); rc=$?; [ \"$out\" = \"没有进行中的对局（先运行 new 开局，或进入 tkw repl）\" ] && [ $rc -eq 1 ]")
set_tests_properties(cli_step_without_session_plain_error PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 读档失败文案可操作化：中文根因标签 + 保留路径，rc 仍 1。
add_test(NAME cli_load_missing_file_reports_reason
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> load /nonexistent-tkw-save.json 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '读取存档失败（文件不存在）' && printf '%s' \"$out\" | grep -q '/nonexistent-tkw-save.json' && [ $rc -eq 1 ]")
set_tests_properties(cli_load_missing_file_reports_reason PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_audit_bad_deck
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> audit --deck /nonexistent-deck-dir 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '加载牌堆失败（文件不存在）' && printf '%s' \"$out\" | grep -q '/nonexistent-deck-dir/deck.json' && [ $rc -eq 1 ]")
set_tests_properties(cli_audit_bad_deck PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# audit 容错报告未知机制名：严格加载拒载，audit 仍须逐卡列出并 rc=0（红→绿主证）。
add_test(NAME cli_audit_reports_unknown_mechanism
         COMMAND sh -c
                 "d=$(mktemp -d); mkdir -p $d/cards; \
                  printf '%s' '{\"name\":\"mini\",\"cards\":[\"ghost\"]}' > $d/deck.json; \
                  printf '%s' '{\"id\":\"ghost\",\"name\":\"幽魂\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":1}],\"effect\":{\"kind\":\"summon\",\"amount\":1,\"scope\":\"one_other\"}}' > $d/cards/ghost.json; \
                  out=$($<TARGET_FILE:tkw> audit --deck $d 2>&1); rc=$?; rm -rf $d; \
                  printf '%s' \"$out\" | grep -q '未实现卡' && \
                  printf '%s' \"$out\" | grep -q '幽魂(ghost)' && [ $rc -eq 0 ]")
set_tests_properties(cli_audit_reports_unknown_mechanism PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# cards：列出牌表。钉牌堆名与首卡「中文名(id) 类别 张数」形态；种类/张数随
# 牌表演进，不钉全表。
add_test(NAME cli_cards_list
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> cards 2>&1); printf '%s' \"$out\" | grep -q '标准版' && printf '%s' \"$out\" | grep -q '杀(sha) 基本 30'")
set_tests_properties(cli_cards_list PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# decks：列出可用牌表。钉两副内置牌表的中文名与种类/张数（数值由目录加载派生），
# 缺省扫描根 resources，rc=0。
add_test(NAME cli_decks_list
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> decks 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '标准版 32 种/108 张' && printf '%s' \"$out\" | grep -q '军争篇 17 种/83 张' && [ $rc -eq 0 ]")
set_tests_properties(cli_decks_list PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# decks --help：可发现性（中文用法段 + 示例）并提示用 --deck 选定牌表。
add_test(NAME cli_decks_help
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> decks --help); printf '%s' \"$out\" | grep -q '用法:' && printf '%s' \"$out\" | grep -q 'tkw decks' && printf '%s' \"$out\" | grep -q -- '--deck' && printf '%s' \"$out\" | grep -q '示例:'")
set_tests_properties(cli_decks_help PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# heroes：列出可用武将。钉张飞/关羽/周瑜/司马懿/马超/黄月英/赵云/甄姬行与技能；已实现技能不再标注「未实现」；缺 heroes.json 回落空数据不报错。
add_test(NAME cli_heroes_list
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> heroes 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '可用武将' && printf '%s' \"$out\" | grep -q '张飞(zhangfei)' && printf '%s' \"$out\" | grep -q '技能: 咆哮' && printf '%s' \"$out\" | grep -q '关羽(guanyu)' && printf '%s' \"$out\" | grep -q '技能: 武圣' && ! printf '%s' \"$out\" | grep -q '武圣（未实现）' && printf '%s' \"$out\" | grep -q '周瑜(zhouyu)' && printf '%s' \"$out\" | grep -q '技能: 英姿' && ! printf '%s' \"$out\" | grep -q '英姿（未实现）' && printf '%s' \"$out\" | grep -q '司马懿(simayi)' && printf '%s' \"$out\" | grep -q '技能: 反馈' && ! printf '%s' \"$out\" | grep -q '反馈（未实现）' && printf '%s' \"$out\" | grep -q '马超(machao)' && printf '%s' \"$out\" | grep -q '技能: 马术' && ! printf '%s' \"$out\" | grep -q '马术（未实现）' && printf '%s' \"$out\" | grep -q '黄月英(huangyueying)' && printf '%s' \"$out\" | grep -q '技能: 奇才' && ! printf '%s' \"$out\" | grep -q '奇才（未实现）' && printf '%s' \"$out\" | grep -q '赵云(zhaoyun)' && printf '%s' \"$out\" | grep -q '技能: 龙胆' && ! printf '%s' \"$out\" | grep -q '龙胆（未实现）' && printf '%s' \"$out\" | grep -q '甄姬(zhenji)' && printf '%s' \"$out\" | grep -q '技能: 倾国' && ! printf '%s' \"$out\" | grep -q '倾国（未实现）' && [ $rc -eq 0 ]")
set_tests_properties(cli_heroes_list PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_heroes_missing
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> heroes --deck resources/junzheng 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '无武将数据' && [ $rc -eq 0 ]")
set_tests_properties(cli_heroes_missing PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_heroes_help
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> heroes --help); printf '%s' \"$out\" | grep -q '用法:' && printf '%s' \"$out\" | grep -q 'tkw heroes' && printf '%s' \"$out\" | grep -q '示例:'")
set_tests_properties(cli_heroes_help PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --hero：指定座位武将后 status 逐座展示武将；无武将的默认局面不新增武将列。
add_test(NAME cli_hero_status
         COMMAND sh -c
                 "out=$(printf 'new --hero P0=zhangfei --players 2 --seed 1\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '武将 张飞' && printf '%s' \"$out\" | grep -q '武将 无' && [ $rc -eq 0 ]")
set_tests_properties(cli_hero_status PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_hero_default_no_column
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); ! printf '%s' \"$out\" | grep -q '武将'")
set_tests_properties(cli_hero_default_no_column PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 英姿武将：选定周瑜后 status 展示武将，且不产生未实现技能警告。
add_test(NAME cli_hero_zhouyu_status
         COMMAND sh -c
                 "out=$(printf 'new --hero P0=zhouyu --players 2 --seed 1\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '武将 周瑜' && ! printf '%s' \"$out\" | grep -q '未实现的技能' && [ $rc -eq 0 ]")
set_tests_properties(cli_hero_zhouyu_status PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --hero 未知 id：中文错误说明座位并指向 tkw heroes，rc=1。
add_test(NAME cli_hero_unknown
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --hero P0=nobody new --players 2 --seed 1 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '武将不存在' && printf '%s' \"$out\" | grep -q 'nobody' && printf '%s' \"$out\" | grep -q 'P0' && [ $rc -eq 1 ]")
set_tests_properties(cli_hero_unknown PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --hero 格式/座位越界：解析期中文错误，rc=1。
add_test(NAME cli_hero_bad_assignment
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --hero P0 new --players 2 --seed 1 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '座位=武将' && [ $rc -eq 1 ] && \
                  out2=$($<TARGET_FILE:tkw> --hero P9=zhangfei new --players 2 --seed 1 2>&1); rc2=$?; printf '%s' \"$out2\" | grep -q '超出玩家数' && [ $rc2 -eq 1 ]")
set_tests_properties(cli_hero_bad_assignment PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --hero 前导零座位与规范写法等价：P00 命中 P0 并在 status 展示武将，rc=0。
add_test(NAME cli_hero_leading_zero
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --hero P00=zhangfei new --players 2 --seed 1 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '武将 张飞' && [ $rc -eq 0 ]")
set_tests_properties(cli_hero_leading_zero PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# load 行内显式 --hero：中文拒绝（rc=1，含替代命令），且早于读文件不报不存在。
add_test(NAME cli_hero_load_rejected
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> load /nonexistent-tkw-save.json --hero P0=zhangfei 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '不支持 --hero' && printf '%s' \"$out\" | grep -q 'new --hero' && ! printf '%s' \"$out\" | grep -q '文件不存在' && [ $rc -eq 1 ]")
set_tests_properties(cli_hero_load_rejected PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# load 帮助面同步拒绝口径，便于在 --help 处直接发现。
add_test(NAME cli_hero_load_help_hint
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> load --help); printf '%s' \"$out\" | grep -q '不支持 --hero' && printf '%s' \"$out\" | grep -q 'new --hero'")
set_tests_properties(cli_hero_load_help_hint PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 同局多座不同武将：status 逐座展示各自的武将名，rc=0。
add_test(NAME cli_hero_multi_seat_status
         COMMAND sh -c
                 "out=$(printf 'new --hero P0=zhangfei --hero P1=guanyu --players 2 --seed 1\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '武将 张飞' && printf '%s' \"$out\" | grep -q '武将 关羽' && [ $rc -eq 0 ]")
set_tests_properties(cli_hero_multi_seat_status PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# identity + --hero：角色列与武将列同显且互不覆盖，rc=0。
add_test(NAME cli_hero_identity_status
         COMMAND sh -c
                 "out=$(printf 'new --mode identity --players 4 --seed 1 --hero P0=zhangfei --hero P1=guanyu\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '模式: 身份局' && printf '%s' \"$out\" | grep -q '角色 主公' && printf '%s' \"$out\" | grep -q '武将 张飞' && printf '%s' \"$out\" | grep -q '武将 关羽' && [ $rc -eq 0 ]")
set_tests_properties(cli_hero_identity_status PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 扩展牌表骨架经既有 --deck 目录选择可发现：头行显示牌堆名，逐卡行显示基础杀、
# 属性杀、防具与武器及其权威张数；默认牌表不动。
add_test(NAME cli_junzheng_cards
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> cards --deck resources/junzheng 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '军争篇' && printf '%s' \"$out\" | grep -q '杀(sha) 基本 30' && printf '%s' \"$out\" | grep -q '火杀(huosha) 基本 5' && printf '%s' \"$out\" | grep -q '雷杀(leisha) 基本 9' && printf '%s' \"$out\" | grep -q '藤甲(tengjia) 装备 2' && printf '%s' \"$out\" | grep -q '酒(jiu) 基本 5' && printf '%s' \"$out\" | grep -q '兵粮寸断(bingliang) 锦囊 2' && printf '%s' \"$out\" | grep -q '古锭刀(guding) 装备 1' && printf '%s' \"$out\" | grep -q '白银狮子(silver_lion) 装备 1' && printf '%s' \"$out\" | grep -q '铁索连环(tiesuo) 锦囊 6' && printf '%s' \"$out\" | grep -q '火攻(huogong) 锦囊 3' && printf '%s' \"$out\" | grep -q '朱雀羽扇(zhuque_shan) 装备 1' && [ $rc -eq 0 ]")
set_tests_properties(cli_junzheng_cards PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 扩展牌表骨架审计：属性杀走既有 Damage 路径，全部可结算；rc=0。
add_test(NAME cli_junzheng_audit
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> audit --deck resources/junzheng 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '牌堆全部可结算' && [ $rc -eq 0 ]")
set_tests_properties(cli_junzheng_audit PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 扩展牌表骨架可独立开局跑到底：属性杀只有伤害路径，正常产出胜者/平局。
add_test(NAME cli_junzheng_deal
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 2 1 --deck resources/junzheng 2>&1); rc=$?; printf '%s' \"$out\" | grep -qE '胜者|平局' && [ $rc -eq 0 ]")
set_tests_properties(cli_junzheng_deal PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# cards --text：逐卡附 CardDef.text 效果文案（数据源同一，默认不输出）。
add_test(NAME cli_cards_text
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> cards --text 2>&1); printf '%s' \"$out\" | grep -q '杀(sha) 基本 30: 出牌阶段限一次' && printf '%s' \"$out\" | grep -q '目标需打出一张「闪」'")
set_tests_properties(cli_cards_text PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# rules：卡牌效果单一入口，列出全部卡文；关键词过滤只留命中卡。
add_test(NAME cli_rules_lookup
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> rules 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '牌表: resources' && printf '%s' \"$out\" | grep -q '卡牌说明（' && printf '%s' \"$out\" | grep -q '杀(sha): ' && [ $rc -eq 0 ]")
set_tests_properties(cli_rules_lookup PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_rules_keyword
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> rules 过河拆桥 2>&1); printf '%s' \"$out\" | grep -q '过河拆桥(guohe): 出牌阶段' && printf '%s' \"$out\" | grep -q '弃置其区域内的一张牌' && ! printf '%s' \"$out\" | grep -q '杀(sha):'")
set_tests_properties(cli_rules_keyword PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_rules_no_match
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> rules 不存在zzz 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '没有匹配的卡牌说明' && [ $rc -eq 0 ]")
set_tests_properties(cli_rules_no_match PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_cards_bad_deck
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> cards --deck /nonexistent-deck-dir 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '加载牌堆失败（文件不存在）' && printf '%s' \"$out\" | grep -q '/nonexistent-deck-dir/deck.json' && [ $rc -eq 1 ]")
set_tests_properties(cli_cards_bad_deck PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 坏牌堆 JSON：配置层 ParseError 中文标签，保留文件路径，rc 仍 1。
add_test(NAME cli_cards_bad_deck_json
         COMMAND sh -c
                 "d=$(mktemp -d); printf '%s' '{not json' > $d/deck.json; \
                  out=$($<TARGET_FILE:tkw> cards --deck $d 2>&1); rc=$?; rm -rf $d; \
                  printf '%s' \"$out\" | grep -Fq '加载牌堆失败（JSON 非法）' && [ $rc -eq 1 ]")
set_tests_properties(cli_cards_bad_deck_json PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 短别名精确命中：st → status（无会话时打印会话: 无并退出 0），不再落入模糊歧义。
add_test(NAME cli_alias_shorthand
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> st 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '会话: 无' && [ $rc -eq 0 ]")
set_tests_properties(cli_alias_shorthand PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 根帮助子命令表应展示别名（框架括号展示），保批量侧可发现性。
add_test(NAME cli_alias_help_discoverable
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --help); printf '%s' \"$out\" | grep -q 'status (st)' && printf '%s' \"$out\" | grep -q 'save (w)' && printf '%s' \"$out\" | grep -q 'load (l)' && printf '%s' \"$out\" | grep -q 'run (r)'")
set_tests_properties(cli_alias_help_discoverable PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 批量入口近距手误仍报歧义而非静默纠错（card → 多候选，退出 2）；候选集随
# 命令面演进，钉「歧义关键词 + 退出 2」，不钉候选名单。
add_test(NAME cli_ambiguous_prefix_still_errors
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> card 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '有歧义' && [ $rc -eq 2 ]")
set_tests_properties(cli_ambiguous_prefix_still_errors PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 内别名直达：st 查看状态、r 跑到结束（seed 1 2p 确定性胜者）；无歧义残留。
# 自动存档指到构建目录，避免污染源码树。
add_test(NAME cli_repl_alias_dispatch
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nst\\nr\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave ${CMAKE_CURRENT_BINARY_DIR}/tkw-alias-e2e.json 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '会话: 进行中' && printf '%s' \"$out\" | grep -q '胜者' && ! printf '%s' \"$out\" | grep -q 'ambiguous' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_alias_dispatch PROPERTIES
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

# 帮助选项描述标注默认值（纯文字，零解析行为）。断言取描述子串，避开全/半角括号差异。
add_test(NAME cli_help_option_defaults_zh
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> new --help); printf '%s' \"$out\" | grep -q '默认 4' && printf '%s' \"$out\" | grep -q '默认 42' && printf '%s' \"$out\" | grep -q '默认 resources' && printf '%s' \"$out\" | grep -q '默认 simple' && printf '%s' \"$out\" | grep -q 'tkw-autosave.json' && printf '%s' \"$out\" | grep -q -- '--history'")
set_tests_properties(cli_help_option_defaults_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# --mode 可发现性：根帮助列出选项并标注默认 brawl（描述文字，非 .default_value）。
add_test(NAME cli_help_mode_option
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --help); printf '%s' \"$out\" | grep -q -- '--mode' && printf '%s' \"$out\" | grep -q '默认 brawl'")
set_tests_properties(cli_help_mode_option PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 高频 leaf 命令 --help 附「示例:」段，含与当前选项面一致的示例行；非高频 leaf
# （repl）不附；REPL 内 help <命令> 走同一渲染，示例同样可见。
add_test(NAME cli_help_leaf_examples_zh
         COMMAND sh -c
                 "for c in new deal load save run step simulate cards decks audit rules; do out=$($<TARGET_FILE:tkw> $c --help); printf '%s' \"$out\" | grep -q '示例:' || exit 1; done && $<TARGET_FILE:tkw> deal --help | grep -q 'tkw --ai aggressive deal 2 1' && $<TARGET_FILE:tkw> cards --help | grep -q 'tkw cards --text' && $<TARGET_FILE:tkw> decks --help | grep -q 'tkw --deck resources/junzheng cards' && $<TARGET_FILE:tkw> rules --help | grep -q 'tkw rules 过河拆桥' && ! $<TARGET_FILE:tkw> repl --help | grep -q '示例:' && rpl=$(printf 'help new\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave ''); printf '%s' \"$rpl\" | grep -q 'tkw new --players 2 --seed 1'")
set_tests_properties(cli_help_leaf_examples_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL ? 应列命令并带中文描述；help <命令> 应走同一中文用法渲染，继承选项段同样中文化。
add_test(NAME cli_repl_query_and_help_zh
         COMMAND sh -c
                 "out=$(printf '?\\nhelp new\\nquit\\n' | $<TARGET_FILE:tkw> repl); echo \"$out\" | grep -q '审计牌堆' && echo \"$out\" | grep -q '用法:' && echo \"$out\" | grep -q '公共选项:'")
set_tests_properties(cli_repl_query_and_help_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL ? <关键词>（带空格，README/用法示例教的形态）应走子串命中列表渲染，不是
# 模糊建议；退出行带前导空格应正常终止循环。钉 shipped 二进制，防子模块回退。
add_test(NAME cli_repl_query_padded_keyword
         COMMAND sh -c
                 "out=$(printf '? save\\n? aud\\n quit\\n' | $<TARGET_FILE:tkw> repl 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '匹配命令' && printf '%s' \"$out\" | grep -q 'save' && printf '%s' \"$out\" | grep -q 'audit' && ! printf '%s' \"$out\" | grep -q '您是否要找' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_query_padded_keyword PROPERTIES
     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 退出行带行尾空白（`quit `）应同样终止循环：不落入模糊歧义、其后输入行
# 不被派发（会话摘要「会话: 无」仅首行 st 一处）。钉 shipped 二进制，防子模块回退。
add_test(NAME cli_repl_quit_trailing_blank
         COMMAND sh -c
                 "out=$(printf 'st\\nquit \\nst\\n' | $<TARGET_FILE:tkw> repl 2>&1); rc=$?; [ $rc -eq 0 ] && ! printf '%s' \"$out\" | grep -q 'ambiguous' && [ \"$(printf '%s' \"$out\" | grep -c '会话: 无')\" -eq 1 ]")
set_tests_properties(cli_repl_quit_trailing_blank PROPERTIES
     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 内 `new --help` 走 App 帮助格式化器（与批量 --help 同一中文渲染路径）：
# 钉中文段标题齐全且无英文残留。与上面 `?`/`help` 的导航渲染链互为补充，两条链
# 分开验收；渲染改回英文即红，防子模块/渲染回退。
add_test(NAME cli_repl_subcommand_help_zh
         COMMAND sh -c
                 "out=$(printf 'new --help\\nquit\\n' | $<TARGET_FILE:tkw> repl 2>&1); printf '%s' \"$out\" | grep -q '用法:' && printf '%s' \"$out\" | grep -q '选项:' && printf '%s' \"$out\" | grep -q '公共选项:' && ! printf '%s' \"$out\" | grep -q 'Usage:'")
set_tests_properties(cli_repl_subcommand_help_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 无会话执行错误的打印路径（run() → print_error → 注入的 error formatter）
# 只输出纯中文消息、不带解析错误前缀；cli_tests 直调 process_line 不经过该路径，E2E 补上。
add_test(NAME cli_repl_runtime_error_no_prefix
         COMMAND sh -c
                 "out=$(printf 'step\\nquit\\n' | $<TARGET_FILE:tkw> repl 2>&1); printf '%s' \"$out\" | grep -q '没有进行中的对局' && ! printf '%s' \"$out\" | grep -qE 'Parse Error|参数错误'")
set_tests_properties(cli_repl_runtime_error_no_prefix PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 解析错误经注入的 error formatter 渲染为中文（批量侧已由
# cli_parse_error_prefix_and_rc/cli_unknown_command_message 覆盖）。
# REPL 内解析失败不改变进程 rc（仍 0），故钉 rc=0 + 中文 + 无英文正文。
add_test(NAME cli_repl_parse_error_zh
         COMMAND sh -c
                 "out=$(printf 'new --bogus\\nquit\\n' | $<TARGET_FILE:tkw> repl 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '参数错误: ' && printf '%s' \"$out\" | grep -q '未知选项' && ! printf '%s' \"$out\" | grep -q 'unknown option' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_parse_error_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# verbose 日志可读性：卡牌事件用中文卡名，且补伤害/体力行（seed 固定，输出确定）。
add_test(NAME cli_verbose_log_zh
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --verbose deal 2 1 2>&1); printf '%s' \"$out\" | grep -q '\\[打出\\] P0 杀' && printf '%s' \"$out\" | grep -q '\\[伤害\\] P0 -> P1 1' && printf '%s' \"$out\" | grep -q '\\[体力\\] P1 4->3/4' && ! printf '%s' \"$out\" | grep -q '\\[弃置\\]  '")
set_tests_properties(cli_verbose_log_zh PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 青龙偃月刀续杀只发一条打出日志：seed 5 实跑含「P0 杀 → P1 闪 → P0 杀」续杀
# 三连；打出的续杀进弃牌堆不另发弃置事件，故该三连后不得紧跟同角色 [弃置] 杀。
add_test(NAME cli_verbose_qinglong_single_line
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --verbose deal 2 5 2>&1); printf '%s' \"$out\" | awk '{ if (p2 ~ /^\\[打出\\] P[0-9]+ 杀$/ && p1 ~ /^\\[打出\\] P[0-9]+ 闪$/ && $0 ~ /^\\[打出\\] P[0-9]+ 杀$/) triple=1; if (waiting && $0 == \"[弃置] \" attacker \" 杀\") bad=1; waiting=0; if ($0 ~ /^\\[打出\\] P[0-9]+ 杀$/) { attacker=$2; waiting=1 } p2=p1; p1=$0 } END { exit !(triple && !bad) }'")
set_tests_properties(cli_verbose_qinglong_single_line PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# verbose 移牌行覆盖区域转移：seed 固定，deal 4 42 含装备/顺手/借刀/延时/闪电/五谷。
add_test(NAME cli_verbose_card_moved
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --verbose deal 4 42 2>&1); printf '%s' \"$out\" | grep -q '\\[移牌\\]' && printf '%s' \"$out\" | grep -q '\\[移牌\\] (无)(临时区) -> P' && printf '%s' \"$out\" | grep -q '\\[移牌\\] P[0-9](装备区) -> P[0-9](手牌)'")
set_tests_properties(cli_verbose_card_moved PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --verbose 可关：启动 --verbose 后 new 继承日志（初始摸牌有 [摸牌]），行内
# --no-verbose 只关本次命令的日志，step 不再打印 [打出] 等事件。
add_test(NAME cli_verbose_negatable
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep --no-verbose\\nquit\\n' | $<TARGET_FILE:tkw> --verbose repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '\\[摸牌\\]' && ! printf '%s' \"$out\" | grep -q '\\[打出\\]' && ! printf '%s' \"$out\" | grep -q 'unknown option' && [ $rc -eq 0 ]")
set_tests_properties(cli_verbose_negatable PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 真人局默认可见事件日志：未显式提供 verbose 时开启；启动 --no-verbose 可关闭；
# 全 AI 局仍静默（不出现事件标签）。真人 step 会进入决策窗口读走 quit，退出码不钉。
add_test(NAME cli_repl_human_default_verbose
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '\\[摸牌\\]' && \
                  quiet=$(printf 'new --players 2 --seed 1\\nstep\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 --no-verbose repl --autosave '' 2>&1); ! printf '%s' \"$quiet\" | grep -q '\\[摸牌\\]' && \
                  ai=$(printf 'new --players 2 --seed 1\\nstep\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); ! printf '%s' \"$ai\" | grep -q '\\[摸牌\\]'")
set_tests_properties(cli_repl_human_default_verbose PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# new 自身的初始发牌也走真人默认日志：脚本只 new 不 step，直接钉初始发牌行，
# 避免与 step 摸牌混同；非真人座位 P1 的摸牌遮挡为「未知牌」，钉 new 把真人
# 座位集合透传给日志订阅；显式 --no-verbose 与全 AI 仍静默。
add_test(NAME cli_repl_new_human_default_verbose
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 repl --autosave '' 2>&1) && \
                  printf '%s' \"$out\" | grep -q '\\[摸牌\\] P0 ' && \
                  printf '%s' \"$out\" | grep -q '\\[摸牌\\] P1 未知牌' && \
                  quiet=$(printf 'new --players 2 --seed 1\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 --no-verbose repl --autosave '' 2>&1) && \
                  ! printf '%s' \"$quiet\" | grep -q '\\[摸牌\\]' && \
                  ai=$(printf 'new --players 2 --seed 1\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1) && \
                  ! printf '%s' \"$ai\" | grep -q '\\[摸牌\\]'")
set_tests_properties(cli_repl_new_human_default_verbose PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 一次性真人局默认事件日志：deal 与建局同口径，显式 --no-verbose 优先，
# 全 AI 一键局保持静默。真人 EOF 可能回合失败，故不钉 rc、只钉文本。
add_test(NAME cli_deal_human_default_verbose
         COMMAND sh -c
                 "loud=$(printf '' | $<TARGET_FILE:tkw> --human P0 deal 2 1 2>&1); printf '%s' \"$loud\" | grep -q '—— 回合 1：P0 ——' && \
                  quiet=$(printf '' | $<TARGET_FILE:tkw> --human P0 --no-verbose deal 2 1 2>&1); ! printf '%s' \"$quiet\" | grep -q '—— 回合' && \
                  ai=$(printf '' | $<TARGET_FILE:tkw> deal 2 1 2>&1); ! printf '%s' \"$ai\" | grep -q '\\['")
set_tests_properties(cli_deal_human_default_verbose PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 真人局日志隐藏对手摸牌牌名：AI 座位摸牌渲染「未知牌」，真人座位仍出牌名。
# deal 跑全回合，初始发牌即覆盖 P1 的进手牌；EOF 可能中途失败，故只钉文本不钉 rc。
# 判定只做锚定整行的字节比较，不由 awk 按字段号取多字节行的 $3：字段切分与字符
# 比较在 BSD awk 上跨平台口径不一致；失败时回显原始输出便于跨平台诊断。
add_test(NAME cli_human_event_log_hides_opponent_draws
         COMMAND sh -c
                 "out=$(printf '' | $<TARGET_FILE:tkw> --human P0 --verbose deal 2 1 2>&1); \
                  dump() { printf '%s\\n' '--- tkw output ---' >&2; printf '%s\\n' \"$out\" >&2; }; \
                  p1=$(printf '%s\\n' \"$out\" | grep '^\\[摸牌\\] P1 '); \
                  p0=$(printf '%s\\n' \"$out\" | grep '^\\[摸牌\\] P0 '); \
                  if [ -z \"$p1\" ]; then echo 'FAIL: P1 无摸牌日志' >&2; dump; exit 1; fi; \
                  if printf '%s\\n' \"$p1\" | grep -qv '^\\[摸牌\\] P1 未知牌$'; then echo 'FAIL: P1 摸牌行泄露牌名' >&2; dump; exit 1; fi; \
                  if [ -z \"$p0\" ]; then echo 'FAIL: P0 无摸牌日志' >&2; dump; exit 1; fi; \
                  if printf '%s\\n' \"$p0\" | grep -q '^\\[摸牌\\] P0 未知牌'; then echo 'FAIL: P0 摸牌行被错误遮挡' >&2; dump; exit 1; fi; \
                  exit 0")
set_tests_properties(cli_human_event_log_hides_opponent_draws PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 裸 tkw 走根 action，覆盖第二条 run_game 调用点：真人座位默认开日志与回合头。
add_test(NAME cli_bare_human_default_verbose
         COMMAND sh -c
                 "out=$(printf '' | $<TARGET_FILE:tkw> --human P0 --players 2 --seed 1 2>&1); printf '%s' \"$out\" | grep -q '—— 回合 1：P0 ——'")
set_tests_properties(cli_bare_human_default_verbose PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 决策窗口可发现性：出牌窗口列出己方完整手牌（含不可出的闪/无懈），
# `card <序号>` 打印候选卡文后再决定；对手手牌仍只给数量。
# seed 固定 → 手牌序与候选序确定；窗口内 quit 会被吞，故末行 step 结束后再退出。
add_test(NAME cli_repl_human_card_lookup
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep\\ncard 1\\npass\\ncard 1\\ndiscard 1 2\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 --no-verbose repl --autosave '' 2>&1); rc=$?; \
                  printf '%s' \"$out\" | grep -q '\\[P0\\] 体力 4/4  手牌 无懈可击/五谷丰登/无中生有/闪/顺手牵羊/杀' && \
                  printf '%s' \"$out\" | grep -q '五谷丰登：出牌阶段' && \
                  printf '%s' \"$out\" | grep -q '无懈可击：抵消一张锦囊牌' && \
                  printf '%s' \"$out\" | grep -q '? 看用法，card <序号> 看牌面' && \
                  printf '%s' \"$out\" | grep -q 'P1 体力 4/4  手牌 4' && \
                  ! printf '%s' \"$out\" | grep -q 'P1 体力 4/4  手牌 无懈' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_human_card_lookup PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 弃牌窗口出口：`pass` 不在弃牌阶段有效，提示必须弃满 N 张的下一步。
# seed 固定 → P0 首回合弃牌数确定（手牌超上限，需弃 2 张）。
add_test(NAME cli_repl_human_discard_pass_hint
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep\\npass\\npass\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 repl --autosave '' 2>&1); \
                  printf '%s' \"$out\" | grep -q '不能 pass' && \
                  printf '%s' \"$out\" | grep -q '需弃'")
set_tests_properties(cli_repl_human_discard_pass_hint PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 决策窗口可发现性：出牌窗口输入 `?` 打印该窗口的输入用法（pass 的语义）。
add_test(NAME cli_repl_human_decision_help
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep\\n?\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 repl --autosave '' 2>&1); \
                  printf '%s' \"$out\" | grep -q '用法：' && \
                  printf '%s' \"$out\" | grep -q 'pass 结束出牌阶段'")
set_tests_properties(cli_repl_human_decision_help PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 弃牌 EOF：窗口吃 quit 非法后 EOF，打印按放弃处理，引擎报弃牌不足并给
# 「可继续推进」恢复引导（失败回合已被引擎消费并推进，下次 step 换角色）。
add_test(NAME cli_repl_human_discard_eof_recovery
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep\\npass\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 repl --autosave '' 2>&1); \
                  printf '%s' \"$out\" | grep -q '弃牌数量不足' && \
                  printf '%s' \"$out\" | grep -q '输入已结束，按放弃处理' && \
                  printf '%s' \"$out\" | grep -q '可继续推进'")
set_tests_properties(cli_repl_human_discard_eof_recovery PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 过程可见时 step/run 打印回合头，含回合序号与当前玩家；全 AI 默认静默无回合头。
add_test(NAME cli_repl_turn_header
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1 --verbose\\nstep\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '—— 回合 1：P0 ——' && \
                  quiet=$(printf 'new --players 2 --seed 1\\nstep\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); ! printf '%s' \"$quiet\" | grep -q '—— 回合'")
set_tests_properties(cli_repl_turn_header PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 日志标签保真：判定翻牌显示 [判定]（含被判定者与判定牌），响应打出显示 [打出]，
# 真实弃置（延时锦囊判定后弃置）仍为 [弃置]。seed 固定，输出确定。
add_test(NAME cli_verbose_judge_response_labels
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --verbose deal 2 1 2>&1); \
                  printf '%s' \"$out\" | grep -q '\\[判定\\] P1 青釭剑' && \
                  printf '%s' \"$out\" | grep -q '\\[打出\\] P0 无懈可击' && \
                  printf '%s' \"$out\" | grep -q '\\[弃置\\] P1 乐不思蜀'")
set_tests_properties(cli_verbose_judge_response_labels PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 击杀奖励摸牌带 [击杀奖励] 标签：seed 3 2p 由玩家击杀（统计用例已钉），
# --verbose 下奖励摸牌必出现该标签，普通摸牌仍为 [摸牌]。
add_test(NAME cli_verbose_kill_reward_label
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --verbose deal 2 3 2>&1); \
                  printf '%s' \"$out\" | grep -q '\\[击杀奖励\\]' && \
                  printf '%s' \"$out\" | grep -q '\\[摸牌\\]'")
set_tests_properties(cli_verbose_kill_reward_label PROPERTIES
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

# REPL 入口引导补「第一局」起手：保留命令列表与退出提示，并给出一条可照抄的建局命令。
add_test(NAME cli_repl_first_game_banner
         COMMAND sh -c
                 "out=$(printf 'quit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '输入 ? 查看命令' && printf '%s' \"$out\" | grep -q 'quit 退出' && printf '%s' \"$out\" | grep -q '第一局' && printf '%s' \"$out\" | grep -q 'new --players 2 --seed 1'")
set_tests_properties(cli_repl_first_game_banner PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 裸 tkw banner 指向真人第一局：保留「无子命令」，并提示先 new 开局。
add_test(NAME cli_bare_root_first_game_hint
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> 2>&1); printf '%s' \"$out\" | grep -q '无子命令' && printf '%s' \"$out\" | grep -q 'new 开局'")
set_tests_properties(cli_bare_root_first_game_hint PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 根帮助示例前置「第一局」三步并指向 rules/card；仍禁带 tkw 前缀的会话流命令。
add_test(NAME cli_help_root_first_game
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --help); printf '%s' \"$out\" | grep -q '第一局' && printf '%s' \"$out\" | grep -q 'rules' && ! printf '%s' \"$out\" | grep -q 'tkw new' && ! printf '%s' \"$out\" | grep -q 'tkw save' && printf '%s' \"$out\" | grep -q 'REPL 会话'")
set_tests_properties(cli_help_root_first_game PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 第一局脚本「说的能做」：seed 1 二人局 P0 首回合手牌 6 > 体力 4 必触发弃牌，
# pass 结束出牌后 discard 1 2 通过；不钉决策窗口提示原文，与窗口文案解耦。
add_test(NAME cli_repl_first_game_script_runs
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstep\\npass\\ndiscard 1 2\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 --no-verbose repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '新对局已开始' && printf '%s' \"$out\" | grep -q '出牌阶段' && printf '%s' \"$out\" | grep -q '弃牌' && printf '%s' \"$out\" | grep -q '会话: 进行中' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_first_game_script_runs PROPERTIES
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

# --history 跨进程持久化：第二进程载入首进程写入的 st，因与末条重复被折叠，
# 文件仍只有一行 st（未载入则追加为两行）。--autosave '' 隔离源码树。
add_test(NAME cli_repl_history_persist
         COMMAND sh -c
                 "f=${CMAKE_CURRENT_BINARY_DIR}/tkw-history-e2e.txt; rm -f $f; \
                  printf 'st\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' --history $f >/dev/null 2>&1; \
                  printf 'st\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' --history $f >/dev/null 2>&1; \
                  n=$(grep -c '^st$' $f); rm -f $f; [ \"$n\" -eq 1 ]")
set_tests_properties(cli_repl_history_persist PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --history 父目录缺失：显式告警并回落内存，不崩。
add_test(NAME cli_repl_history_missing_dir_warns
         COMMAND sh -c
                 "out=$(printf 'quit\\n' | $<TARGET_FILE:tkw> repl --autosave '' --history /nonexistent-dir/x 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '命令历史目录不存在' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_history_missing_dir_warns PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 解析错误保留中文统一前缀并退出 2（执行错误走另一路径，退出 1）。
add_test(NAME cli_parse_error_prefix_and_rc
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> new --bogus 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '^参数错误: ' && [ $rc -eq 2 ]")
set_tests_properties(cli_parse_error_prefix_and_rc PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 未知子命令落到框架 unknown command 文案并退出 2；近距手误已在更早的分派处纠错或
# 报歧义，故该路径不会给出 did-you-mean 建议。文案由项目侧渲染为中文，这里只钉关键词与退出码。
add_test(NAME cli_unknown_command_message
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> nosuchcmd 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '未知命令' && [ $rc -eq 2 ]")
set_tests_properties(cli_unknown_command_message PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 对局统计块：批量跑局结束追加 回合数/胜者/每人体力/击杀/伤害/治疗（追加段，
# 与既有 胜者|平局 关键词断言不冲突）。seed 1 2p 闪电击杀，双方击杀均为 0。
add_test(NAME cli_stats_block_deal
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 2 1 2>&1); printf '%s' \"$out\" | grep -q '对局统计' && printf '%s' \"$out\" | grep -q '回合数' && printf '%s' \"$out\" | grep -q '胜者' && printf '%s' \"$out\" | grep -q '体力' && printf '%s' \"$out\" | grep -q '击杀' && printf '%s' \"$out\" | grep -q '伤害' && printf '%s' \"$out\" | grep -q '治疗'")
set_tests_properties(cli_stats_block_deal PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 对局统计块在 REPL 会话流同样输出（new → run），阵亡者显示「阵亡」；
# 自动存档关空串，避免默认路径把 tkw-autosave.json 写到源码树。
add_test(NAME cli_stats_block_repl_run
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nrun\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '对局统计' && printf '%s' \"$out\" | grep -q '阵亡'")
set_tests_properties(cli_stats_block_repl_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 击杀计数：seed 3 2p 对局由玩家击杀（非闪电），胜者击杀 ≥ 1（确定性）。
add_test(NAME cli_stats_kill_count
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 2 3 2>&1); printf '%s' \"$out\" | grep -qE '击杀 [1-9]'")
set_tests_properties(cli_stats_kill_count PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --ai aggressive：完整跑局 + 统计块（与默认 deal 同关键词口径，仅 AI 档不同）。
add_test(NAME cli_ai_aggressive_deal
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 2 1 --ai aggressive 2>&1); printf '%s' \"$out\" | grep -qE '胜者|平局' && printf '%s' \"$out\" | grep -q '对局统计'")
set_tests_properties(cli_ai_aggressive_deal PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 值域校验：非法 --ai 值解析期拒绝（enum 映射外，Parse Error rc=2）。
add_test(NAME cli_ai_bogus_value_rejected
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 2 1 --ai bogus 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '参数错误' && [ $rc -eq 2 ]")
set_tests_properties(cli_ai_bogus_value_rejected PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 值域校验：非法 --mode 值解析期拒绝（enum 映射外，Parse Error rc=2）。
add_test(NAME cli_mode_bogus_value_rejected
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 2 1 --mode bogus 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '参数错误' && [ $rc -eq 2 ]")
set_tests_properties(cli_mode_bogus_value_rejected PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# simulate 批量模拟：aggressive 档完整跑 20 局，钉汇总关键词与「种子 1..20」
# 缺省基种子文案（Options.seed 缺省 42 不适用于 simulate）。
add_test(NAME cli_simulate_aggressive
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> simulate 20 2 --ai aggressive 2>&1); rc=$?; [ $rc -eq 0 ] && printf '%s' \"$out\" | grep -q '模拟 20 局' && printf '%s' \"$out\" | grep -q '种子 1..20' && printf '%s' \"$out\" | grep -q 'ai=aggressive' && printf '%s' \"$out\" | grep -q '胜' && printf '%s' \"$out\" | grep -q '平均回合'")
set_tests_properties(cli_simulate_aggressive PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 身份局批量模拟：头行标 mode=identity，汇总按阵营输出。
add_test(NAME cli_simulate_identity
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> simulate 3 4 --mode identity 2>&1); rc=$?; [ $rc -eq 0 ] && printf '%s' \"$out\" | grep -q 'mode=identity' && printf '%s' \"$out\" | grep -q '阵营'")
set_tests_properties(cli_simulate_identity PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 确定性：同参数两跑输出逐字节一致（种子完全由基种子 + 局序号决定）。
add_test(NAME cli_simulate_deterministic
         COMMAND sh -c
                 "a=$($<TARGET_FILE:tkw> simulate 3 2 2>&1); b=$($<TARGET_FILE:tkw> simulate 3 2 2>&1); [ \"$a\" = \"$b\" ]")
set_tests_properties(cli_simulate_deterministic PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 坏牌堆目录：错误面与 audit/cards 同款（加载牌堆失败中文标签，rc=1）。
add_test(NAME cli_simulate_bad_deck
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> simulate 1 2 --deck /nonexistent-deck-dir 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '加载牌堆失败（文件不存在）' && printf '%s' \"$out\" | grep -q '/nonexistent-deck-dir/deck.json' && [ $rc -eq 1 ]")
set_tests_properties(cli_simulate_bad_deck PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# audit/simulate 不支持真人：--human 硬拒绝为纯中文错误、给出替代出口并退出 1。
add_test(NAME cli_audit_rejects_human
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --human P0 audit 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '不支持 --human' && printf '%s' \"$out\" | grep -q '请直接运行 tkw audit' && [ $rc -eq 1 ]")
set_tests_properties(cli_audit_rejects_human PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_simulate_rejects_human
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> simulate 1 2 --human P0 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '不支持 --human' && printf '%s' \"$out\" | grep -q '请直接运行 tkw simulate' && [ $rc -eq 1 ]")
set_tests_properties(cli_simulate_rejects_human PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --no-human 清空启动 --human 带入的真人座位（status 行显示「真人座位: 无」）。
add_test(NAME cli_new_no_human_clears
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --human P0 new --no-human --players 2 --seed 1 2>&1); printf '%s' \"$out\" | grep -q '真人座位: 无'")
set_tests_properties(cli_new_no_human_clears PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 启动选项继承不被选项默认值压过：--players 2 启动后 new 应建 2 人局。
add_test(NAME cli_repl_startup_options_inherited
         COMMAND sh -c
                 "out=$(printf 'new\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> --players 2 --seed 1 repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '存活: 2'")
set_tests_properties(cli_repl_startup_options_inherited PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# step 自身声明公共选项：本命令「选项」段（在「公共选项」之前）应含 --players。
add_test(NAME cli_help_step_common_options
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> step --help 2>&1); printf '%s' \"$out\" | awk '/^选项:/{f=1} /^公共选项:/{f=0} f && /--players/{x=1} END{exit !x}'")
set_tests_properties(cli_help_step_common_options PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 存档保留 AI 档与统计：load 后 status 显示存档 AI 档（此处 aggressive）。
add_test(NAME cli_save_load_ai_roundtrip
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1 --ai aggressive\\nstep\\nsave ${CMAKE_CURRENT_BINARY_DIR}/tkw-ai-roundtrip.json\\nnew --players 2 --seed 1\\nload ${CMAKE_CURRENT_BINARY_DIR}/tkw-ai-roundtrip.json\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; rm -f ${CMAKE_CURRENT_BINARY_DIR}/tkw-ai-roundtrip.json; printf '%s' \"$out\" | grep -q 'AI 难度: aggressive' && [ $rc -eq 0 ]")
set_tests_properties(cli_save_load_ai_roundtrip PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 位置参数越界：文案带当前值与合法范围 [2, 8]，rc=1（解析期 --players 走 rc=2 另一路径）。
add_test(NAME cli_deal_players_out_of_range
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> deal 99 1 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '超出范围 [2, 8]' && printf '%s' \"$out\" | grep -q '99' && [ $rc -eq 1 ]")
set_tests_properties(cli_deal_players_out_of_range PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME cli_simulate_players_out_of_range
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> simulate 1 99 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '超出范围 [2, 8]' && printf '%s' \"$out\" | grep -q '99' && [ $rc -eq 1 ]")
set_tests_properties(cli_simulate_players_out_of_range PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 真人座位越界：文案带可用座位，rc=1。
add_test(NAME cli_human_seat_out_of_range
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> --human P9 new --players 2 --seed 1 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '真人座位不存在: P9' && printf '%s' \"$out\" | grep -q '可用座位' && [ $rc -eq 1 ]")
set_tests_properties(cli_human_seat_out_of_range PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# status 结束态：run 后 status 显示已结束与胜者，不再提示下一回合。
# new 的进行中 status 仍含一次「下一回合」，故按出现次数钉（全程仅 1 次）。
add_test(NAME cli_status_finished
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nrun\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '会话: 已结束' && printf '%s' \"$out\" | grep -q '胜者: P0' && [ \"$(printf '%s' \"$out\" | grep -c '下一回合')\" -eq 1 ]")
set_tests_properties(cli_status_finished PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# status 展示真人座位己方手牌牌名；对手行仍只给数量且不泄漏牌名；标准牌表无横置，
# 局面段不得出现横置标记。
add_test(NAME cli_status_human_hand
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 repl --autosave '' 2>&1); rc=$?; \
                  printf '%s' \"$out\" | grep -q 'P0 体力 4/4 手牌 无懈可击/五谷丰登/无中生有/闪 装备 无 判定 无' && \
                  printf '%s' \"$out\" | grep -q 'P1 体力 4/4 手牌 4 装备 无 判定 无' && \
                  ! printf '%s' \"$out\" | grep -q 'P1 体力 4/4 手牌 无懈' && \
                  ! printf '%s' \"$out\" | grep -q '横置' && [ $rc -eq 0 ]")
set_tests_properties(cli_status_human_hand PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 身份局人数不足：建局期文案含当前值与可复制的 --players 建议，退出 1。
add_test(NAME cli_identity_too_few_players
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> new --mode identity --players 2 2>&1); rc=$?; printf '%s' \"$out\" | grep -q '身份模式人数须为' && printf '%s' \"$out\" | grep -q '当前 2' && printf '%s' \"$out\" | grep -q -- '请用 --players 4' && [ $rc -eq 1 ]")
set_tests_properties(cli_identity_too_few_players PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 身份局 REPL 终局：seed 1 4p 主公阵营胜，钉阵营/内奸/平局三关键词之一。
add_test(NAME cli_identity_repl_run_outcome
         COMMAND sh -c
                 "out=$(printf 'new --mode identity --players 4 --seed 1\\nrun\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; printf '%s' \"$out\" | grep -qE '阵营|内奸|平局' && [ $rc -eq 0 ]")
set_tests_properties(cli_identity_repl_run_outcome PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 身份局存读往返：模式与角色随存档恢复，load 后 status 显示身份局模式与逐座角色。
add_test(NAME cli_identity_save_load_roundtrip
         COMMAND sh -c
                 "out=$(printf 'new --mode identity --players 4 --seed 1\\nstep\\nsave ${CMAKE_CURRENT_BINARY_DIR}/tkw-identity-e2e.json\\nnew --players 2 --seed 1\\nload ${CMAKE_CURRENT_BINARY_DIR}/tkw-identity-e2e.json\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); rc=$?; rm -f ${CMAKE_CURRENT_BINARY_DIR}/tkw-identity-e2e.json; printf '%s' \"$out\" | grep -q '模式: 身份局' && printf '%s' \"$out\" | grep -q '角色 主公' && [ $rc -eq 0 ]")
set_tests_properties(cli_identity_save_load_roundtrip PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 身份局真人局 status 角色收敛：只显主公（P0）与真人座位，其余「未知」。
add_test(NAME cli_identity_human_status_hides_roles
         COMMAND sh -c
                 "out=$(printf 'new --mode identity --players 4 --seed 1 --human P0\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> --no-verbose repl --autosave '' 2>&1); rc=$?; \
                  printf '%s' \"$out\" | grep -q '角色 主公' && \
                  printf '%s' \"$out\" | grep -q '角色 未知' && \
                  ! printf '%s' \"$out\" | grep -qE '角色 (忠臣|反贼|内奸)' && [ $rc -eq 0 ]")
set_tests_properties(cli_identity_human_status_hides_roles PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# status 展示本会话牌表来源（默认 resources；REPL 启动选项被行内建局继承）。
add_test(NAME cli_status_deck_source
         COMMAND sh -c
                 "out=$(printf 'new --players 2 --seed 1\\nstatus\\nquit\\n' | $<TARGET_FILE:tkw> repl --autosave '' 2>&1); printf '%s' \"$out\" | grep -q '牌表: resources'")
set_tests_properties(cli_status_deck_source PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 一次性命令牌表来源可见化：cards/audit/simulate 输出头均显示实际所用目录（默认
# resources）；内联与前置 --deck resources 两位置等价，均显示同一来源。
add_test(NAME cli_oneshot_deck_source
         COMMAND sh -c
                 "out=$($<TARGET_FILE:tkw> cards 2>&1); printf '%s' \"$out\" | grep -q '牌表: resources' && \
                  out=$($<TARGET_FILE:tkw> audit 2>&1); printf '%s' \"$out\" | grep -q '牌表: resources' && \
                  out=$($<TARGET_FILE:tkw> simulate 1 2 2>&1); printf '%s' \"$out\" | grep -q '牌表: resources' && \
                  inline=$($<TARGET_FILE:tkw> cards --deck resources 2>&1); printf '%s' \"$inline\" | grep -q '牌表: resources' && \
                  front=$($<TARGET_FILE:tkw> --deck resources cards 2>&1); printf '%s' \"$front\" | grep -q '牌表: resources'")
set_tests_properties(cli_oneshot_deck_source PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 只读命令继承启动 --deck：启动指向自建 mini 牌表（非默认 resources）后，
# cards/rules/audit 输出头应亮明该目录并列出其中的卡。
add_test(NAME cli_repl_readonly_inherit_deck
         COMMAND sh -c
                 "d=$(mktemp -d); mkdir -p $d/cards; printf '%s' '{\"name\":\"inherit\",\"cards\":[\"h0\"]}' > $d/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"继承测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $d/cards/h0.json; \
                  out=$(printf 'cards\\nrules\\naudit\\nquit\\n' | $<TARGET_FILE:tkw> --deck $d repl --autosave '' 2>&1); rc=$?; rm -rf $d; \
                  printf '%s' \"$out\" | grep -Fq \"牌表: $d\" && printf '%s' \"$out\" | grep -q '继承测' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_readonly_inherit_deck PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 负向硬约束：启动 --human 的座位不得被只读命令继承，否则 cards 会被误拒。
add_test(NAME cli_repl_readonly_no_human_inherit
         COMMAND sh -c
                 "d=$(mktemp -d); mkdir -p $d/cards; printf '%s' '{\"name\":\"inherit\",\"cards\":[\"h0\"]}' > $d/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"继承测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $d/cards/h0.json; \
                  out=$(printf 'cards\\nquit\\n' | $<TARGET_FILE:tkw> --human P0 --deck $d repl --autosave '' 2>&1); rc=$?; rm -rf $d; \
                  printf '%s' \"$out\" | grep -Fq \"牌表: $d\" && ! printf '%s' \"$out\" | grep -q '不支持 --human' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_readonly_no_human_inherit PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 行内 --deck 仍优先于启动选项。
add_test(NAME cli_repl_readonly_inline_deck_override
         COMMAND sh -c
                 "d=$(mktemp -d); mkdir -p $d/cards; printf '%s' '{\"name\":\"inherit\",\"cards\":[\"h0\"]}' > $d/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"继承测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $d/cards/h0.json; \
                  out=$(printf 'cards --deck resources\\nquit\\n' | $<TARGET_FILE:tkw> --deck $d repl --autosave '' 2>&1); rc=$?; rm -rf $d; \
                  printf '%s' \"$out\" | grep -q '牌表: resources' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_readonly_inline_deck_override PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# REPL 批量命令（deal/simulate）继承启动 --deck：不存在的目录应在 REPL 内报
# 加载失败（REPL 错误不改变进程退出码），而非静默回落 resources 跑局。
add_test(NAME cli_repl_batch_inherit_deck_fail
         COMMAND sh -c
                 "out=$(printf 'deal 2 1\\nsimulate 1 2\\nquit\\n' | $<TARGET_FILE:tkw> --deck /nonexistent-inherit repl --autosave '' 2>&1); rc=$?; \
                  printf '%s' \"$out\" | grep -q '加载牌堆失败' && printf '%s' \"$out\" | grep -Fq '/nonexistent-inherit' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_batch_inherit_deck_fail PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 活动会话优先：REPL 内 new --deck B 后，cards 与 status 的牌表来源一致（均为 B）。
add_test(NAME cli_repl_active_session_deck_wins
         COMMAND sh -c
                 "a=$(mktemp -d); b=$(mktemp -d); mkdir -p $a/cards $b/cards; \
                  printf '%s' '{\"name\":\"A\",\"cards\":[\"h0\"]}' > $a/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"A测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $a/cards/h0.json; \
                  printf '%s' '{\"name\":\"B\",\"cards\":[\"h0\"]}' > $b/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"B测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $b/cards/h0.json; \
                  out=$(printf 'new --players 2 --seed 1 --hand 0 --deck %s\\ncards\\nstatus\\nquit\\n' \"$b\" | $<TARGET_FILE:tkw> --deck $a repl --autosave '' 2>&1); rc=$?; rm -rf $a $b; \
                  printf '%s' \"$out\" | grep -q 'B测' && [ \"$(printf '%s' \"$out\" | grep -Fc \"牌表: $b\")\" -ge 3 ] && ! printf '%s' \"$out\" | grep -Fq \"牌表: $a\" && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_active_session_deck_wins PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 行内 --deck 仍最高优先：活动会话为 B 时 cards --deck A 读启动牌表 A。
add_test(NAME cli_repl_active_session_deck_inline_override
         COMMAND sh -c
                 "a=$(mktemp -d); b=$(mktemp -d); mkdir -p $a/cards $b/cards; \
                  printf '%s' '{\"name\":\"A\",\"cards\":[\"h0\"]}' > $a/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"A测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $a/cards/h0.json; \
                  printf '%s' '{\"name\":\"B\",\"cards\":[\"h0\"]}' > $b/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"B测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $b/cards/h0.json; \
                  out=$(printf 'new --players 2 --seed 1 --hand 0 --deck %s\\ncards --deck %s\\nquit\\n' \"$b\" \"$a\" | $<TARGET_FILE:tkw> --deck $a repl --autosave '' 2>&1); rc=$?; rm -rf $a $b; \
                  printf '%s' \"$out\" | grep -Fq \"牌表: $a\" && printf '%s' \"$out\" | grep -q 'A测' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_active_session_deck_inline_override PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# 无活动会话回落启动：不 new 时 cards 读启动 --deck A（批量/纯查询路径行为不变）。
add_test(NAME cli_repl_no_session_falls_back_to_startup
         COMMAND sh -c
                 "a=$(mktemp -d); mkdir -p $a/cards; \
                  printf '%s' '{\"name\":\"A\",\"cards\":[\"h0\"]}' > $a/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"A测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $a/cards/h0.json; \
                  out=$(printf 'cards\\nrules\\naudit\\nquit\\n' | $<TARGET_FILE:tkw> --deck $a repl --autosave '' 2>&1); rc=$?; rm -rf $a; \
                  printf '%s' \"$out\" | grep -Fq \"牌表: $a\" && printf '%s' \"$out\" | grep -q 'A测' && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_no_session_falls_back_to_startup PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# load 刻意非对称：仍以启动 base 匹配存档指纹。启动 A 存档后切活动会话 B，load 无
# --deck 应由 base=A 命中成功（若误跟随活动会话 B 则报牌表不符）。
add_test(NAME cli_repl_load_ignores_active_deck
         COMMAND sh -c
                 "a=$(mktemp -d); b=$(mktemp -d); mkdir -p $a/cards $b/cards; save=${CMAKE_CURRENT_BINARY_DIR}/tkw-repl-load-active.json; \
                  printf '%s' '{\"name\":\"A\",\"cards\":[\"h0\"]}' > $a/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"A测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $a/cards/h0.json; \
                  printf '%s' '{\"name\":\"B\",\"cards\":[\"h0\"]}' > $b/deck.json; printf '%s' '{\"id\":\"h0\",\"name\":\"B测\",\"type\":\"basic\",\"copies\":[{\"suit\":\"spade\",\"number\":7}]}' > $b/cards/h0.json; \
                  out=$(printf 'new --players 2 --seed 1 --hand 0\\nsave %s\\nnew --players 2 --seed 1 --hand 0 --deck %s\\nload %s\\ncards\\nquit\\n' \"$save\" \"$b\" \"$save\" | $<TARGET_FILE:tkw> --deck $a repl --autosave '' 2>&1); rc=$?; rm -f $save; rm -rf $a $b; \
                  ! printf '%s' \"$out\" | grep -q '牌表不符' && printf '%s' \"$out\" | grep -Fq \"牌表: $a\" && [ $rc -eq 0 ]")
set_tests_properties(cli_repl_load_ignores_active_deck PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

endif()

# 可选 TUI 端到端：仅 TKW_ENABLE_TUI=ON 时注册。add_test 里的 $<TARGET_FILE:tkw-tui>
# 在 generate 期求值，tui/ 虽后于 tests/ 注册也能解析；门控必须用根级 option 变量，
# configure 到此 if(TARGET tkw-tui) 尚为假。默认 OFF 时本块整体不存在，ctest 与
# FTXUI 探测均零变化。
if(TKW_ENABLE_TUI)
  # 面板键位状态机单测：只驱动 DecisionPanel 的选择/提交回调，不启屏幕事件循环。
  # 源含 tui/decision_panel.cpp；FTXUI 目标在根级后于本目录定义，链接名在 generate
  # 期解析。仅在 ON 构建注册，OFF 时不引入 FTXUI 依赖。
  add_executable(tui_panel_tests test_main.cpp tui/decision_panel_test.cpp
                                 tui/render_test.cpp
                                 ${CMAKE_SOURCE_DIR}/tui/decision_panel.cpp
                                 ${CMAKE_SOURCE_DIR}/tui/render.cpp)
  target_include_directories(tui_panel_tests PRIVATE ${CMAKE_SOURCE_DIR}/tui)
  target_link_libraries(tui_panel_tests PRIVATE ftxui::ftxui tkw_tui doctest)
  doctest_discover_tests(tui_panel_tests)

  # 非 TTY 守卫与 --help 用例用 sh -c 包装 rc 断言，仅 Unix 注册；Windows 侧 TUI
  # 覆盖交给跨平台的 tui_panel_tests。
  if(UNIX)
    # 非 TTY 守卫：ctest 的输出本就是管道，直接运行即触发 stdin/stdout 双 fd 守卫；
    # 包装 sh -c 只为把子进程 rc=1 转成断言结果，不需 script。
    add_test(NAME tui_non_tty_guard
             COMMAND sh -c
                     "out=$($<TARGET_FILE:tkw-tui> 2>&1); rc=$?; printf '%s' \"$out\" | grep -Fq '需要交互式终端' && [ $rc -eq 1 ]")
    set_tests_properties(tui_non_tty_guard PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # --help/-h 必须在非 TTY 守卫前短路：ctest 下本就是管道，rc=0 且打出命令表即为
    # 绕过守卫的证据（无参走守卫则是 rc=1）。不启 pty。
    add_test(NAME tui_help_flag
             COMMAND sh -c
                     "out=$($<TARGET_FILE:tkw-tui> --help 2>&1); rc=$?; \
                      printf '%s' \"$out\" | grep -Fq 'TUI 命令' && \
                      printf '%s' \"$out\" | grep -Fq 'cards' && [ $rc -eq 0 ] && \
                      out2=$($<TARGET_FILE:tkw-tui> -h 2>&1); rc2=$?; \
                      printf '%s' \"$out2\" | grep -Fq 'TUI 命令' && [ $rc2 -eq 0 ]")
    set_tests_properties(tui_help_flag PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()

  # PTY 冒烟：依赖 util-linux script 提供 pty，缺失则整条不注册（照抄 raw 冒烟先例）。
  # ctest 下 script 继承的终端窗口是 0x0，须先在 pty 内 stty 固定尺寸。
  # 在临时目录建 resources 符号链接并 cd 进去跑：牌表可加载、退出自动存档落临时
  # 目录（不污染源码树、不覆盖开发者存档）。只钉 rc 与面板标题，不钉布局/动态值；
  # grep -a 处理 FTXUI 输出里的 ANSI 转义，避免被当作二进制而静默不匹配。
  if(UNIX AND TKW_SCRIPT_EXE)
    add_test(NAME tui_pty_smoke
             COMMAND sh -c
                     "d=$(mktemp -d); ln -s '${CMAKE_SOURCE_DIR}/resources' $d/resources; \
                      out=$(cd $d && { sleep 0.5; printf 'quit\\n'; sleep 0.5; } | script -qec 'stty rows 40 cols 120; $<TARGET_FILE:tkw-tui>' /dev/null 2>&1); rc=$?; rm -rf $d; \
                      printf '%s' \"$out\" | grep -aq '棋盘' && printf '%s' \"$out\" | grep -aq '装备' && printf '%s' \"$out\" | grep -aq '判定' && [ $rc -eq 0 ]")
    set_tests_properties(tui_pty_smoke PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 30)

    # 真人决策面板冒烟：命令栏开真人局 → step 弹出决策面板（钉「出牌阶段」，仅
    # 真人面板出现）→ Enter 数次于面板 → quit。只钉关键词与 rc，不赌牌局走向；
    # 每个输入前留 ≥0.6s 让 FTXUI 消费前一帧，TIMEOUT 30 把 hang 转成失败。
    add_test(NAME tui_pty_human_smoke
             COMMAND sh -c
                     "d=$(mktemp -d); ln -s '${CMAKE_SOURCE_DIR}/resources' $d/resources; \
                      out=$(cd $d && { sleep 1; printf 'new --human P0 --players 2 --seed 1\\n'; sleep 0.8; \
                      printf 'step\\n'; sleep 1.2; printf '\\n'; sleep 0.6; printf '\\n'; sleep 0.6; \
                      printf 'quit\\n'; sleep 0.8; } | script -qec 'stty rows 40 cols 120; $<TARGET_FILE:tkw-tui>' /dev/null 2>&1); rc=$?; rm -rf $d; \
                      printf '%s' \"$out\" | grep -aq '出牌阶段' && [ $rc -eq 0 ]")
    set_tests_properties(tui_pty_human_smoke PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 30)

    # 待决中退出：面板阻塞 worker 时 quit 必须唤醒并干净结束（钉无 hang）。
    add_test(NAME tui_pty_human_quit_pending
             COMMAND sh -c
                     "d=$(mktemp -d); ln -s '${CMAKE_SOURCE_DIR}/resources' $d/resources; \
                      out=$(cd $d && { sleep 1; printf 'new --human P0 --players 2 --seed 1\\n'; sleep 0.8; \
                      printf 'step\\n'; sleep 1.2; printf 'quit\\n'; sleep 0.8; } | script -qec 'stty rows 40 cols 120; $<TARGET_FILE:tkw-tui>' /dev/null 2>&1); rc=$?; rm -rf $d; \
                      [ $rc -eq 0 ]")
    set_tests_properties(tui_pty_human_quit_pending PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 30)

    # 身份局角色占位：argv 直接开身份局（首帧即出棋盘，省一次 new 交互），只钉两个
    # 稳定事实——P0 恒主公可见 [主公]、非真人非主公座位（P2/P3）隐藏为 [未知] 占位；
    # 不赌隐藏座具体是忠/反/内（角色按 identity rng 洗牌），grep -F 避免方括号被当
    # 正则字符类；grep -a 处理 FTXUI 输出里的 ANSI 转义。
    add_test(NAME tui_pty_identity_hidden_role
             COMMAND sh -c
                     "d=$(mktemp -d); ln -s '${CMAKE_SOURCE_DIR}/resources' $d/resources; \
                      out=$(cd $d && { sleep 1; printf 'quit\\n'; sleep 0.8; } | script -qec 'stty rows 40 cols 120; $<TARGET_FILE:tkw-tui> --mode identity --human P1 --players 4 --seed 1' /dev/null 2>&1); rc=$?; rm -rf $d; \
                      printf '%s' \"$out\" | grep -aqF '[主公]' && printf '%s' \"$out\" | grep -aqF '[未知]' && [ $rc -eq 0 ]")
    set_tests_properties(tui_pty_identity_hidden_role PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 30)

    # 小终端（24 行仍属 Full）不隐藏命令输入：只钉占位符可见与 rc；尺寸固定
    # 避免继承 ctest 的 0x0 窗口。既有 40x120 用例继续作为「≥24 行不降级」护栏。
    add_test(NAME tui_pty_small_terminal
             COMMAND sh -c
                     "d=$(mktemp -d); ln -s '${CMAKE_SOURCE_DIR}/resources' $d/resources; \
                      out=$(cd $d && { sleep 0.5; printf 'quit\\n'; sleep 0.5; } | script -qec 'stty rows 24 cols 80; $<TARGET_FILE:tkw-tui>' /dev/null 2>&1); rc=$?; rm -rf $d; \
                      printf '%s' \"$out\" | grep -aq '输入命令' && [ $rc -eq 0 ]")
    set_tests_properties(tui_pty_small_terminal PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 30)
  endif()
endif()
