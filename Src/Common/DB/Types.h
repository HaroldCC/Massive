/**
 * @file Types.h
 * @brief DB 层基础类型：DBValue、DBResult、EOp 枚举、Column 标志位
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Common/Core/Types.h"

namespace MMO::DB
{

    struct Timestamp; // 前向声明，用于 DBValue(Timestamp) 构造函数

    /**
     * @brief 数据库值类型（参数化 SQL 传参 + 结果读取）
     *
     * 内部以 text 格式存储 string，和 libpq PQexecParams text format 无缝匹配。
     */
    class DBValue
    {
    public:
        DBValue() = default;

        explicit DBValue(int32_t v) : _text(std::to_string(v)), _isNull(false)
        {
        }

        explicit DBValue(int64_t v) : _text(std::to_string(v)), _isNull(false)
        {
        }

        explicit DBValue(float v) : _text(std::to_string(v)), _isNull(false)
        {
        }

        explicit DBValue(double v) : _text(std::to_string(v)), _isNull(false)
        {
        }

        explicit DBValue(std::string v) : _text(std::move(v)), _isNull(false)
        {
        }

        explicit DBValue(const char *v) : _text(v), _isNull(false)
        {
        }

        explicit DBValue(const Timestamp &v);

        explicit DBValue(std::nullptr_t) : _isNull(true)
        {
        }

        bool IsNull() const
        {
            return _isNull;
        }

        // 用于 PQexecParams 的 text 格式 C 字符串指针（nullptr if null）
        const char *CStr() const
        {
            return _isNull ? nullptr : _text.c_str();
        }

        // 用于 PQexecParams 的长度数组
        int Length() const
        {
            return _isNull ? 0 : static_cast<int>(_text.size());
        }

        // 是否 binary format（统一 text format，返回 0）
        int Format() const
        {
            return 0;
        }

        const std::string &Text() const
        {
            return _text;
        }

        /**
         * @brief 转换为原生类型（结果读取时使用），非法格式返回 0
         */
        int32_t AsInt32() const noexcept;
        int64_t AsInt64() const noexcept;
        float   AsFloat() const noexcept;
        double  AsDouble() const noexcept;

    private:
        std::string _text;
        bool        _isNull = true;
    };

    // SQL 行：字段名 → DBValue 的映射
    using DBRow = std::vector<std::pair<std::string, DBValue>>;

    /**
     * @brief 查询结果
     *
     * 包含影响行数（INSERT/UPDATE/DELETE）或字段列表 + 行集（SELECT）。
     */
    class DBResult
    {
    public:
        explicit DBResult(void *pgResult); // 从 PGresult* 构造
        ~DBResult();

        DBResult(DBResult &&other) noexcept;
        DBResult &operator=(DBResult &&other) noexcept;

        DBResult(const DBResult &)            = delete;
        DBResult &operator=(const DBResult &) = delete;

        // 行数
        int RowCount() const
        {
            return _rowCount;
        }

        // 列数
        int ColCount() const
        {
            return _colCount;
        }

        // 列名
        const std::vector<std::string> &Columns() const
        {
            return _columns;
        }

        /**
         * @brief 获取指定单元格值（行列索引）
         */
        DBValue Get(int row, int col) const;
        /**
         * @brief 获取整行（列名 → DBValue）
         */
        DBRow GetRow(int row) const;

        // 是否查询成功（非 PGError）
        bool IsOK() const
        {
            return _ok;
        }

        // 影响行数（INSERT/UPDATE/DELETE）
        int AffectedRows() const
        {
            return _affectedRows;
        }

    private:
        void                    *_pgResult     = nullptr;
        bool                     _ok           = false;
        int                      _rowCount     = 0;
        int                      _colCount     = 0;
        int                      _affectedRows = 0;
        std::vector<std::string> _columns;
    };

    // ── 运算符枚举 ──

    enum class EOp : uint8
    {
        EQ, // =
        NE, // <>
        LT, // <
        LE, // <=
        GT, // >
        GE, // >=
    };

    enum class ELogicalOp : uint8
    {
        AND,
        OR
    };

    // ── Column 标志位 ──

    using ColumnFlags = uint32;

    inline constexpr ColumnFlags kNone      = 0;
    inline constexpr ColumnFlags kPK        = 1 << 0; // 主键
    inline constexpr ColumnFlags kAutoInc   = 1 << 1; // 自增
    inline constexpr ColumnFlags kRequired  = 1 << 2; // NOT NULL
    inline constexpr ColumnFlags kNullable  = 1 << 3; // 可空
    inline constexpr ColumnFlags kDefaulted = 1 << 4; // 有默认值

} // namespace MMO::DB
