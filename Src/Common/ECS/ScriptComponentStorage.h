/**
 * @file ScriptComponentStorage.h
 * @brief 脚本组件的 SoA Blob 列存储
 *
 * 每个列是一个连续的 byte blob，通过 sparse set 实现 O(1) 查找。
 * 采用 swap-with-last 删除策略，零空闲碎片。
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO::ECS
{

/**
 * @brief 脚本组件的 SoA Blob 列存储
 *
 * 每个 ScriptComponentStorage 对应一个脚本组件类型。
 * 数据以连续 byte blob 形式存储，通过 sparse set 映射 entityId → 密集索引。
 */
class ScriptComponentStorage
{
public:
    /**
     * @brief 构造
     * @param name            组件名
     * @param componentSize   单组件字节数
     */
    ScriptComponentStorage(std::string name, size_t componentSize);

    // 组件名
    const std::string& Name() const { return _name; }
    // 单组件字节数
    size_t Stride() const { return _stride; }

    // 是否有该 entity 的组件
    bool Has(uint32 entityID) const { return _sparse.contains(entityID); }

    /**
     * @brief 获取组件数据指针
     * @param entityID  entity ID
     * @return 组件数据指针，不存在返回 nullptr
     */
    void* GetPtr(uint32 entityID);
    const void* GetPtr(uint32 entityID) const;

    /**
     * @brief 放置或覆盖组件数据
     * @param entityID  entity ID
     * @param src      源数据指针（拷贝 _stride 字节）
     */
    void Emplace(uint32 entityID, const void* src);

    /**
     * @brief O(1) 删除（swap-with-last）
     * @param entityID  entity ID
     */
    void Remove(uint32 entityID);

    // 密集数据指针（SoA blob 起始地址）
    uint8_t* DataPtr() { return _data.data(); }
    const uint8_t* DataPtr() const { return _data.data(); }

    // 当前存储的组件数量
    size_t Count() const { return _data.size() / _stride; }

private:
    std::string                         _name;
    size_t                              _stride = 0;
    std::vector<uint8_t>                _data;    // SoA blob 数据
    std::unordered_map<uint32, uint32>  _sparse;  // entityID → 密集索引
};

} // namespace MMO::ECS
