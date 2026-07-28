--- a/Src/World/MassiveModule.cpp
+++ b/Src/World/MassiveModule.cpp
@@
-        ModuleLibrary lib(this);
-        lib.addBuiltInModule();
-        auto *builtin = Module::require("$");
-        if (builtin)
-        {
-            addBuiltinDependency(lib, builtin, true);
-        }
+        ModuleLibrary lib(this);
+        lib.addBuiltInModule();
+        auto *builtin = Module::require("$");
+        if (builtin)
+        {
+            addBuiltinDependency(lib, builtin, true);
+        }
+
+        // 注册额外的类型映射（BattleStats 等）
+        // 需要在 addExtern 之前调用，使得 daScript 能识别这些 C++ 类型
+        RegisterDasBindings(*this, lib);
@@
-        addExtern<DAS_BIND_FUN(Bridge_EntityPosition)>(*this, lib, "EntityPosition",
-                                                        SideEffects::none);
+        addExtern<DAS_BIND_FUN(Bridge_EntityPosition)>(*this, lib, "EntityPosition",
+                                                        SideEffects::none);
 
-        // EntitiesInRadius 需要显式接收 context 参数以便在宿主堆中分配 TArray
-        addExtern<DAS_BIND_FUN(Bridge_EntitiesInRadius)>(*this, lib, "EntitiesInRadius",
-                                                        SideEffects::none)
-            ->args({"center", "radius", "context"});
+        // EntitiesInRadius 需要显式接收 context 参数以便在宿主堆中分配 TArray
+        addExtern<DAS_BIND_FUN(Bridge_EntitiesInRadius)>(*this, lib, "EntitiesInRadius",
+                                                        SideEffects::none)
+            ->args({"center", "radius", "context"});
@@
-        // ── 3.2 属性查询（BattleStats 已映射到 Das 结构体）──
-        addExtern<DAS_BIND_FUN(Bridge_EntityGetBattleStats)>(*this, lib, "EntityGetBattleStats",
-                                                              SideEffects::none);
+        // ── 3.2 属性查询（BattleStats 已映射到 Das 结构体）──
+        addExtern<DAS_BIND_FUN(Bridge_EntityGetBattleStats)>(*this, lib, "EntityGetBattleStats",
+                                                              SideEffects::none);
*** End Patch