// ServerTick.das — 顶层 Tick 调度入口
// 所属: WorldServer
// Phase 3: 脚本 Tick + CPPSystems (MovementSystem)

options gen2
options indenting = 4

require massive

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
}

[export]
def Update(sceneID: uint, dt: float)
{
    // MovementSystem 在脚本 Tick 后运行 Position += Velocity × dt
    // 当前 entity 无 Velocity → Position 不变
    EntityPosition(testEntityId)
}
