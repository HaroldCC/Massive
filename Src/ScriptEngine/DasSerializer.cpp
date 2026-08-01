#include "DasSerializer.h"
#include "Common/Core/ByteBuffer.h"
#include "Common/Core/Types.h"
#include "Common/Crypto/Aes256Gcm.h"
#include "Common/Crypto/Hex.h"
#include "daScript/ast/ast.h"
#include "daScript/ast/ast_serializer.h"
#include "daScript/misc/smart_ptr.h"
#include "Common/Log/Log.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

/**

.dasbin 文件布局（全大端，经 ByteBuffer）：
┌──────────────── Header（结构体 DasbinHeader，逐字段读写）─────────────┐
│ magic         : uint8[7]    = "DASBIN\0"                          │
│ formatVersion : uint32      （本格式版本 kDabinFormatVersion）      │
│ dasVersion    : uint32      （AstSerializer::getVersion()==93）     │
│ pointerSize   : uint32      （sizeof(void*)）                       │
│ flags         : uint32      （bit0 = 加密）                         │
│ depCount      : uint32                                             │
├──────────────── 依赖表（depCount 项）────────────────────────────┤
│ 每项: pathLen:uint16 + path[pathLen] + mtime:uint64               │
├──────────────── Payload ──────────────────────────────────────┤
│ payloadLen : uint64                                               │
│ payload[payloadLen]                                               │
│   - 未加密: 直接是 serializeProgram 的 blob                        │
│   - 加密  : iv[12] + (ciphertext + GCM tag[16])                    │
└───────────────────────────────────────────────────────────────┘

注意：Header 必须逐字段经 ByteBuffer 读写（大端），禁止 memcpy 直读结构体——
结构体存在 padding 且是 native 字节序，与磁盘布局不一致。

 */

namespace MMO
{
    using Crypto::Aes256Gcm; // Aes256Gcm 定义于 MMO::Crypto

    static constexpr char   kDasbinMagic[7]  = {'D', 'A', 'S', 'B', 'I', 'N', '\0'};
    static constexpr uint32 kDasbinMagicSize = sizeof(kDasbinMagic);

    /// .dasbin 文件头（纯数据聚合，磁盘布局见文件顶部注释）
    struct DasbinHeader
    {
        uint8  magic[kDasbinMagicSize]; // "DASBIN\0"
        uint32 formatVersion;           // 本格式版本号
        uint32 dasVersion;              // AstSerializer 版本
        uint32 pointerSize;             // 序列化时的 sizeof(void*)
        uint32 flags;                   // 位标志，bit0 = 加密
        uint32 depCount;                // 依赖文件个数
    };

    /// Header 在磁盘上的固定字节数（不含依赖表）
    static constexpr size_t kDasbinHeaderSize = kDasbinMagicSize + 5 * sizeof(uint32);

    /// 逐字段大端写入 Header
    static void WriteDasbinHeader(ByteBuffer &buf, const DasbinHeader &header)
    {
        buf.WriteBytes(header.magic, kDasbinMagicSize);
        buf.WriteUint32(header.formatVersion);
        buf.WriteUint32(header.dasVersion);
        buf.WriteUint32(header.pointerSize);
        buf.WriteUint32(header.flags);
        buf.WriteUint32(header.depCount);
    }

    /// 逐字段大端读取 Header（带长度校验），失败返回 false
    static bool ReadDasbinHeader(ByteBuffer &buf, DasbinHeader &header)
    {
        if (buf.ReadableBytes() < kDasbinHeaderSize)
        {
            return false;
        }
        buf.ReadBytes(header.magic, kDasbinMagicSize);
        header.formatVersion = buf.ReadUint32();
        header.dasVersion    = buf.ReadUint32();
        header.pointerSize   = buf.ReadUint32();
        header.flags         = buf.ReadUint32();
        header.depCount      = buf.ReadUint32();
        return true;
    }

    static bool ReadWholeFile(const std::string &path, std::vector<uint8> &out);
    static bool WriteWholeFile(const std::string &path, const uint8 *data, size_t len);
    static bool DecodeKey(const std::string &keyHex, uint8 outKey[32], std::string &err);

