# 身份系统（ECS_01）

> 契约见 `ECS_00_总纲与契约.md` §4.1（uint64 位布局）。本篇给出**可照抄实现**：
> `EntityID.h` → `EntityRegistry` → `Scene` 重构 → `WorldSession` 归位。
> 交付后：`WorldServer` 编译通过，EnterWorld 创建实体走新身份系统。

---

## 1. 交付清单

| # | 文件 | 动作 |
|---|---|---|
| 1 | `Src/Common/ECS/EntityID.h` | 新建（uint64 契约） |
| 2 | `Src/Common/ECS/EntityRegistry.h` | 新建（index+version 自管） |
| 3 | `Src/Common/ECS/EntityRegistry.cpp` | 新建 |
| 4 | `Src/Common/ECS/Scene.h` | 重写（持有 EntityRegistry + entt::registry） |
| 5 | `Src/Common/ECS/Scene.cpp` | 重写 |
| 6 | `Src/World/WorldSession.h` | 改（`Entity` → `uint64 entityID`） |
| 7 | `Src/World/Handler/EnterWorldHandler.h/.cpp` | 重建（走新 Scene API） |
| 8 | `Src/World/WorldServer.cpp` | 改（接线 EnterWorld + 断线清理） |
| 9 | `Src/World/xmake.lua` | 改（Handler 目录回归） |
| 10 | `Src/Common/ECS/xmake.lua` | 改（去 libDaScript 依赖） |

---

## 2. `EntityID.h` — uint64 身份契约

```cpp
// Src/Common/ECS/EntityID.h
#pragma once

#include <format>
#include <functional>

#include "Common/Core/Types.h"

namespace MMO::ECS
{

    /**
     * @brief 强类型实体 ID（uint64 位布局：scene(16) | index(28) | version(20)）
     *
     * 为什么用 struct 而非裸 uint64 / enum class（2026-08-03 拍板）：
     *   1. 与普通整数强区分（杜绝把 entityID 当 sessionID/accountID 用）——
     *      类比 EnTT 的 enum class entity。
     *   2. struct 比 enum class 灵活：可内置位运算封装（MakeEntityID/SceneOf/...），
     *      调用方无需到处 cast；enum class 做位运算必须 static_cast。
     *   3. 隐式 uint64 转换 + std::hash/std::formatter 特化：跨语言/跨协议边界
     *      （protobuf/脚本/日志）零摩擦传递，同时保留强类型语义。
     *
     * 位布局（ECS_00 §4.1 契约，不可改动）：
     *   63          48 47                      20 19                  0
     *  ┌──────────────┬──────────────────────────┬────────────────────┐
     *  │  scene (16)  │        index (28)         │    version (20)    │
     *  └──────────────┴──────────────────────────┴────────────────────┘
     */
    struct EntityID
    {
        uint64 raw = 0;  // 裸值（跨语言/序列化/比较用）

        constexpr EntityID() = default;
        constexpr EntityID(uint64 value) : raw(value) {}

        constexpr bool operator==(const EntityID &other) const { return raw == other.raw; }
        constexpr bool operator!=(const EntityID &other) const { return raw != other.raw; }
        constexpr explicit operator bool() const { return raw != 0; }
        constexpr operator uint64() const { return raw; }

        /** @brief 无效实体 ID */
        static constexpr EntityID Invalid() { return EntityID(0); }
    };

    // scene(16) | index(28) | version(20)
    inline constexpr uint64 kSceneBits   = 16;
    inline constexpr uint64 kIndexBits   = 28;
    inline constexpr uint64 kVersionBits = 20;

    inline constexpr uint64 kSceneShift  = kIndexBits + kVersionBits;       // 48
    inline constexpr uint64 kIndexShift  = kVersionBits;                    // 20
    inline constexpr uint64 kVersionMask = (uint64(1) << kVersionBits) - 1; // 0xFFFFF
    inline constexpr uint64 kIndexMask   = (uint64(1) << kIndexBits) - 1;   // 0xFFFFFFF
    inline constexpr uint64 kSceneMask   = (uint64(1) << kSceneBits) - 1;   // 0xFFFF

    /** @brief 无效实体哨兵 */
    inline constexpr EntityID kInvalidEntityID = EntityID::Invalid();

    /** @brief 组装 EntityID */
    inline constexpr EntityID MakeEntityID(uint16 scene, uint32 index, uint32 version)
    {
        return EntityID((uint64(scene) << kSceneShift)
                        | (uint64(index & kIndexMask) << kIndexShift)
                        | (uint64(version) & kVersionMask));
    }

    /** @brief 取 scene 段 */
    inline constexpr uint16 SceneOf(EntityID id) { return uint16((id.raw >> kSceneShift) & kSceneMask); }
    /** @brief 取 index 段 */
    inline constexpr uint32 IndexOf(EntityID id) { return uint32((id.raw >> kIndexShift) & kIndexMask); }
    /** @brief 取 version 段 */
    inline constexpr uint32 VersionOf(EntityID id) { return uint32(id.raw & kVersionMask); }

    inline constexpr bool IsValidEntity(EntityID id) { return id.raw != 0; }

} // namespace MMO::ECS

// std::hash / std::formatter 特化（见实际 EntityID.h 实现）——
//   · std::hash<EntityID>：哈希 raw（unordered_map/set 可用）
//   · std::formatter<EntityID>：Log::Info("{}", eid) 打印 raw 值
```

