<div align="center">
  <img src="docs/assets/tkw-logo.svg" width="200" alt="tkw logo" />

  <h1>三国杀式卡牌对局引擎（tkw）</h1>

  <p>
    <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" />
    <img alt="Platform: Windows / Linux / macOS" src="https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-1f6feb?style=for-the-badge" />
    <img alt="CMake 3.21+" src="https://img.shields.io/badge/CMake-3.21%2B-064F8C?style=for-the-badge&logo=cmake&logoColor=white" />
  </p>

  <p>
    <a href="https://pjh456.github.io/three_kingdoms_war_mud/">API 文档</a>
  </p>
</div>

本仓库游戏灵感来源自《三国杀》桌游，提供 TUI 与 CLI 两种方式进行游玩。

目前已完全实现单机的多人混战基础模式，并在此基础上添加了测试性的武将、性别与军争玩法。

## 游戏特性

- 全仓库使用 C++20 编写，在开发过程中使用现代 C++ 语法与设计；
- 对局全程离线，无需联网，存档数据可在本地读写；
- 添加基于状态机驱动的 AI，并能根据回合进行不同决策；
- 完整实现《三国杀》的基础玩法；
- 支持 REPL 逐回合解释操作与一键模拟全局等多种模式；
- 支持 CLI 与 TUI 两种游玩方式。

## 环境依赖

- 支持 C++20 的编译器，目前已在 GCC、Clang 与 MSVC 下进行测试，详见仓库 CI
- CMake 版本 ≥ 3.21
- Ninja（可选）
- Git

## 快速开始

仓库的依赖作为子模块在 Github 平台进行分层管理，在构建前需要递归克隆：

```sh
git clone https://github.com/pjh456/three_kingdoms_war_mud.git --recursive
```

如果已经克隆仓库，进入仓库目录后执行：

```sh
git submodule update --init --recursive
```

在递归拉取子模块后，使用 CMake 开始构建项目：

```sh
cmake -S . -B build  # 配置 CMake 项目
cmake --build build  # 构建 CMake 项目
```

除此之外，还可以通过以下指令运行项目自带的单元测试：

```sh
ctest --test-dir build --output-on-failure
```

构建产物说明如下：

- CLI 产物路径在 `build/src` 下，有一个 `tkw` 的可执行程序
- TUI 默认不构建。可以通过下述编译开关选项指定开启，产物在 `build/tui` 下，有一个 `tkw-tui` 的可执行程序
- CMake 编译开关：`-DTKW_ENABLE_TESTS=ON|OFF`（是否编译测试，默认 ON）、`-DTKW_ENABLE_TUI=ON|OFF`（是否编译 TUI，默认 OFF）。
- 可执行程序只建议在仓库根目录运行，因为卡牌表需要在运行路径下解析 `resources/` 配置路径；同样地，资源与存档的相对路径都相对启动时的工作目录。

## 快速开始

### 全员 AI 模拟对局

```sh
tkw                            # 直接跑一局 AI 对局（默认 4 人、种子 42）
tkw deal 2 1                   # 2 人、种子 1
tkw --mode identity deal 5 1   # 身份局：5 人、种子 1
```

`deal <玩家数> <种子>` 是位置参数，两个都必填。

同时还支持批量模拟：

```sh
tkw simulate 100 4   # 100 局、4 人（第二个参数可选）
```

`simulate` 只跑全 AI 对局，会拒绝真人座位参数。默认情况下随机种子为 `1`，且每局种子递增。模拟的输出结果含局数、种子区间、胜场 / 阵营胜场、平局、平均回合。

AI 包含两种决策：`simple`（贪心，默认）和 `aggressive`（优先伤害与多目标）。两者共享同一套合法动作与响应机制。`aggressive` 不保证在所有局面更优。

### 真人游玩对局

```sh
tkw --human P0 repl   # P0 由玩家操作，进入 REPL 对局
```

初始游玩的时候可以通过以下指令快速游玩：

```sh
tkw --human P0 repl        # 进 REPL，P0 由你操作
new --players 2 --seed 1   # 开新局
step                       # 推进一个回合
```

轮到玩家时，按窗口提示操作：

- 出牌窗口：`play <序号>` 出牌（可连续出牌），`pass` 结束出牌阶段。
- 弃牌窗口：手牌超过当前体力时出现，`discard <序号> ...` 弃足够张数（这个窗口不能 `pass`）。
- 响应 / 濒死救场 / 无懈可击 / 五谷丰登亮牌：都用 `play <序号>`，`pass` 放弃（强制选择的除外）。
- 窗口里输入 `?` 或 `help` 看用法；`card <序号>` 查看候选牌的完整效果文案。