    bool DasLangSerializer::Save(const std::string                                &outPath,
                                 das::ProgramPtr                                   program,
                                 das::ModuleGroup                                 &libGroup,
                                 const std::vector<std::pair<std::string, int64>> &deps,
                                 const std::string                                &keyHex)
    {
        // 序列化Program -> 原始blob
        das::SerializationStorageVector storage;
        {
            das::AstSerializer ser(&storage, true);
            ser.thisModuleGroup = &libGroup;
            ser.serializeProgram(program, libGroup);
            ser.moduleLibrary = nullptr;
        }

        const uint8 *blob    = storage.buffer.data();
        size_t       blobLen = storage.buffer.size();

        bool               encrypted = !keyHex.empty();
        std::vector<uint8> payloadOwned;
        const uint8       *payload    = blob;
        size_t             payloadLen = blobLen;
        if (encrypted)
        {
            uint8       key[32];
            std::string kerror;
            if (!DecodeKey(keyHex, key, kerror))
            {
                Log::Error("DasLangSerializer Save bad key:{}", kerror);
                return false;
            }

            uint8 iv[Aes256Gcm::kIvSize] = {0};
            std::memcpy(iv,
                        &blobLen,
                        sizeof(blobLen) < Aes256Gcm::kIvSize ? sizeof(blobLen) : Aes256Gcm::kIvSize);

            auto enc = Crypto::Aes256Gcm::Encrypt(key, iv, blob, blobLen);
            if (!enc)
            {
                Log::Error("DasLangSerializer Save encrypt failed");
                return false;
            }
            payloadOwned.reserve(Aes256Gcm::kIvSize + enc->Size());
            payloadOwned.insert(payloadOwned.end(), iv, iv + Aes256Gcm::kIvSize);
            payloadOwned.insert(payloadOwned.end(), enc->Data(), enc->Data() + enc->Size());
            payload    = payloadOwned.data();
            payloadLen = payloadOwned.size();
        }

        // 写文件头
        DasbinHeader header;
        std::memcpy(header.magic, kDasbinMagic, kDasbinMagicSize);
        header.formatVersion = kDasbinFormatVersion;
        header.dasVersion    = das::AstSerializer::getVersion();
        header.pointerSize   = sizeof(void *);
        header.flags         = encrypted ? kDasbinFlagEncrypted : 0u;
        header.depCount      = static_cast<uint32>(deps.size());

        ByteBuffer buf = ByteBuffer::Own(payloadLen + 256);
        WriteDasbinHeader(buf, header);

        // 写依赖文件信息
        for (const auto &[path, mtime] : deps)
        {
            buf.WriteUint16(static_cast<uint16>(path.size()));
            buf.WriteBytes(reinterpret_cast<const uint8 *>(path.data()), path.size());
            buf.WriteUint64(static_cast<uint64>(mtime));
        }

        buf.WriteUint64(static_cast<uint64>(payloadLen));
        buf.WriteBytes(payload, payloadLen);

        // 写文件内容
        if (!WriteWholeFile(outPath, buf.Data(), buf.Size()))
        {
            Log::Error("DasLangSerializer save write file faile:{}", outPath);
            return false;
        }

        return true;
    }