**为什么不直接暴露 EnTT 的 version？** EnTT 3.16 `entt_traits<uint32>` 是 entity 20 位 + version 12 位。
我们的 `EntityID` 用 28+20，且**网络/DB/脚本传的是 uint64 EntityID**，EnTT 的 `entt::entity` 只作为
registry 内部句柄。`EntityRegistry` 负责两者映射 + version 校验，杜绝悬垂句柄。

**强类型 EntityID 的跨边界规则**（2026-08-03 拍板）：
1. C++ 内部：一律 `ECS::EntityID` 强类型，禁止裸 `uint64`/`uint32` 混用（杜绝
   entityID 被当 sessionID/accountID 用）
2. 网络/DB/脚本边界：`static_cast<uint64>(eid)`（隐式转换）进出——protobuf 字段是 uint64
3. 日志：`Log::Info("{}", eid)` 直接用（std::formatter 特化）
4. 容器键：`unordered_map<EntityID, T>` 直接用（std::hash 特化）

### 2.1 `EntityIndex` — 实体内部索引（EnTT index）

```cpp
/**
 * @brief 实体内部索引（EnTT registry 内下标，32 位）
 *
 * 与 EntityID 的关系：
 *   - EntityIndex == EntityID 的 index 字段（低 28 位），二者对齐
 *   - 但 EntityIndex 缺 scene/version，不是对外身份
 *
 * 用途：Grid 格子 / DirtyIndex 脏标记 / SystemAOI 候选集——每帧热路径，
 * 只存 32 位 index 省内存 + 少位运算；且这些容器本就是 per-scene 的。
 *
 * 为什么单独一个类型：与 EntityID（对外身份）和普通 uint32（sessionID 等）
 * 在类型层面区分，杜绝混用。
 */
struct EntityIndex
{
    uint32 raw = 0;

    constexpr EntityIndex() = default;
    constexpr EntityIndex(uint32 value) : raw(value) {}

    constexpr bool operator==(const EntityIndex &other) const { return raw == other.raw; }
    constexpr bool operator!=(const EntityIndex &other) const { return raw != other.raw; }
    constexpr explicit operator bool() const { return raw != 0; }
    constexpr operator uint32() const { return raw; }

    static constexpr EntityIndex Invalid() { return EntityIndex(0xFFFFFFFFu); }
};
// + std::hash / std::formatter 特化（同 EntityID）
```

**EntityID vs EntityIndex 使用场景**（2026-08-04 拍板）：

| | `EntityID`（64 位强类型） | `EntityIndex`（32 位强类型） |
|---|---|---|
| 语义 | 对外身份（scene\|index\|version） | 对内热路径索引（EnTT index） |
| 使用方 | session/脚本/网络/组件 API | Grid/DirtyIndex/SystemAOI/复制 |
| 转换 | `IndexOf(eid)` → EntityIndex | `MakeEntityID(scene, idx, ver)` → EntityID |
| 越界风险 | version 校验防悬垂 | 帧内有效，随 registry 生命周期 |

