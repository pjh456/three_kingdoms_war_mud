# 跨平台 MUD 三国杀单机游戏（tkw）

三国杀式卡牌对局引擎：C++20 header-only 引擎 + `tkw` 命令行/REPL，牌表数据驱动
（`resources/` 下的 JSON）。AI 与真人混坐对局：判定 → 摸牌 → 出牌（杀/锦囊/装备）→
弃牌，结算伤害、濒死救场与阵亡，可一键跑完整局，也可进 REPL 逐回合参与。

本项目以终端文本交互呈现三国杀式对局：玩家在本机与 AI 对战，
通过命令或决策面板出牌、响应与选择目标。CLI/REPL 可用于逐回合游玩与脚本运行，
可选 TUI 则提供棋盘、手牌、日志和状态面板。对局运行不需要服务器或账号；
获取子模块与首次下载构建依赖时可能需要联网。

## 功能概览

- **对局模式**：2–8 人乱斗、4–8 人身份局；支持真人与 AI 混合座位。
- **规则结算**：判定、摸牌、出牌、弃牌、距离与攻击范围、装备、
  普通/火焰/雷电伤害、铁索连环、濒死救援与无懈可击链。
- **资源内容**：标准版 32 种/108 张牌、军争篇 17 种/83 张牌，
  以及标准目录中的 8 名武将；两套随附牌表目前均可通过结算审计。
- **交互与查询**：中文命令帮助、手牌决策、牌面说明、牌表与武将扫描。
- **会话管理**：逐回合推进、运行到结束、JSON 存读档、退出自动存档与命令历史。
- **模拟与开发**：两档 AI、随机种子、跨局统计、header-only 核心和分层测试。

## 文档导航

