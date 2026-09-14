/**
 * @file weapon.hpp
 * @brief 「杀」结算管线：指定目标后 → 防具 → 响应 → 被闪后 → 命中前 → 伤害 → 命中后。
 * @details 7 件装备的效果不再写成 if 链，而是注册到 sha_hook_table()：
 *          每项声明「能力 / 插桩点 / 挂在哪一方 / 回调」，新增武器只加一行。
 *          规则约定（简单版）：
 *          - 雌雄双股剑：杀指定唯一目标且目标为异性时可发动，目标二选一：
 *            弃置一张手牌，或令使用者摸一张牌；
 *          - 仁王盾：黑杀无效（青釭剑无视防具可穿透）；
 *          - 八卦阵：需出闪时可判定，判定描述来自装备数据（当前为红色=闪）；
 *          - 青龙偃月刀：被闪后可再对同一目标使用一张杀；
 *          - 贯石斧：被闪后可弃两张牌令杀依然命中；
 *          - 寒冰剑：命中前可防止伤害改为弃置目标两张牌（仅手牌/装备区；对手手牌
 *            不可见，由引擎随机暗抽）；
 *          - 麒麟弓：造成伤害后可弃置目标一匹坐骑（由使用者选哪一匹）；
 *          - 古锭刀：杀造成伤害时目标无手牌则伤害 +1（锁定技，无决策窗）；
 *          - 方天画戟：杀为最后一张手牌时可额外指定至多两名目标
 *            （作用于目标集合，经目标数校验放宽实现，不走本表钩子）；
 *          - 丈八蛇矛：两张手牌当一张「杀」（虚拟杀无花色，仁王盾黑杀
 *            判定不适用；消费与结算入口在 resolver 层，不走本表钩子）。
 * @note 响应牌消费归本模块与 resolver 层（单一写者）；钩子内的决策询问经
 *       DecisionSource，钩子只改 `ShaContext`，由主流程统一落子。
 * @ingroup tkw_game_resolve
 */

#ifndef INCLUDE_TKW_GAME_WEAPON_HPP
#define INCLUDE_TKW_GAME_WEAPON_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "card/catalog.hpp"
#include "card/def.hpp"
#include "card/manager.hpp"
#include "game/core/context.hpp"
#include "game/core/decision.hpp"
#include "game/core/state.hpp"
#include "game/query/equip.hpp"
#include "game/resolve/combat.hpp"
#include "game/resolve/response.hpp"
#include "util/types.hpp"

namespace tkw
{
    namespace game
    {
        /** @brief 「杀」结算上下文：钩子读写的共享状态。 */
        struct ShaContext
        {
            GameContext &ctx;   /**< 对局上下文；钩子经此读写卡牌/实体/事件。 */
            DecisionSource &ai; /**< 决策源；钩子经此询问可选触发。 */
            const card::Card &sha; /**< 该杀卡牌对象（花色供黑杀判定）。 */
            std::string attacker;  /**< 攻击方实体 id。 */
            std::string target;    /**< 目标实体 id。 */
            int amount = 1;        /**< 伤害量基数（钩子经 `damage_bonus` 追加）。 */
            card::DamageType damage_type =
                card::DamageType::Normal; /**< 伤害属性（火杀/雷杀透传） */
            int damage_bonus = 0; /**< 命中伤害修正：目标侧火焰脆弱等累加 */
            bool ignore_armor = false; /**< 攻击方无视防具（青釭剑） */
            bool responded = false;    /**< 目标已打出/视为闪 */
            bool blocked = false;      /**< 防具直接无效（仁王盾） */
            bool prevented = false;    /**< 伤害被替代（寒冰剑） */
            int target_count = 1;      /**< 该杀指定的目标数（雌雄仅唯一目标） */
            bool virtual_sha = false; /**< 虚拟杀（丈八两张当杀）：无花色，黑杀判定不适用 */
        };

