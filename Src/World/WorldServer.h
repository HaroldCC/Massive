/**
 * @file WorldServer.h
 * @brief WorldServer 主类——游戏逻辑核心进程
 *
 * 线程模型：
 *   IOContextPool (N 线程): Gate/Center 网络 IO
 *   LogicThread (1 线程):   Per-Session inbox Drain + 20ms Tick 游戏逻辑
 *   DBWorkerPool (3-5 线程): libpq 阻塞查询
 */
#pragma once

#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Common/Core/Types.h"

#include <daScript/ast/ast.h>
#include <daScript/simulate/simulate.h>

#include "Common/ECS/MassiveModule.h"
#include "Common/Log/Log.h"
#include "Common/Network/IOContextPool.h"
#include "Common/Network/MessageDispatcher.h"
#include "Common/Network/PacketHeader.h"
#include "Common/Network/TCPAcceptor.h"
#include "World/CenterClient.h"
#include "World/GateConnection.h"
#include "World/Handler/EnterWorldHandler.h"
#include "World/Handler/MoveHandler.h"
#include "World/LogicThread.h"
#include "World/ScriptDispatchRegistry.h"
#include "World/System/System.h"
#include "World/WorldConfig.h"
#include "World/WorldSession.h"

namespace MMO
{

    class WorldServer
    {
    public:
        bool Init(const WorldConfig &cfg);
        void Run();
        void Stop();

        /**
         * @brief 发送已序列化的 protobuf body 到客户端（供脚本桥接调用）
         *
         * 与 SendToClient<TMsg> 不同，数据已经序列化为 protobuf bytes，
         * 跳过 SerializeToArray 步骤，直接加密 + 组帧出站。
         *
         * @warning 必须在 LogicThread 中调用（独占 _sessions 读写权限）
         * @param sessionID  目标 Session
         * @param msgID      消息 ID（EMsgID）
         * @param data       protobuf 序列化后的字节数组
         * @param len        字节长度
         */
        void SendRawToClient(uint32 sessionID, uint32 msgID,
                            const uint8 *data, size_t len);

        /**
         * @brief 获取脚本 Context（供 *.gen.cpp 中的 Dispatch 函数使用）
         */
        das::Context *GetScriptContext() const { return _scriptCtx.get(); }

        /**
         * @brief 获取 dispatch_msg 函数缓存（供 *.gen.cpp 中的 Dispatch 函数使用）
         */
        das::SimFunction *GetDispatchMsgFunction() const { return _fnDispatchMsg; }

    private:
        // ── Init 阶段 ──
        bool InitCenterClient(const WorldConfig &cfg);
        bool InitGateAcceptor(const WorldConfig &cfg);

        // ── 消息分发注册 ──
        void RegisterHandlers();

        // ── LogicThread 回调 ──
        void OnTick(std::chrono::milliseconds elapsed);
        void OnMessage(uint32 sessionID, WorldSession &ws, const LogicMessage &msg);
        void OnPreProcess();
        void OnPostFlush();

        // ── Gate 控制消息处理 ──
        void OnControlMessage(uint32 ctrlMsgID, const uint8 *data, size_t len);
        void OnDisconnectNtf(uint32 sessionID);
        void OnSessionRebindReq(const uint8 *data, size_t len);

        // ── 断线超时 ──
        void OnDisconnectTimeout(uint32 accountID);

        /**
         * @brief 网络复制：消费 AOI 可见集 → 差量同步 → SendToClient
         * @param scene      目标场景
         * @param dt         帧间隔
         * @param visibleSets  AOI 计算结果（playerEID → VisibleSet）
         */
        void SystemReplicate(ECS::Scene &scene, float dt,
                             const std::unordered_map<uint32_t, VisibleSet> &visibleSets);

        // ── Center 通知 ──
        void NotifyCenterPlayerOnline(uint32 accountID);
        void NotifyCenterPlayerOffline(uint32 accountID);

        // ── 过载保护 ──
        enum class ELoadLevel : uint8
        {
            NORMAL,
            WARNING,
            DEGRADED
        };
        void       UpdateLoadLevel(size_t sessionCount, size_t pendingMessages);
        void       ApplyLoadLevel(ELoadLevel oldLevel, ELoadLevel newLevel);
        ELoadLevel _loadLevel = ELoadLevel::NORMAL;

        // ── 未路由消息处理（EnterWorldReq Fallback）──
        void ProcessUnroutedMessages();

