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

// ── handler 参考示例：展示 [msg_handler] 宏用法 ──
// 宏在编译期从 req 类型推导 msgID（MoveReq → MSG_MOVE_REQ），并在 [init] 注入注册代码。
// 真实业务 handler 按同一模式编写；消息会优先进脚本分发（C++ 同 msgID 的 handler 被旁路）。
[msg_handler]
def handle_move_test(sessionID : uint; req : MoveReq)
{
    LogInfo("handle_move_test: session={sessionID} seq={req.sequence} speed={req.speed}")
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