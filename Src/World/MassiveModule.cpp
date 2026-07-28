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
*** End Patch