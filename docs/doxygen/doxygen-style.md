# Doxygen 注释规范（tkw）

> 本文件是并行注释任务的**唯一规范来源**。所有注释 agent 必须先读本文件，
> 不得自行揣测风格。目标：让 `doxygen docs/doxygen/Doxyfile` 在 `WARN_AS_ERROR=YES` 下零警告。
> 本文件与同目录工具纳入版本控制。

## 0. 硬约束（违反即任务失败）

- **只改注释。** 不得改动任何代码 token：标识符、类型、参数、默认值、`#include`、
  字符串字面量、预处理指令、`constexpr` 数值、`enum` 顺序一律不动。
- **不得新增/删除文件**，不得新增 `.cpp`（各模块是 `.hpp` + `.cpp` 的 `tkw_*` 静态库）。
- **不得运行 `cmake`/编译/`ctest`/`git commit`**。收尾由编排者单点构建。
- **不得改 `CMakeLists.txt`、`docs/doxygen/Doxyfile`、`docs/doxygen/`**。
- 注释语言：**中文**（与仓库现有文风一致）；标识符、Doxygen 标签名保持英文。
- 改动须为**纯注释行**：`git diff -w` 中不应出现代码行变化。

## 1. 文件头

每个头文件（`.hpp`）顶部、`#ifndef` 之前，必须有：

```cpp
/**
 * @file   decision.hpp
 * @brief  玩家决策源（策略接口）。
 * @details 结算器/回合流程需要「玩家做什么选择」时调用。实现即玩家策略：
 *          测试注入确定性假策略，CLI/TUI/网络层注入真人输入。
 * @ingroup tkw_game_core
 */
```

- `@file` 值 = 文件名（含扩展名），不含路径。
- `@brief` **一行**，以句号结尾，说明「这是什么」。
- `@details` 承载原 `@brief` 里的长句与设计动机；可省略。
- `@ingroup` 取值见第 7 节，必须与该文件所属 CMake target 对应。
- 其他可选：`@note`、`@warning`、`@see`。

## 2. 类型（class / struct / enum / using）

```cpp
/**
 * @brief  对局规则数值（默认值 = 标准多人乱斗）。
 * @details 这是「规则数值」的唯一事实源；引擎经 rules_of(ctx) 读取。
 * @note   新增数值一律加在这里，不要在引擎里写魔法数。
 * @warning 修改默认值会改变所有既有存档语义，需同步评估 `save::kVersion`。
 */
struct RulesConfig
{
    int draw_per_turn = 2; /**< 每回合摸牌阶段摸牌数。 */
    ...
};
```

- `struct`/`class` 的**每个公共成员**后置 `/**< ... */`，一句语义 + 句号。
- 枚举的**每个枚举值**都要注释（行内 `/**< */` 或前置 `/** */`）。
- `using`/类型别名用 `@brief`；模板别名用 `@tparam`：见第 4 节。
- `@pre`/`@post` 只用于描述类型层面的**不变量**（如「不可拷贝/移动」）。

## 3. 函数 / 方法

标签顺序固定：`@brief` → `@details` → `@param` → `@tparam` → `@return` →
`@retval` → `@pre` → `@post` → `@note` → `@warning` → `@see`。

```cpp
/**
 * @brief  响应窗口：选择打出的响应牌（杀/闪）。
 * @details 结算器在需要响应时询问；实际消费与落子由结算器负责（单一写者）。
 * @param[in] ctx       只读容器视图（不含 EventBus/Rng）。
 * @param[in] entity_id 被询问的实体 id。
 * @param[in] kind      需要的响应牌类别。
 * @param[in] prompt    来源牌/使用者/伤害量（只读事实，来源未知时留空）。
 * @return 要打出的手牌；`None` = 不响应。
 * @retval Some 引擎将校验手牌确含该牌并负责消费。
 * @retval None 表示放弃响应。
 * @pre   `ctx` 生命周期覆盖本次调用，`entity_id` 在 `ctx` 中可查。
 * @post  本接口不改变任何状态。
 * @warning 实现不得缓存 `ctx` 内指针，跨调用即失效。
 * @see   ReadOnlyContext, turn::execute_turn
 */
```

- **`@param` 方向**：`[in]` 只读输入、`[out]` 纯输出、`[in,out]` 既读又写。
  参数描述中的类型名用 `@p` 或反引号，路径/标识符用反引号。
- **`@return`** 一句总述返回值语义；**`@retval`** 逐值展开
  （`Ok/Err`、`Some/None`、枚举结果）。有多个返回分支时必须用 `@retval`。
- 简单存取器（逻辑显然）可只写 `@brief`，但**公共接口的每个参数都要有 `@param`**。
- `override`/`virtual` 实现若语义与基类一致，写 `/** @copydoc 基类::方法 */`
  或一行 `@brief` + `@note 实现基类契约`，不重复长文档。

## 4. 模板

- `@tparam` 逐个模板参数说明约束：

