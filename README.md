# 三国杀式卡牌对局引擎（tkw）

三国杀式卡牌对局引擎：C++20 header-only 引擎 + `tkw` 命令行/REPL，牌表数据驱动
（`resources/` 下的 JSON）。AI 与真人混坐对局：判定 → 摸牌 → 出牌（杀/锦囊/装备）→
弃牌，结算伤害、濒死救场与阵亡，可一键跑完整局，也可进 REPL 逐回合参与。

## 构建与测试

依赖 C++20 编译器、CMake ≥ 3.21、Ninja。第三方库是 git 子模块且为**两层**结构
（`thirdparty/pjh_json` 内还嵌套子模块），克隆后必须用 `--recursive` 补齐：

```sh
git clone <仓库地址> three_kingdoms_war_mud
cd three_kingdoms_war_mud
git submodule update --init --recursive

cmake -B build -G Ninja       # 配置（首次构建需联网拉取 doctest v2.5.0）
cmake --build build           # 构建，产物 build/src/tkw
ctest --test-dir build        # 运行全部测试
```

- 测试构建开关 `-DTKW_ENABLE_TESTS=ON|OFF`（默认 ON）；OFF 时 `tests/` 不进入构建。
- 可选终端界面开关 `-DTKW_ENABLE_TUI=ON|OFF`（默认 OFF）；OFF 时不探测、不拉取
  FTXUI，对 `tkw`/引擎/现有测试零影响，详见下文「可选终端界面（tkw-tui）」。
- `build/` 是构建产物，不入库。
- 下文示例中 `tkw` 均指 `./build/src/tkw`，且在仓库根目录执行（默认牌表路径
  `resources/` 相对当前工作目录）。

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
  [--human <座位>] [--no-human]`、`deal <players> <seed>`、`step`、`run`/`r`、
  `status`/`st`、`save`/`w <file>`、`load`/`l <file>`、`cards [--text] [--deck 路径]`、
  `rules [关键词] [--deck 路径]`、`audit [--deck 路径]`、`quit`/`q`、`help`/`?`；
  `Esc`/`Ctrl-C` 退出。
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
- 卡牌查询：命令栏 `cards [--text]`、`rules [关键词]`、`audit` 就地出结果，逐行
  写入日志面板（长列表用日志翻阅）；牌表来源优先序为行内 `--deck` > 活动会话 >
  启动 `--deck`（与 REPL 只读命令同口径），行内坏路径写一行「加载牌堆失败」提示、
  不阻断会话。`simulate` 仍在 CLI 执行，命令栏输入会提示改用 `tkw`。
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
- Windows/MSVC 未验证。

## 命令速查

与 `tkw --help` 的子命令表同源（措辞以 `--help` 为准）：

| 命令 | 别名 | 描述 |
|---|---|---|
| `tkw`（无子命令） | — | 直接跑一局 AI 对局（默认 4 人、种子 42） |
| `audit` | — | 审计牌堆，列出引擎未实现的卡 |
| `cards` | — | 列出牌表（牌堆种类与张数；`--text` 附效果文案） |
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
| `--ai <simple\|aggressive>` | AI 难度（默认 `simple` 贪心；`aggressive` 伤害/多目标先行） |
| `--mode <brawl\|identity>` | 对局模式（默认 `brawl` 乱斗；`identity` 身份局需 4–8 人，`load` 以存档为准） |

上表的选项各命令都声明并接受，但生效面不同：`--deck`/`--players`/`--hand`/`--seed`/`--ai`/`--mode`
只对建局/载入类命令（裸 `tkw`/`deal`/`new`/`load`/`repl`/`simulate`）实际生效；`cards`/`audit`/
`rules` 只读 `--deck`；`step`/`run`/`status`/`save` 只读取其中的 `--verbose`（`step`/`run`）
或全不读取（`status`/`save`）。`--human` 只在运行真人参与对局的命令生效，`audit`/`cards`/
`rules`/`simulate` 会明确拒绝；`--autosave`/`--history` 只在 `repl` 生效。**`--mode` 在 `load` 上
不生效**：载入的模式与角色以存档为准，避免用命令行强行改写存档模式。

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

## 自定义牌表

牌表是纯数据目录，默认 `resources/`：

```
<牌表目录>/
├── deck.json           # name / expansion / cards（卡牌 id 列表）
└── cards/<id>.json     # 每张卡一个定义文件，id 须与文件名一致
```

- `deck.json` 的 `cards` 引用顺序 = 牌堆构建顺序；
- 卡文件定义 `id`/`name`/`type`/`subtype`/`copies`（逐张列花色与点数，条数即
  张数）/`text`（效果文案），按需再带 `effect`（主动效果）/`equip`（装备）/
  `judge`（判定）/`abilities`（被动能力）；
- 新增卡牌只改牌表目录，代码零改动；`tkw --deck <dir> audit` 可审计未实现卡
  （未知机制名逐卡列出，只审机制、不建局；`deal`/`simulate` 遇到未知机制仍会拒绝建局）；
- 用 `--deck <dir>` 指向自定义牌表，如 `tkw --deck mydeck deal 2 1`。

标准版牌表共 32 个卡牌定义、108 张牌。

## 规则简表（默认值）

| 项 | 值 |
|---|---|
| 初始/上限体力 | 4 |
| 初始手牌 | 每人 4 张 |
| 摸牌阶段 | 每回合摸 2 张 |
| 「杀」次数 | 每回合 1 次（装备诸葛连弩不限） |
| 手牌上限 | 当前体力值（超出时弃牌） |
| 击杀奖励（乱斗） | 摸 3 张牌 |
| 身份局击杀奖惩 | 击杀反贼摸 3 张；主公击杀忠臣弃光手牌与装备；其余身份无奖励 |
| 玩家数 | 2–8 人（身份局需 4–8 人） |
| 胜利条件 | 唯一存活；达到 1000 回合上限判平局 |
| 身份局胜利 | 主公阵营胜（主公存活）/ 反贼胜（主公阵亡且内奸非唯一存活者）/ 内奸胜（唯一存活内奸）；同归于尽为平局 |

规则数值数据驱动；每张牌的效果文案可直接查询：`tkw rules` 列出全部、
`tkw rules <关键词>` 过滤，`tkw cards --text` 在牌表后附文案。详细规则以
`tkw --help` 与卡面文案为准。

## 存档说明

- `save <file>` / `load <file>`（别名 `w` / `l`）手动存读；存档为 JSON
  （`format: tkw-save`，version 1），写入走原子替换（先写临时文件再 rename），
  读取时全量校验通过后才落子。
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
├── tests/                  # doctest，目录镜像 src，12 个测试可执行文件
├── resources/              # 数据驱动牌表：deck.json + cards/<id>.json
├── thirdparty/             # 子模块：pjh_result / pjh_json / pjh_cli / pjh_platform
└── build/                  # 构建产物（不入库）
```

## 免责说明

README 的命令速查与用法示例与 `tkw --help` 保持同源以防漂移，如有出入以
`tkw --help` 为准；REPL 内可用 `?` 与 `help <命令>` 查看。
