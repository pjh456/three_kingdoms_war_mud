/**
 * @file   decision_source.hpp
 * @brief  TUI 真人决策接缝：把引擎的决策请求折成纯值面板，经阻塞握手交给 UI 线程。
 * @details 线程契约：worker 线程在 `Decider::decide` 内构造纯值面板并等待，UI
 *          主线程经 `fetch_new`/`submit` 交接；退出先 `cancel` 唤醒再 join。
 *          面板只含字符串与产出载荷，不持 `catalog`/`Game` 指针，故待决期渲染
 *          不依赖引擎生命周期。
 * @note   本文件无 FTXUI、无输出副作用，可脱离 TTY 单测。
 * @ingroup tkw_tui
 */
#ifndef INCLUDE_TKW_TUI_DECISION_SOURCE_HPP
#define INCLUDE_TKW_TUI_DECISION_SOURCE_HPP

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "card/card.hpp"
#include "card/catalog.hpp"
#include "card/def.hpp"
#include "game/ai/decider.hpp"
#include "game/ai/legal.hpp"
#include "game/core/decision.hpp"
#include "hero/def.hpp"

namespace tkw
{
    namespace tui
    {
        /**
         * @brief 面板候选项：展示文本 + 回填 DecisionChoice 所需的纯值载荷。
         * @note text 在构造面板时由目录一次性解析成展示名，之后不再触目录。
         * @note card_* 三项是把牌面折成的展示纯值（查看牌面用）；对手手牌占位
         *       候选 hidden 为真且三项恒空，不泄漏隐藏信息。second_card_* 三项
         *       是丈八蛇矛 pair 第二张牌的同类纯值，只在决策者本手牌内折出。
         */
        struct PanelOption
        {
            std::string text;               /**< 已解析展示名/标签/目标串。 */
            std::string instance_id;        /**< Play/Response/Peach/Counter/Discard。 */
            std::string second_instance_id; /**< Response pair（两张当杀）。 */
            std::vector<std::string> targets; /**< Play 目标。 */
            std::size_t option_index = 0;   /**< PickCard/PickRevealed 候选下标。 */
            bool accepted = false;          /**< Trigger：true = 发动。 */
            std::string card_name;          /**< 卡牌展示名；无牌候选为空。 */
            std::string card_meta;          /**< 花色点数展示串（如 ♠7）；隐藏/无牌为空。 */
            std::string card_text;          /**< 卡牌效果文案；空 = 无说明。 */
            std::string second_card_name;   /**< pair 第二张牌展示名；非 pair 为空。 */
            std::string second_card_meta;   /**< pair 第二张牌花色点数串；非 pair 为空。 */
            std::string second_card_text;   /**< pair 第二张牌效果文案；空 = 无说明。 */
            bool hidden = false;            /**< 对手手牌占位：`card_*` 三项恒空。 */
            bool recast = false;            /**< Play：重铸（弃置此牌并摸一张）。 */
            bool converted_sha = false;     /**< Play：单张转化当杀（武圣红牌当杀）。 */
        };

        /**
         * @brief 待决决策的纯值视图：只含字符串与载荷，不持 catalog/Game 指针。
         * @note 在 worker 阻塞前由 DecisionRequest 折成，UI 渲染期只读本结构；
         *       即便引擎对象已析构也不会触碰悬垂指针。
         */
        struct DecisionPanelView
        {
            /** @brief 决策类别。 */
            tkw::game::ai::DecisionKind kind = tkw::game::ai::DecisionKind::Play;
            std::string actor;   /**< 决策者 id。 */
            std::string title;   /**< 主标题（按 kind 定文案，不含 actor 前缀）。 */
            std::vector<PanelOption> options; /**< 纯值候选列表。 */
            bool multi = false;  /**< Discard：多选。 */
            bool toggle = false; /**< Discard：空格切换。 */
            int need_count = 0;  /**< Discard：需选数量。 */
            bool allow_pass = false; /**< pass 是否合法。 */
            bool yes_no = false;     /**< Trigger：y/n 两选项。 */
        };

        namespace detail
        {
            /** @brief 对手手牌候选项的遮挡占位文本。 */
            inline constexpr const char *kHiddenHandPlaceholder = "（未知手牌）";

