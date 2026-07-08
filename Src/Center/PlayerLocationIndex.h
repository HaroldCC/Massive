/**
 * @file PlayerLocationIndex.h
 * @brief 玩家位置索引——accountId → 所在 WorldServer
 *
 * 纯 IO 进程使用读写锁（读多写少）。
 */
#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Common/Core/Types.h"

namespace MMO
{

    class PlayerLocationIndex
    {
    public:
        // 玩家上线（注册到某 World）
        void RegisterPlayer(uint32 accountID, const std::string &serviceID);

        // 玩家下线
        void UnregisterPlayer(uint32 accountID);

        // 查询玩家所在 World
        std::optional<std::string> GetServiceID(uint32 accountID) const;

        // 清空某 World 的所有记录（World 重连重建用）
        void ClearWorld(const std::string &serviceID);

        // 总在线玩家数
        uint32 GetTotalOnline() const;

    private:
        mutable std::shared_mutex               _mutex;
        std::unordered_map<uint32, std::string> _accountToService;
    };

} // namespace MMO
