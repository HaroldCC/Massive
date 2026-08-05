#pragma once

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"
#include <unordered_map>

namespace MMO::ECS
{
    /**
     * @brief 均匀格子空间索引（2D XZ 平面，3D 高度 Y 单独判）
     *
     * 格子是静态的（kGridCellSize=10 世界单位）。实体按 (cellX, cellZ) 归属。
     * 移动时移格（旧格删除，新格插入）。
     *
     * 存储：cellKey(int64) → vector<EntityIndex>
     * 查询：半径范围内枚举覆盖格子，收集实体，Y 轴过滤由调用方做。
     *
     * 注意：key 是 EntityIndex（EnTT 内部索引），不是对外 EntityID——
     * 每帧热路径只存 32 位 index 省内存；且 Grid 是 per-scene 的，scene 位无意义。
     */
    class Grid
    {
    public:
        explicit Grid(float cellSize = 10.0f);

        /**
         * @brief 插入实体到格子
         * @param entityIndex 实体内部索引（EnTT index）
         * @param x 坐标x
         * @param z 坐标z
         */
        void Insert(EntityIndex entityIndex, float x, float z);

        /**
         * @brief 更新实体位置
         * @param entityIndex 实体内部索引
         * @param newX 新坐标x
         * @param newZ 新坐标z
         */
        void Update(EntityIndex entityIndex, float newX, float newZ);

        /**
         * @brief 删除实体
         * @param entityIndex 实体内部索引
         */
        void Remove(EntityIndex entityIndex);

        /**
         * @brief 半径查询
         * @param x 坐标x
         * @param z 坐标z
         * @param radius 半径
         * @param out 输出（EntityIndex 候选集，调用方二次过滤）
         */
        void QueryRadius(float x, float z, float radius, std::vector<EntityIndex> &out) const;

        size_t CellCount() const
        {
            return _cells.size();
        }

        void Clear();

    private:
        int64 CellKey(int cellX, int cellZ) const;

        void CellRange(float x, float z, float radius, int &minX, int &maxX, int &minZ, int &maxZ) const;

    private:
        float                                               _cellSize {0.0f};
        std::unordered_map<int64, std::vector<EntityIndex>> _cells;
        std::unordered_map<EntityIndex, int64>              _entityCell;
    };
} // namespace MMO::ECS