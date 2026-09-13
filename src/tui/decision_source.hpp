/**
 * @file decision_source.hpp
 * @brief TUI 真人决策接缝：把引擎的决策请求折成纯值面板，经阻塞握手交给 UI 线程。
 * @note 线程契约：worker 线程在 Decider::decide 内构造纯值面板并等待，UI 主线程
 *       经 fetch_new/submit 交接；退出先 cancel 唤醒再 join。面板只含字符串与
 *       产出载荷，不持 catalog/Game 指针，故待决期渲染不依赖引擎生命周期。
 *       本文件无 FTXUI、无输出副作用，可脱离 TTY 单测。
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
#include <optional>
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

namespace tkw
{
    namespace tui
    {
        /**
         * @brief 面板候选项：展示文本 + 回填 DecisionChoice 所需的纯值载荷。
         * @note text 在构造面板时由目录一次性解析成展示名，之后不再触目录。
         */
        struct PanelOption
        {
            std::string text;               /**< 已解析展示名/标签/目标串 */
            std::string instance_id;        /**< Play/Response/Peach/Counter/Discard */
            std::string second_instance_id; /**< Response pair（两张当杀） */
            std::vector<std::string> targets; /**< Play 目标 */
            std::size_t option_index = 0;   /**< PickCard/PickRevealed 候选下标 */
            bool accepted = false;          /**< Trigger：true=发动 */
        };

        /**
         * @brief 待决决策的纯值视图：只含字符串与载荷，不持 catalog/Game 指针。
         * @note 在 worker 阻塞前由 DecisionRequest 折成，UI 渲染期只读本结构；
         *       即便引擎对象已析构也不会触碰悬垂指针。
         */
        struct DecisionPanelView
        {
            tkw::game::ai::DecisionKind kind = tkw::game::ai::DecisionKind::Play;
            std::string actor;   /**< 决策者 id */
            std::string title;   /**< 主标题（按 kind 定文案，不含 actor 前缀） */
            std::vector<PanelOption> options;
            bool multi = false;  /**< Discard：多选 */
            bool toggle = false; /**< Discard：空格切换 */
            int need_count = 0;  /**< Discard：需选数量 */
            bool allow_pass = false; /**< pass 是否合法 */
            bool yes_no = false;     /**< Trigger：y/n 两选项 */
        };

        namespace detail
        {
            /** 对手手牌候选项的遮挡占位文本。 */
            inline constexpr const char *kHiddenHandPlaceholder = "（未知手牌）";

            /** @brief PickCard 候选来源分区标签；其余决策类别返回空串。 */
            inline const char *zone_tag(tkw::card::Zone zone)
            {
                switch (zone)
                {
                case tkw::card::Zone::Hand:
                    return "[手]";
                case tkw::card::Zone::Equip:
                    return "[装]";
                case tkw::card::Zone::Judge:
                    return "[判]";
                default:
                    return "";
                }
            }

            /** @brief 装备能力一句话效果（只读展示，不打印卡牌 text）。 */
            inline const char *ability_hint(tkw::card::Ability ability)
            {
                switch (ability)
                {
                case tkw::card::Ability::NoShaLimit:
                    return "出杀不受次数限制";
                case tkw::card::Ability::IgnoreArmor:
                    return "无视目标的防具";
                case tkw::card::Ability::Cixiong:
                    return "杀唯一异性目标时可令其弃一张手牌或令你摸一张";
                case tkw::card::Ability::ExtraShaAfterJink:
                    return "杀被闪后可再出一张杀";
                case tkw::card::Ability::TwoCardsAsSha:
                    return "两张手牌可当一张杀";
                case tkw::card::Ability::DiscardTwoForceDamage:
                    return "弃两张牌令此杀依然造成伤害";
                case tkw::card::Ability::MultiTargetSha:
                    return "杀为最后一张手牌时可额外指定目标";
                case tkw::card::Ability::DiscardHorseOnDamage:
                    return "造成伤害后可弃置目标一匹坐骑";
                case tkw::card::Ability::DamageAsDiscard:
                    return "可弃置目标两张牌以防止此伤害";
                case tkw::card::Ability::JudgementJink:
                    return "需出闪时可判定，红色结果视为闪";
                case tkw::card::Ability::BlackShaImmune:
                    return "黑色杀对你无效";
                }
                return "装备能力";
            }

            /**
             * @brief 从装备区反查携带该能力的装备名；查不到回落「装备能力」。
             * @note 只读 req.view.equip 与目录，构造面板时调用一次。
             */
            inline std::string ability_name(const tkw::game::ai::DecisionRequest &req)
            {
                if (req.catalog)
                {
                    for (const auto &c : req.view.equip)
                    {
                        const auto def = req.catalog->find(c.def_id);
                        if (def.is_none())
                            continue;
                        for (const auto ability : def.unwrap()->abilities)
                            if (ability == req.ability)
                                return tkw::card::display_name(*def.unwrap());
                    }
                }
                return "装备能力";
            }

            /**
             * @brief 响应窗口标题：有来源牌时给来源与伤害后果，否则回落「需打出」口径。
             * @param pair 是否两张手牌当杀窗口。
             */
            inline std::string response_title(
                const tkw::game::ai::DecisionRequest &req, bool pair)
            {
                const bool jink =
                    req.response_kind == tkw::card::ResponseKind::Jink;
                const std::string need =
                    pair ? "需打出两张手牌当杀"
                         : (jink ? "需打出闪" : "需打出杀");
                if (req.response_source.empty())
                    return pair ? "响应（杀）：打出两张手牌当杀"
                                : "响应（" + need + "）";

                std::string title =
                    "响应（来源: " + req.response_user + " 的 " +
                    tkw::card::display_name(req.catalog, req.response_source) +
                    "，" + need;
                if (req.response_damage > 0)
                    title += "；不出将受到 " +
                             std::to_string(req.response_damage) + " 点伤害";
                title += "）";
                return title;
            }

            /** @brief 亮牌窗口标题：按来源结算分别渲染五谷丰登/麒麟弓。 */
            inline std::string reveal_title(
                const tkw::game::ai::DecisionRequest &req)
            {
                if (req.reveal_source == tkw::game::RevealSource::Qilin)
                    return "麒麟弓：选择目标坐骑";
                return "五谷丰登亮牌（每名角色依次选一张）";
            }

            /**
             * @brief 无懈窗口提示标题：使用者/目标集合/锦囊名；判定窗口无使用者。
             */
            inline std::string counter_title(
                const tkw::game::ai::DecisionRequest &req)
            {
                std::string title = "无懈可击窗口";
                if (req.counter_user.empty())
                {
                    title += "（延时锦囊判定";
                    if (!req.counter_trick.empty())
                        title += "：" + tkw::card::display_name(
                                             req.catalog, req.counter_trick);
                    title += "）";
                }
                else
                {
                    title += "（使用者: " + req.counter_user;
                    if (!req.counter_trick.empty())
                        title += " 的 " + tkw::card::display_name(
                                             req.catalog, req.counter_trick);
                    title += "）";
                }
                for (std::size_t i = 0; i < req.counter_targets.size(); ++i)
                {
                    if (i > 0)
                        title += ",";
                    else
                        title += "目标: ";
                    title += req.counter_targets[i];
                }
                return title;
            }

            /** @brief 弃牌原因 → 中文文案（与真人窗口同口径）。 */
            inline const char *reason_text(tkw::game::DiscardReason reason)
            {
                switch (reason)
                {
                case tkw::game::DiscardReason::TurnLimit:
                    return "手牌超上限";
                case tkw::game::DiscardReason::AbilityCost:
                    return "装备能力代价";
                case tkw::game::DiscardReason::CixiongChoice:
                    return "雌雄双股剑（可放弃）";
                }
                return "弃牌";
            }

            /**
             * @brief 借刀杀人候选的受害者是否为决策者本人。
             * @param holder 出参：命中时写入持武器者 id（targets[0]）。
             * @return 是则 true；非借刀/非双目标/受害者非本人则 false。
             */
            inline bool is_self_target_borrowed_sword(
                const tkw::game::ai::DecisionRequest &req,
                const tkw::game::LegalAction &act, std::string &holder)
            {
                if (!req.catalog || act.targets.size() != 2 ||
                    act.targets[1] != req.actor)
                    return false;

                const auto def = req.catalog->find(act.card.def_id);
                if (def.is_none() || def.unwrap()->effect.is_none())
                    return false;
                if (def.unwrap()->effect.unwrap().kind !=
                    tkw::card::CardEffectKind::BorrowedSword)
                    return false;

                holder = act.targets[0];
                return true;
            }

            /** @brief 目标列表 → 逗号分隔串（空列表返回空串）。 */
            inline std::string join_targets(const std::vector<std::string> &targets)
            {
                std::string out;
                for (std::size_t i = 0; i < targets.size(); ++i)
                {
                    if (i > 0)
                        out += ",";
                    out += targets[i];
                }
                return out;
            }

            /**
             * @brief Play 候选的一行文本：牌名 + 实例 + 可选第二张 + 可选目标 + 借刀警示。
             */
            inline std::string play_option_text(
                const tkw::game::ai::DecisionRequest &req,
                const tkw::game::LegalAction &act)
            {
                std::string text =
                    tkw::card::display_name(req.catalog, act.card.def_id) + " " +
                    act.card.instance_id;
                if (!act.second_instance_id.empty())
                    text += " + " + act.second_instance_id;
                if (!act.targets.empty())
                    text += " -> " + join_targets(act.targets);
                std::string holder;
                if (is_self_target_borrowed_sword(req, act, holder))
                    text += "（警告：" + holder +
                            " 将对你出杀，可能致你受伤或阵亡）";
                return text;
            }

            /** @brief PickCard 候选文本：手牌遮挡、装备/判定明置并带分区标签。 */
            inline std::string pick_option_text(
                const tkw::game::ai::DecisionRequest &req, std::size_t index)
            {
                const tkw::card::Zone zone =
                    index < req.zone_labels.size() ? req.zone_labels[index]
                                                   : tkw::card::Zone::Hand;
                std::string text = zone_tag(zone);
                if (!text.empty())
                    text += " ";
                const bool hidden =
                    index >= req.zone_labels.size() ||
                    zone == tkw::card::Zone::Hand;
                if (hidden)
                {
                    text += kHiddenHandPlaceholder;
                    return text;
                }
                text += tkw::card::display_name(req.catalog, req.options[index].def_id) +
                        " " + req.options[index].instance_id;
                return text;
            }
        }  // namespace detail

        /**
         * @brief 把决策请求折成纯值面板视图。
         * @param req 引擎决策请求；catalog 仅在本函数内被读一次。
         * @return 面板值视图；kind/标题/候选/多选与 pass 语义均由 req 字段驱动。
         * @note 本函数是唯一触点目录的时机，产物不持任何指针。
         */
        inline DecisionPanelView make_panel(
            const tkw::game::ai::DecisionRequest &req)
        {
            using tkw::game::ai::DecisionKind;

            DecisionPanelView panel;
            panel.kind = req.kind;
            panel.actor = req.actor;

            switch (req.kind)
            {
            case DecisionKind::Play:
            {
                panel.title = "出牌阶段";
                panel.allow_pass = true;
                for (const auto &act : req.legal)
                {
                    PanelOption opt;
                    opt.text = detail::play_option_text(req, act);
                    opt.instance_id = act.card.instance_id;
                    opt.second_instance_id = act.second_instance_id;
                    opt.targets = act.targets;
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Response:
            {
                const bool pair = !req.legal.empty();
                panel.title = detail::response_title(req, pair);
                panel.allow_pass = true;
                if (pair)
                {
                    for (const auto &act : req.legal)
                    {
                        PanelOption opt;
                        opt.text =
                            tkw::card::display_name(req.catalog, act.card.def_id) +
                            " " + act.card.instance_id + " + " +
                            act.second_instance_id;
                        opt.instance_id = act.card.instance_id;
                        opt.second_instance_id = act.second_instance_id;
                        panel.options.push_back(std::move(opt));
                    }
                }
                else
                {
                    for (const auto &c : req.options)
                    {
                        PanelOption opt;
                        opt.text =
                            tkw::card::display_name(req.catalog, c.def_id) + " " +
                            c.instance_id;
                        opt.instance_id = c.instance_id;
                        panel.options.push_back(std::move(opt));
                    }
                }
                break;
            }
            case DecisionKind::Peach:
            {
                panel.title = "濒死救场（濒死者: " + req.dying + "）";
                panel.allow_pass = true;
                for (const auto &c : req.options)
                {
                    PanelOption opt;
                    opt.text = tkw::card::display_name(req.catalog, c.def_id) +
                               " " + c.instance_id;
                    opt.instance_id = c.instance_id;
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Counter:
            {
                panel.title = detail::counter_title(req);
                panel.allow_pass = true;
                for (const auto &c : req.options)
                {
                    PanelOption opt;
                    opt.text = tkw::card::display_name(req.catalog, c.def_id) +
                               " " + c.instance_id;
                    opt.instance_id = c.instance_id;
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Trigger:
            {
                panel.title = "发动 " + detail::ability_name(req) + "（" +
                              detail::ability_hint(req.ability) + "）？";
                panel.allow_pass = true;
                panel.yes_no = true;
                PanelOption yes;
                yes.text = "发动";
                yes.accepted = true;
                panel.options.push_back(std::move(yes));
                PanelOption no;
                no.text = "不发动";
                no.accepted = false;
                panel.options.push_back(std::move(no));
                break;
            }
            case DecisionKind::PickCard:
            {
                panel.title = "选择目标区域的牌（目标: " + req.target + "）";
                panel.allow_pass = true;
                for (std::size_t i = 0; i < req.options.size(); ++i)
                {
                    PanelOption opt;
                    opt.text = detail::pick_option_text(req, i);
                    opt.instance_id = req.options[i].instance_id;
                    opt.option_index = i;
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::PickRevealed:
            {
                panel.title = detail::reveal_title(req);
                panel.allow_pass = false;
                for (std::size_t i = 0; i < req.options.size(); ++i)
                {
                    PanelOption opt;
                    opt.text =
                        tkw::card::display_name(req.catalog, req.options[i].def_id) +
                        " " + req.options[i].instance_id;
                    opt.instance_id = req.options[i].instance_id;
                    opt.option_index = i;
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            case DecisionKind::Discard:
            {
                panel.title = "弃牌（" +
                              std::string(detail::reason_text(req.discard_reason)) +
                              "，需弃 " + std::to_string(req.count) + " 张）";
                panel.multi = true;
                panel.toggle = true;
                panel.need_count = req.count;
                panel.allow_pass =
                    req.discard_reason == tkw::game::DiscardReason::CixiongChoice;
                for (const auto &c : req.options)
                {
                    PanelOption opt;
                    opt.text = tkw::card::display_name(req.catalog, c.def_id) +
                               " " + c.instance_id;
                    opt.instance_id = c.instance_id;
                    panel.options.push_back(std::move(opt));
                }
                break;
            }
            }
            return panel;
        }

        /**
         * @brief 面板选中项 → 引擎决策结果（纯映射 + 防御性校验）。
         * @param panel    当前待决面板（值视图）。
         * @param selected 选中候选的 0 基下标；Discard 为多选。
         * @param pass     是否放弃；仅 panel.allow_pass 为真时合法。
         * @param out      出参：合法时写入决策结果。
         * @return 合法返回 true 并写 out；越界/重复/数量不符/非法 pass 返回 false。
         * @note 非法只返回 false，不修改 out 以外的状态，由调用方保持待决。
         */
        inline bool make_choice(const DecisionPanelView &panel,
                                 const std::vector<std::size_t> &selected,
                                 bool pass, tkw::game::ai::DecisionChoice &out)
        {
            using tkw::game::ai::DecisionKind;

            out = tkw::game::ai::DecisionChoice{};

            if (panel.kind == DecisionKind::Trigger)
            {
                if (pass)
                {
                    out.accepted = false;
                    return true;
                }
                if (selected.size() != 1 || selected[0] >= 2)
                    return false;
                out.accepted = selected[0] == 0;
                return true;
            }

            if (pass)
            {
                if (!panel.allow_pass)
                    return false;
                return true;  // 空选择：结束出牌/不响应/不救/不出无懈/放弃选牌
            }

            if (panel.kind == DecisionKind::Discard)
            {
                if (selected.size() !=
                    static_cast<std::size_t>(panel.need_count))
                    return false;
                std::vector<std::size_t> seen;
                for (const std::size_t i : selected)
                {
                    if (i >= panel.options.size())
                        return false;
                    if (std::find(seen.begin(), seen.end(), i) != seen.end())
                        return false;
                    seen.push_back(i);
                }
                for (const std::size_t i : selected)
                    out.discards.push_back(panel.options[i].instance_id);
                return true;
            }

            if (selected.size() != 1)
                return false;
            const std::size_t index = selected[0];
            if (index >= panel.options.size())
                return false;
            const PanelOption &opt = panel.options[index];

            switch (panel.kind)
            {
            case DecisionKind::Play:
                out.instance_id = tkw::Option<std::string>::Some(opt.instance_id);
                out.second_instance_id = opt.second_instance_id;
                out.targets = opt.targets;
                return true;
            case DecisionKind::Response:
                out.instance_id = tkw::Option<std::string>::Some(opt.instance_id);
                out.second_instance_id = opt.second_instance_id;
                return true;
            case DecisionKind::Peach:
            case DecisionKind::Counter:
                out.instance_id = tkw::Option<std::string>::Some(opt.instance_id);
                return true;
            case DecisionKind::PickCard:
            case DecisionKind::PickRevealed:
                out.option_index = tkw::Option<std::size_t>::Some(index);
                return true;
            default:
                return false;
            }
        }

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
             * @brief 构造决策源。
             * @param humans   真人座位 id 集合。
             * @param fallback 非真人座位的回落决策器；不得为空。
             */
            TuiDecisionSource(
                std::vector<std::string> humans,
                std::unique_ptr<tkw::game::ai::Decider> fallback) :
                humans_(humans.begin(), humans.end()),
                fallback_(std::move(fallback))
            {
            }

            /**
             * @brief 决策入口：真人座位阻塞等待 UI 提交，其余回落 fallback。
             * @return 真人座位：UI 提交的选择；取消时返回默认选择。
             * @note 面板在取得锁后、阻塞前折成纯值，随后释放锁再通知 UI，
             *       避免 UI 回调持锁重入。
             */
            tkw::game::ai::DecisionChoice decide(
                const tkw::game::ai::DecisionRequest &req) override
            {
                if (humans_.count(req.actor) == 0)
                    return fallback_->decide(req);

                std::unique_lock<std::mutex> lock(m_);
                if (cancelled_.load())
                    return {};
                panel_ = make_panel(req);
                has_pending_ = true;
                taken_ = false;
                submitted_ = false;
                std::function<void()> notify = notify_;
                lock.unlock();
                if (notify)
                    notify();
                lock.lock();
                cv_.wait(lock, [this]
                         { return submitted_ || cancelled_.load(); });
                if (cancelled_.load())
                {
                    has_pending_ = false;
                    return {};
                }
                has_pending_ = false;
                taken_ = false;
                return choice_;
            }

            /**
             * @brief 取走当前待决面板（每个待决仅返回真一次）。
             * @param out 出参：有待决且未被取走时写入面板值视图。
             * @return 取到返回 true；无待决/已取走/已取消返回 false。
             */
            bool fetch_new(DecisionPanelView &out)
            {
                std::lock_guard<std::mutex> lock(m_);
                if (!has_pending_ || taken_ || cancelled_.load())
                    return false;
                out = *panel_;
                taken_ = true;
                return true;
            }

            /** @brief 是否有未被取走的待决。 */
            bool has_pending() const
            {
                std::lock_guard<std::mutex> lock(m_);
                return has_pending_ && !taken_ && !cancelled_.load();
            }

            /**
             * @brief 提交当前待决的选择并唤醒 worker。
             * @param selected 选中候选的 0 基下标；Discard 为多选。
             * @param pass     是否放弃。
             * @return 提交被接受返回 true；无待决/已取消/选择非法返回 false，
             *         且不改变待决状态（面板保持可继续提交）。
             */
            bool submit(std::vector<std::size_t> selected, bool pass)
            {
                std::lock_guard<std::mutex> lock(m_);
                if (!has_pending_ || cancelled_.load())
                    return false;
                tkw::game::ai::DecisionChoice choice;
                if (!panel_ || !make_choice(*panel_, selected, pass, choice))
                    return false;
                choice_ = std::move(choice);
                submitted_ = true;
                cv_.notify_all();
                return true;
            }

            /**
             * @brief 取消待决并唤醒阻塞的 decide。
             * @note 置位与清 pending 同在锁内，避免丢唤醒；此后 fetch_new 恒假。
             */
            void cancel()
            {
                {
                    std::lock_guard<std::mutex> lock(m_);
                    cancelled_.store(true);
                    has_pending_ = false;
                    panel_.reset();
                }
                cv_.notify_all();
            }

            /**
             * @brief 设置待决/唤醒回调；由控制器接 UI 主循环投递。
             * @note 回调在 decide 释放锁后调用，不得在其中回调本对象的阻塞 API。
             */
            void set_notify(std::function<void()> notify)
            {
                std::lock_guard<std::mutex> lock(m_);
                notify_ = std::move(notify);
            }

        private:
            mutable std::mutex m_;
            std::condition_variable cv_;
            std::set<std::string> humans_;
            std::unique_ptr<tkw::game::ai::Decider> fallback_;
            std::function<void()> notify_;
            std::optional<DecisionPanelView> panel_;
            bool has_pending_ = false;
            bool taken_ = false;
            bool submitted_ = false;
            tkw::game::ai::DecisionChoice choice_;
            std::atomic<bool> cancelled_{false};
        };
    }  // namespace tui
}  // namespace tkw

#endif  // INCLUDE_TKW_TUI_DECISION_SOURCE_HPP
