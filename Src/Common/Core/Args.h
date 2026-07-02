/**
 * @file Args.h
 * @brief 轻量级命令行参数解析（header-only，零依赖）
 *
 * 用法：
 * @code
 *   auto args = MMO::Args(argc, argv);
 *   auto configPath = args.Get("--config-path", "Config/center.toml");
 *   auto keyPath    = args.Get("--key-path",   "Config/login.key");
 *   if (args.Has("--help")) { PrintUsage(); return 0; }
 *   auto positional = args.Positional(); // 非 --flag 的参数
 * @endcode
 *
 * 设计原则：
 *   - Header-only，不引入第三方依赖
 *   --flag value 和 --flag=value 两种风格都支持
 *   - 不抛异常
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace MMO
{

    class Args
    {
    public:
        Args() = default;

        /**
         * @brief 从 argc/argv 构造
         */
        Args(int argc, char **argv)
        {
            for (int i = 1; i < argc; ++i)
            {
                _tokens.push_back(argv[i]);
            }
        }

        /**
         * @brief 检查是否存在某个 flag（如 --help）
         */
        [[nodiscard]] bool Has(const std::string &flag) const noexcept
        {
            return std::find(_tokens.begin(), _tokens.end(), flag) != _tokens.end();
        }

        /**
         * @brief 获取 flag 的值
         *
         * 支持两种风格：
         *   --config-path "Config/center.toml"
         *   --config-path=Config/center.toml
         *
         * @param flag          flag 名称（如 "--config-path"）
         * @param defaultVal    未指定时的默认值
         * @return flag 的值，或 defaultVal
         */
        [[nodiscard]] std::string Get(const std::string &flag,
                                      const std::string &defaultVal = "") const noexcept
        {
            // --flag=value 风格
            auto eq_prefix = flag + "=";
            for (const auto &token : _tokens)
            {
                if (token.find(eq_prefix) == 0)
                {
                    return token.substr(eq_prefix.size());
                }
            }

            // --flag value 风格
            for (std::size_t i = 0; i + 1 < _tokens.size(); ++i)
            {
                if (_tokens[i] == flag)
                {
                    return _tokens[i + 1];
                }
            }

            return defaultVal;
        }

        /**
         * @brief 获取剩余的位置参数（不以 -- 开头的参数）
         */
        [[nodiscard]] std::vector<std::string> Positional() const noexcept
        {
            std::vector<std::string> result;
            bool                     skip_next = false;
            for (std::size_t i = 0; i < _tokens.size(); ++i)
            {
                if (skip_next)
                {
                    skip_next = false;
                    continue;
                }
                const auto &tok = _tokens[i];
                if (tok.find("--") == 0)
                {
                    // --flag=value 风格：不 skip
                    if (tok.find('=') != std::string::npos)
                    {
                        continue;
                    }
                    // --flag value 风格：skip 下一个
                    if (i + 1 < _tokens.size() && _tokens[i + 1].find("--") != 0)
                    {
                        skip_next = true;
                    }
                    continue;
                }
                result.push_back(tok);
            }
            return result;
        }

        /**
         * @brief 参数个数
         */
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _tokens.size();
        }

    private:
        std::vector<std::string> _tokens;
    };

} // namespace MMO