        inline void resolve_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &attacker,
            const card::Card &sha, const std::string &target, int amount,
            int target_count = 1, bool virtual_sha = false,
            card::DamageType damage_type = card::DamageType::Normal,
            int damage_bonus = 0);

        // ── 装备效果（钩子实现）────────────────────────────────────────

        /**
         * @brief 攻击者手牌中的第一张杀（青龙偃月刀续杀用）。
         * @param[in] ctx    只读上下文。
         * @param[in] player 查询的实体 id。
         * @return 手牌序首张满足杀响应定义的牌；`None` = 无杀。
         * @retval Some 该手牌副本。
         * @retval None 无真杀手牌。
         */
        inline Option<card::Card> find_sha_in_hand(
            const GameContext &ctx, const std::string &player)
        {
            return find_hand_card_matching(
                ctx, player, [](const card::CardDef &def)
                { return is_response_def(def, card::ResponseKind::Sha); });
        }

        /**
         * @brief 弃置目标 count 张牌（寒冰剑）。
         * @param[in] ctx      对局上下文。
         * @param[in] ai       决策源；由攻击方逐张选择目标区域内的牌。
         * @param[in] attacker 选择方实体 id（寒冰剑使用者）。
         * @param[in] target   被弃牌实体 id。
         * @param[in] count    请求弃置的张数。
         * @return 实际弃成功的张数（决策源返回幽灵 id 或选不满时少于请求数）。
         * @post 弃置的牌已进入弃牌堆并发弃置事件；装备区失去时联动
         *       `apply_equip_lost`。
         * @note 候选仅目标手牌与装备区：判定区延时锦囊不可被寒冰剑取走。
         */
        inline int discard_target_cards(
            GameContext &ctx, DecisionSource &ai,
            const std::string &attacker, const std::string &target, int count)
        {
            int discarded = 0;
            for (int i = 0; i < count; ++i)
            {
                const auto picked = ai.pick_card_from_target(
                    ctx, attacker, target, PickCardScope::HandEquip);
                if (picked.is_none())
                    break;
                const auto chosen = resolve_target_pick(ctx, target, picked.unwrap());
                if (chosen.is_none())
                    break;
                if (remove_any_and_discard(ctx, target, chosen.unwrap().instance_id)
                        .is_some())
                    ++discarded;
            }
            return discarded;
        }

        /**
         * @brief 卡牌定义是否为坐骑（麒麟弓预检与弃置共用）。
         * @param[in] def 卡牌定义。
         * @return 为进攻/防御坐骑时为 true；否则 false。
         * @retval true  装备槽为 `OffensiveHorse` 或 `DefensiveHorse`。
         * @retval false 非装备或无装备定义。
         */
        inline bool is_horse_def(const card::CardDef &def)
        {
            if (def.equip.is_none())
                return false;
            const auto slot = def.equip.unwrap().slot;
            return slot == card::EquipSlot::OffensiveHorse ||
                   slot == card::EquipSlot::DefensiveHorse;
        }