REPL 里的常用命令：

- `?` 列出所有命令，`help <命令>` 看单条用法，`quit` 退出。
- `step` 执行当前会话的一个回合，可反复输入。
- `run`（别名 `r`）跑到当前会话结束；`status`（`st`）查看当前状态。

一个 `step` = 一个角色的完整回合。期间可能连续出现出牌、响应、濒死救场、无懈可击、选目标、弃牌等多个窗口。

座位是 `P0` 到 `P<人数-1>`。不指定 `--hero` 就是无名座位，不会自动随机选将。

事件日志：真人局默认开启，全 AI 局默认关闭。`--verbose` / `--no-verbose` 可覆盖。回合开头会打印 `—— 回合 N：<玩家> ——`。真人局里对手的摸牌只显示「未知牌」。

## 终端界面（TUI）

TUI 默认关闭，可以通过以下内容开始构建：

```sh
cmake -B build -DTKW_ENABLE_TUI=ON  # 首次联网拉取 FTXUI v7.0.3
cmake --build build # 产物 build/tui/tkw-tui
.\build\tui\tkw-tui.exe # Windows 下的产物
./build/tui/tkw-tui # Linux 下的产物
```

- 关闭时不探测也不下载 FTXUI，对 CLI 零影响。
- 同样地，构建产物必须在仓库根目录运行。需要在真正的交互式终端里跑（stdin/stdout 都必须是 TTY）。
- 不是 TTY 会打印「需要交互式终端」并以退出码 1 退出。
- 查看帮助用 `tkw-tui --help`，非 TTY 也能用，退出码 0。
- 四个面板：棋盘 / 手牌 / 日志 / 状态。

真人局在 TUI 里，底部命令栏变成决策面板：

- `↑` / `↓` 选择候选，`Enter` 确认。
- `p` 放弃（只在允许放弃的窗口）。
- 弃牌窗口用空格多选，选够张数后 `Enter`。
- 数字键 `1`–`9` 直选。
- 待决期 `q` 退出；`Esc` / `Ctrl-C` 全局退出。

日志面板：

- `PgUp` / `PgDn` 翻页（始终可用）。
- `End` 回最新、`Home` 回最早（只在命令栏为空时生效）。
- 终端过小时会自动隐藏次要面板，但保留底部命令输入行。

命令栏支持这些只读查询与批量命令：

`cards [--text] [--deck 路径]`、`rules [关键词] [--deck 路径]`、`audit [--deck 路径]`、`decks [--deck 路径]`、`heroes [--deck 路径]`。

注意：TUI 的 `heroes` / `decks` 只接受 `--deck <路径>`，不接受位置参数 `[目录]`。CLI 的 `heroes` / `decks` 才接受位置参数目录。

其他：TUI 事件日志恒开，没有 `--verbose` / `--no-verbose`。退出时若有进行中的会话会自动存档，退出信息写到 stderr。

## 命令速查

| 命令                       | 别名 | 说明                                                     |
| -------------------------- | ---- | -------------------------------------------------------- |
| `tkw`（无子命令）          | —    | 直接跑一局 AI 对局（默认 4 人、种子 42）                 |
| `audit`                    | —    | 审计牌堆，列出引擎未实现的卡                             |
| `cards`                    | —    | 列出牌表（种类与张数）；`--text` 附效果文案              |
| `decks [目录]`             | —    | 列出可用牌表（扫描根目录及直接子目录，缺省 `resources`） |
| `heroes [目录]`            | —    | 列出可用武将（随 `--deck`；缺 `heroes.json` 显示无数据） |
| `rules [关键词]`           | —    | 查询卡牌效果说明（可按关键词过滤）                       |
| `deal <玩家数> <种子>`     | —    | 跑一局（位置参数，两个都必填）                           |
| `simulate <局数> [玩家数]` | —    | 批量模拟（第二个参数可选）                               |
| `new`                      | —    | 开新对局（用 `--players` / `--seed` / `--hand`）         |
| `step`                     | —    | 执行当前会话的一个回合                                   |
| `run`                      | `r`  | 跑到当前会话结束                                         |
| `status`                   | `st` | 查看当前会话状态                                         |
| `save <file>`              | `w`  | 保存当前对局                                             |
| `load <file>`              | `l`  | 加载存档                                                 |
| `repl`                     | —    | 进入交互模式（`?` 查看命令，`quit` 退出）                |

