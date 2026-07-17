// ServerTick.das — 顶层 Tick 调度入口
// 所属: WorldServer
// Phase 1.4: DECS smoke test

options gen2
options indenting = 4

require daslib/decs_boost
require massive

[export]
def Init()
{
    print("▌▌▌ DECS SMOKE TEST ▐▐▐\n")
    LogInfo("DECS smoke test: init OK")

    // 最小 DECS 验证：create_entity 是 deferred 的，必须 commit()
    let eid = create_entity() <| @(eid, cmp)
    {
        LogInfo("DECS: entity created id={eid.id} gen={eid.generation}")
    }
    LogInfo("DECS: eid reserved, id={eid.id}")

    // commit() 才真正执行所有 deferred action（create/delete/set）
    commit()
    LogInfo("DECS: commit done, entity is now alive")
}

[export]
def Update(sceneID: uint, dt: float)
{
    LogInfo("tick: {sceneID}  {dt}")
}