**转换规则**：
- `EntityID → EntityIndex`：`EntityIndex(IndexOf(eid))`
- `EntityIndex → entt::entity`：`entt::entity(static_cast<entt::id_type>(idx.raw))`
- `entt::entity → EntityIndex`：`EntityIndex(static_cast<uint32>(entt::to_integral(e)))`

---

## 3. `EntityRegistry` — index+version 自管

```cpp
// Src/Common/ECS/EntityRegistry.h
#pragma once

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "Common/ECS/EntityID.h"

namespace MMO::ECS
{

    /**
     * @brief EntityID ↔ entt::entity 映射 + version 自管回收
     *
     * - 连续数组 _versions[index] = 当前 version（0 表示槽位空闲）
     * - 空闲槽位链表 _freeList 复用（LIFO，cache 友好）
     * - version 溢出回绕安全：version 从 1 递增，0 保留给空闲槽
     */
    class EntityRegistry
    {
    public:
        EntityRegistry() = default;
        explicit EntityRegistry(uint16 sceneId) : _sceneId(sceneId) {}

        // ── 生命周期 ──

        /**
         * @brief 创建实体
         * @return EntityID（强类型，scene|index|version）
         */
        EntityID Create();

        /**
         * @brief 销毁实体（version++，槽位进 freeList）
         * @param id EntityID
         * @return true 销毁成功；false 无效/已销毁/版本不匹配
         */
        bool Destroy(EntityID id);

        /**
         * @brief 校验 EntityID 是否有效（index 越界 / 版本不匹配 → 无效）
         */
        bool IsValid(EntityID id) const;

        /**
         * @brief 解析 EntityID → EnTT 内部句柄
         * @return entt::entity；无效返回 entt::null
         */
        entt::entity Resolve(uint64 id) const;

        /**
         * @brief 由 EnTT 句柄反查 EntityID（无版本校验，内部用）
         */
        uint64 ToEntityID(entt::entity e) const;

        uint16 SceneId() const { return _sceneId; }
        uint32 Count() const { return _aliveCount; }

    private:
        uint16               _sceneId   = 0;
        std::vector<uint32>  _versions;   // index → version（0 = 空闲）
        std::vector<entt::entity> _enttByIdx; // index → entt::entity（配套对齐）
        std::vector<uint32>  _freeList;   // 空闲 index 栈（LIFO）
        uint32               _aliveCount = 0;
    };

} // namespace MMO::ECS
```

```cpp
// Src/Common/ECS/EntityRegistry.cpp
#include "Common/ECS/EntityRegistry.h"

namespace MMO::ECS
{

    uint64 EntityRegistry::Create()
    {
        uint32 index = 0;
        entt::entity e;

        if (!_freeList.empty())
        {
            index = _freeList.back();
            _freeList.pop_back();
            e = _enttByIdx[index];
            // 槽位复用：version 递增（0 保留给空闲槽，故先自增再取）
            const uint32 newVersion = _versions[index] + 1;
            _versions[index] = (newVersion & kVersionMask) == 0 ? 1 : newVersion; // 回绕保护
        }
        else
        {
            index = static_cast<uint32>(_versions.size());
            _versions.push_back(1);
            e = entt::entity(static_cast<entt::id_type>(index));
            _enttByIdx.push_back(e);
        }

        ++_aliveCount;
        return MakeEntityID(_sceneId, index, _versions[index]);
    }

    bool EntityRegistry::Destroy(uint64 id)
    {
        if (!IsValid(id))
        {
            return false;
        }

        const uint32 index = IndexOf(id);
        // version 递增标记销毁，槽位进 freeList
        _versions[index] = (_versions[index] + 1) & kVersionMask;
        _freeList.push_back(index);
        --_aliveCount;
        return true;
    }

    bool EntityRegistry::IsValid(uint64 id) const
    {
        if (!IsValidEntity(id) || SceneOf(id) != _sceneId)
        {
            return false;
        }

        const uint32 index = IndexOf(id);
        if (index >= _versions.size())
        {
            return false;
        }

        // 槽位空闲（version==0）或版本不匹配 → 无效
        return _versions[index] != 0 && _versions[index] == VersionOf(id);
    }

    entt::entity EntityRegistry::Resolve(uint64 id) const
    {
        if (!IsValid(id))
        {
            return entt::null;
        }
        return _enttByIdx[IndexOf(id)];
    }

    uint64 EntityRegistry::ToEntityID(entt::entity e) const
    {
        const uint32 index = static_cast<uint32>(entt::to_integral(e));
        if (index >= _versions.size() || _versions[index] == 0)
        {
            return kInvalidEntityID;
        }
        return MakeEntityID(_sceneId, index, _versions[index]);
    }

} // namespace MMO::ECS
```

