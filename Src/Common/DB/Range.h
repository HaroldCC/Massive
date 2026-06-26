/**
 * @file Range.h
 * @brief 类型安全查询构建器 Range<T>
 *
 * 使用示例：
 * @code
 *   DB::Range<PlayersTable>()
 *       .Where(PlayersTable::PlayerID == 1001)
 *       .SingleOrDefault([](std::optional<PlayerRow> player) { ... });
 * @endcode
 *
 * 底层调用 DBWorkerPool::AsyncQuery 执行参数化 SQL。
 */
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/DB/Types.h"
#include "Common/DB/Column.h"
#include "Common/DB/DBWorkerPool.h"

namespace MMO::DB
{

    /**
     * @brief 类型安全查询构建器
     * @tparam Table  表结构体，须提供 kTableName 和 RowType
     */
    template <typename Table>
    class Range
    {
    public:
        using RowType = typename Table::RowType;

        Range() : _tableName(Table::kTableName)
        {
        }

        /**
 * @brief 显式指定 DBWorkerPool
 */
        explicit Range(DBWorkerPool *pool) : _tableName(Table::kTableName), _pool(pool)
        {
        }

        // ── WHERE ──

        /**
         * @brief 添加查询条件
         * @param expr   Column::operator== 等返回的 Expression
         * @param more   可选更多条件（隐式 AND）
         */
        template <typename... Exprs>
        Range &Where(auto expr, Exprs... more)
        {
            AddExpr(expr);
            if constexpr (sizeof...(more) > 0)
            {
                return Where(more...);
            }
            return *this;
        }

        // ── SELECT ──

        template <typename... Cols>
        Range &Select(Cols... cols)
        {
            (_selects.push_back(cols.name), ...);
            return *this;
        }

        // ── ORDER BY / LIMIT ──

        Range &OrderBy(const auto &col)
        {
            _orderBy   = col.name;
            _orderDesc = false;
            return *this;
        }

        Range &OrderByDesc(const auto &col)
        {
            _orderBy   = col.name;
            _orderDesc = true;
            return *this;
        }

        Range &Limit(int n)
        {
            _limit = n;
            return *this;
        }

        // ── 执行操作 ──

        /**
         * @brief 异步查询：结果行数组
         * @param callback  void(std::vector<RowType>)
         */
        void ToArray(std::function<void(std::vector<RowType>)> callback)
        {
            std::vector<DBValue> params;
            auto                 sql  = BuildSelectSQL(params);
            auto                 pool = _pool ? _pool : &DBWorkerPool::Instance();
            pool->AsyncQuery(std::move(sql),
                             std::move(params),
                             [cb = std::move(callback)](const DBResult &res) {
                                 std::vector<RowType> rows;
                                 if (!res.IsOK())
                                 {
                                     cb(std::move(rows));
                                     return;
                                 }
                                 rows.reserve(static_cast<size_t>(res.RowCount()));
                                 for (int i = 0; i < res.RowCount(); ++i)
                                 {
                                     // DeserializeRow 需要表结构体实现，待生成代码集成
                                 }
                                 cb(std::move(rows));
                             });
        }

        /**
         * @brief 异步查询：单行或空
         * @param callback  void(std::optional<RowType>)
         */
        void SingleOrDefault(std::function<void(std::optional<RowType>)> callback)
        {
            Limit(1);
            ToArray([cb = std::move(callback)](std::vector<RowType> rows) {
                if (rows.empty())
                {
                    cb(std::nullopt);
                }
                else
                {
                    cb(std::move(rows[0]));
                }
            });
        }

        /**
         * @brief 异步 INSERT
         * @param rows  要插入的行
         */
        template <typename... Rows>
        void Insert(Rows &...rows)
        {
            (BuildInsertSingle(rows), ...);
        }

        // 构建 SQL（用于调试/日志）
        std::string BuildSQL() const
        {
            std::vector<DBValue> params;
            return BuildSelectSQL(params);
        }

    private:
        void AddExpr(const Expression<int32_t> &expr)
        {
            _int32Exprs.push_back(expr);
        }

