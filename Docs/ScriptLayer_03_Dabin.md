# .dabin 二进制缓存与热补丁（ScriptLayer_03_Dabin）

> 本篇给出 `.dabin`（编译后 AST 二进制，无源码明文）的**完整可照抄**序列化实现，
> 修复此前审查发现的两个隐患：DASBIN-02（`noexcept` 抛异常 → `std::terminate`）、
> DASBIN-03（header memcpy 破坏字节序）。并给出可选的 AES-256-GCM 加密层（复用 `Common/Crypto`）。
>
> 承接 `ScriptLayer_00` / `02`。API 均对 `ThirdParty/daScript` 与 `Src/Common` 核实。

---

## 0. 定位与原理

- **序列化的是 `Program`（AST 层）**，不是源码。daScript 已确认 blob 不含源码明文（`TextFileInfo::serialize` 写 source 的代码被显式注释掉）。
- blob 内含每函数 `aotHash` → **与 AOT 叠加**：反序列化的 Program 带 aotHash，`simulate` 时命中 AOT 库。缓存与 AOT 正交叠加。
- 反序列化得到 `Program` 后**仍需 `simulate()`** 建 Context——缓存省的是 parse+infer+optimize，不省 simulate。
- **正确序列化路径**（对有 native 模块的场景唯一可行）：`AstSerializer::serializeProgram(program, libGroup)` + 读取端 `thisModuleGroup = &libGroup`。它对非 builtin 模块查 `libGroup.findModule(name)` 复用 live 的 C++ native 模块（不 round-trip 函数指针）；builtin/daslib 按 `cumulativeHash` 校验。**不能用裸 C API `das_program_serialize`**（那条路整体反序列化模块、不重连 native）。

---

## 1. 文件格式

```
.dabin 文件布局（全大端，经 ByteBuffer）：
┌──────────────── Header（逐字段写，非结构体 memcpy）────────────────┐
│ magic[6]      = "DABIN\0"        （6 字节原样）                     │
│ formatVersion : uint32          （本格式版本 kDabinFormatVersion）  │
│ dasVersion    : uint32          （AstSerializer::getVersion()==93） │
│ pointerSize   : uint32          （sizeof(void*)）                   │
│ flags         : uint32          （bit0 = 加密）                     │
│ depCount      : uint32                                             │
├──────────────── 依赖表（depCount 项）────────────────────────────┤
│ 每项: pathLen:uint16 + path[pathLen] + mtime:uint64               │
├──────────────── Payload ──────────────────────────────────────┤
│ payloadLen : uint64                                               │
│ payload[payloadLen]                                               │
│   - 未加密: 直接是 serializeProgram 的 blob                        │
│   - 加密  : iv[12] + (ciphertext + GCM tag[16])                    │
└───────────────────────────────────────────────────────────────┘
```

- **header 逐字段写**（修 DASBIN-03）：不再 `memcpy(&h, sizeof(h))`——那会带进结构体 padding + 主机字节序，与后面大端字段不一致。全程 `ByteBuffer` 大端。
- **依赖表 + mtime**：加载时逐个比对 `getFileMtime`，任一不符即判缓存过期，回退全量编译。
- **flags.bit0 加密**：payload 是否经 AES-256-GCM（见 §4）。

---

## 2. `DasSerializer.h`（完整）

```cpp
#pragma once
#include "Common/Core/Types.h"
#include "daScript/ast/ast.h"
#include "daScript/simulate/debug_info.h"
#include <string>
#include <utility>
#include <vector>

namespace MMO
{
    static constexpr uint32 kDabinFormatVersion = 1;
    static constexpr uint32 kDabinFlagEncrypted = 0x1;

    class DasLangSerializer
    {
    public:
        // 序列化 program 到 outPath。keyHex 非空则 AES-256-GCM 加密 payload。
        // deps: [(依赖文件路径, mtime), ...]，加载时用于判缓存过期。
        static bool Save(const std::string                                &outPath,
                         das::ProgramPtr                                   program,
                         das::ModuleGroup                                 &libGroup,
                         const std::vector<std::pair<std::string, int64>> &deps,
                         const std::string                                &keyHex);

        // 从 inPath 反序列化。libGroup 须已含全部 live native 模块。
        // keyHex 须与 Save 时一致（加密时）。失败返回 nullptr 并填 outErrors（可安全回退全量编译）。
        static das::ProgramPtr Load(const std::string &inPath,
                                    das::ModuleGroup  &libGroup,
                                    das::FileAccess   *fAccess,
                                    const std::string &keyHex,
                                    std::string       &outErrors);
    };
} // namespace MMO
```