[构建与测试](#构建与测试) · [跨平台构建与运行](#跨平台构建与运行) ·
[快速开始](#快速开始) · [可选终端界面](#可选终端界面tkw-tui) ·
[命令速查](#命令速查) · [REPL 用法](#repl-用法) · [内置牌表](#内置牌表) ·
[武将](#武将) · [自定义牌表](#自定义牌表) · [规则简表](#规则简表默认值) ·
[存档说明](#存档说明) · [开发与测试](#开发与测试) · [常见问题](#常见问题)

## 构建与测试

依赖支持 C++20 的编译器、CMake ≥ 3.21；以下示例使用 Ninja，亦可使用其他 CMake 生成器。
第三方库是 git 子模块且为**两层**结构
（`thirdparty/pjh_json` 内还嵌套子模块），克隆后必须用 `--recursive` 补齐：

```sh
git clone <仓库地址> three_kingdoms_war_mud
cd three_kingdoms_war_mud
git submodule update --init --recursive

cmake -S . -B build -G Ninja  # 配置（首次构建需联网拉取 doctest v2.5.0）
cmake --build build           # 构建，产物 build/src/tkw
ctest --test-dir build --output-on-failure  # 运行全部测试，失败时显示输出
```

- 测试构建开关 `-DTKW_ENABLE_TESTS=ON|OFF`（默认 ON）；OFF 时 `tests/` 不进入构建。
- 可选终端界面开关 `-DTKW_ENABLE_TUI=ON|OFF`（默认 OFF）；OFF 时不探测、不拉取
  FTXUI，对 `tkw`/引擎/现有测试零影响，详见下文「可选终端界面（tkw-tui）」。
- `build/` 是构建产物，不入库。
- 下文示例中 `tkw` 均指 `./build/src/tkw`，且在仓库根目录执行（默认牌表路径
  `resources/` 相对当前工作目录）。

## 跨平台构建与运行

项目使用 CMake 和平台适配库面向 Windows、Linux、macOS。
编译器需要支持项目实际使用的 C++20 功能（包括 `std::jthread` 等标准库功能）；
仅启用 `-std=c++20` 并不能补齐旧标准库缺少的实现。

### Windows：Ninja / MinGW 或 MSVC

MinGW 的 C++ 编译器与 Ninja 需要位于 `PATH` 中。使用 MSVC + Ninja 时，
请先进入 Visual Studio 的开发者命令行，使编译器与 Windows SDK 可用。
在仓库根目录的 PowerShell 中执行：

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

.\build\src\tkw.exe --help
.\build\src\tkw.exe --human P0 repl
```

若使用 Visual Studio 多配置生成器，构建与测试需指定配置，产物路径也会多一层：

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022"
cmake --build build-msvc --config Release
ctest --test-dir build-msvc -C Release --output-on-failure
.\build-msvc\src\Release\tkw.exe --human P0 repl
```

源码与资源使用 UTF-8，顶层 CMake 为 MSVC 添加 `/utf-8`。
终端还需能显示中文；推荐使用支持 UTF-8 和中文字体的终端。
本地已有 Windows/MinGW 的 CLI 构建产物；MSVC、macOS、Linux
及各平台 TUI 的实际验证范围应以相应构建与测试结果为准。

### Linux / macOS

安装支持 C++20 的编译器、CMake、Git 和 Ninja 后，在仓库根目录执行：

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/tkw --human P0 repl
```

如需明确选择编译器，可在首次配置时追加 `-DCMAKE_CXX_COMPILER=clang++`
或 `-DCMAKE_CXX_COMPILER=g++`。切换生成器或编译器时请使用新的构建目录。

### 仅构建游戏与离线依赖

只需 CLI 时，可关闭测试下载；TUI 默认为 OFF：

```sh
cmake -S . -B build-cli -G Ninja -DTKW_ENABLE_TESTS=OFF -DTKW_ENABLE_TUI=OFF
cmake --build build-cli --target tkw
```

离线构建需要提前初始化所有递归子模块。启用测试时，
可用 `-DFETCHCONTENT_SOURCE_DIR_DOCTEST=<doctest-src>` 指向预置源码；
FTXUI 的离线配置见「可选终端界面（tkw-tui）」一节。

`thirdparty/pjh_cli` 在当前 `.gitmodules` 中使用 GitHub SSH 地址。
若未配置 SSH 密钥，可在本地覆盖该子模块地址后再初始化：

```sh
git submodule init
git config submodule.thirdparty/pjh_cli.url https://github.com/pjh456/pjh_cli
git submodule update --init --recursive
```

下文 shell 示例中的 `tkw` 为可执行文件的简称，并非构建后自动加入 `PATH` 的命令。
Windows/Ninja 下请替换为 `.\build\src\tkw.exe`，
Linux/macOS 下替换为 `./build/src/tkw`。
资源与存档的相对路径均从启动程序时的工作目录解析。

## 快速开始

```sh
tkw                   # 裸命令：跑一局 4 人 AI 对局到结束
tkw deal 2 1          # 位置参数跑一局：2 人、种子 1
tkw --mode identity deal 5 1  # 身份局：5 人、种子 1（身份局需 4–8 人）
tkw --human P0 repl   # P0 真人参与，REPL 交互模式
```

第一局（P0 真人、2 人局，seed 1 输出确定，可逐行照抄）：

```sh
tkw --human P0 repl        # 进入 REPL，P0 真人参与（真人局默认显示事件日志）
new --players 2 --seed 1   # 开新局
step                       # 推进一个回合；轮到你时依次经过出牌/弃牌窗口
pass                       # 出牌阶段不出牌（可改 play <序号> 出牌，可连续出牌）
discard 1 2                # 弃牌阶段手牌超上限，按提示弃够张数（不能 pass）
```

之后重复 `step` 直到分出胜负；`run` 可直接跑到结束，`status` 随时查看局面，
`quit` 退出（有进行中会话时自动存档）。

对局结束打印胜者（或平局）与对局统计块（回合数/每人体力/击杀/伤害/治疗）。

`step` 推进的是一个角色的完整回合；期间可能出现出牌、响应、触发技能、
选目标与弃牌等多个真人决策窗口。`run` 在真人局中仍会等待真人决策。
上面的出牌与弃牌序号仅用于示范，实际请按当前窗口显示的候选项与所需张数操作。

座位从 `P0` 到 `P<人数-1>`。未指定 `--hero` 时使用无名座位，
不会自动随机选将。想以张飞参与身份局，可这样启动：

```sh
tkw --human P0 --hero P0=zhangfei --mode identity --players 5 --seed 1 repl
```

进入后执行 `new` 继承启动选项，再用 `step` 或 `run` 推进。

## 可选终端界面（tkw-tui）

除 CLI/REPL 外，另有一个可选的 FTXUI 全屏前端，默认关闭：

```sh
cmake -B build-tui -G Ninja -DTKW_ENABLE_TUI=ON   # 首次配置需联网拉取 FTXUI v7.0.3
cmake --build build-tui --target tkw-tui          # 产物 build-tui/tui/tkw-tui
./build-tui/tui/tkw-tui
./build-tui/tui/tkw-tui --human P0 --players 2 --seed 1   # 真人局：P0 由你操作
```

- 开关 `-DTKW_ENABLE_TUI=ON|OFF`（默认 OFF）；OFF 时不探测、不拉取 FTXUI，对
  `tkw`/引擎/现有测试零影响。
- 必须在仓库根目录运行：默认牌表 `resources/` 相对当前工作目录；且需要交互式终端
  （标准输入与标准输出均为 TTY）。任一非 TTY 时打印「需要交互式终端」并以 1 退出，
  请改用 `tkw repl`。
- 四面板展示棋盘/手牌/日志/状态（棋盘逐座显示体力/手牌数/装备/判定/距离，
  空区显示「无」），底部命令栏：`new [--players N] [--seed S]
  [--mode brawl|identity] [--ai simple|aggressive] [--deck P] [--hand N]
  [--human <座位>] [--no-human] [--hero <座位=id>]`、`deal <players> <seed>`、`step`、`run`/`r`、
   `status`/`st`、`save`/`w <file>`、`load`/`l <file>`、`cards [--text] [--deck 路径]`、
    `rules [关键词] [--deck 路径]`、`audit [--deck 路径]`、`decks [--deck 路径]`、`heroes [目录] [--deck 路径]`、
    `simulate <局数> [玩家数] [--seed S] [--ai simple|aggressive] [--hand N]
    [--mode brawl|identity] [--deck 路径]`、`quit`/`q`、
   `help [命令]`/`? [关键词]`；`Esc`/`Ctrl-C` 退出。
- 命令可发现性对齐 CLI：未知命令附邻近拼写建议（如 `runn` → `run`）；
  不带参数的 `help`/`?` 打印全量表，`help <命令>` 看单条命令用法，
  `? <关键词>` 过滤命令表（如 `? 牌`）。
- 反馈对齐 CLI：`step`/`run` 每回合在日志写「—— 回合 N：P ——」回合头，终局追加
  「对局统计:」块（回合数/胜者/体力/击杀/伤害/治疗），状态面板显示「存活: N」；
  `new`/`load` 对未实现卡写警告进日志（只提示、不阻断）。`tkw-tui --help`/`-h`
  打印命令与用法（纯文本，非 TTY 亦可，退出码 0）。
- 事件日志恒开：日志面板持续显示打出/弃置/判定/摸牌/伤害等事件；TUI 不提供
  `--verbose`/`--no-verbose` 开关。
- 日志面板支持翻阅：`PgUp`/`PgDn` 上下翻页（始终可用）、`End` 回到最新（贴尾）、
  `Home` 回到最早；后两者仅命令栏为空时接管，输入非空时 `Home`/`End` 留给命令编辑
  光标。日志溢出时右侧显示滚动条。终端过小时自动隐藏次要面板（先棋盘/
  手牌，再状态）并保留底部命令输入行，同时提示被隐藏的面板。
- 真人参与：启动即真人局 `./build-tui/tui/tkw-tui --human P0 --players 2 --seed 1`
  （`--human` 可重复，`--no-human` 清空），也可在命令栏 `new --human P0 ...` 开局。
  轮到你时底部命令栏替换为决策面板，按键：`↑`/`↓` 选择候选，`Enter` 确认，`p`
  放弃（仅允许放弃的窗口），弃牌窗口用 `空格` 多选、选够张数后 `Enter`，数字键
  1–9 直选（多选时切换勾选）；待决期 `q` 退出，`Esc`/`Ctrl-C` 仍全局退出。
- 隐藏信息：真人局中非真人座位的摸牌/击杀奖励只显占位「未知牌」；身份局未终局
  只显示主公与真人座位的角色，其余座位显示「未知」占位（终局揭示全部）；手牌面板
  只展开**首个**真人座位（多真人同时展开属后续里程碑），其余只给数量；选择对手区域
  的牌时手牌出「未知手牌」占位，装备/判定区明置。
- 卡牌查询与批量模拟：命令栏 `cards [--text]`、`rules [关键词]`、`audit`、
  `decks` 就地出结果，
  逐行写入日志面板（长列表用日志翻阅）；牌表来源优先序为行内 `--deck` > 活动会话 >
  启动 `--deck`（与 REPL 只读命令同口径），行内坏路径写一行「加载牌堆失败」提示、
  不阻断会话。`simulate <局数> [玩家数]` 在后台线程跑完全 AI 批量并把跨局聚合结果
  （牌表来源/局数/种子区间/AI 档/胜场或阵营/平局/平均回合）就地写入日志面板，
  基种子缺省 1；运行期间可翻阅日志，`q` 在局边界取消退出；数百局以上的极大批量
  建议退出后用 `tkw simulate`（脚本化、无 UI 线程）。
- 退出时若有进行中的会话，自动存档到当前目录的 `tkw-autosave.json`（与 REPL 同口径），
  退出信息写 stderr。
- FTXUI 获取：优先 `find_package(ftxui CONFIG QUIET)`，未安装则 `FetchContent` 钉
  `v7.0.3`；离线可用 `-DFETCHCONTENT_SOURCE_DIR_FTXUI=<ftxui-src>` 指向预置源码，
  或用 `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` 配合已 populate 的 `_deps`。
- 同时开启测试（默认 ON）时会注册 TUI 端到端测试：非 TTY 守卫直接运行，`--help`/
  `-h` 在管道下同样 rc=0；PTY 冒烟需 util-linux `script`（缺失则该条不注册），除
  退出冒烟外还覆盖真人决策面板（`tui_pty_human_smoke` 钉面板出现，
  `tui_pty_human_quit_pending` 钉待决中退出无 hang）与小终端
  （`tui_pty_small_terminal` 钉 24×80 下命令输入可见）。
- Windows/MSVC TUI 未验证；上述类 Unix 可执行路径在 Windows/Ninja 下对应
  `.\build-tui\tui\tkw-tui.exe`。Visual Studio 多配置生成器还需加入配置目录，
  如 `build-tui/tui/Release/tkw-tui.exe`。

## 命令速查

与 `tkw --help` 的子命令表同源（措辞以 `--help` 为准）：

| 命令 | 别名 | 描述 |
|---|---|---|
| `tkw`（无子命令） | — | 直接跑一局 AI 对局（默认 4 人、种子 42） |
| `audit` | — | 审计牌堆，列出引擎未实现的卡 |
| `cards` | — | 列出牌表（牌堆种类与张数；`--text` 附效果文案） |
| `decks [目录]` | — | 列出可用牌表（扫描根目录及直接子目录，缺省 `resources`） |
| `heroes [目录]` | — | 列出可用武将（随 `--deck` 选择；缺 `heroes.json` 显示无数据） |
| `rules [关键词]` | — | 查询卡牌效果说明（`CardDef.text`，可按关键词过滤） |
| `deal <玩家数> <种子>` | — | 跑一局：deal <玩家数> <种子> |
| `simulate <局数> [玩家数]` | — | 批量模拟：simulate <局数> [玩家数] |
| `new` | — | 开新对局（用 `--players`/`--seed`/`--hand`） |
| `step` | — | 执行当前会话的一个回合 |
| `run` | `r` | 跑到当前会话结束 |
| `status` | `st` | 查看当前会话状态 |
| `save <file>` | `w` | 保存当前对局：save <file> |
| `load <file>` | `l` | 加载存档：load <file> |
| `repl` | — | 进入交互模式（? 查看命令，quit 退出） |

公共选项（各命令可用）：

| 选项 | 说明 |
|---|---|
| `-d, --deck <dir>` | 资源目录（含 `deck.json` 与 `cards/`），默认 `resources` |
| `-p, --players <n>` | 玩家数（2–8；REPL 内缺省继承启动 `--players`，否则默认 4） |
| `--hand <n>` | 初始手牌数（默认 4） |
| `-s, --seed <n>` | 随机种子（默认 42） |
| `-v, --verbose` | 打印事件日志（摸牌/击杀奖励、打/弃牌、判定翻牌、移牌、伤害、体力、阵亡；真人局默认开启，`--no-verbose` 关闭） |
| `--autosave <path>` | REPL 退出时自动存档路径（空串关闭，默认 `tkw-autosave.json`） |
| `--history <path>` | REPL 命令历史文件（默认不持久化，仅本次会话；父目录须已存在） |
| `--human <seat>` | 真人座位（可重复：`--human P0 --human P2`；存档不保存，读档后需重新指定） |
| `--no-human` | 清空真人座位（REPL 内覆盖启动/会话带入的 `--human`；与 `--human` 同给时清空优先） |
| `--hero <seat=id>` | 武将选择（可重复：`--hero P0=zhangfei --hero P2=guanyu`；武将数据随 `--deck` 目录的 `heroes.json`；`load` 行内给出会被拒绝） |
| `--ai <simple\|aggressive>` | AI 难度（默认 `simple` 贪心；`aggressive` 伤害/多目标先行） |
| `--mode <brawl\|identity>` | 对局模式（默认 `brawl` 乱斗；`identity` 身份局需 4–8 人，`load` 以存档为准） |

上表的选项各命令都声明并接受，但生效面不同：`--deck`/`--players`/`--hand`/`--seed`/`--ai`/`--mode`
只对建局/载入类命令（裸 `tkw`/`deal`/`new`/`load`/`repl`/`simulate`）实际生效；`cards`/`audit`/
`rules` 只读 `--deck`；`step`/`run`/`status`/`save` 只读取其中的 `--verbose`（`step`/`run`）
或全不读取（`status`/`save`）。`--human` 只在运行真人参与对局的命令生效，`audit`/`cards`/
`rules`/`heroes`/`simulate` 会明确拒绝；`--autosave`/`--history` 只在 `repl` 生效。**`--mode` 在 `load` 上
不生效**：载入的模式与角色以存档为准，避免用命令行强行改写存档模式。**`--hero` 在 `load` 行内会被拒绝**
（rc=1 并提示替代命令）：武将随存档恢复，没有读档换将，要改选请用 `new --hero <座位>=<武将>`；REPL 启动
`--hero` 只作会话默认，不会让后续 `load` 误判。

REPL 内只读/批量命令（`cards`/`rules`/`audit`/`deal`/`simulate`）的默认牌表来源：有活动会话
时读该会话牌表（与 `status` 展示一致），无活动会话时回落启动 `--deck`；行内 `--deck` 始终
优先。`load` 仍以启动 `--deck` 匹配存档指纹，不跟随活动会话。

会话命令（`new`/`step`/`run`/`status`/`save`/`load`）共享同一进程内的会话，通常在
`tkw repl` 内逐条输入使用；在 REPL 外单独执行不会保留会话（单独 `tkw new` 只开一局
并打印状态，随后进程即退出）。

## REPL 用法

```sh
tkw repl
```

进入时打印引导：`输入 ? 查看命令，help <命令> 看用法，quit 退出`。

- `?` 列出命令（带描述），`? <关键词>` 过滤查找；`help <命令>` 或 `<命令> --help`
  看用法；
- 会话流：`new --players 2 --seed 1` 开新局 → `step` 执行一个回合（可重复）/
  `run` 跑到结束 → `status` 看状态 → `save s.json` 存档 / `load s.json` 续玩 →
  `quit` 退出；
- 身份局：`new --mode identity --players 5 --seed 1`（4–8 人）开身份局，`status`
  显示「模式: 身份局」与逐座角色（主公/忠臣/反贼/内奸）；有真人参与（`--human`）
  且未终局时只显主公与真人座位角色，其余占位「未知」，终局揭示全部；终局按阵营
  给出「主公阵营胜/反贼阵营胜/内奸胜」；`run`/`deal`/`simulate` 同样支持
  `--mode identity`；
- 事件日志：真人参与的对局（`--human`）**默认开启**，全 AI 局默认关闭；用
  `--verbose`/`--no-verbose` 可显式覆盖。REPL 启动带 `--verbose` 时 `new`/`load`
  建局继承该开关；在 `step`/`run` 上行内加 `--no-verbose` 可只关本次输出的日志，
  下一行仍回落会话默认，无需重启；`step`/`run` 在每个回合执行前打印
   `—— 回合 N：<玩家> ——` 回合头，使事件可归属到具体回合；击杀奖惩发放的
   摸牌打印为 `[击杀奖励] <角色> <卡名>`，与常规 `[摸牌]` 区分；真人局中
   非真人座位的摸牌只出事件与占位 `未知牌`，不暴露对手牌名；
- 真人参与（第一局可照抄）：`tkw --human P0 repl`，进入后输入
  `new --players 2 --seed 1` 开新局，再重复 `step` 推进；轮到你时按窗口提示操作：
  - 出牌窗口：`play <序号>` 出牌（可连续出牌），`pass` 结束出牌阶段；窗口会列出你的
    **完整手牌**（对手只给数量），`card <序号>` 查看候选牌完整效果文案；
  - 弃牌窗口：手牌超过当前体力时出现，`discard <序号> ...` 弃够张数（该窗口不能 `pass`）；
  - 响应窗口（需打出闪/杀）、濒死救场（需打出桃）、无懈可击窗口、五谷丰登亮牌：
    同用 `play <序号>` 决定、`pass` 放弃（强制选择除外）；窗口内输入 `?`/`help` 看用法。
  - REPL 内可用 `new --no-human` 清空启动选项带入的真人座位（同命令给 `--human` 时
    清空优先）。`--human` 只对运行对局的命令有效，`audit`/`cards`/`rules`/`simulate`
    会拒绝并提示；
- 卡牌效果速查：`tkw rules [关键词]` 列出/过滤卡牌说明，`tkw cards --text` 列牌表
  并附文案；决策窗口内直接 `card <序号>`，无需翻牌表文件；
- REPL 退出时若有进行中的会话，自动存档到当前目录的 `tkw-autosave.json`
  （`--autosave <path>` 可改路径，空串关闭）；`--human` 设置不存入存档，
  读档后需重新指定。
- 命令历史默认仅本次会话；`--history <path>` 指定文件后跨进程保留，重启后
  可用上下方向键召回。文件为 UTF-8 一行一条，相对路径按当前工作目录解析、`~` 不展开，
  父目录须已存在（缺失时打印告警并回落内存）；写失败不阻塞 REPL。

## 内置牌表

仓库自带两副牌表，`tkw decks` 一览：默认目录 `resources/` 是标准版，其子目录
`resources/junzheng/` 是军争篇。

```sh
tkw decks
# 可用牌表（tkw decks [目录] 扫描；用 --deck <路径> 选择）:
#   resources  标准版 32 种/108 张
#   resources/junzheng  军争篇 17 种/83 张
```

用 `--deck <路径>` 选定牌表，例如军争篇首局：

```sh
tkw --deck resources/junzheng cards     # 先看军争篇牌表构成
tkw --deck resources/junzheng deal 2 1  # 2 人、种子 1 用军争篇跑一局
tkw --deck resources/junzheng repl      # 或进 REPL 逐回合玩（再 new/step）
```

TUI 命令栏同样支持 `decks [--deck 路径]` 就地列出，结果写入日志面板。

军争篇是一套独立牌表，不会自动叠加标准版卡牌；当前目录不含
`heroes.json`，所以直接切换到军争篇不会加载标准目录的武将。
其中包含火杀、雷杀、酒、兵粮寸断、铁索连环、火攻、藤甲、
白银狮子、古锭刀与朱雀羽扇等定义；具体构成以 `cards --text` 输出为准。

## 武将

武将数据与牌表同根（`<deck>/heroes.json` + `<deck>/heroes/<id>.json`），独立于
牌表、不参与牌表指纹，故换/改武将不会让旧存档报牌表不符；因此改动同名武将的
`skills`/`hp`/`gender` 也不会触发读档的牌表不符，技能行为由引擎代码按 `hero` id
重定位，改数据前请确认与代码支持的技能集一致。`tkw heroes` 一览：

```sh
tkw heroes
# 可用武将（tkw heroes [目录] 扫描；随 --deck 选择）:
#   张飞(zhangfei) 4体力 男 技能: 咆哮
#   关羽(guanyu) 4体力 男 技能: 武圣
#   周瑜(zhouyu) 3体力 男 技能: 英姿
#   司马懿(simayi) 3体力 男 技能: 反馈
#   马超(machao) 4体力 男 技能: 马术
#   黄月英(huangyueying) 3体力 女 技能: 奇才
#   赵云(zhaoyun) 4体力 男 技能: 龙胆
#   甄姬(zhenji) 3体力 女 技能: 倾国
```

用 `--hero <座位>=<武将>` 指定（可重复）；`status` 逐座展示武将，存档保留选择；
`load` 恢复存档内的武将选择，行内给 `--hero` 会被拒绝，要改选请在 `new` 上重新指定：

```sh
tkw --hero P0=zhangfei deal 2 1     # P0 张飞：咆哮锁定技，使用【杀】无次数限制
tkw --hero P0=zhouyu deal 2 1       # P0 周瑜：英姿锁定技，摸牌阶段多摸一张
tkw --hero P0=machao deal 4 1       # P0 马超：马术锁定技，计算距离 -1
tkw --hero P0=huangyueying deal 4 1 # P0 黄月英：奇才锁定技，锦囊无距离限制
tkw --hero P0=zhaoyun deal 2 1      # P0 赵云：龙胆转化技，闪当杀、杀当闪
tkw --hero P0=zhenji deal 2 1       # P0 甄姬：倾国转化技，黑色牌当闪
tkw --hero P0=zhangfei repl         # 或进 REPL：new --hero P0=zhangfei --players 2 --seed 1
```

尚无 `heroes.json` 的自定义牌表照常可玩（武将回落无名座位）。被选中武将若含
引擎尚未实现的技能，建局入口会打印中文警告，不会静默按无技能结算。

## AI 与批量模拟

`simple` 采用贪心策略，`aggressive` 优先考虑伤害与多目标动作；
两者共享合法动作与响应机制。身份局 AI 会按角色阵营进行决策。
这两档是策略区别，并不保证 aggressive 在所有牌表或局面中胜率更高。

```sh
tkw --ai aggressive deal 4 42
tkw simulate 100 4 --seed 1 --ai aggressive
tkw simulate 100 5 --mode identity --seed 1
tkw --deck resources/junzheng simulate 100 2
```

`simulate` 为全 AI 模拟，拒绝真人座位；缺省基种子为 1，
每局使用递增的种子。结果包含局数、种子区间、AI 档、
胜场/阵营胜场、平局和平均回合等汇总信息。
在相同代码、资源、参数和决策序列下，固定种子便于复现与比较；
修改代码或牌表后，结果可能随之变化。

## 自定义牌表

牌表是纯数据目录，默认 `resources/`：

```
<牌表目录>/
├── deck.json           # name / expansion / cards（卡牌 id 列表）
├── cards/<id>.json     # 每张卡一个定义文件，id 须与文件名一致
├── heroes.json         # 可选：武将目录（name / expansion + heroes id 列表）
└── heroes/<id>.json    # 可选：单武将定义（id / name / gender / hp / skills / text）
```

- `deck.json` 的 `cards` 引用顺序 = 牌堆构建顺序；
- 卡文件定义 `id`/`name`/`type`/`subtype`/`copies`（逐张列花色与点数，条数即
  张数）/`text`（效果文案），按需再带 `effect`（主动效果）/`equip`（装备）/
  `judge`（判定）/`abilities`（被动能力）；
- 使用引擎已支持的机制新增卡牌时，只需修改牌表目录；新增机制仍需实现对应代码与测试。
  `tkw --deck <dir> audit` 可审计未实现卡
  （未知机制名逐卡列出，只审机制、不建局；`deal`/`simulate` 遇到未知机制仍会拒绝建局）；
- 用 `--deck <dir>` 指向自定义牌表，如 `tkw --deck mydeck deal 2 1`。

标准版牌表共 32 个卡牌定义、108 张牌。

卡牌 ID 需要同时对应入口引用、文件名和文件中的 `id`；
`copies` 中每个对象表示一张实体牌，不能仅修改文字说明来改变效果。
`effect.kind`、`abilities` 与武将 `skills` 使用引擎支持的机制名，
未知机制可能通过审计被列出，但正常资源加载会拒绝它们。

完成修改后，先检查加载与机制覆盖，再运行实际对局：

```sh
tkw --deck mydeck cards --text
tkw --deck mydeck audit
tkw --deck mydeck deal 2 1
```

`audit` 是机制覆盖检查，不能替代规则测试。定义字段与可用枚举可参照
`resources/cards/`、`resources/heroes/` 中的现有 JSON，
以及 `src/card/catalog.hpp`、`src/hero/catalog.hpp` 的解析表。

## 规则简表（默认值）

| 项 | 值 |
|---|---|
| 初始/上限体力 | 无名座位默认 4；选将后以武将配置为准 |
| 初始手牌 | 每人 4 张 |
| 摸牌阶段 | 每回合摸 2 张 |
| 「杀」次数 | 每回合 1 次（诸葛连弩或咆哮可解除限制） |
| 手牌上限 | 当前体力值（超出时弃牌） |
| 击杀奖励（乱斗） | 摸 3 张牌 |
| 身份局击杀奖惩 | 击杀反贼摸 3 张；主公击杀忠臣弃光手牌与装备；其余身份无奖励 |
| 玩家数 | 2–8 人（身份局需 4–8 人） |
| 乱斗胜利条件 | 唯一存活；无存活者为平局，未终局且超出默认 1000 回合上限时按平局处理 |
| 身份局胜利 | 主公阵营胜（主公存活且反贼、内奸全部阵亡）/ 反贼胜（主公阵亡且内奸非唯一存活者）/ 内奸胜（唯一存活内奸）；同归于尽为平局 |

全局默认数值集中在 `src/game/core/rules.hpp` 的 `RulesConfig` 中；
CLI 提供人数与初始手牌等选项，其他数值可通过引擎配置修改，
并非所有规则都能直接从牌表 JSON 调整。每张牌的效果文案可直接查询：`tkw rules` 列出全部、
`tkw rules <关键词>` 过滤，`tkw cards --text` 在牌表后附文案。详细规则以
`tkw --help` 与卡面文案为准。

身份局主公固定为 `P0`，其余角色按种子随机分配，配比如下：

| 人数 | 主公 | 忠臣 | 反贼 | 内奸 |
|---|---|---|---|---|
| 4 | 1 | 1 | 1 | 1 |
| 5 | 1 | 1 | 2 | 1 |
| 6 | 1 | 1 | 3 | 1 |
| 7 | 1 | 2 | 3 | 1 |
| 8 | 1 | 2 | 4 | 1 |

界面中“回合数”按每个角色执行一次回合计数，并非全体角色各行动一次才加 1。

## 存档说明

- `save <file>` / `load <file>`（别名 `w` / `l`）手动存读；存档为 JSON
  （`format: tkw-save`，基础格式 version 1），写入走原子替换（先写临时文件再 rename），
  读取时全量校验通过后才落子。
- **格式版本**：写入时按实际状态选择版本，无连环和武将状态时为 v1，
  含连环状态时为 v2，含武将选择时为 v3；当前读取端接受这三个版本。
  旧二进制不支持的新版本会被明确拒绝，避免丢失规则状态。
- **牌表指纹校验**：存档记录保存时牌表语义字段的 FNV-1a 指纹；加载时与当前
  `--deck` 牌表比对，牌表已改动则拒绝加载：
  `存档加载失败（牌表不符）: deck.hash`。
- **AI 档与统计入档**：存档记录会话的 AI 档与对局统计（伤害/治疗/击杀/最近伤害
  来源/阵亡），读档后自动恢复；`load <file> --ai <档>` 显式覆盖存档 AI 档。旧档
  无这些字段时回落命令行取值与空统计，仍可加载。`--human` 与 `--verbose` 仍不
  入档（读档命令行的 `--verbose` 自行控制日志）。
- **模式与角色入档**：身份局存档额外记录 `mode` 与逐座 `roles`，读档后自动恢复
  模式与角色；`load` 的模式以存档为准，命令行 `--mode` 不生效（与 `--ai` 的覆盖
  语义刻意区分）。乱斗存档不写这两项，旧档缺字段时回落乱斗 + 空角色表，仍可加载。
- **自动存档**：REPL 退出时若有进行中的会话，自动存档到当前目录
  `tkw-autosave.json`；`--autosave <path>` 改路径，空串关闭。

存档还包含牌堆与各区域牌、实体状态、随机数状态和下一回合进度，
用于在回合边界继续对局。手动保存适合在一次 `step` 完成后执行；
不能把它理解为任意决策窗口的即时快照。

读取非默认牌表的存档时，需要选回保存时的资源目录。
例如先执行 `tkw --deck resources/junzheng repl`，再输入
`load save.json`；也可在已有 REPL 中使用
`load save.json --deck resources/junzheng`。牌表语义发生变化导致指纹不符时，
请使用对应旧资源读档，或新建对局。

## 目录结构

```
three_kingdoms_war_mud/
├── CMakeLists.txt          # 顶层：C++20、TKW_ENABLE_TESTS / TKW_ENABLE_TUI 开关
├── src/                    # 引擎（header-only，INTERFACE 库）
│   ├── main.cpp            #   CLI 入口（src 下唯一 .cpp）
│   ├── cli/  io/  config/  #   命令树 / 文件读写 / 资源加载
│   ├── card/  entity/  event/  # 卡牌域 / 实体域 / 事件总线
│   ├── game/               #   对局五层 core/query/resolve/flow/ai（严格单向依赖）
│   ├── tui/                #   TUI 纯视图模型（可见性/快照/日志/命令/控制器，无 FTXUI）
│   └── save/  util/        #   存档 / 通用（Result、随机源）
├── tui/                    # 可选终端界面（TKW_ENABLE_TUI=ON 时构建 tkw-tui，含 FTXUI）
├── tests/                  # doctest，目录镜像 src，默认 13 个测试可执行文件
├── resources/              # 数据驱动牌表：deck.json + cards/<id>.json
├── thirdparty/             # 子模块：pjh_result / pjh_json / pjh_cli / pjh_platform
└── build/                  # 构建产物（不入库）
```

## 开发与测试

默认配置构建核心测试与 CLI 测试，也构建不依赖 FTXUI 的 TUI 共享逻辑测试；
启用 `TKW_ENABLE_TUI` 后会额外构建面板/渲染测试。
使用 shell 与 PTY 的端到端测试按平台和工具条件注册，
所以不同平台的 CTest 用例数量可能不同。

```sh
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -N                     # 列出已注册用例
cmake --build build --target game_tests      # 单独构建游戏规则测试目标
```

Visual Studio 多配置构建的上述构建/测试命令需分别追加
`--config Release` 与 `-C Release`。默认测试目标覆盖实体、事件、
文件与配置、卡牌与武将目录、规则结算、AI、回放、随机数、
存档以及 CLI/TUI 交互。

开发入口可按改动范围查找：

| 改动内容 | 主要位置 |
|---|---|
| 卡牌/武将 JSON 及解析 | `resources/`、`src/card/catalog.hpp`、`src/hero/catalog.hpp` |
| 全局数值与角色配比 | `src/game/core/rules.hpp`、`src/game/core/roles.hpp` |
| 回合与终局流程 | `src/game/flow/` |
| 效果、响应与装备结算 | `src/game/resolve/`、`src/game/core/effect.hpp` |
| AI 与真人决策 | `src/game/ai/`、`src/game/core/decision.hpp` |
| 命令与中文帮助 | `src/cli/` |
| 存档格式、读取与写入 | `src/save/` |
| TUI 共享状态与 FTXUI 前端 | `src/tui/`、`tui/` |

新增结算机制时，应更新解析映射、机制实现状态与结算路径，
并在 `tests/` 中补充相应回归用例；涉及可交互动作时，也要核对
AI 与两种真人前端是否能完成选择。

## 常见问题

- **输入 `tkw` 提示找不到命令**：请使用构建产物的完整相对路径，
  例如 PowerShell 中的 `.\build\src\tkw.exe`。
- **找不到资源或加载牌堆失败**：默认 `resources/` 相对工作目录。
  请从仓库根目录运行，或传入 `--deck <资源目录>`；路径含空格时加引号。
- **子模块初始化失败**：先执行递归初始化；SSH 认证失败可使用前文的
  `pjh_cli` HTTPS 本地覆盖方式。
- **编译器不支持某个标准库功能**：检查编译器及其标准库的 C++20 支持，
  并确认 CMake 选中了预期编译器。
- **TUI 提示需要交互式终端**：它要求 stdin/stdout 均为 TTY，
  管道或重定向场景请使用 `tkw repl`；`tkw-tui --help` 可在非 TTY 下查看帮助。
- **读档提示牌表不符**：选回保存时的牌表和资源版本；模式与武将从存档恢复，
  `--mode` 不能强制更改存档模式。
- **关闭真人后仍出现决策提示**：在 REPL 中用 `new --no-human` 新建全 AI 局；
  真人配置与事件日志开关是不同设置。

## 第三方依赖

| 组件 | 用途 | 获取方式 |
|---|---|---|
| `pjh_result` | Result/Option 与错误传播 | Git 子模块 |
| `pjh_json` | JSON 资源与存档处理 | Git 子模块，含嵌套依赖 |
| `pjh_platform` | 文件系统与终端平台适配 | Git 子模块 |
| `pjh_cli` | 命令行解析与 REPL | Git 子模块 |
| doctest v2.5.0 | 测试框架 | 启用测试时由 FetchContent 获取 |
| FTXUI | 可选全屏终端界面 | 优先使用已安装包，否则获取 v7.0.3 |

各第三方组件的许可证与详细说明请查看相应目录。

## 免责说明

README 的命令速查与用法示例与 `tkw --help` 保持同源以防漂移，如有出入以
`tkw --help` 为准；REPL 内可用 `?` 与 `help <命令>` 查看。