        void AddExpr(const Expression<int64_t> &expr)
        {
            _int64Exprs.push_back(expr);
        }

        void AddExpr(const Expression<float> &expr)
        {
            _floatExprs.push_back(expr);
        }

        void AddExpr(const Expression<double> &expr)
        {
            _doubleExprs.push_back(expr);
        }

        void AddExpr(const Expression<std::string> &expr)
        {
            _stringExprs.push_back(expr);
        }

        void AddExpr(const CompoundExpr &expr)
        {
            _compoundExprs.push_back(expr);
        }

        std::string BuildSelectSQL(std::vector<DBValue> &outParams) const
        {
            std::string sql = "SELECT ";
            if (_selects.empty())
            {
                sql += '*';
            }
            else
            {
                for (size_t i = 0; i < _selects.size(); ++i)
                {
                    if (i > 0)
                    {
                        sql += ", ";
                    }
                    sql += _selects[i];
                }
            }
            sql += " FROM ";
            sql += _tableName;

            auto whereClause = BuildWhereSQL(outParams);
            if (!whereClause.empty())
            {
                sql += " WHERE ";
                sql += whereClause;
            }

            if (!_orderBy.empty())
            {
                sql += " ORDER BY ";
                sql += _orderBy;
                if (_orderDesc)
                {
                    sql += " DESC";
                }
            }

            if (_limit > 0)
            {
                sql += " LIMIT ";
                sql += std::to_string(_limit);
            }

            return sql;
        }

        std::string BuildWhereSQL(std::vector<DBValue> &params) const
        {
            std::string result;

            auto appendExpr = [&params, &result](const auto &expr, int &paramIdx) -> void {
                if (!result.empty())
                {
                    result += " AND ";
                }
                result += expr.columnName;
                switch (expr.op)
                {
                    case EOp::EQ:
                        result += " = $";
                        break;
                    case EOp::NE:
                        result += " <> $";
                        break;
                    case EOp::LT:
                        result += " < $";
                        break;
                    case EOp::LE:
                        result += " <= $";
                        break;
                    case EOp::GT:
                        result += " > $";
                        break;
                    case EOp::GE:
                        result += " >= $";
                        break;
                }
                result += std::to_string(++paramIdx);
                params.emplace_back(expr.value);
            };

            int paramIdx = 0;

            for (auto &expr : _int32Exprs)
            {
                appendExpr(expr, paramIdx);
            }
            for (auto &expr : _int64Exprs)
            {
                appendExpr(expr, paramIdx);
            }
            for (auto &expr : _floatExprs)
            {
                appendExpr(expr, paramIdx);
            }
            for (auto &expr : _doubleExprs)
            {
                appendExpr(expr, paramIdx);
            }
            for (auto &expr : _stringExprs)
            {
                appendExpr(expr, paramIdx);
            }

            return result;
        }

        template <typename R>
        void BuildInsertSingle(const R &row)
        {
            std::vector<DBValue> params;
            std::string          sql  = BuildInsertSQL(row, params);
            auto                 pool = _pool ? _pool : &DBWorkerPool::Instance();
            pool->AsyncQuery(std::move(sql), std::move(params), nullptr);
        }

        template <typename R>
        std::string BuildInsertSQL(const R &row, std::vector<DBValue> &params) const
        {
            // 默认实现——需要实际表定义，由代码生成填充
            (void)row;
            (void)params;
            return {};
        }

        const char *_tableName;

        std::vector<Expression<int32_t>>     _int32Exprs;
        std::vector<Expression<int64_t>>     _int64Exprs;
        std::vector<Expression<float>>       _floatExprs;
        std::vector<Expression<double>>      _doubleExprs;
        std::vector<Expression<std::string>> _stringExprs;
        std::vector<CompoundExpr>            _compoundExprs;

        std::vector<std::string> _selects;
        std::string              _orderBy;
        bool                     _orderDesc = false;
        int                      _limit     = -1;

        DBWorkerPool *_pool = nullptr;
    };

} // namespace MMO::DB
