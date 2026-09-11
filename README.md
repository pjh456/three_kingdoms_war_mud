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
- `build/` 是构建产物，不入库。
- 下文示例中 `tkw` 均指 `./build/src/tkw`，且在仓库根目录执行（默认牌表路径
  `resources/` 相对当前工作目录）。

## 快速开始

```sh
tkw                   # 裸命令：跑一局 4 人 AI 对局到结束
tkw deal 2 1          # 位置参数跑一局：2 人、种子 1
tkw --human P0 repl   # P0 真人参与，REPL 交互模式
```

对局结束打印胜者（或平局）与对局统计块（回合数/每人体力/击杀/伤害/治疗）。

## 命令速查

与 `tkw --help` 的子命令表同源（措辞以 `--help` 为准）：

| 命令 | 别名 | 描述 |
|---|---|---|
| `tkw`（无子命令） | — | 直接跑一局 AI 对局（默认 4 人、种子 42） |
| `audit` | — | 审计牌堆，列出引擎未实现的卡 |
| `cards` | — | 列出牌表（牌堆种类与张数） |
| `deal <玩家数> <种子>` | — | 跑一局：deal <玩家数> <种子> |
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
| `-p, --players <n>` | 玩家数（2–8，默认 4） |
| `--hand <n>` | 初始手牌数（默认 4） |
| `-s, --seed <n>` | 随机种子（默认 42） |
| `-v, --verbose` | 打印事件日志（摸/打/弃牌、伤害、体力、阵亡） |
| `--autosave <path>` | REPL 退出时自动存档路径（空串关闭，默认 `tkw-autosave.json`） |
| `--human <seat>` | 真人座位（可重复：`--human P0 --human P2`；存档不保存，读档后需重新指定） |

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
- 真人参与：`tkw --human P0 repl`，轮到你时按提示输入 `play <序号>`（出牌）或
  `pass`（不出），弃牌阶段输入 `discard <序号> ...`；
- REPL 退出时若有进行中的会话，自动存档到当前目录的 `tkw-autosave.json`
  （`--autosave <path>` 可改路径，空串关闭）；`--human` 设置不存入存档，
  读档后需重新指定。

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
- 新增卡牌只改牌表目录，代码零改动；`tkw --deck <dir> audit` 可审计未实现卡；
- 用 `--deck <dir>` 指向自定义牌表，如 `tkw --deck mydeck deal 2 1`。

标准版牌表共 32 个卡牌定义、108 张牌。

## 规则简表（默认值）

| 项 | 值 |
|---|---|
| 初始/上限体力 | 4 |
| 初始手牌 | 每人 4 张 |
| 摸牌阶段 | 每回合摸 2 张 |
| 「杀」次数 | 每回合 1 次（装备诸葛连弩不限） |
| 手牌上限 | 体力上限（超出时弃牌） |
| 击杀奖励 | 摸 3 张牌 |
| 玩家数 | 2–8 人 |
| 胜利条件 | 唯一存活；达到 1000 回合上限判平局 |

规则数值数据驱动，每张牌的效果文案见卡文件 `text` 字段；详细规则以 `tkw --help`
与卡面文案为准。

## 存档说明

- `save <file>` / `load <file>`（别名 `w` / `l`）手动存读；存档为 JSON
  （`format: tkw-save`，version 1），写入走原子替换（先写临时文件再 rename），
  读取时全量校验通过后才落子。
- **牌表指纹校验**：存档记录保存时牌表语义字段的 FNV-1a 指纹；加载时与当前
  `--deck` 牌表比对，牌表已改动则拒绝加载：
  `存档加载失败 (kind=2): deck.hash`（DeckMismatch）。
- **自动存档**：REPL 退出时若有进行中的会话，自动存档到当前目录
  `tkw-autosave.json`；`--autosave <path>` 改路径，空串关闭。

## 目录结构

```
three_kingdoms_war_mud/
├── CMakeLists.txt          # 顶层：C++20、TKW_ENABLE_TESTS 开关
├── src/                    # 引擎（header-only，INTERFACE 库）
│   ├── main.cpp            #   CLI 入口（src 下唯一 .cpp）
│   ├── cli/  io/  config/  #   命令树 / 文件读写 / 资源加载
│   ├── card/  entity/  event/  # 卡牌域 / 实体域 / 事件总线
│   ├── game/               #   对局五层 core/query/resolve/flow/ai（严格单向依赖）
│   └── save/  util/        #   存档 / 通用（Result、随机源）
├── tests/                  # doctest，目录镜像 src，11 个测试可执行文件
├── resources/              # 数据驱动牌表：deck.json + cards/<id>.json
├── thirdparty/             # 子模块：pjh_result / pjh_json / pjh_cli / pjh_platform
└── build/                  # 构建产物（不入库）
```

## 免责说明

README 的命令速查与用法示例与 `tkw --help` 保持同源以防漂移，如有出入以
`tkw --help` 为准；REPL 内可用 `?` 与 `help <命令>` 查看。