        // ── 控制消息处理（_ctrlQueue 消费）──
        void ProcessControlMessages();

        // ── 脚本引擎（Phase 2）──

        /**
         * @brief 初始化 DasLang 脚本引擎
         *
         * Phase 2: 创建 Context + MassiveModule(15 函数) + 编译 ServerTick.das
         * @return 成功返回 true
         */
        bool InitScriptEngine();

        /**
         * @brief 编译 DasLang 入口脚本
         * @param entryFile  入口 .das 文件路径
         * @return 编译成功的 Program；失败返回 nullptr
         */
        das::ProgramPtr CompileDaScript(const std::string &entryFile,
                                            das::ModuleGroup &libGroup);

        /**
         * @brief 加密 protobuf 消息并发送到客户端
         * @warning 必须在 LogicThread 中调用（独占 _sessions 读写权限）
         * @tparam TMsg protobuf 消息类型
         * @param sessionID  目标 Session
         * @param msgID      消息 ID（EMsgID）
         * @param msg        消息
         */
        template <typename TMsg>
        void SendToClient(uint32 sessionID, uint32 msgID, const TMsg &msg)
        {
            // 零分配序列化：ByteSizeLong → ByteBuffer::Own → SerializeToArray
            size_t bodySize = static_cast<size_t>(msg.ByteSizeLong());
            auto   buf      = ByteBuffer::Own(bodySize);
            bool   ok       = msg.SerializeToArray(buf.WritePtr(), static_cast<int>(bodySize));
            if (!ok)
            {
                Log::Error("SendToClient: SerializeToArray failed session={} msgID={}", sessionID, msgID);
                return;
            }
            buf.SetWritePos(bodySize);

            auto it = _sessions.find(sessionID);
            if (it == _sessions.end())
            {
                Log::Warn("SendToClient: session {} not found", sessionID);
                return;
            }

            // 加密 = [Seq:4B][Ciphertext+Tag]
            auto encrypted = it->second.crypto.Encrypt(buf.Data(), buf.Size());
            if (encrypted.Size() == 0)
            {
                return;
            }

            // 构建完整包: [PacketHeader:12B][encrypted]
            // PacketHeader = {length, msgID, sessionID}，全大端
            uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + encrypted.Size());
            auto   frame    = ByteBuffer::Own(totalLen);
            frame.WriteUint32(totalLen); // PacketHeader.length
            frame.WriteUint32(msgID);
            frame.WriteUint32(sessionID);
            frame.WriteBytes(encrypted.Data(), encrypted.Size());

            _gateConnMgr->SendToGate(it->second.gateServerID, sessionID, std::move(frame));
        }

        // ── 消息分发（按 msgID 查表）──
        MessageDispatcher<uint32> _dispatcher; // context = sessionID

        // ── 组件 ──
        std::unique_ptr<IOContextPool>     _ioPool;
        std::unique_ptr<TCPAcceptor>       _gateAcceptor;
        std::unique_ptr<CenterClient>      _centerClient;
        std::unique_ptr<GateConnectionMgr> _gateConnMgr;
        LogicThread                        _logicThread;

        // ── Session 存储（IO 线程读锁 + LogicThread 独占写）──
        std::shared_mutex                        _sessionsMtx;
        std::unordered_map<uint32, WorldSession> _sessions;

        // ── 场景 ──
        SceneManager _sceneMgr;

        // ── 过载统计 ──
        size_t _prevQueueDepth = 0;

        // ── 配置 ──
        WorldConfig       _config;
        std::atomic<bool> _running {false};

        // ── 脚本引擎（Phase 2）──
        std::shared_ptr<das::Context>     _scriptCtx;       // DasLang 执行上下文
        das::ProgramPtr                   _scriptProgram;  // 当前编译的脚本 Program
        das::SimFunction                 *_fnInit   = nullptr; // 脚本 init() 函数
        das::SimFunction                 *_fnUpdate = nullptr; // 脚本 update() 函数
        das::SimFunction                 *_fnDispatchMsg = nullptr; // 脚本 dispatch_msg() 函数
        std::unique_ptr<MassiveModule>    _massiveModule;   // 桥接模块（持有 WorldServer raw ptr）

        /// CodeReview #3: 自适应 GC 的堆大小基线（GC 后更新）
        uint64_t _lastGCHeapSize = 0;

        // ── 网络复制（Phase 5）──
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> _aoiStates; // playerEID → 上帧可见 entity
    };

} // namespace MMO
