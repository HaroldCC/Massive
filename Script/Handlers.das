// Handlers.das — 消息处理业务逻辑
// 所属: WorldServer
// Phase 3 (23_ProtoScriptBinding.md): [msg_handler] 注解 + protobuf 消息直接绑定

options gen2
options indenting = 4

require AutoGen/HandlerRegistry public
require massive

// ── MoveReq ──

[msg_handler(msg="MoveReq")]
def handle_move(sessionID : uint; req : MoveReq)
{
    if (req.speed < 0.0 || req.speed > 50.0) {
        LogWarn("handle_move: invalid speed={req.speed} session={sessionID}")
        return
    }

    let fullEid = FindEntityBySession(sessionID)
    if (fullEid == uint64(0)) {
        return
    }

    LogInfo("handle_move: seq={req.sequence} pos=({req.position.x},{req.position.y},{req.position.z})")
}

// ── HeartbeatReq —— 简单回包，脚本处理 ──

[msg_handler(msg="HeartbeatReq")]
def handle_heartbeat(sessionID : uint; req : HeartbeatReq)
{
    // 心跳仅记录，不需要业务逻辑
    LogInfo("handle_heartbeat: session={sessionID} client_time={req.client_time}")
}

// ── LoginAuthReq —— 含 string/bytes 字段验证 ──

[msg_handler(msg="LoginAuthReq")]
def handle_login_auth(sessionID : uint; req : LoginAuthReq)
{
    // username / password 都是 addExternProperty 注册的属性——不带括号，
    // 与标量字段语法完全一致
    let user = req.username
    let pw   = req.password
    LogInfo("handle_login_auth: session={sessionID} user={user}")
}

// ── LoginEnterWorldReq ──

[msg_handler(msg="LoginEnterWorldReq")]
def handle_login_enter_world(sessionID : uint; req : LoginEnterWorldReq)
{
    LogInfo("handle_login_enter_world: session={sessionID} nonce={req.nonce}")
}