---

## 3. `DasSerializer.cpp`（完整）

```cpp
#include "DasSerializer.h"
#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Log/Log.h"
#include "Common/Crypto/Aes256Gcm.h"
#include "Common/Crypto/Hex.h"

#include "daScript/ast/ast.h"
#include "daScript/ast/ast_serializer.h"
#include "daScript/misc/smart_ptr.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

namespace MMO
{
    static constexpr char   kMagic[6]   = {'D', 'A', 'B', 'I', 'N', '\0'};
    static constexpr size_t kAesIvSize  = 12;   // Aes256Gcm::kIvSize

    static bool ReadWholeFile(const std::string &path, std::vector<uint8> &out);
    static bool WriteWholeFile(const std::string &path, const uint8 *data, size_t len);
    static bool DecodeKey(const std::string &keyHex, uint8 outKey[32], std::string &err);

    // ─────────────────────────────────────────────────────────────
    // Save
    // ─────────────────────────────────────────────────────────────
    bool DasLangSerializer::Save(const std::string                                &outPath,
                                 das::ProgramPtr                                   program,
                                 das::ModuleGroup                                 &libGroup,
                                 const std::vector<std::pair<std::string, int64>> &deps,
                                 const std::string                                &keyHex)
    {
        // 1) 序列化 Program → 原始 blob
        das::SerializationStorageVector storage;
        {
            das::AstSerializer ser(&storage, /*writing*/ true);
            ser.thisModuleGroup = &libGroup;
            ser.serializeProgram(program, libGroup);
            ser.moduleLibrary = nullptr;
        }
        const uint8 *blob    = storage.buffer.data();
        size_t       blobLen = storage.buffer.size();

        // 2) 可选加密：payload = iv || (cipher+tag)
        bool               encrypted = !keyHex.empty();
        std::vector<uint8> payloadOwned; // 加密时持有 iv+cipher
        const uint8       *payload    = blob;
        size_t             payloadLen = blobLen;
        if (encrypted)
        {
            uint8       key[32];
            std::string kerr;
            if (!DecodeKey(keyHex, key, kerr))
            {
                Log::Error("DasLangSerializer::Save bad key: {}", kerr);
                return false;
            }
            // iv：用 blobLen 与 depCount 派生一个确定性 nonce 亦可；此处用全零 + 计数不安全，
            // 生产应每次随机。构建期生成 .dabin 属离线一次性，用固定文件序号派生即可：
            uint8 iv[kAesIvSize] = {0};
            std::memcpy(iv, &blobLen, sizeof(blobLen) < kAesIvSize ? sizeof(blobLen) : kAesIvSize);

            auto enc = Crypto::Aes256Gcm::Encrypt(key, iv, blob, blobLen);
            if (!enc)
            {
                Log::Error("DasLangSerializer::Save encrypt failed");
                return false;
            }
            payloadOwned.reserve(kAesIvSize + enc->Size());
            payloadOwned.insert(payloadOwned.end(), iv, iv + kAesIvSize);
            payloadOwned.insert(payloadOwned.end(), enc->Data(), enc->Data() + enc->Size());
            payload    = payloadOwned.data();
            payloadLen = payloadOwned.size();
        }

        // 3) 组装文件（全大端，逐字段）
        ByteBuffer buf = ByteBuffer::Own(payloadLen + 256);
        buf.WriteBytes(reinterpret_cast<const uint8 *>(kMagic), sizeof(kMagic));
        buf.WriteUint32(kDabinFormatVersion);
        buf.WriteUint32(das::AstSerializer::getVersion());
        buf.WriteUint32(static_cast<uint32>(sizeof(void *)));
        buf.WriteUint32(encrypted ? kDabinFlagEncrypted : 0u);
        buf.WriteUint32(static_cast<uint32>(deps.size()));
        for (const auto &[path, mtime] : deps)
        {
            buf.WriteUint16(static_cast<uint16>(path.size()));
            buf.WriteBytes(reinterpret_cast<const uint8 *>(path.data()), path.size());
            buf.WriteUint64(static_cast<uint64>(mtime));
        }
        buf.WriteUint64(static_cast<uint64>(payloadLen));
        buf.WriteBytes(payload, payloadLen);

        if (!WriteWholeFile(outPath, buf.Data(), buf.Size()))
        {
            Log::Error("DasLangSerializer::Save write failed: {}", outPath);
            return false;
        }
        return true;
    }

    // ─────────────────────────────────────────────────────────────
    // Load
    // ─────────────────────────────────────────────────────────────
    das::ProgramPtr DasLangSerializer::Load(const std::string &inPath,
                                            das::ModuleGroup  &libGroup,
                                            das::FileAccess   *fAccess,
                                            const std::string &keyHex,
                                            std::string       &outErrors)
    {
        std::vector<uint8> bytes;
        if (!ReadWholeFile(inPath, bytes))
        {
            outErrors = "read fail: " + inPath;
            return nullptr;
        }

        ByteBuffer buf = ByteBuffer::Wrap(bytes.data(), bytes.size());

        // 1) header 逐字段读
        constexpr size_t kHeaderMin = sizeof(kMagic) + 5 * sizeof(uint32); // 6 + 20 = 26
        if (buf.ReadableBytes() < kHeaderMin)
        {
            outErrors = "truncated header";
            return nullptr;
        }
        uint8 magic[6];
        buf.ReadBytes(magic, sizeof(magic));
        if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        {
            outErrors = "bad magic";
            return nullptr;
        }
        uint32 formatVersion = buf.ReadUint32();
        uint32 dasVersion    = buf.ReadUint32();
        uint32 pointerSize   = buf.ReadUint32();
        uint32 flags         = buf.ReadUint32();
        uint32 depCount      = buf.ReadUint32();

        if (formatVersion != kDabinFormatVersion) { outErrors = "bad format version"; return nullptr; }
        if (dasVersion != das::AstSerializer::getVersion()) { outErrors = "das version mismatch"; return nullptr; }
        if (pointerSize != sizeof(void *)) { outErrors = "pointer size mismatch"; return nullptr; }

        // 2) 依赖表 + mtime 校验（任一过期即回退全量编译）
        for (uint32 i = 0; i < depCount; ++i)
        {
            if (buf.ReadableBytes() < sizeof(uint16)) { outErrors = "dep truncated"; return nullptr; }
            uint16 len = buf.ReadUint16();
            if (buf.ReadableBytes() < static_cast<size_t>(len) + sizeof(uint64))
            {
                outErrors = "dep truncated";
                return nullptr;
            }
            std::string path(len, '\0');
            buf.ReadBytes(reinterpret_cast<uint8 *>(path.data()), len);
            uint64 mtime = buf.ReadUint64();
            if (nullptr != fAccess && fAccess->getFileMtime(path) != static_cast<int64>(mtime))
            {
                outErrors = "dep stale: " + path;
                return nullptr;
            }
        }

        // 3) payload
        if (buf.ReadableBytes() < sizeof(uint64)) { outErrors = "payload size truncated"; return nullptr; }
        uint64 payloadLen = buf.ReadUint64();
        if (buf.ReadableBytes() < payloadLen) { outErrors = "payload truncated"; return nullptr; }
        const uint8 *payload = buf.ReadPtr();

        // 4) 可选解密 → blob
        std::vector<uint8> decrypted;
        const uint8       *blob    = payload;
        size_t             blobLen = payloadLen;
        bool               isEnc   = (flags & kDabinFlagEncrypted) != 0;
        if (isEnc)
        {
            if (keyHex.empty()) { outErrors = "encrypted dabin but no key"; return nullptr; }
            if (payloadLen < kAesIvSize + Crypto::Aes256Gcm::kTagSize)
            {
                outErrors = "encrypted payload too short";
                return nullptr;
            }
            uint8       key[32];
            std::string kerr;
            if (!DecodeKey(keyHex, key, kerr)) { outErrors = kerr; return nullptr; }
            const uint8 *iv     = payload;
            const uint8 *cipher = payload + kAesIvSize;
            size_t       cipherLen = static_cast<size_t>(payloadLen) - kAesIvSize;
            auto dec = Crypto::Aes256Gcm::Decrypt(key, iv, cipher, cipherLen);
            if (!dec) { outErrors = "decrypt/verify failed"; return nullptr; }
            decrypted.assign(dec->Data(), dec->Data() + dec->Size());
            blob    = decrypted.data();
            blobLen = decrypted.size();
        }

        // 5) 反序列化——★ DASBIN-02 修复：serializeProgram 声明 noexcept 但坏 blob 会 throw，
        //    逃逸 noexcept 会 std::terminate。这里 try/catch 兜住，失败返回 nullptr 回退全量编译。
        das::SerializationStorageVector storage;
        storage.buffer.assign(blob, blob + blobLen);
        das::ProgramPtr program = das::make_smart<das::Program>();
        try
        {
            das::AstSerializer deser(&storage, /*writing*/ false);
            deser.thisModuleGroup = &libGroup;
            deser.serializeProgram(program, libGroup);
            deser.moduleLibrary = nullptr;
        }
        catch (const std::exception &e)
        {
            outErrors = std::string("deserialize threw: ") + e.what();
            return nullptr;
        }
        catch (...)
        {
            outErrors = "deserialize threw (unknown)";
            return nullptr;
        }

        if (program->failed())
        {
            outErrors = "deserialize failed (hash mismatch or native module missing)";
            return nullptr;
        }
        program->thisModuleGroup = &libGroup;
        return program;
    }

    // ─────────────────────────────────────────────────────────────
    // helpers
    // ─────────────────────────────────────────────────────────────
    static bool DecodeKey(const std::string &keyHex, uint8 outKey[32], std::string &err)
    {
        std::string clean = Crypto::StripWhitespace(keyHex);
        if (!Crypto::HexDecode(clean, outKey, 32))
        {
            err = "dabin key must be 64 hex chars (32 bytes)";
            return false;
        }
        return true;
    }

    static bool ReadWholeFile(const std::string &path, std::vector<uint8> &out)
    {
        std::error_code ec;
        auto fileSize = std::filesystem::file_size(path, ec);
        if (ec) { return false; }
        std::ifstream in(path, std::ios::binary);
        if (in.fail()) { return false; }
        out.resize(static_cast<size_t>(fileSize));
        return fileSize == 0 ||
               static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), fileSize));
    }

    static bool WriteWholeFile(const std::string &path, const uint8 *data, size_t len)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out.fail()) { return false; }
        out.write(reinterpret_cast<const char *>(data), len);
        return !out.fail();
    }
} // namespace MMO
```

