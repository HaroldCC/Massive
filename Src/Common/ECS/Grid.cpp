#include "Grid.h"
#include <cmath>
#include <unordered_set>

namespace MMO::ECS
{
    Grid::Grid(float cellSize) : _cellSize(cellSize > 0.0f ? cellSize : 10.0f)
    {
    }

    /**
     * @brief 插入实体到格子
     * @param entityIndex 实体内部索引
     * @param x 坐标x
     * @param z 坐标z
     */
    void Grid::Insert(EntityIndex entityIndex, float x, float z)
    {
        const int   cx  = static_cast<int>(std::floor(x / _cellSize));
        const int   cz  = static_cast<int>(std::floor(z / _cellSize));
        const int64 key = CellKey(cx, cz);
        _cells[key].push_back(entityIndex);
        _entityCell[entityIndex] = key;
    }

    /**
     * @brief 更新实体位置
     * @param entityIndex 实体内部索引
     * @param newX 新坐标x
     * @param newZ 新坐标z
     */
    void Grid::Update(EntityIndex entityIndex, float newX, float newZ)
    {
        auto it = _entityCell.find(entityIndex);
        if (it == _entityCell.end())
        {
            Insert(entityIndex, newX, newZ);
            return;
        }

        const int64 oldKey = it->second;
        const int   ncx    = static_cast<int>(std::floor(newX / _cellSize));
        const int   ncz    = static_cast<int>(std::floor(newZ / _cellSize));
        const int64 newKey = CellKey(ncx, ncz);
        if (oldKey == newKey)
        {
            return;
        }
        Remove(entityIndex);
        Insert(entityIndex, newX, newZ);
    }

    /**
     * @brief 删除实体
     * @param entityIndex 实体内部索引
     */
    void Grid::Remove(EntityIndex entityIndex)
    {
        auto it = _entityCell.find(entityIndex);
        if (it == _entityCell.end())
        {
            return;
        }

        auto &vec = _cells[it->second];
        vec.erase(std::remove(vec.begin(), vec.end(), entityIndex), vec.end());
        if (vec.empty())
        {
            _cells.erase(it->second);
        }
        _entityCell.erase(it);
    }

    /**
     * @brief 半径查询
     * @param x 坐标x
     * @param z 坐标z
     * @param radius 半径
     * @param out 输出
     */
    void Grid::QueryRadius(float x, float z, float radius, std::vector<EntityIndex> &out) const
    {
        int minX, maxX, minZ, maxZ;
        CellRange(x, z, radius, minX, maxX, minZ, maxZ);

        // 覆盖格子去重收集（实体可能跨格边界被重复枚举——用哈希集去重，避免 O(n²)）
        // 注意：5w 实体场景下 std::find(out) 线性去重是平方开销，改 unordered_set 摊还 O(1)
        std::unordered_set<EntityIndex> seen;
        for (int cx = minX; cx <= maxX; ++cx)
        {
            for (int cz = minZ; cz <= maxZ; ++cz)
            {
                auto it = _cells.find(CellKey(cx, cz));
                if (it != _cells.end())
                {
                    for (EntityIndex idx : it->second)
                    {
                        seen.insert(idx);
                    }
                }
            }
        }

        // 精确距离过滤由调用方（AOI 系统）拿 Position 组件做；
        // 这里输出已去重的候选集。
        out.assign(seen.begin(), seen.end());
    }

    void Grid::Clear()
    {
        _cells.clear();
        _entityCell.clear();
    }

    int64 Grid::CellKey(int cellX, int cellZ) const
    {
        return (static_cast<int64>(cellX) << 32) ^ static_cast<uint32>(cellZ);
    }

    void Grid::CellRange(float x, float z, float radius, int &minX, int &maxX, int &minZ, int &maxZ) const
    {
        minX = static_cast<int>(std::floor((x - radius) / _cellSize));
        maxX = static_cast<int>(std::floor((x + radius) / _cellSize));
        minZ = static_cast<int>(std::floor((z - radius) / _cellSize));
        maxZ = static_cast<int>(std::floor((z + radius) / _cellSize));
    }
} // namespace MMO::ECS