            /**
             * @brief  PickCard 候选来源分区标签；其余决策类别返回空串。
             * @param[in] zone 牌区。
             * @return 分区中文标签（`[手]`/`[装]`/`[判]`）；其它区返回空串。
             */
            const char *zone_tag(tkw::card::Zone zone);

            /**
             * @brief  装备能力一句话效果（只读展示，不打印卡牌 `text`）。
             * @param[in] ability 装备能力。
             * @return 中文效果描述。
             */
            const char *ability_hint(tkw::card::Ability ability);

            /**
             * @brief  从装备区反查携带该能力的装备名；查不到回落「装备能力」。
             * @param[in] req 决策请求；只读 `req.view.equip` 与目录。
             * @return 装备展示名；目录缺失或无匹配时返回「装备能力」。
             * @note   构造面板时调用一次。
             */
            std::string ability_name(
                const tkw::game::ai::DecisionRequest &req);

            /**
             * @brief  武将触发技标题：技能中文名 + 一句话效果；反馈携带伤害来源。
             * @param[in] req 决策请求；只读 `hero_skill` 与 `trigger_cause`。
             * @return 中文标题（yes/no 面板）。
             * @note   与装备能力标题同构，只在 `hero_trigger` 时使用。
             */
            std::string hero_trigger_title(
                const tkw::game::ai::DecisionRequest &req);

            /**
             * @brief  响应窗口标题：有来源牌时给来源与伤害后果，否则回落「需打出」
             *         口径。
             * @param[in] req  决策请求；只读响应来源、使用者与伤害量。
             * @param[in] pair 是否两张手牌当杀窗口。
             * @return 中文标题。
             */
            std::string response_title(
                const tkw::game::ai::DecisionRequest &req, bool pair);

            /**
             * @brief  亮牌窗口标题：按来源结算分别渲染五谷丰登/麒麟弓/火攻。
             * @param[in] req 决策请求；只读 `reveal_source`。
             * @return 中文标题。
             */
            std::string reveal_title(
                const tkw::game::ai::DecisionRequest &req);

            /**
             * @brief  无懈窗口提示标题：使用者/目标集合/锦囊名；判定窗口无使用者。
             * @param[in] req 决策请求；只读使用者、目标集合与锦囊名。
             * @return 中文标题。
             */
            std::string counter_title(
                const tkw::game::ai::DecisionRequest &req);

            /**
             * @brief  借刀杀人候选的受害者是否为决策者本人。
             * @param[in]  req    决策请求；只读目录与使用者。
             * @param[in]  act    候选合法动作。
             * @param[out] holder 命中时写入持武器者 id（`targets[0]`）。
             * @return 是则 true；非借刀/非双目标/受害者非本人则 false。
             */
            bool is_self_target_borrowed_sword(
                const tkw::game::ai::DecisionRequest &req,
                const tkw::game::LegalAction &act, std::string &holder);

            /**
             * @brief  目标列表 → 逗号分隔串。
             * @param[in] targets 目标 id 列表。
             * @return 逗号分隔串；空列表返回空串。
             */
            std::string join_targets(
                const std::vector<std::string> &targets);

            /**
             * @brief  花色 → UTF-8 符号。
             * @param[in] suit 花色。
             * @return 花色符号；未知值兜底问号（只读展示）。
             */
            const char *suit_glyph(tkw::card::Suit suit);

            /**
             * @brief  实体牌花色点数 → 展示串（如 ♠7）。
             * @param[in] c 实体牌。
             * @return 花色 + 点数字符串；供面板查看牌面。
             */
            std::string card_meta(const tkw::card::Card &c);

            /**
             * @brief  候选是否属于对手手牌、需向决策者遮挡内容。
             * @param[in] req   当前决策请求。
             * @param[in] index 候选的 0 基下标。
             * @return 仅 PickCard 的手牌候选返回 true；缺少分区标签时防御性返回
             *         true（平行数组缺口不得导致漏遮）。
             * @note   装备区/判定区为明置信息，其余决策类别的候选均为决策者自己
             *         可见的牌，一律返回 false。此谓词是候选遮挡的唯一判据。
             */
            bool is_hidden_pick_option(
                const tkw::game::ai::DecisionRequest &req, std::size_t index);