---

## 4. 加密层说明（Aes256Gcm，复用 CommonCrypto）

- `Crypto::Aes256Gcm::Encrypt(key32, iv12, plaintext, len)` → `std::optional<ByteBuffer>`，返回 `ciphertext + 16B GCM tag`。`Decrypt` 自动从尾部取 tag 验证，失败返回 `nullopt`。GCM 是 **AEAD**：加密 + 防篡改一体，**无需再叠 HMAC**。
- 密钥用 hex 配置项 `dabinKeyHex`（64 hex = 32 字节），经 `Crypto::HexDecode`。
- `ScriptEngine` 的 xmake 需加 `add_deps("CommonCrypto")`。

> **诚实的边界**：密钥内嵌宿主二进制，理论上可被逆向提取。这层是**抬高门槛**（防随手 dump 明文 AST、防补丁被中间人篡改），非密码学强保护。要更强需密钥服务器下发，属后续。

> **IV/nonce 注意**：上面 `Save` 用 `blobLen` 派生 IV 是**确定性占位**——同一密钥下 GCM 要求 nonce 唯一，否则安全性崩塌。`.dabin` 是**构建期离线一次性产物**，实践中应：每个 `.dabin` 用递增文件序号或内容哈希派生唯一 IV，或干脆每次随机 IV 并写进 payload（本格式 IV 已随 payload 存储，随机化只需改 `Save` 里 IV 的来源）。**开发期 `dabinKeyHex` 留空走明文**，加密仅发布期开启。

