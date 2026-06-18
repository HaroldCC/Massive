/**
 * @file Types.cpp
 * @brief DBResult + DBValue 转换实现
 */

#include "Common/DB/Types.h"

#include <cstdlib>
#include <libpq-fe.h>

namespace MMO::DB
{

// ── DBValue 转换 ──

int32_t DBValue::AsInt32() const noexcept
{
    if (_isNull || _text.empty()) return 0;
    char* end = nullptr;
    auto val = std::strtol(_text.c_str(), &end, 10);
    return end == _text.c_str() ? 0 : static_cast<int32_t>(val);
}

int64_t DBValue::AsInt64() const noexcept
{
    if (_isNull || _text.empty()) return 0;
    char* end = nullptr;
    auto val = std::strtoll(_text.c_str(), &end, 10);
    return end == _text.c_str() ? 0 : val;
}

float DBValue::AsFloat() const noexcept
{
    if (_isNull || _text.empty()) return 0.0f;
    char* end = nullptr;
    auto val = std::strtof(_text.c_str(), &end);
    return end == _text.c_str() ? 0.0f : val;
}

double DBValue::AsDouble() const noexcept
{
    if (_isNull || _text.empty()) return 0.0;
    char* end = nullptr;
    auto val = std::strtod(_text.c_str(), &end);
    return end == _text.c_str() ? 0.0 : val;
}

// ── DBResult ──

DBResult::DBResult(void* pgResult)
    : _pgResult(pgResult)
{
    if (!_pgResult)
    {
        return;
    }

    auto* res = static_cast<PGresult*>(_pgResult);
    auto status = PQresultStatus(res);

    if (status == PGRES_TUPLES_OK)
    {
        _ok = true;
        _rowCount = PQntuples(res);
        _colCount = PQnfields(res);
        _columns.reserve(static_cast<size_t>(_colCount));
        for (int i = 0; i < _colCount; ++i)
        {
            _columns.emplace_back(PQfname(res, i));
        }
    }
    else if (status == PGRES_COMMAND_OK)
    {
        _ok = true;
        const char* affected = PQcmdTuples(res);
        if (affected && affected[0] != '\0')
        {
            _affectedRows = static_cast<int>(std::strtol(affected, nullptr, 10));
        }
    }
}

DBResult::~DBResult()
{
    if (_pgResult)
    {
        PQclear(static_cast<PGresult*>(_pgResult));
    }
}

DBResult::DBResult(DBResult&& other) noexcept
    : _pgResult(other._pgResult)
    , _ok(other._ok)
    , _rowCount(other._rowCount)
    , _colCount(other._colCount)
    , _affectedRows(other._affectedRows)
    , _columns(std::move(other._columns))
{
    other._pgResult = nullptr;
}

DBResult& DBResult::operator=(DBResult&& other) noexcept
{
    if (this != &other)
    {
        if (_pgResult)
        {
            PQclear(static_cast<PGresult*>(_pgResult));
        }
        _pgResult = other._pgResult;
        _ok = other._ok;
        _rowCount = other._rowCount;
        _colCount = other._colCount;
        _affectedRows = other._affectedRows;
        _columns = std::move(other._columns);
        other._pgResult = nullptr;
    }
    return *this;
}

DBValue DBResult::Get(int row, int col) const
{
    if (!_pgResult || !_ok || row < 0 || row >= _rowCount || col < 0 || col >= _colCount)
    {
        return DBValue(nullptr);
    }

    auto* res = static_cast<PGresult*>(_pgResult);
    if (PQgetisnull(res, row, col))
    {
        return DBValue(nullptr);
    }

    return DBValue(std::string(PQgetvalue(res, row, col)));
}

DBRow DBResult::GetRow(int row) const
{
    DBRow result;
    if (!_pgResult || !_ok || row < 0 || row >= _rowCount)
    {
        return result;
    }

    result.reserve(static_cast<size_t>(_colCount));
    for (int col = 0; col < _colCount; ++col)
    {
        result.emplace_back(_columns[col], Get(row, col));
    }
    return result;
}

} // namespace MMO::DB