            /**
             * @brief  把实体牌折成三串展示纯值：牌名、花色点数与效果文案。
             * @param[out] name 展示名（目录缺失回落 `def_id`）。
             * @param[out] meta 花色点数展示串（如 ♠7）。
             * @param[out] text 效果文案；目录缺失或定义无文案时留空。
             * @param[in]  req  决策请求；目录仅在此函数内被只读。
             * @param[in]  c    实体牌；`def_id` 为空（隐藏占位槽）时三串原样不动。
             * @note   产物为字符串，不持目录指针；效果文案缺失留空，由渲染侧回落
             *         「（无说明）」。
             */
            void fill_card_strings(std::string &name, std::string &meta,
                                   std::string &text,
                                   const tkw::game::ai::DecisionRequest &req,
                                   const tkw::card::Card &c);

            /**
             * @brief  把实体牌折成候选的展示纯值：牌名、花色点数与效果文案。
             * @param[in,out] opt 就地写入 `card_name`/`card_meta`/`card_text`。
             * @param[in]     req 决策请求；目录缺失时只填牌名与花色点数。
             * @param[in]     c   实体牌；`def_id` 为空（隐藏占位槽）时直接返回，
             *                    不折出假牌面。
             * @note   只在 `make_panel` 内调用，产物为字符串，不持目录指针；效果
             *         文案缺失留空，由渲染侧回落「（无说明）」。
             */
            void fill_card_fields(PanelOption &opt,
                                  const tkw::game::ai::DecisionRequest &req,
                                  const tkw::card::Card &c);

            /**
             * @brief  折 pair 第二张牌的展示纯值：在本手牌内按 `instance_id` 查。
             * @param[in,out] opt         命中时写入 `second_card_*` 三项。
             * @param[in]     req         决策请求；只读 `req.view.hand`。
             * @param[in]     instance_id 第二张实体牌的 `instance_id`。
             * @note   只查决策者自己的手牌；未命中时三字段留空、渲染侧回落只显
             *         主牌，不因平行数组缺口折出假牌面。pair 恒来自决策者本手牌，
             *         不泄漏对手信息。
             */
            void fill_second_card_fields(
                PanelOption &opt, const tkw::game::ai::DecisionRequest &req,
                const std::string &instance_id);

            /**
             * @brief  Play 候选的一行文本：牌名 + 实例 + 可选转化/第二张 + 可选
             *         目标 + 重铸后缀 + 借刀警示。
             * @param[in] req 决策请求。
             * @param[in] act 候选合法动作。
             * @return 中文展示文本。
             */
            std::string play_option_text(
                const tkw::game::ai::DecisionRequest &req,
                const tkw::game::LegalAction &act);

            /**
             * @brief  PickCard 候选文本：手牌遮挡、装备/判定明置并带分区标签。
             * @param[in] req   决策请求。
             * @param[in] index 候选的 0 基下标。
             * @return 中文展示文本。
             */
            std::string pick_option_text(
                const tkw::game::ai::DecisionRequest &req, std::size_t index);
        }  // namespace detail

        /**
         * @brief  把决策请求折成纯值面板视图。
         * @param[in] req 引擎决策请求；`catalog` 仅在本函数内被读一次。
         * @return 面板值视图；kind/标题/候选/多选与 pass 语义均由 req 字段驱动。
         * @note   本函数是唯一触点目录的时机，产物不持任何指针。
         */
        DecisionPanelView make_panel(
            const tkw::game::ai::DecisionRequest &req);

        /**
         * @brief  面板选中项 → 引擎决策结果（纯映射 + 防御性校验）。
         * @param[in]  panel    当前待决面板（值视图）。
         * @param[in]  selected 选中候选的 0 基下标；Discard 为多选。
         * @param[in]  pass     是否放弃；仅 `panel.allow_pass` 为真时合法。
         * @param[out] out      合法时写入决策结果。
         * @return 合法返回 true 并写 `out`；越界/重复/数量不符/非法 pass 返回 false。
         * @note   非法只返回 false，不修改 `out` 以外的状态，由调用方保持待决。
         */
        bool make_choice(const DecisionPanelView &panel,
                         const std::vector<std::size_t> &selected, bool pass,
                         tkw::game::ai::DecisionChoice &out);