> **注意**：EnTT 的 `entt::entity(index)` 构造——`entt::entity` 是 `enum class entity : id_type`，
> 直接用整数构造是合法的（`entt::entity{index}`）。`entt::to_integral(e)` 返回 `id_type`。
> 我们**不用 EnTT 自己的 version**（它是 12 位），entity 的版本位永远为 0，真正的版本在
> `_versions` 数组里。

---

## 4. `Scene` — 组件容器 + 身份注册表

```cpp
// Src/Common/ECS/Scene.h
#pragma once

#include <entt/entt.hpp>

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"
#include "Common/ECS/EntityRegistry.h"

namespace MMO::ECS
{

    /**
     * @brief 场景——entt::registry（组件数据唯一所有者）+ EntityRegistry（身份映射）
     *
     * 铁律 1：组件数据永远在 entt::registry，脚本只持有 uint64 EntityID。
     */
    class Scene
    {
    public:
        explicit Scene(uint16 sceneId);

        Scene(const Scene &)            = delete;
        Scene &operator=(const Scene &) = delete;
        Scene(Scene &&)                 = delete;
        Scene &operator=(Scene &&)      = delete;

        ~Scene() = default;

        uint16 SceneId() const { return _sceneId; }

        // ── 实体生命周期 ──

        uint64 CreateEntity();
        bool   DestroyEntity(uint64 entityID);
        bool   IsValid(uint64 entityID) const;
        entt::entity Resolve(uint64 entityID) const { return _registry.Resolve(entityID); }
        uint64 ToEntityID(entt::entity e) const { return _registry.ToEntityID(e); }

        // ── EnTT 组件操作（组件数据唯一入口）──

        template <typename T>
        T &GetComponent(uint64 entityID)
        {
            return _entt.get<T>(_registry.Resolve(entityID));
        }

        template <typename T>
        const T &GetComponent(uint64 entityID) const
        {
            return _entt.get<T>(_registry.Resolve(entityID));
        }

        template <typename T>
        bool HasComponent(uint64 entityID) const
        {
            return _entt.all_of<T>(_registry.Resolve(entityID));
        }

        template <typename T, typename... Args>
        T &EmplaceComponent(uint64 entityID, Args &&...args)
        {
            return _entt.emplace<T>(_registry.Resolve(entityID), std::forward<Args>(args)...);
        }

        template <typename T>
        void RemoveComponent(uint64 entityID)
        {
            _entt.remove<T>(_registry.Resolve(entityID));
        }

        // ── EnTT registry 直通（C++ 系统用）──

        entt::registry &Registry() { return _entt; }
        const entt::registry &Registry() const { return _entt; }

        EntityRegistry &Identities() { return _registry; }
        const EntityRegistry &Identities() const { return _registry; }

    private:
        uint16          _sceneId;
        EntityRegistry  _registry;
        entt::registry  _entt;
    };

} // namespace MMO::ECS
```

```cpp
// Src/Common/ECS/Scene.cpp
#include "Common/ECS/Scene.h"

namespace MMO::ECS
{

    Scene::Scene(uint16 sceneId) : _sceneId(sceneId), _registry(sceneId)
    {
    }

    uint64 Scene::CreateEntity()
    {
        return _registry.Create();
    }

    bool Scene::DestroyEntity(uint64 entityID)
    {
        // 先销毁 EnTT 组件（触发析构），再回收身份槽
        auto e = _registry.Resolve(entityID);
        if (e == entt::null)
        {
            return false;
        }
        _entt.destroy(e);
        return _registry.Destroy(entityID);
    }

} // namespace MMO::ECS
```

