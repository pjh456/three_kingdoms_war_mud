/**
 * @file main.cpp
 * @brief CLI 入口（pjh_cli）：构建命令树后解析 argv 并分派 help/version/action。
 * @note 命令树与执行体在 cli/commands.hpp，便于测试复用同一棵树。
 */

#include <iostream>

#include <pjh_cli.hpp>

#include "cli/commands.hpp"
#include "cli/error_zh.hpp"

int main(int argc, char **argv)
{
    pjh::cli::App app("tkw", "0.1.0", "三国杀式卡牌对局引擎");
    tkw::cli::Session session;  // 跨命令持有的对局会话
    tkw::cli::build_app(app, session);

    // 批量入口也启用模糊匹配：唯一近距匹配自动纠错，多候选报歧义。
    auto parsed = app.parse_fuzzy(argc, argv);
    if (parsed.is_err())
    {
        std::cerr << tkw::cli::render_error_zh(parsed.unwrap_err()) << "\n";
        return 2;
    }

    auto &ctx = parsed.unwrap();
    if (ctx.help_requested())
    {
        std::cout << ctx.help_text();
        return 0;
    }
    if (ctx.version_requested())
    {
        std::cout << ctx.version_text();
        return 0;
    }

    auto executed = ctx.matched_command()->execute(ctx);
    if (executed.is_err())
    {
        std::cerr << executed.unwrap_err().what() << "\n";
        return 1;
    }
    return 0;
}
