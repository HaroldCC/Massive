/**
 * @file ScriptDispatchRegistry.h
 * @brief msgID → 脚本消息分发函数注册表
 *
 * 各 Src/World/AutoGen/<ProtoFileName>.gen.cpp 在初始化阶段调用 Register()
 * 注册自己的分发函数；WorldServer::OnMessage 统一调用 Dispatch()。
 * 与 Common/Network/MessageDispatcher 的定长数组设计保持一致，
 * 但分发目标是脚本而非 C++ Handler，故单独成表，不复用该类模板。
 *
 * @note CodeReview #7: 当前为静态单例设计——与 AutoGen 代码生成器产出兼容。
 *   同一进程多 WorldServer 实例时，后初始化的会覆盖先初始化的注册表。
 *   如有分场景部署需求，需同时修改 GenMsgBindings.py 生成 Instance 签名。
 */
#pragma once

#include <array>

#include "Common/Core/Types.h"

namespace MMO
{

    class WorldServer;

    /**
     * @brief 脚本分发函数签名——解析 protobuf + 转发到 daScript
     * @return 解析成功且已转发返回 true；解析失败返回 false
     */
    using ScriptDispatchFn = bool (*)(WorldServer &server, uint32 sessionID,
                                      const uint8 *body, size_t len);

    /**
     * @brief msgID → ScriptDispatchFn 定长表，O(1) 查表
     */
    class ScriptDispatchRegistry
    {
    public:
        /**
         * @brief 注册一个 msgID 的分发函数——由各 *.gen.cpp 在启动期调用
         */
        static void Register(uint32 msgID, ScriptDispatchFn fn);

        /**
         * @brief 按 msgID 查表并分发
         * @return 找到对应分发函数且分发成功返回 true；未注册该 msgID 返回 false
         */
        static bool Dispatch(WorldServer &server, uint32 sessionID, uint32 msgID,
                              const uint8 *body, size_t len);

    private:
        static std::array<ScriptDispatchFn, kMaxHandlers> &Table();
    };

} // namespace MMO