> **要点**：`CreateEntity` 只分配身份，**不自动加组件**——组件由业务层按需 emplace。
> `DestroyEntity` 先 `_entt.destroy`（清理组件）再回收身份槽，顺序不可颠倒。

---

## 5. `WorldSession` 归位

```cpp
// Src/World/WorldSession.h（改动）
#pragma once

#include <chrono>

#include "Common/Core/Types.h"
#include "Common/ECS/EntityID.h"
#include "Common/Network/CryptoSession.h"
#include "Common/Queue/LogicMessage.h"
#include "Common/Queue/MPSCQueue.h"

namespace MMO
{

    struct WorldSession
    {
        uint32        sessionID = 0;    // Gate 分配的 sessionID
        uint32        accountID = 0;    // 玩家账号 ID
        uint64        entityID  = 0;    // World 侧玩家实体（uint64 EntityID，0 = 未入世界）
        CryptoSession crypto;           // AES-256-GCM 加解密上下文
        uint16        gateServerID = 0; // 当前连接的 Gate 实例 ID
        uint32        gateConnIdx  = 0; // 对应 Gate 连接在 GateConnectionMgr 中的索引

        MPSCQueue<LogicMessage> inbox;

        std::chrono::steady_clock::time_point lastRecvTime;
        bool                                  disconnected = false;
    };

} // namespace MMO
```

---

## 6. `EnterWorldHandler` 重建

> 从 `Handler/` 重建。逻辑：SessionToken 验证（复用既有代码）→ `Scene::CreateEntity()` → 写 `WorldSession`。

```cpp
// Src/World/Handler/EnterWorldHandler.h
#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/ECS/Scene.h"
#include "World/WorldSession.h"

namespace MMO
{

    using GateSendFn = std::function<void(uint32 sessionID, uint32 msgID, ByteBuffer body)>;

    class EnterWorldHandler
    {
    public:
        /**
         * @brief 处理 EnterWorldReq
         * @param sessionID    Gate sessionId
         * @param body         protobuf 请求体
         * @param len          长度
         * @param sessions     会话表（IO 线程读锁外，此处由 LogicThread 独占）
         * @param lss          LoginServerSecret（SessionToken 验证）
         * @param gateServerID Gate 实例 ID
         * @param defaultScene 默认场景（实体创建目标）
         * @param gateSendFn   出站回调
         */
        static void Handle(uint32                                    sessionID,
                           const uint8                              *body,
                           size_t                                    len,
                           std::unordered_map<uint32, WorldSession> &sessions,
                           const uint8                              *lss,
                           uint16                                    gateServerID,
                           ECS::Scene                               &defaultScene,
                           GateSendFn                                gateSendFn);
    };

} // namespace MMO
```

