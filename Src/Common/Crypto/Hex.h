/**
 * @file Hex.h
 * @brief 通用 hex decode 工具函数
 *
 * 提取自 LoginConfig.cpp / WorldConfig.cpp 中重复的 hex decode 逻辑。
 */
#pragma once

#include <cctype>
#include <string>

#include "Common/Core/Types.h"

namespace MMO::Crypto
{

    /**
     * @brief hex 字符 → 4-bit 值
     * @param c  hex 字符（0-9, a-f, A-F）
     * @return 4-bit 值，非法字符返回 -1
     */
    inline int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    }

    /**
     * @brief hex 字符串 → 字节数组，失败返回 false
     * @param hex   hex 字符串（不含空格，长度为 outLen * 2）
     * @param out   输出缓冲区
     * @param outLen 输出缓冲区长度（字节数）
     * @return 成功返回 true；hex 长度不匹配或含非法字符返回 false
     */
    inline bool HexDecode(const std::string &hex, uint8 *out, size_t outLen)
    {
        if (hex.size() != outLen * 2)
        {
            return false;
        }

        for (size_t i = 0; i < outLen; ++i)
        {
            int hi = HexValue(hex[i * 2]);
            int lo = HexValue(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            out[i] = static_cast<uint8>((static_cast<unsigned>(hi) << 4) | static_cast<unsigned>(lo));
        }

        return true;
    }

    /**
     * @brief 移除字符串中的空白字符（空格/tab/换行/回车）
     */
    inline std::string StripWhitespace(const std::string &s)
    {
        std::string result;
        result.reserve(s.size());
        for (char c : s)
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
            {
                result += c;
            }
        }
        return result;
    }

} // namespace MMO::Crypto