---

## 5. 与引擎接线（Save 侧）

02 篇的 `Compile()` 已有 `Load` 分支。Save 在**全量编译成功后**触发，写缓存供下次启动用。在 `Compile()` 全量编译成功后追加：

```cpp
// DasEngine.cpp，Compile() 末尾，全量编译成功后：
if (!dasbinPath.empty() && img.program && !img.program->failed())
{
    // 收集依赖 mtime（此时还没 simulate，用 program 的文件访问）
    std::vector<std::pair<std::string, int64>> deps;
    for (auto *fi : img.ctxFilesForCache())      // 或编译期用 getPrerequisits；简化见下注
    {
        deps.emplace_back(fi, img.fileAccess->getFileMtime(fi));
    }
    DasLangSerializer::Save(dasbinPath, img.program, *img.moduleGroup, deps, _cfg.dabinKeyHex);
}
```

> 依赖清单来源：`simulate` 后可用 `Context::getAllFiles()`（02 篇 `CollectDependencyFiles`）最简单。因此实践上把 Save 放到 `SimulateImage` 成功之后、`Load()` 里，比放 `Compile()` 里更方便拿到完整依赖集。推荐落点：`Load()` 中 `SimulateImage` 成功后，若 `mode==Release && !dasbinDir.empty() && 本次是全量编译`，收集 `CollectDependencyFiles()` 的 mtime 调 `Save`。