    das::ProgramPtr DasLangSerializer::Load(const std::string &inPath,
                                            das::ModuleGroup  &libGroup,
                                            das::FileAccess   *fAccess,
                                            const std::string &keyHex,
                                            std::string       &outErrors)
    {
        std::vector<uint8> bytes;
        if (!ReadWholeFile(inPath, bytes))
        {
            outErrors = std::format("read fail:{}", inPath);
            return nullptr;
        }

        ByteBuffer buf = ByteBuffer::Wrap(bytes.data(), bytes.size());

        DasbinHeader header;
        if (!ReadDasbinHeader(buf, header))
        {
            outErrors = "DasLangSerializer Load truncated header";
            return nullptr;
        }

        if (std::memcmp(header.magic, "DASBIN", sizeof(header.magic)) != 0)
        {
            outErrors = "Read bad magic";
            return nullptr;
        }

        if (header.formatVersion != kDasbinFormatVersion)
        {
            outErrors = "Read bad format version";
            return nullptr;
        }

        if (header.dasVersion != das::AstSerializer::getVersion())
        {
            outErrors = "daslang ast serializer version not match";
            return nullptr;
        }

        if (header.pointerSize != sizeof(void *))
        {
            outErrors = "Pointer size not match";
            return nullptr;
        }

        // 依赖表+mtime校验
        for (uint32 i = 0; i < header.depCount; ++i)
        {
            if (buf.ReadableBytes() < sizeof(uint16))
            {
                outErrors = "Depend file truncated";
                return nullptr;
            }

            uint16 len = buf.ReadUint16();
            if (buf.ReadableBytes() < static_cast<size_t>(len + sizeof(uint64)))
            {
                outErrors = "Depend file trundcated";
                return nullptr;
            }

            std::string path(len, '\0');
            buf.ReadBytes(reinterpret_cast<uint8 *>(path.data()), len);
            uint64 mtime = buf.ReadUint64();
            if (nullptr != fAccess && fAccess->getFileMtime(path) != static_cast<int64>(mtime))
            {
                outErrors = std::format("Depend file:{} not match", path);
                return nullptr;
            }
        }

        // payload
        if (buf.ReadableBytes() < sizeof(uint64))
        {
            outErrors = "payload size truncated";
            return nullptr;
        }

        uint64 payloadLen = buf.ReadUint64();
        if (buf.ReadableBytes() < payloadLen)
        {
            outErrors = "payload truncated";
            return nullptr;
        }

        const uint8 *payload = buf.ReadPtr();

        // 密文 blob 解密（与 Save 的 iv[12] + ciphertext + tag[16] 布局对应）
        std::vector<uint8> decrypted;
        const uint8       *blob        = payload;
        size_t             blobLen     = payloadLen;
        bool               isEncrypted = (header.flags & kDasbinFlagEncrypted) != 0;
        if (isEncrypted)
        {
            if (keyHex.empty())
            {
                outErrors = "encrypted dasbin but no key";
                return nullptr;
            }

            if (payloadLen < (Aes256Gcm::kIvSize + Aes256Gcm::kTagSize))
            {
                outErrors = "encrypted payload too short";
                return nullptr;
            }

            uint8       key[32];
            std::string kerror;
            if (!DecodeKey(keyHex, key, kerror))
            {
                outErrors = std::format("DasLangSerializer Load bad key:{}", kerror);
                return nullptr;
            }

            const uint8 *iv        = payload;
            const uint8 *cipher    = payload + Aes256Gcm::kIvSize;
            size_t       cipherLen = static_cast<size_t>(payloadLen) - Aes256Gcm::kIvSize;
            auto         dec       = Aes256Gcm::Decrypt(key, iv, cipher, cipherLen);
            if (!dec)
            {
                outErrors = "DasLangSerializer Load decrypt failed (bad key or corrupted data)";
                return nullptr;
            }

            decrypted.assign(dec->Data(), dec->Data() + dec->Size());
            blob    = decrypted.data();
            blobLen = decrypted.size();
        }

        // 反序列化
        das::SerializationStorageVector storage;
        storage.buffer.assign(blob, blob + blobLen);
        das::ProgramPtr program = das::make_smart<das::Program>();
        try
        {
            das::AstSerializer deser(&storage, false);
            deser.thisModuleGroup = &libGroup;
            deser.serializeProgram(program, libGroup);
            deser.moduleLibrary = nullptr;
        }
        catch (const std::exception &e)
        {
            outErrors = std::format("deserialize throw:{}", e.what());
            return nullptr;
        }

        if (program->failed())
        {
            outErrors = "deserialize failed (module hash mismatch or native module missing)";
            return nullptr;
        }

        program->thisModuleGroup = &libGroup;
        return program;
    }

    static bool ReadWholeFile(const std::string &path, std::vector<uint8> &out)
    {
        std::error_code ec;
        auto            fileSize = std::filesystem::file_size(path, ec);
        if (ec)
        {
            Log::Error("Read File:{} error:{}", path, ec.message());
            return false;
        }

        std::ifstream inFs(path, std::ios::binary);
        if (inFs.fail())
        {
            Log::Error("Read File:{} failed", path);
            return false;
        }
        out.resize(static_cast<size_t>(fileSize));
        return fileSize == 0 || static_cast<bool>(inFs.read(reinterpret_cast<char *>(out.data()), fileSize));
    }

    static bool WriteWholeFile(const std::string &path, const uint8 *data, size_t len)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream outFs(path, std::ios::binary | std::ios::trunc);
        if (outFs.fail())
        {
            Log::Error("write file:{} error:{}", path, ec.message());
            return false;
        }

        outFs.write(reinterpret_cast<const char *>(data), len);

        return !outFs.fail();
    }

    /// 将 64 位 hex 字符串解码为 32 字节密钥
    static bool DecodeKey(const std::string &keyHex, uint8 outKey[32], std::string &err)
    {
        std::string clean = Crypto::StripWhitespace(keyHex);
        if (!Crypto::HexDecode(clean, outKey, 32))
        {
            err = "dasbin key must be 64 hex chars (32 bytes)";
            return false;
        }

        return true;
    }
} // namespace MMO