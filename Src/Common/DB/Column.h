/**
 * @file Column.h
 * @brief 类型安全 Column 定义 + Expression 构建
 *
 * Column::operator== 返回 Expression 而非 bool——这是 C++ 能实现 LINQ 语法的根基。
 */
#pragma once

#include <string>
#include <vector>

#include "Common/DB/Types.h"

namespace MMO::DB
{

    // 前向声明（Expression::operator&& 返回 CompoundExpr）
    struct CompoundExpr;

    /**
     * @brief 表达式构建（Column::operator== 等返回的类型）
     * @tparam T  值类型（int32_t/int64_t/float/double/std::string）
     */
    template <typename T>
    struct Expression
    {
        const char *columnName;
        EOp         op;
        T           value;

        auto operator&&(const CompoundExpr &other) const -> CompoundExpr;
        auto operator||(const CompoundExpr &other) const -> CompoundExpr;
    };

    /**
     * @brief 复合表达式（多个 Expression 通过 AND/OR 组合）
     */
    struct CompoundExpr
    {
        std::vector<Expression<int32_t>>     int32Exprs;
        std::vector<Expression<int64_t>>     int64Exprs;
        std::vector<Expression<float>>       floatExprs;
        std::vector<Expression<double>>      doubleExprs;
        std::vector<Expression<std::string>> stringExprs;
        ELogicalOp                           logicalOp = ELogicalOp::AND;

        CompoundExpr() = default;

        template <typename T>
        CompoundExpr(const Expression<T> &a, const Expression<T> &b, ELogicalOp op) : logicalOp(op)
        {
            Add(a);
            Add(b);
        }

        template <typename T>
        void Add(const Expression<T> &expr)
        {
            if constexpr (std::is_same_v<T, int32_t>)
            {
                int32Exprs.push_back(expr);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                int64Exprs.push_back(expr);
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                floatExprs.push_back(expr);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                doubleExprs.push_back(expr);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                stringExprs.push_back(expr);
            }
        }

        // AND 组合（合并 other 的表达式到本对象尾部）
        CompoundExpr operator&&(const CompoundExpr &other) const
        {
            CompoundExpr result = *this;
            result.logicalOp    = ELogicalOp::AND;
            result.int32Exprs.insert(result.int32Exprs.end(),
                                     other.int32Exprs.begin(),
                                     other.int32Exprs.end());
            result.int64Exprs.insert(result.int64Exprs.end(),
                                     other.int64Exprs.begin(),
                                     other.int64Exprs.end());
            result.floatExprs.insert(result.floatExprs.end(),
                                     other.floatExprs.begin(),
                                     other.floatExprs.end());
            result.doubleExprs.insert(result.doubleExprs.end(),
                                      other.doubleExprs.begin(),
                                      other.doubleExprs.end());
            result.stringExprs.insert(result.stringExprs.end(),
                                      other.stringExprs.begin(),
                                      other.stringExprs.end());
            return result;
        }

        // OR 组合
        CompoundExpr operator||(const CompoundExpr &other) const
        {
            CompoundExpr result = *this;
            result.logicalOp    = ELogicalOp::OR;
            result.int32Exprs.insert(result.int32Exprs.end(),
                                     other.int32Exprs.begin(),
                                     other.int32Exprs.end());
            result.int64Exprs.insert(result.int64Exprs.end(),
                                     other.int64Exprs.begin(),
                                     other.int64Exprs.end());
            result.floatExprs.insert(result.floatExprs.end(),
                                     other.floatExprs.begin(),
                                     other.floatExprs.end());
            result.doubleExprs.insert(result.doubleExprs.end(),
                                      other.doubleExprs.begin(),
                                      other.doubleExprs.end());
            result.stringExprs.insert(result.stringExprs.end(),
                                      other.stringExprs.begin(),
                                      other.stringExprs.end());
            return result;
        }
    };

    // Expression::operator&& / || 实现（后置，此时 CompoundExpr 已完整定义）

    template <typename T>
    inline auto Expression<T>::operator&&(const CompoundExpr &other) const -> CompoundExpr
    {
        CompoundExpr result;
        result.logicalOp = ELogicalOp::AND;
        result.Add(*this);
        result.int32Exprs.insert(result.int32Exprs.end(), other.int32Exprs.begin(), other.int32Exprs.end());
        result.int64Exprs.insert(result.int64Exprs.end(), other.int64Exprs.begin(), other.int64Exprs.end());
        result.floatExprs.insert(result.floatExprs.end(), other.floatExprs.begin(), other.floatExprs.end());
        result.doubleExprs.insert(result.doubleExprs.end(),
                                  other.doubleExprs.begin(),
                                  other.doubleExprs.end());
        result.stringExprs.insert(result.stringExprs.end(),
                                  other.stringExprs.begin(),
                                  other.stringExprs.end());
        return result;
    }

    template <typename T>
    inline auto Expression<T>::operator||(const CompoundExpr &other) const -> CompoundExpr
    {
        CompoundExpr result;
        result.logicalOp = ELogicalOp::OR;
        result.Add(*this);
        result.int32Exprs.insert(result.int32Exprs.end(), other.int32Exprs.begin(), other.int32Exprs.end());
        result.int64Exprs.insert(result.int64Exprs.end(), other.int64Exprs.begin(), other.int64Exprs.end());
        result.floatExprs.insert(result.floatExprs.end(), other.floatExprs.begin(), other.floatExprs.end());
        result.doubleExprs.insert(result.doubleExprs.end(),
                                  other.doubleExprs.begin(),
                                  other.doubleExprs.end());
        result.stringExprs.insert(result.stringExprs.end(),
                                  other.stringExprs.begin(),
                                  other.stringExprs.end());
        return result;
    }

    /**
     * @brief Column 定义——描述一个数据库列
     * @tparam T  列的值类型（int32_t/int64_t/float/double/std::string）
     */
    template <typename T>
    struct Column
    {
        const char *name;  // PostgreSQL 列名
        ColumnFlags flags; // kPK / kRequired / kNullable / kDefaulted 等
        T           default_value {};

        // SQL 参数占位符索引（构建参数化 SQL 时自增）
        mutable int paramIndex = -1;

        constexpr auto operator==(T value) const
        {
            return Expression<T> {name, EOp::EQ, value};
        }

        constexpr auto operator!=(T value) const
        {
            return Expression<T> {name, EOp::NE, value};
        }

        constexpr auto operator<(T value) const
        {
            return Expression<T> {name, EOp::LT, value};
        }

        constexpr auto operator<=(T value) const
        {
            return Expression<T> {name, EOp::LE, value};
        }

        constexpr auto operator>(T value) const
        {
            return Expression<T> {name, EOp::GT, value};
        }

        constexpr auto operator>=(T value) const
        {
            return Expression<T> {name, EOp::GE, value};
        }
    };

    /**
     * @brief UpdateSet — UPDATE 的 SET 子句构建器
     *
     * 使用示例：
     *   UpdateSet{}.Set(PlayersTable::Level, 61).Set(PlayersTable::Gold, 99999LL)
     */
    class UpdateSet
    {
    public:
        template <typename T>
        UpdateSet &Set(const Column<T> &col, T value)
        {
            _assignments.emplace_back(Assignment {col.name, DBValue(value)});
            return *this;
        }

        const auto &Assignments() const
        {
            return _assignments;
        }

    private:
        struct Assignment
        {
            std::string columnName;
            DBValue     value;
        };

        std::vector<Assignment> _assignments;
    };

} // namespace MMO::DB