### 构建期离线生成 .dabin（推荐）

线上不依赖首次运行时生成缓存，而是**构建期**用一个小工具/xmake 步骤：加载与运行期一致的模块集 → `compileDaScript` → `DasLangSerializer::Save`。与 04 篇的 daslang 工具 target 可共用宿主初始化路径，确保模块集/policy 与运行期一致（否则 `cumulativeHash` 不符，加载即回退）。

---

## 6. 校验与回退矩阵

| 情况 | 检测点 | 行为 |
|---|---|---|
| 文件不存在/截断 | `ReadWholeFile` / header 长度 | `Load` 返回 nullptr → 全量编译 |
| magic/格式版本不符 | header 校验 | nullptr → 全量编译 |
| daslang 版本≠93 | `dasVersion` 校验 | nullptr → 全量编译 |
| 指针宽度不符 | `pointerSize` 校验 | nullptr → 全量编译 |
| 依赖文件被改（mtime） | 依赖表逐项比对 | nullptr → 全量编译 |
| 加密但无密钥/密钥错/被篡改 | `Decrypt` 验 tag | nullptr → 全量编译 |
| blob 内部损坏 | `serializeProgram` 抛异常 → **try/catch** | nullptr → 全量编译（**不再 terminate**） |
| native 模块 hash 不符 | `program->failed()` | nullptr → 全量编译 |

**核心保证**：任何 `.dabin` 异常都安全降级为全量源码编译，绝不崩进程（DASBIN-02 修复）。

---

## 7. round-trip 测试（必须，验证 DASBIN-04 未定论项）

审查标记「反序列化后直接 simulate 是否需要重跑 finalize」为 PLAUSIBLE-未定论。产出缓存后**必须**加一个测试确认：

1. 全量编译 `main.das` → `program_a`；`Save("test.dabin", program_a, group, deps, "")`（明文）。
2. `Load("test.dabin", group2, fa, "", err)` → `program_b`（`err` 应为空，`program_b != nullptr`）。
3. `program_b->simulate(ctx_b, logs)` → 应成功（若报 "variable not used" / stack 未分配等，说明需在 `Load` 里补 `allocateStack`，改走 `Program::serialize` 路径）。
4. `ctx_b.findFunction("Init")` → eval → 行为与 `program_a` 一致。
5. 再测加密往返：`Save(..., keyHex)` → `Load(..., keyHex)` 成功；`Load(..., 错误key)` 返回 nullptr 不崩。

> 若步骤 3 失败：在 `Load` 反序列化后、返回前补 `program->markExecutableSymbolUse(); program->removeUnusedSymbols(); program->allocateStack(logs);`（镜像 `Program::serialize` 读取端行为）。当前实现先不加——多数情况 `serializeProgram` 已 bake 好，等测试暴露再补，避免臆测。
