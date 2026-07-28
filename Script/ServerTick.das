// ServerTick.das — 顶层 Tick 调度入口
// 所属: WorldServer
// Phase 3 (23_ProtoScriptBinding.md): 引入 Handlers.das 以注册 [msg_handler]

options gen2
options indenting = 4

require massive
require Handlers

var testEntityId : uint64

[export]
def Init()
{
    print("PHASE 3 CPPSYSTEMS TEST\n")
    LogInfo("Phase 3: CPPSystems smoke test")

    // EEntityType::Player = 1, EEntityType::Monster = 3
    testEntityId = CreateEntity(float3(100.0, 0.0, 200.0), 1)   // Player type
    LogInfo("CreateEntity: OK")

    let pos = EntityPosition(testEntityId)
    LogInfo("Init pos: OK")

    LogInfo("IsPlayer={EntityIsPlayer(testEntityId)}")
    LogInfo("IsDead={EntityIsDead(testEntityId)}")
    LogInfo("Phase 3 init done")

    // Phase 3: 校验 handler 注册完整性
    validate_handler_registry()
}

[export]
def Update(sceneID: uint, dt: float)
{
    // MovementSystem 在脚本 Tick 后运行 Position += Velocity × dt
    // 当前 entity 无 Velocity → Position 不变
    EntityPosition(testEntityId)
}
