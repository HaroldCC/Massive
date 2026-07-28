--- a/Script/Tests/Test_EntitiesInRadius.das
+++ b/Script/Tests/Test_EntitiesInRadius.das
@@
+// Test_EntitiesInRadius.das — 最小集成测试，验证 EntitiesInRadius 行为
+
+require massive
+
+def RunTest()
+{
+    print("Running EntitiesInRadius test...\n")
+
+    let id1 = CreateEntity(float3(0.0,0.0,0.0), 1)
+    let id2 = CreateEntity(float3(10.0,0.0,0.0), 1)
+
+    let arr = EntitiesInRadius(float3(0.0,0.0,0.0), 5.0)
+
+    if (arr.size() != 1u)
+    {
+        print("Test FAILED: expected 1, got {arr.size()}\n")
+        return
+    }
+
+    print("Test PASSED: EntitiesInRadius returns 1 as expected\n")
+}
+
+RunTest()
+