## 启动选项

| 选项                        | 说明                                                        |
| --------------------------- | ----------------------------------------------------------- |
| `-d, --deck <dir>`          | 资源目录，默认 `resources`                                  |
| `-p, --players <n>`         | 玩家数（2–8；REPL 内缺省继承启动 `--players`，否则 4）      |
| `--hand <n>`                | 初始手牌数（默认 4）                                        |
| `-s, --seed <n>`            | 随机种子（默认 42）                                         |
| `-v, --verbose`             | 打印事件日志（真人局默认开启；`--no-verbose` 关闭）         |
| `--autosave <path>`         | REPL 退出自动存档路径（空串关闭，默认 `tkw-autosave.json`） |
| `--history <path>`          | REPL 命令历史文件（默认不持久化；父目录须已存在）           |
| `--human <seat>`            | 真人座位（可重复，如 `--human P0 --human P2`；存档不保存）  |
| `--no-human`                | 清空真人座位（与 `--human` 同时给时，清空优先）             |
| `--hero <seat=id>`          | 指定武将（可重复，如 `--hero P0=zhangfei`）                 |
| `--ai <simple\|aggressive>` | AI 难度（默认 `simple`）                                    |
| `--mode <brawl\|identity>`  | 对局模式（默认 `brawl` 乱斗；`identity` 身份局需 4–8 人）   |

选项生效范围：

- `--deck` / `--players` / `--hand` / `--seed` / `--ai` / `--mode` 只对建局 / 载入类命令生效：裸 `tkw`、`deal`、`new`、`load`、`repl`、`simulate`。
- `audit`、`rules` 只读 `--deck`；`cards` 读 `--deck` 和 `--text`。
- `step`、`run` 只读 `--verbose`；`status`、`save` 不读任何选项。
- `--autosave`、`--history` 只在 `repl` 生效。
- `--mode` 在 `load` 上不生效：模式与角色以存档为准。
- `--hero` 在 `load` 行内会被拒绝（退出码 1，并提示改用 `new --hero <座位>=<武将>`）：武将随存档恢复，没有读档换将。
- `--human` 只在真正跑对局的命令生效；`audit`、`cards`、`decks`、`heroes`、`rules`、`simulate` 会明确拒绝。
- 别名：`run` / `r`、`status` / `st`、`save` / `w`、`load` / `l`。

## 规则

| 项               | 值                                                                                                                 |
| ---------------- | ------------------------------------------------------------------------------------------------------------------ |
| 初始 / 上限体力  | 无名座位 4；选将后以武将为准                                                                                       |
| 初始手牌         | 每人 4 张                                                                                                          |
| 摸牌阶段         | 每回合摸 2 张                                                                                                      |
| 「杀」次数       | 每回合 1 次（诸葛连弩或咆哮可解除）                                                                                |
| 手牌上限         | 当前体力值，超出要弃牌                                                                                             |
| 击杀奖励（乱斗） | 摸 3 张                                                                                                            |
| 身份局击杀奖惩   | 击杀反贼摸 3 张；主公击杀忠臣弃光手牌与装备；其余无奖励                                                            |
| 玩家数           | 2–8 人（身份局需 4–8 人）                                                                                          |
| 乱斗胜利         | 唯一存活；无存活者平局；超过默认 1000 回合上限按平局                                                               |
| 身份局胜利       | 主公阵营胜（主公存活且反贼、内奸全阵亡）/ 反贼胜（主公阵亡且内奸非唯一存活）/ 内奸胜（唯一存活内奸）；同归于尽平局 |

身份局里主公固定是 `P0`，其余角色按种子随机分配。配比（主公 / 忠臣 / 反贼 / 内奸）：

| 人数 | 主公 | 忠臣 | 反贼 | 内奸 |
| ---- | ---- | ---- | ---- | ---- |
| 4    | 1    | 1    | 1    | 1    |
| 5    | 1    | 1    | 2    | 1    |
| 6    | 1    | 1    | 3    | 1    |
| 7    | 1    | 2    | 3    | 1    |
| 8    | 1    | 2    | 4    | 1    |

界面里的「回合数」是按每个角色执行一次回合计数，不是全体各动一次才加 1。

## 牌表

仓库自带两副牌表：

- `resources/`：标准版，32 种 / 108 张。
- `resources/junzheng/`：军争篇，17 种 / 83 张（测试版）。

`tkw decks` 可以一览。用 `--deck <路径>` 选牌表：

