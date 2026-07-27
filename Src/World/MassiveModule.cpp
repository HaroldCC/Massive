--- a/Src/World/MassiveModule.cpp
+++ b/Src/World/MassiveModule.cpp
@@
-        // ── 3.1 空间查询 ──
-        addExtern<DAS_BIND_FUN(Bridge_EntityPosition)>(*this, lib, "EntityPosition",
-                                                        SideEffects::none);
+        // ── 3.1 空间查询 ──
+        addExtern<DAS_BIND_FUN(Bridge_EntityPosition)>(*this, lib, "EntityPosition",
+                                                        SideEffects::none);
+
+        // EntitiesInRadius 需要显式接收 context 参数以便在宿主堆中分配 TArray
+        addExtern<DAS_BIND_FUN(Bridge_EntitiesInRadius)>(*this, lib, "EntitiesInRadius",
+                                                        SideEffects::none)
+            ->args({"center", "radius", "context"});
@@
-        // ── 3.2 属性查询（需 BattleStats 类型注册）──
-        // addExtern<DAS_BIND_FUN(Bridge_EntityGetBattleStats)>(*this, lib, "EntityGetBattleStats",
-        //                                                       SideEffects::none);
+        // ── 3.2 属性查询（BattleStats 已映射到 Das 结构体）──
+        addExtern<DAS_BIND_FUN(Bridge_EntityGetBattleStats)>(*this, lib, "EntityGetBattleStats",
+                                                              SideEffects::none);
*** End Patch