```cpp
// Src/World/Handler/EnterWorldHandler.cpp
#include "World/Handler/EnterWorldHandler.h"

#include "Common/Crypto/SessionToken.h"
#include "Common/Log/Log.h"

#include <Login.pb.h>
#include <MsgID.pb.h>

namespace MMO
{

    void EnterWorldHandler::Handle(uint32                                    sessionID,
                                   const uint8                              *body,
                                   size_t                                    len,
                                   std::unordered_map<uint32, WorldSession> &sessions,
                                   const uint8                              *lss,
                                   uint16                                    gateServerID,
                                   ECS::Scene                               &defaultScene,
                                   GateSendFn                                gateSendFn)
    {
        Proto::LoginEnterWorldReq req;
        if (!req.ParseFromArray(body, static_cast<int>(len)))
        {
            Log::Warn("EnterWorld: parse failed ({} bytes)", len);
            return;
        }

        // SessionToken 验证（复用既有流程）
        if (static_cast<size_t>(req.session_token().size()) != Crypto::SessionToken::kTotalSize)
        {
            Log::Warn("EnterWorld: invalid token size={}", req.session_token().size());
            return;
        }
        auto tokenOpt =
            Crypto::SessionToken::FromBuffer(reinterpret_cast<const uint8 *>(req.session_token().data()),
                                             static_cast<size_t>(req.session_token().size()));
        if (!tokenOpt)
        {
            return;
        }
        auto payloadOpt = Crypto::SessionTokenBuilder::Verify(lss, *tokenOpt);
        if (!payloadOpt)
        {
            Log::Debug("EnterWorld: token verify failed");
            return;
        }

        const uint32 accountID = payloadOpt->accountId;
        const uint64 clientRandom = req.nonce();

        // 创建实体（仅身份，无组件——组件由 ECS_02 的业务层按需添加）
        const uint64 entityID = defaultScene.CreateEntity();

        CryptoSession crypto;
        crypto.Init(payloadOpt->sessionKey.Data(), clientRandom);

        WorldSession ws;
        ws.sessionID    = sessionID;
        ws.accountID    = accountID;
        ws.entityID     = entityID;
        ws.crypto       = std::move(crypto);
        ws.gateServerID = gateServerID;
        ws.lastRecvTime = std::chrono::steady_clock::now();
        ws.disconnected = false;

        sessions[sessionID] = std::move(ws);

        Log::Info("EnterWorld: accountID={} entityID={} sessionID={}",
                  accountID, entityID, sessionID);

        // 回包（位置由后续系统填充，此处 0）
        Proto::LoginEnterWorldRsp rsp;
        rsp.mutable_error()->set_code(0);
        rsp.set_player_id(static_cast<uint32>(entityID)); // 客户端用低 32 位即可，全量走复制
        rsp.set_scene_id(defaultScene.SceneId());
        rsp.mutable_position()->set_x(0.0f);
        rsp.mutable_position()->set_y(0.0f);
        rsp.mutable_position()->set_z(0.0f);

        size_t bodySize = static_cast<size_t>(rsp.ByteSizeLong());
        auto   buf      = ByteBuffer::Own(bodySize);
        rsp.SerializeToArray(buf.WritePtr(), static_cast<int>(bodySize));
        buf.SetWritePos(bodySize);
        gateSendFn(sessionID, Proto::MSG_LOGIN_ENTER_WORLD_RSP, std::move(buf));
    }

} // namespace MMO
```

> **注意**：`EntityID` 是 uint64，客户端 `player_id` 是 uint32 字段——MVP 先传低 32 位
> （`static_cast<uint32>(entityID)`），完整的 uint64 实体信息通过 `EntityReplicateNtf`（ECS_05）
> 下发。设计上允许，因为 `index` 28 位在低 32 位内。

---

## 7. `WorldServer.cpp` 接线改动

```cpp
// WorldServer.cpp — ProcessUnroutedMessages 恢复真实处理
void WorldServer::ProcessUnroutedMessages()
{
    auto &unrouted = _gateConnMgr->GetUnroutedQueue();

    std::vector<LogicMessage> batch;
    unrouted.DrainAll(batch);

    for (auto &msg : batch)
    {
        if (msg.msgID != Proto::MSG_LOGIN_ENTER_WORLD_REQ)
        {
            Log::Warn("WorldServer: unexpected unrouted msgID={}", msg.msgID);
            continue;
        }

        // 场景：默认场景（ECS_03 将引入 SceneManager 管理多场景）
        auto *scene = _sceneMgr.GetDefaultScene();
        if (!scene)
        {
            Log::Warn("WorldServer: no default scene for EnterWorld");
            continue;
        }

        std::unique_lock lock(_sessionsMtx);
        EnterWorldHandler::Handle(
            msg.sessionID,
            msg.body.Data(),
            msg.body.Size(),
            _sessions,
            _config.security.loginServerSecret,
            1, // gateID（MVP 固定）
            *scene,
            [this](uint32 sessionID, uint32 msgID, ByteBuffer rawBody) {
                // 出站：EnterWorldRsp 不走加密；其他走加密
                auto it = _sessions.find(sessionID);
                if (msgID == Proto::MSG_LOGIN_ENTER_WORLD_RSP)
                {
                    uint32 totalLen = static_cast<uint32>(sizeof(PacketHeader) + rawBody.Size());
                    ByteBuffer frame = ByteBuffer::Own(totalLen);
                    frame.WriteUint32(totalLen);
                    frame.WriteUint32(msgID);
                    frame.WriteUint32(sessionID);
                    frame.WriteBytes(rawBody.Data(), rawBody.Size());
                    _gateConnMgr->SendToGate(1, sessionID, std::move(frame));
                    return;
                }
                // 其他消息加密出站（复用既有逻辑，代码略）
                ...
            });
    }
}
```