```sh
tkw --deck resources/junzheng deal 2 1   # 2 人、种子 1，用军争篇
tkw --deck resources/junzheng repl       # 或进 REPL 逐回合玩
```

军争篇是独立牌表，不会叠加标准版。它目录里没有 `heroes.json`，所以切到军争篇不会加载武将（玩家将是无名座位）。

## 武将（实验性功能）

`tkw heroes` 可以一览。用 `--hero <座位>=<武将>` 指定：

```sh
tkw --hero P0=zhangfei deal 2 1   # P0 用张飞
```

| 武将   | id             | 体力 | 性别 | 技能                             |
| ------ | -------------- | ---- | ---- | -------------------------------- |
| 张飞   | `zhangfei`     | 4    | 男   | 咆哮（杀无次数限制）             |
| 关羽   | `guanyu`       | 4    | 男   | 武圣（红色牌当杀）               |
| 周瑜   | `zhouyu`       | 3    | 男   | 英姿（摸牌阶段多摸一张）         |
| 司马懿 | `simayi`       | 3    | 男   | 反馈（受伤后获得伤害来源一张牌） |
| 马超   | `machao`       | 4    | 男   | 马术（计算距离 -1）              |
| 黄月英 | `huangyueying` | 3    | 女   | 奇才（使用锦囊无距离限制）       |
| 赵云   | `zhaoyun`      | 4    | 男   | 龙胆（闪当杀、杀当闪）           |
| 甄姬   | `zhenji`       | 3    | 女   | 倾国（黑色牌当闪）               |

- 武将数据在 `<牌表目录>/heroes.json` + `<牌表目录>/heroes/<id>.json`，独立于牌表。
- 武将不参与牌表校验，所以换武将不会让旧存档报错。
- 若武将含引擎尚未实现的技能，建局时会打印中文警告，不会静默当成无技能。
- 没有 `heroes.json` 的自定义牌表照常可玩，武将回落无名座位。

## 存档

- `save <file>`（别名 `w`）/ `load <file>`（别名 `l`）手动存读。
- 存档是 JSON。写入是原子替换（先写临时文件再改名）。读取时全部校验通过才落子。
- 存档记录会话的 AI 档与对局统计，读档后自动恢复。`load <file> --ai <档>` 可覆盖存档里的 AI 档。
- 身份局存档额外记录模式与逐座角色，读档后自动恢复。
- `--human` 与 `--verbose` 不存进存档；读档后要重新指定真人座位。
- 自动存档：REPL 退出时若有进行中的会话，存到当前目录 `tkw-autosave.json`。
- 手动保存适合在一次 `step` 完成之后做，不是任意决策窗口的即时快照。

读非默认牌表的存档时，要选回保存时的资源目录：

```sh
tkw --deck resources/junzheng repl   # 再 load save.json
# 或在 REPL 里：load save.json --deck resources/junzheng
```

## 常见问题

- **提示找不到 `tkw` 命令**：用完整路径，如 `.\build\src\tkw.exe`。
- **找不到资源 / 加载牌堆失败**：默认 `resources/` 相对当前目录。请在仓库根目录运行，或用 `--deck <资源目录>`；路径含空格要加引号。
- **子模块拉不下来**：先递归初始化。`thirdparty/pjh_cli` 用的是 GitHub SSH 地址，没有 SSH 密钥可以在本地覆盖成 HTTPS：

  ```sh
  git config submodule.thirdparty/pjh_cli.url https://github.com/pjh456/pjh_cli
  ```

- **Windows 下运行产生乱码**：终端窗口不支持 UTF-8 编码导致，在运行前先使用 `chcd 65001` 切换终端编码即可。
- **提示需要交互式终端（TUI）**：它要求 stdin/stdout 都是 TTY。管道或重定向场景请用 `tkw repl`。
- **读档提示牌表不符**：选回保存时的牌表，或新建对局。
- **关掉真人后还出现决策提示**：在 REPL 里用 `new --no-human` 新建全 AI 局。

## 已知限制

- 命令行与 REPL 里看不到手牌的花色和点数（全屏 TUI 才显示，如 `♠7`）。所以关羽武圣、甄姬倾国、火攻这类要看颜色 / 花色的牌，在 CLI 里不太好判断。
- 界面里不显示角色的性别。雌雄双股剑按双方性别判定，但你看不到对方性别。
- 不指定 `--hero` 时座位是无名角色；此时性别按座位奇偶占位（P0 男、P1 女…），并非真实设定。
- 军争篇牌表没有武将数据，切过去就没有武将可用。
