/**
 * @file ScriptComponentStorage.cpp
 * @brief 脚本组件 SoA Blob 列存储实现
 */

#include "Common/ECS/ScriptComponentStorage.h"

#include <cstring>

namespace MMO::ECS
{

ScriptComponentStorage::ScriptComponentStorage(std::string name, size_t componentSize)
    : _name(std::move(name))
    , _stride(componentSize)
{
}

void* ScriptComponentStorage::GetPtr(uint32 entityID)
{
    auto it = _sparse.find(entityID);
    if (it == _sparse.end())
    {
        return nullptr;
    }
    return _data.data() + it->second * _stride;
}

const void* ScriptComponentStorage::GetPtr(uint32 entityID) const
{
    auto it = _sparse.find(entityID);
    if (it == _sparse.end())
    {
        return nullptr;
    }
    return _data.data() + it->second * _stride;
}

void ScriptComponentStorage::Emplace(uint32 entityID, const void* src)
{
    auto it = _sparse.find(entityID);
    if (it != _sparse.end())
    {
        std::memcpy(_data.data() + it->second * _stride, src, _stride);
        return;
    }

    size_t index = _data.size() / _stride;
    _data.resize(_data.size() + _stride);
    std::memcpy(_data.data() + index * _stride, src, _stride);
    _sparse[entityID] = static_cast<uint32>(index);
}

void ScriptComponentStorage::Remove(uint32 entityID)
{
    auto it = _sparse.find(entityID);
    if (it == _sparse.end())
    {
        return;
    }

    size_t lastIndex = _data.size() / _stride - 1;
    size_t currentIndex = it->second;

    if (currentIndex != lastIndex)
    {
        auto* dst = _data.data() + currentIndex * _stride;
        auto* src = _data.data() + lastIndex * _stride;
        std::memcpy(dst, src, _stride);

        for (auto& [eid, idx] : _sparse)
        {
            if (idx == lastIndex && eid != entityID)
            {
                _sparse[eid] = static_cast<uint32>(currentIndex);
                break;
            }
        }
    }

    _data.resize(_data.size() - _stride);
    _sparse.erase(it);
}

} // namespace MMO::ECS
