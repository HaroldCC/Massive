/**
 * @file ErrorCode.h
 * @brief 项目级错误码枚举（与 std::expected / std::optional 搭配）
 */
#pragma once

#include "Common/Core/Types.h"

/**
 * @brief 全项目统一错误码
 *
 * 与 C++23 std::expected 搭配：
 *   std::expected<Entity, EErrorCode> CreatePlayer(const std::string& name);
 *
 * 预分配段：
 *   100-199  DB 模块
 *   200-299  Network 模块
 *   300-399  ECS 模块
 *   400-499  Crypto 模块
 *   500-599  Timer 模块
 */
enum class EErrorCode : uint32
{
    /**
 * @brief 成功
 */
    OK = 0,
    /**
 * @brief 未分类错误
 */
    UNKNOWN = 1,
    /**
 * @brief 非法参数
 */
    INVALID_ARGUMENT = 2,
    /**
 * @brief 越界
 */
    OUT_OF_RANGE = 3,
    /**
 * @brief 实体/资源不存在
 */
    NOT_FOUND = 4,
    /**
 * @brief 不支持的操作
 */
    NOT_SUPPORTED = 5,
    /**
 * @brief 内部错误（不应发生）
 */
    INTERNAL = 6,
    /**
 * @brief 超时
 */
    TIMEOUT = 7,
    /**
 * @brief 重复创建
 */
    ALREADY_EXISTS = 8,
    /**
 * @brief 权限不足
 */
    PERMISSION_DENIED = 9,
    /**
 * @brief 尚未初始化
 */
    NOT_INITIALIZED = 10,
    /**
 * @brief 资源耗尽（内存/连接/句柄）
 */
    RESOURCE_EXHAUSTED = 11,
};

/**
 * @brief 错误码 → 可读字符串
 * @param code  错误码
 * @return 错误描述字符串
 */
const char *ToString(EErrorCode code);