        /**
         * @brief 目标装备区是否有坐骑（麒麟弓无马时不询问发动）。
         * @param[in] ctx    只读上下文。
         * @param[in] target 查询的实体 id。
         * @return 至少一件坐骑时为 true；否则 false。
         */
        inline bool target_has_horse(const GameContext &ctx, const std::string &target)
        {
            for (const auto &c : ctx.cards->equip(target))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_horse_def(*def.unwrap()))
                    return true;
            }
            return false;
        }

        /**
         * @brief 目标装备区的全部坐骑（装备区顺序，供麒麟弓选弃与预检共用）。
         * @param[in] ctx    只读上下文。
         * @param[in] target 查询的实体 id。
         * @return 坐骑牌副本列表（装备区顺序）；无则为空。
         */
        inline std::vector<card::Card> target_horses(
            const GameContext &ctx, const std::string &target)
        {
            std::vector<card::Card> out;
            for (const auto &c : ctx.cards->equip(target))
            {
                const auto def = ctx.catalog->find(c.def_id);
                if (def.is_some() && is_horse_def(*def.unwrap()))
                    out.push_back(c);
            }
            return out;
        }

        /**
         * @brief 雌雄双股剑：唯一异性目标时由目标二选一。
         * @details 杀指定唯一目标且目标为异性时，使用者可发动，由目标二选一：
         *          弃置一张手牌，或令使用者摸一张牌。
         * @param[in,out] sc 杀结算上下文；读目标/攻击方，落子弃牌或摸牌。
         * @note 可发动（卡面「你可以」）：使用者拒绝则无效果；目标无手牌时
         *       只能令使用者摸牌。目标的选择走 choose_discards：返回空（或
         *       引用无效）即视为选择令使用者摸一张，非空且牌存在则弃置该牌；
         *       目标实际弃哪张牌由决策源决定（人可 pass）。
         */
        inline void hook_cixiong(ShaContext &sc)
        {
            // 仅唯一目标触发（方天多目标杀不触发）
            if (sc.target_count != 1)
                return;
            const auto attacker = sc.ctx.entities->find(sc.attacker);
            const auto target = sc.ctx.entities->find(sc.target);
            if (attacker.is_none() || target.is_none())
                return;
            // 同性不触发
            if (attacker.unwrap()->get_gender() == target.unwrap()->get_gender())
                return;

            // 使用者可选：拒绝则不弃不摸
            if (!sc.ai.trigger_effect(sc.ctx, sc.attacker, card::Ability::Cixiong))
                return;

            // 目标有手牌：弃一张，或（空/无效选择）令使用者摸一张
            if (sc.ctx.cards->hand_size(sc.target) > 0)
            {
                const auto discards = sc.ai.choose_discards(
                    sc.ctx, sc.target, 1, DiscardReason::CixiongChoice);
                for (const auto &id : discards)
                {
                    if (remove_and_discard(sc.ctx, sc.target, id).is_some())
                        return;
                    break;
                }
            }

            // 无手牌或目标选择放弃弃牌：使用者摸一张牌
            apply_draw(sc.ctx, sc.attacker, 1);
        }

        /**
         * @brief 仁王盾：黑色的杀对你无效（青釭剑可穿透，虚拟杀不适用）。
         * @param[in,out] sc 杀结算上下文；命中黑色真杀时置 `blocked`。
         * @note 虚拟杀无花色，黑杀判定短路；青釭剑无视防具时穿透。
         */
        inline void hook_renwang(ShaContext &sc)
        {
            if (!sc.ignore_armor && !sc.virtual_sha && is_black_suit(sc.sha.suit))
                sc.blocked = true;
        }

        /**
         * @brief 藤甲：普通杀（含丈八虚拟杀）对你无效；火焰伤害 +1。
         * @param[in,out] sc 杀结算上下文；普通伤害置 `blocked`，火焰累加
         *                   `damage_bonus`。
         * @note 青釭剑无视防具：无效与火焰脆弱一并穿透；雷电与决斗不受影响。
         *       锁定技，无决策窗口。
         */
        inline void hook_tengjia(ShaContext &sc)
        {
            // 青釭剑穿透：藤甲不生效，普通杀照常命中且火焰不加伤
            if (sc.ignore_armor)
                return;

            // 普通杀无效（含丈八两张当杀的虚拟杀）
            if (sc.damage_type == card::DamageType::Normal)
            {
                sc.blocked = true;
                return;
            }

            // 火焰伤害 +1
            if (sc.damage_type == card::DamageType::Fire)
                sc.damage_bonus += 1;
        }

        /**
         * @brief 需打出闪时的八卦阵判定：目标可选发动，红色判定视为打出闪。
         * @param[in] ctx    对局上下文。
         * @param[in] ai     决策源；询问是否发动八卦阵。
         * @param[in] target 需出闪的实体 id。
         * @return 判定结果是否视为打出闪。
         * @retval true  已判定且结果按装备数据映射为闪。
         * @retval false 未发动/无防具/判定失败/非红。
         * @post 判定牌已消费并发布判定语义的弃置事件。
         * @note 只消费判定牌并发判定弃置事件；是否实际出闪由调用方决定。
         */
        inline bool trigger_bagua_jink(
            GameContext &ctx, DecisionSource &ai, const std::string &target)
        {
            if (!ai.trigger_effect(ctx, target, card::Ability::JudgementJink))
                return false;
            const card::CardDef *armor =
                find_equipment(ctx, target, card::Ability::JudgementJink);
            if (!armor || armor->judge.is_none())
                return false;
            auto judge = perform_judgement(ctx);
            if (judge.is_none())
                return false;
            const card::Card judge_card = std::move(judge).unwrap();
            discard_and_emit(ctx, target, judge_card, DiscardKind::Judgement);
            return judge_result(armor->judge.unwrap(), judge_card) ==
                   card::JudgeAction::Jink;
        }

        /**
         * @brief 八卦阵：需出闪时可判定，判定描述来自装备数据。
         * @param[in,out] sc 杀结算上下文；判定视为闪时置 `responded`。
         * @note 目标可选择发动（卡面「可进行判定」）：拒绝则跳过判定，
         *       由后续响应窗口决定是否出闪。青釭剑无视防具时不发动。
         */
        inline void hook_bagua(ShaContext &sc)
        {
            if (sc.ignore_armor || sc.responded)
                return;
            if (trigger_bagua_jink(sc.ctx, sc.ai, sc.target))
                sc.responded = true;
        }

        /**
         * @brief 青龙偃月刀：目标打出闪后可再对同一目标使用一张杀。
         * @param[in,out] sc 杀结算上下文；递归发起一次续杀结算。
         * @note 续杀须攻击方手牌有真杀且可选发动；续杀消费该杀并发布打出事件，
         *       递归结算同目标。
         */
        inline void hook_qinglong(ShaContext &sc)
        {
            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::ExtraShaAfterJink))
                return;
            auto extra = find_sha_in_hand(sc.ctx, sc.attacker);
            if (extra.is_none())
                return;
            auto removed =
                sc.ctx.cards->remove_from_hand(sc.attacker, extra.unwrap().instance_id);
            if (removed.is_some())
            {
                card::Card extra_card = std::move(removed).unwrap();
                sc.ctx.cards->discard(extra_card);
                // 打出的牌只发打出事件；进弃牌堆是打出的必然后果，不另发弃置事件
                emit_card_played(sc.ctx, sc.attacker, extra_card);
            }
            resolve_sha(
                sc.ctx, sc.ai, sc.attacker, extra.unwrap(), sc.target, sc.amount, 1,
                false, sc.damage_type);
        }

        /**
         * @brief 贯石斧：目标打出闪后可弃两张牌令杀依然命中。
         * @param[in,out] sc 杀结算上下文；弃满两张时清 `responded` 强制命中。
         * @note 弃满两张才能发动：攻击方手牌不足 2 张不发动（不询问、不弃牌、
         *       不强制命中）；实际弃不满 2 张（含幽灵引用）不强制命中。
         */
        inline void hook_guanshi(ShaContext &sc)
        {
            // 发动前置：手牌不足两张付不起代价，直接不发动
            if (sc.ctx.cards->hand_size(sc.attacker) <
                static_cast<std::size_t>(rules_of(sc.ctx).two_card_cost))
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DiscardTwoForceDamage))
                return;

            const auto discards = sc.ai.choose_discards(
                sc.ctx, sc.attacker, rules_of(sc.ctx).two_card_cost,
                DiscardReason::AbilityCost);

            // 只计数实际弃成功的牌，封顶 two_card_cost 张
            int discarded = 0;
            for (const auto &id : discards)
            {
                if (discarded == rules_of(sc.ctx).two_card_cost)
                    break;
                if (remove_and_discard(sc.ctx, sc.attacker, id).is_some())
                    ++discarded;
            }

            // 弃满两张才强制命中，否则杀仍视为被闪
            if (discarded == rules_of(sc.ctx).two_card_cost)
                sc.responded = false;
        }

        /**
         * @brief 寒冰剑：防止伤害改为弃置目标两张牌。
         * @param[in,out] sc 杀结算上下文；目标弃满两张时置 `prevented`。
         * @note 目标弃满两张才免伤：目标手牌+装备不足 2 张不发动（不询问、
         *       不弃牌、不免伤；判定区不计入代价，延时锦囊不可取）；实际弃不满
         *       2 张（含幽灵引用）不免伤。
         */
        inline void hook_hanbing(ShaContext &sc)
        {
            // 发动前置：可选区（手牌+装备）不足两张付不起代价，直接不发动
            if (sc.ctx.cards->hand_size(sc.target) +
                    sc.ctx.cards->equip_size(sc.target) <
                static_cast<std::size_t>(rules_of(sc.ctx).two_card_cost))
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DamageAsDiscard))
                return;

            const int discarded = discard_target_cards(
                sc.ctx, sc.ai, sc.attacker, sc.target,
                rules_of(sc.ctx).two_card_cost);

            // 弃满两张才免伤，否则伤害照常落地
            if (discarded == rules_of(sc.ctx).two_card_cost)
                sc.prevented = true;
        }

        /**
         * @brief 古锭刀：目标没有手牌时，此杀伤害 +1。
         * @param[in,out] sc 杀结算上下文；目标无手牌时累加 `damage_bonus`。
         * @note 锁定技，无决策窗口；方天多目标杀逐目标判定（各看自身手牌数）。
         */
        inline void hook_guding(ShaContext &sc)
        {
            if (sc.ctx.cards->hand_size(sc.target) == 0)
                sc.damage_bonus += 1;
        }

        /**
         * @brief 麒麟弓：造成伤害后，目标装备区有坐骑时询问攻击方是否弃置其一。
         * @param[in,out] sc 杀结算上下文；命中后弃置目标一匹坐骑。
         * @note 无坐骑不询问（避免空操作）；由使用者选弃哪一匹，决策源返回
         *       None 或引用不在候选中时回落首个（已发动则必弃一张）。
         */
        inline void hook_qilin(ShaContext &sc)
        {
            if (!target_has_horse(sc.ctx, sc.target))
                return;

            if (!sc.ai.trigger_effect(
                    sc.ctx, sc.attacker, card::Ability::DiscardHorseOnDamage))
                return;

            // 候选为目标的全部坐骑（装备区顺序），攻击方从中选一张
            const auto horses = target_horses(sc.ctx, sc.target);
            if (horses.empty())
                return;
            std::string chosen = horses.front().instance_id;
            const auto picked = sc.ai.pick_from_revealed(
                sc.ctx, sc.attacker, horses, RevealSource::Qilin);
            if (picked.is_some())
                for (const auto &h : horses)
                    if (h.instance_id == picked.unwrap().instance_id)
                    {
                        chosen = h.instance_id;
                        break;
                    }

            auto removed = sc.ctx.cards->remove_from_equip(sc.target, chosen);
            if (removed.is_some())
            {
                card::Card card = std::move(removed).unwrap();
                discard_and_emit(sc.ctx, sc.target, card);
            }
        }

        // ── 管线 ────────────────────────────────────────────────────────

        /** @brief 插桩点（顺序即规则顺序）。 */
        enum class ShaPhase : std::uint8_t
        {
            OnTarget,  /**< 指定目标之后（防具之前） */
            Armor,     /**< 防具拦截 */
            Respond,   /**< 响应窗口（判定/出闪） */
            PostJink,  /**< 被闪之后 */
            PreDamage, /**< 命中之前 */
            OnHit,     /**< 造成伤害之后 */
        };

        /** @brief 钩子注册项：能力 / 插桩点 / 挂在哪一方 / 回调。 */
        struct ShaHook
        {
            card::Ability ability;  /**< 触发该钩子的装备能力。 */
            ShaPhase phase;         /**< 执行的插桩点。 */
            bool attacker_side;     /**< true = 挂在攻击方，false = 挂在目标。 */
            void (*fn)(ShaContext &); /**< 钩子回调。 */
        };

        /**
         * @brief 全部装备钩子（新增武器 = 加一行）。
         * @return 进程内静态钩子表引用；顺序即规则执行顺序。
         */
        inline const std::vector<ShaHook> &sha_hook_table()
        {
            static const std::vector<ShaHook> table = {
                {card::Ability::Cixiong, ShaPhase::OnTarget, true, hook_cixiong},
                {card::Ability::BlackShaImmune, ShaPhase::Armor, false, hook_renwang},
                {card::Ability::VineArmor, ShaPhase::Armor, false, hook_tengjia},
                {card::Ability::JudgementJink, ShaPhase::Respond, false, hook_bagua},
                {card::Ability::ExtraShaAfterJink, ShaPhase::PostJink, true,
                 hook_qinglong},
                {card::Ability::DiscardTwoForceDamage, ShaPhase::PostJink, true,
                 hook_guanshi},
                {card::Ability::DamageAsDiscard, ShaPhase::PreDamage, true, hook_hanbing},
                {card::Ability::GudingBlade, ShaPhase::PreDamage, true, hook_guding},
                {card::Ability::DiscardHorseOnDamage, ShaPhase::OnHit, true, hook_qilin},
            };
            return table;
        }

        /**
         * @brief 执行某插桩点上所有已装备能力的钩子。
         * @param[in,out] sc    杀结算上下文；钩子按注册序读写。
         * @param[in]     phase 要执行的插桩点。
         * @post 挂在该阶段且持有对应能力者已按注册序执行钩子。
         */
        inline void run_sha_phase(ShaContext &sc, ShaPhase phase)
        {
            for (const auto &h : sha_hook_table())
            {
                if (h.phase != phase)
                    continue;
                const std::string &owner = h.attacker_side ? sc.attacker : sc.target;
                if (has_ability(sc.ctx, owner, h.ability))
                    h.fn(sc);
            }
        }

        /**
         * @brief 需打出闪的共用响应入口：先跑八卦阵判定，未视为闪再开真闪响应窗。
         * @param[in] ctx    对局上下文。
         * @param[in] ai     决策源。
         * @param[in] target 需出闪的实体 id。
         * @param[in] prompt 响应来源与后果（透传给决策源；八卦阵触发不消费来源牌）。
         * @return 是否完成闪响应（视同闪或实际打出闪）。
         * @retval true  八卦阵判定视为闪，或实际消费一张真闪。
         * @retval false 未响应；状态不变（八卦阵未发动的判定牌不消费）。
         * @note 杀响应与万箭共用「需闪」语义，既有 Respond 阶段目标侧被动
         *       （当前仅八卦阵）由此对两者一致生效；无对应防具的目标不会被询问。
         *       新增 Respond 阶段目标侧被动时，若也应作用于万箭，须同时接入本入口。
         */
        inline bool request_jink(
            GameContext &ctx, DecisionSource &ai, const std::string &target,
            const ResponsePrompt &prompt)
        {
            // 有对应防具才询问发动，避免对未装备者多开触发窗口
            if (has_ability(ctx, target, card::Ability::JudgementJink) &&
                trigger_bagua_jink(ctx, ai, target))
                return true;

            // 无装备生效：开真闪响应窗口（打出真闪）
            return request_response(
                ctx, ai, target, card::ResponseKind::Jink, prompt);
        }

        /**
         * @brief 「杀」结算主流程：attacker 对 target 使用杀。
         * @param[in] ctx          对局上下文。
         * @param[in] ai           决策源；防具/响应/钩子触发询问。
         * @param[in] attacker     攻击方实体 id。
         * @param[in] sha          该杀的卡牌对象（花色用于仁王盾黑杀判定）。
         * @param[in] target       目标实体 id。
         * @param[in] amount       伤害量（config 驱动，当前数据均为 1）。
         * @param[in] target_count 该杀指定的目标总数（缺省 1；多目标杀逐目标
         *                         结算时由调用方传入，供仅唯一目标触发的能力判定）。
         * @param[in] virtual_sha  是否虚拟杀（丈八两张当杀）：真无花色，仁王盾
         *                         黑杀判定短路；此时 sha 参数可为占位对象。
         * @param[in] damage_type  伤害属性（默认普通；火杀/雷杀由 effect 透传）。
         * @param[in] damage_bonus 命中伤害修正初值（酒等调用方传入；目标侧藤甲钩子
         *                         在其上继续累加）。
         * @post 命中时目标已扣血，并可能已触发濒死/死亡/击杀奖惩与命中后钩子；
         *       被闪/防具无效/伤害替代时不扣血，响应牌与判定牌已按规则消费。
         * @note 执行顺序固定：朱雀羽扇转化 → OnTarget → Armor → Respond →
         *       PostJink → PreDamage → 伤害 → OnHit。
         */
        inline void resolve_sha(
            GameContext &ctx, DecisionSource &ai, const std::string &attacker,
            const card::Card &sha, const std::string &target, int amount,
            int target_count, bool virtual_sha, card::DamageType damage_type,
            int damage_bonus)
        {
            ShaContext sc{ctx, ai, sha, attacker, target, amount};
            sc.ignore_armor = has_ability(ctx, attacker, card::Ability::IgnoreArmor);
            sc.target_count = target_count;
            sc.virtual_sha = virtual_sha;
            sc.damage_type = damage_type;
            // 加成初值先落位，钩子（藤甲火焰脆弱等）在 Armor 阶段累加
            sc.damage_bonus = damage_bonus;

            // 朱雀羽扇：普通杀使用时可转为火焰伤害。非锁定技、可放弃、无每回合
            // 限制；火杀/雷杀属性非普通，不询问（只能转化普通杀）
            if (sc.damage_type == card::DamageType::Normal &&
                has_ability(ctx, attacker, card::Ability::FireShaConvert) &&
                ai.trigger_effect(ctx, attacker, card::Ability::FireShaConvert))
                sc.damage_type = card::DamageType::Fire;

            run_sha_phase(sc, ShaPhase::OnTarget);

            run_sha_phase(sc, ShaPhase::Armor);
            if (sc.blocked)
                return;

            run_sha_phase(sc, ShaPhase::Respond);
            if (!sc.responded)
                sc.responded = request_response(
                    ctx, ai, target, card::ResponseKind::Jink,
                    {sc.sha.def_id, sc.attacker, sc.amount});

            if (sc.responded)
                run_sha_phase(sc, ShaPhase::PostJink);

            if (!sc.responded)
            {
                run_sha_phase(sc, ShaPhase::PreDamage);
                if (sc.prevented)
                    return;
                CombatResolver(ctx, ai).deal_damage(
                    target, DamageSpec::Builder{}
                                .source(attacker)
                                .damage_val(amount + sc.damage_bonus)
                                .damage_type(sc.damage_type)
                                .ignore_armor(sc.ignore_armor)
                                .build());
                run_sha_phase(sc, ShaPhase::OnHit);
            }
        }
    }
}

#endif  // INCLUDE_TKW_GAME_WEAPON_HPP