```cpp
/**
 * @brief  精确类型事件总线。
 * @tparam Event 事件类型；按 `typeid(Event)` 精确分发（不做基类归并）。
 * @param[in] priority 执行优先级；同优先级按注册序。
 * @return RAII 订阅句柄，析构即退订。
 */
template <typename Event>
class CommonEventBus;
```

## 5. 错误契约（本仓无异常）

- 本仓错误一律走 `Result`/`Option`（`src/util/types.hpp`），**不写 `@throws`**。
- `Result<T,E>`：用 `@retval Ok` / `@retval Err(<ErrorKind>)` 描述。
- `Option<T>`：用 `@retval Some` / `@retval None` 描述。
- 错误/枚举取值可用 `@see` 指向对应 enum。
- 失败时的状态保证写进 `@post`（如「失败时目标对象不被污染」）。

## 6. 跨引用与分组

- 类型、函数用 `@ref 名字` 或直接写名字（Doxygen 自动链接）。
- 相关接口用 `@see A, B, C` 收尾。
- 不要手写 URL；不要写 `@author`/`@date`/版本号（git 已记录）。

## 7. 模块分组（`@ingroup` 取值表）

分组定义集中在 `docs/doxygen/doxygen-groups.dox`（**不要**在各头文件里 `@defgroup`）。
`@ingroup` 必须与文件所属 CMake target 一致：

| 目录 | `@ingroup` / CMake target |
|---|---|
| `src/util/` | `tkw_util` |
| `src/io/` | `tkw_io` |
| `src/config/` | `tkw_config` |
| `src/event/` | `tkw_event` |
| `src/entity/` | `tkw_entity` |
| `src/card/` | `tkw_card` |
| `src/hero/` | `tkw_hero` |
| `src/save/` | `tkw_save` |
| `src/game/core/` | `tkw_game_core` |
| `src/game/query/` | `tkw_game_query` |
| `src/game/resolve/` | `tkw_game_resolve` |
| `src/game/flow/` | `tkw_game_flow` |
| `src/game/ai/` | `tkw_game_ai` |
| `src/cli/` | `tkw_cli` |
| `src/tui/` | `tkw_tui` |

## 8. 写作要求

- **只写现状**，不写「将来」「TODO」「暂未」（除非现有注释已如此且属实）。
- **这是契约文档**：用祈使/规范语气（「调用方必须先…」「本接口不改变状态」）
  优于描述性语气。
- `@brief` 单行，控制在 ~80 字符内（列宽硬上限 90）。
- 用反引号标注标识符、路径、标签（如 `` `None` ``、`` `save::read` ``）。
- 不重复代码已显而易见的字面意思；重点写**前置条件、副作用、失败语义、不变量**。

## 8.1 全局视角（重要）

注释是**领域模型文档**，不是某个模块或某个调用点的说明。定义类型、枚举、成员、
字段时，必须回答「它在整局模型里**是什么**、承载什么语义、有什么不变量」，而不是
「谁查它、谁负责、在哪个文件」。

反面（局部视野）→ 正面（领域契约）：

- ✗ `装备被动能力（一件装备可带多个，经 equip.hpp 查询）。`
- ✓ `装备的被动能力：一件装备可同时具备多项，每项对应一条由装备区结算点触发的
  被动规则；不含任何能力时为空。`
- ✗ `效果类别的实现状态（经 effect.hpp 属性表查询）。`
- ✓ `效果类别是否已被引擎实现结算；未实现的效果在建局时经审计告警，不静默按
  无效果处理。`
- ✗ `由调用方负责落盘原子性。`
- ✓ `@post 写出为原子替换：目标路径在成功前保持旧内容；失败不产生半截文件。`

硬性规则：

- 类型/枚举/成员/字段的 `@brief` **不得**出现 `.hpp` 文件名、模块名，或
  「经 xxx 查询」「由 xxx 负责」「调用方」等**以使用方式充当定义**的措辞。
- 跨模块关系若确有助益，只放 `@see`，不写「见 xxx.hpp」。
- 函数契约中「调用方须…」（`@pre`/`@post`）是**正当的契约语言**，保留；
  但它描述**规则义务与不变量**，不是「本函数被哪个流程调用」。
- 文档必须回答：这是什么、在整局生命周期中如何变化、边界在哪里、如何失败。
  从整体开发视角出发，而非单个消费点的视野。

## 9. 机械检查

```sh
python3 docs/doxygen/doxygen_coverage.py src        # 静态缺 tag 报告（启发式）
doxygen docs/doxygen/Doxyfile                       # 权威门禁（WARN_AS_ERROR=YES）
```

静态脚本只做结构检查（`@file`、doc 块缺 `@brief`、`@param` 数量不符），
**Doxygen 警告才是最终验收标准**。

依赖环境：`doxygen` 与 `graphviz`（`HAVE_DOT=YES` 需 `dot`；从仓库根运行）。