---

## 8. 构建脚本变动

### 8.1 `Src/Common/ECS/xmake.lua`

```lua
--- @file xmake.lua
--- @brief CommonECS — ECS 基建（身份 + 场景 + 索引）

target("CommonECS")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    add_deps("CommonCore")
    add_deps("entt", {public = true})
```

> **移除 `libDaScript` 依赖**——CommonECS 是纯 C++ 层，不依赖脚本引擎（依赖方向铁律）。

### 8.2 `Src/World/xmake.lua`

```lua
target("WorldServer")
    set_kind("binary")
    if is_mode("release") then
        add_rules("Rules.das_aot", {service = "world", entry = "World/main.das"})
        add_includedirs("$(projectdir)/Src")
    end
    -- Handler 目录回归（EnterWorldHandler 重建后重新纳入）
    add_files("**.cpp", {excludes = {
        "DasModule/WorldScriptModule.cpp",
        "AutoGen/*.gen.cpp"
    }})
    add_headerfiles("*.h")
    add_deps(
        "CommonCore", "CommonDB", "CommonNetwork", "CommonQueue",
        "CommonCrypto", "CommonECS", "CommonTimer", "CommonConfig",
        "CommonLog", "Proto", "ScriptEngine", "ProtoScriptModule"
    )
    if is_mode("release") then
        add_deps("AotGen")
    end
    add_deps("asio", {public = true})
```

> `add_files("**.cpp")` 通配已覆盖 `Handler/`——删目录后重建无需改 xmake。

### 8.3 配置 `Config/world.toml`

```toml
[world]
id = 1
max_players = 10000
persistent_scenes = ["1"]
```

> `persistent_scenes` 含义从"场景参数"简化为"场景 ID 列表"（参数由 `SceneConfig` 统一管理，见 ECS_03）。

---

## 9. 验证步骤（本篇验收）

```powershell
# 1. 构建
xmake build WorldServer

# 2. 启动（需先启动 Center + Gate + Login，或用 ServerCtl）
#    单独验证脚本引擎 + 实体创建：
#    - 日志出现 "InitScriptEngine: OK"
#    - 无编译错误

# 3. 冒烟（可选）：TestClient 登录 → EnterWorld
#    预期：WorldServer 日志 "EnterWorld: accountID=X entityID=Y"
```

**验收标准**：
- [ ] `xmake build WorldServer` 零错误
- [ ] `EntityRegistry` 单测（`Tools/Tests/` 或临时 main）：Create/Destroy/IsValid 版本校验正确
- [ ] EnterWorld 创建实体后 `WorldSession.entityID != 0`
- [ ] 销毁后 `IsValid(entityID) == false`，同槽位重建 version 递增

---

## 10. 本篇注意点（踩坑预警）

1. **`entt::entity` 构造**：`entt::entity(index)` 用整数构造 enum class 是合法的，但**不要**把
   EnTT 的 version 位写进去（我们 version 自管，EnTT 位恒 0）。
2. **`entt::to_integral`** 返回 `entt::id_type`（uint32），需 cast 到 `uint32`。
3. **Destroy 顺序**：先 `_entt.destroy` 再 `_registry.Destroy`，不可颠倒（组件析构需要 entity 还活着）。
4. **`_enttByIdx` 与 `_versions` 必须同步 push**——两个数组按 index 对齐。
5. **version 回绕**：`(v+1) & kVersionMask == 0` 时重置为 1（0 保留给空闲槽语义）。
