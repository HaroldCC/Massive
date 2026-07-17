// ServerTick.das — 顶层 Tick 调度入口
// 所属: WorldServer
// Phase 2: MassiveModule 桥接函数验证

options gen2
options indenting = 4

require massive

var testEntityId : uint64

[export]
def Init()
{
    print("PHASE 2 BRIDGE TEST\n")
    LogInfo("Phase 2: bridge test start")

    testEntityId = CreateEntity(float3(100.0, 0.0, 200.0), 1)
    LogInfo("CreateEntity: OK")

    let pos = EntityPosition(testEntityId)
    LogInfo("EntityPosition: OK")

    var isPlayer = EntityIsPlayer(testEntityId)
    var isMonster = EntityIsMonster(testEntityId)
    var isDead = EntityIsDead(testEntityId)
    LogInfo("IsPlayer={isPlayer} IsMonster={isMonster} IsDead={isDead}")

    LogInfo("GetDeltaTime: {GetDeltaTime()}s")
    LogInfo("Phase 2 bridge test done")
}

[export]
def Update(sceneID: uint, dt: float)
{
    var pos = EntityPosition(testEntityId)
    LogInfo("Update OK")
}
