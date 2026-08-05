// Script/World/main.das — World 服务脚本入口
//
// 链路：
//   C++ Dispatch<Msg>Req → ctx->evalWithCatch(DispatchMsg, {msgID, sessionID, &req})
//   → DispatchMsg（require 的 MsgHandlerRegistry 模块导出）查 g_HandlerRegistry
//   → [msg_handler] 注册的 handler
//
// 注意：
//   - require Common（C++ DasCommonModule：LogInfo 等）
//   - require ../Common/MsgHandlerRegistry.das（手写宏模块，提供 DispatchMsg + [msg_handler]；
//     带 .das 后缀才是文件路径 require，否则按模块名解析找不到）
//   - 业务 handler 用 [msg_handler] 注解后，宏在 [init] 阶段自动填充分发表

options gen2
options indenting = 4

require Common
require world           // 服务专用 C++ 模块（WorldScriptModule）：MoveReq 等消息类型 + EMsgID
require ../Common/MsgHandlerRegistry.das
require ../Common/GameEventRegistry.das
require ../Common/GameSystemRegistry.das

// ── handler 参考示例：展示 [msg_handler] 宏用法 ──
// 宏在编译期从 req 类型推导 msgID（MoveReq → MSG_MOVE_REQ），并在 [init] 注入注册代码。
// 真实业务 handler 按同一模式编写；消息会优先进脚本分发（C++ 同 msgID 的 handler 被旁路）。
[msg_handler]
def handle_move_test(sessionID : uint; req : MoveReq)
{
    LogInfo("handle_move_test: session={sessionID} seq={req.sequence} speed={req.speed}")
}

// ── [game_event] 示例：类型化事件分发（ECS_06 决策 3）──
// 宏从参数类型推导事件枚举（EntityDamagedEvent → GAME_EVENT_ENTITY_DAMAGED），
// C++ 侧 EmitEntityDamaged(...) 产生事件 → 这里收到并处理。
[game_event]
def EntityDamaged(ev : EntityDamagedEvent)
{
    LogInfo("game_event: EntityDamaged target={ev.target_id} attacker={ev.attacker_id} dmg={ev.damage}")
}

[game_event]
def EntityDied(ev : EntityDiedEvent)
{
    LogInfo("game_event: EntityDied entity={ev.entity_id}")
}

// ── [game_system] 示例：低频决策（每 1 秒跑一次）──
[game_system(interval = 1.0)]
def ai_tick(dt : float)
{
    // Bridge 冒烟：创建实体 → 设位置 → 查回（验证脚本↔C++ 实体操作）
    // eid 是强类型 EntityID（与 sessionID/accountID 强区分）——{eid} 可直接格式化（十六进制）
    let eid = EntityCreate()
    if (EntityIsValid(eid))
    {
        EntitySetPosition(eid, float3(1.5, 2.5, 3.5))
        let pos = EntityGetPosition(eid)
        LogInfo("game_system: ai_tick dt={dt} eid={eid} pos=({pos.x},{pos.y},{pos.z})")
    }
}

[export]
def Init()
{
    LogInfo("Script Init called (World)")
}

[export]
def Update()
{
}