        /**
         * @class TuiDecisionSource
         * @brief 真人座位阻塞待决、其余座位回落注入 Decider 的决策源。
         * @note 路由按决策请求的 actor：命中 humans 则折面板并通过条件变量等待
         *       UI 提交；否则直接调用 fallback。取消只置位并通知，唤醒阻塞的
         *       decide 返回默认选择。
         */
        class TuiDecisionSource : public tkw::game::ai::Decider
        {
        public:
            /**
             * @brief  构造决策源。
             * @param[in] humans   真人座位 id 集合。
             * @param[in] fallback 非真人座位的回落决策器。
             * @pre    `fallback` 不得为空。
             */
            TuiDecisionSource(
                std::vector<std::string> humans,
                std::unique_ptr<tkw::game::ai::Decider> fallback) :
                m_humans(humans.begin(), humans.end()),
                m_fallback(std::move(fallback))
            {
            }

            /**
             * @brief  决策入口：真人座位阻塞等待 UI 提交，其余回落 fallback。
             * @param[in] req 引擎决策请求。
             * @return 真人座位：UI 提交的选择；取消时返回默认选择。
             * @note   面板在取得锁后、阻塞前折成纯值；释放锁先调用即将阻塞回调
             *         投递快照，再在锁内置 pending 可见，最后通知 UI 唤醒，避免
             *         回调持锁重入；快照先于 pending 可见，保证主线程观测到待决
             *         后写入的运行中拒绝提示不会被回调快照整段覆盖。
             */
            tkw::game::ai::DecisionChoice decide(
                const tkw::game::ai::DecisionRequest &req) override;

            /**
             * @brief  取走当前待决面板（每个待决仅返回真一次）。
             * @param[out] out 有待决且未被取走时写入面板值视图。
             * @return 取到返回 true；无待决/已取走/已取消返回 false。
             */
            bool fetch_new(DecisionPanelView &out);

            /** @brief 是否有未被取走的待决。
             * @return 存在待决且未被取走、未取消时为 true。 */
            bool has_pending() const;

            /**
             * @brief  提交当前待决的选择并唤醒 worker。
             * @param[in] selected 选中候选的 0 基下标；Discard 为多选。
             * @param[in] pass     是否放弃。
             * @return 提交被接受返回 true；无待决/已提交尚未唤醒/已取消/选择非法
             *         返回 false，且不改变待决状态（面板保持可继续提交）。
             */
            bool submit(std::vector<std::size_t> selected, bool pass);

            /**
             * @brief 取消待决并唤醒阻塞的 decide。
             * @note 置位与清 pending 同在锁内，避免丢唤醒；此后 fetch_new 恒假。
             */
            void cancel();

            /**
             * @brief  设置待决/唤醒回调；由控制器接 UI 主循环投递。
             * @param[in] notify 回调；在 decide 释放锁后调用。
             * @note   不得在其中回调本对象的阻塞 API。
             */
            void set_notify(std::function<void()> notify);

            /**
             * @brief  设置「即将阻塞」回调；真人决策释放锁后、阻塞等待前调用一次。
             * @param[in] on_wait 回调；在 `m_mutex` 释放后调用。
             * @note   可安全回送最新快照；不得在其中回调本对象的阻塞 API。
             */
            void set_on_wait(std::function<void()> on_wait);

        private:
            mutable std::mutex m_mutex;
            std::condition_variable m_cv;
            std::set<std::string> m_humans;
            std::unique_ptr<tkw::game::ai::Decider> m_fallback;
            std::function<void()> m_notify;
            std::function<void()> m_on_wait;
            tkw::Option<DecisionPanelView> m_panel =
                tkw::Option<DecisionPanelView>::None();
            bool m_has_pending = false;
            bool m_taken = false;
            bool m_submitted = false;
            tkw::game::ai::DecisionChoice m_choice;
            std::atomic<bool> m_cancelled{false};
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_DECISION_SOURCE_HPP
