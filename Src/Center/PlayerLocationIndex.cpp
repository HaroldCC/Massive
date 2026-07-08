/**
 * @file PlayerLocationIndex.cpp
 * @brief 玩家位置索引实现
 */

#include "Center/PlayerLocationIndex.h"

namespace MMO
{

    void PlayerLocationIndex::RegisterPlayer(uint32 accountID, const std::string &serviceID)
    {
        std::unique_lock lock(_mutex);
        _accountToService[accountID] = serviceID;
    }

    void PlayerLocationIndex::UnregisterPlayer(uint32 accountID)
    {
        std::unique_lock lock(_mutex);
        _accountToService.erase(accountID);
    }

    std::optional<std::string> PlayerLocationIndex::GetServiceID(uint32 accountID) const
    {
        std::shared_lock lock(_mutex);
        auto             it = _accountToService.find(accountID);
        if (it == _accountToService.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    uint32 PlayerLocationIndex::GetTotalOnline() const
    {
        std::shared_lock lock(_mutex);
        return static_cast<uint32>(_accountToService.size());
    }

    void PlayerLocationIndex::ClearWorld(const std::string &serviceID)
    {
        std::unique_lock lock(_mutex);
        std::erase_if(_accountToService, [&](const auto &pair) {
            return pair.second == serviceID;
        });
    }

} // namespace MMO
