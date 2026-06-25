/**
 * @file ByteBuffer.h
 * @brief 双模式字节缓冲区（拥有 + 借用）
 *
 * Own 模式：网络发包构建、DB 数据持有——自动扩容
 * Wrap 模式：网络收包解析、ECS blob 快照——零 copy，不可扩容
 * 网络序 big-endian，Read/Write 自动完成转换
 */
#pragma once

#include <cstring>

#include "Common/Core/Types.h"

namespace MMO
{

    /**
     * @brief 双模式字节缓冲区
     *
     * 支持 Own（拥有内存，可扩容）和 Wrap（借用外部内存，零 copy，不可扩容）两种模式。
     * 所有 Read 操作自动将 big-endian 转换为 native 序，
     * 所有 Write 操作自动转换 native 序为 big-endian。
     */
    class ByteBuffer
    {
    public:
        // ── 工厂方法 ──

        /** @brief 创建拥有内存的缓冲区，自动扩容 */
        static ByteBuffer Own(size_t initialCapacity = 256);

        /** @brief 借用外部只读内存，零 copy，不可扩容 */
        static ByteBuffer Wrap(const uint8 *data, size_t len);
        /** @brief 借用外部可写内存，零 copy，不可扩容 */
        static ByteBuffer Wrap(uint8 *data, size_t len);

        /** @brief 拷贝外部数据，拥有所有权 */
        static ByteBuffer Copy(const uint8 *data, size_t len);

        // ── 生命周期 ──

        ByteBuffer() : _ownsMemory(false)
        {
        }

        ByteBuffer(ByteBuffer &&other) noexcept
            : _data(other._data)
            , _capacity(other._capacity)
            , _readPos(other._readPos)
            , _writePos(other._writePos)
            , _ownsMemory(other._ownsMemory)
        {
            other._data       = nullptr;
            other._capacity   = 0;
            other._readPos    = 0;
            other._writePos   = 0;
            other._ownsMemory = false;
        }

        ByteBuffer &operator=(ByteBuffer &&other) noexcept
        {
            if (this != &other)
            {
                Release();
                _data             = other._data;
                _capacity         = other._capacity;
                _readPos          = other._readPos;
                _writePos         = other._writePos;
                _ownsMemory       = other._ownsMemory;
                other._data       = nullptr;
                other._capacity   = 0;
                other._readPos    = 0;
                other._writePos   = 0;
                other._ownsMemory = false;
            }
            return *this;
        }

        ~ByteBuffer()
        {
            Release();
        }

        ByteBuffer(const ByteBuffer &)            = delete;
        ByteBuffer &operator=(const ByteBuffer &) = delete;

        // ── 读操作（big-endian → native）──

        /** @brief 读取 uint8 */
        uint8 ReadUint8()
        {
            CheckRead(1);
            return _data[_readPos++];
        }

        /** @brief 读取 uint16（big-endian → native） */
        uint16 ReadUint16()
        {
            CheckRead(2);
            uint16 result = static_cast<uint16>((static_cast<uint16>(_data[_readPos]) << 8)
                                                | static_cast<uint16>(_data[_readPos + 1]));
            _readPos += 2;
            return result;
        }

        /** @brief 读取 uint32（big-endian → native） */
        uint32 ReadUint32()
        {
            CheckRead(4);
            uint32 result = (static_cast<uint32>(_data[_readPos]) << 24)
                            | (static_cast<uint32>(_data[_readPos + 1]) << 16)
                            | (static_cast<uint32>(_data[_readPos + 2]) << 8)
                            | static_cast<uint32>(_data[_readPos + 3]);
            _readPos += 4;
            return result;
        }

        /** @brief 读取 uint64（big-endian → native） */
        uint64 ReadUint64()
        {
            CheckRead(8);
            uint64 result = (static_cast<uint64>(_data[_readPos]) << 56)
                            | (static_cast<uint64>(_data[_readPos + 1]) << 48)
                            | (static_cast<uint64>(_data[_readPos + 2]) << 40)
                            | (static_cast<uint64>(_data[_readPos + 3]) << 32)
                            | (static_cast<uint64>(_data[_readPos + 4]) << 24)
                            | (static_cast<uint64>(_data[_readPos + 5]) << 16)
                            | (static_cast<uint64>(_data[_readPos + 6]) << 8)
                            | static_cast<uint64>(_data[_readPos + 7]);
            _readPos += 8;
            return result;
        }

        /** @brief 读取 float（big-endian → native） */
        float ReadFloat()
        {
            uint32 bits = ReadUint32();
            float  result;
            std::memcpy(&result, &bits, sizeof(float));
            return result;
        }

        /** @brief 读取 double（big-endian → native） */
        double ReadDouble()
        {
            uint64 bits = ReadUint64();
            double result;
            std::memcpy(&result, &bits, sizeof(double));
            return result;
        }

        /** @brief 读取指定字节数到输出缓冲区 */
        void ReadBytes(uint8 *out, size_t count)
        {
            CheckRead(count);
            std::memcpy(out, _data + _readPos, count);
            _readPos += count;
        }

        // ── 写操作（native → big-endian）──

        /** @brief 写入 uint8 */
        void WriteUint8(uint8 v)
        {
            EnsureWrite(1);
            _data[_writePos++] = v;
        }

        /** @brief 写入 uint16（native → big-endian） */
        void WriteUint16(uint16 v)
        {
            EnsureWrite(2);
            _data[_writePos]     = static_cast<uint8>(v >> 8);
            _data[_writePos + 1] = static_cast<uint8>(v);
            _writePos += 2;
        }

        /** @brief 写入 uint32（native → big-endian） */
        void WriteUint32(uint32 v)
        {
            EnsureWrite(4);
            _data[_writePos]     = static_cast<uint8>(v >> 24);
            _data[_writePos + 1] = static_cast<uint8>(v >> 16);
            _data[_writePos + 2] = static_cast<uint8>(v >> 8);
            _data[_writePos + 3] = static_cast<uint8>(v);
            _writePos += 4;
        }

        /** @brief 写入 uint64（native → big-endian） */
        void WriteUint64(uint64 v)
        {
            EnsureWrite(8);
            _data[_writePos]     = static_cast<uint8>(v >> 56);
            _data[_writePos + 1] = static_cast<uint8>(v >> 48);
            _data[_writePos + 2] = static_cast<uint8>(v >> 40);
            _data[_writePos + 3] = static_cast<uint8>(v >> 32);
            _data[_writePos + 4] = static_cast<uint8>(v >> 24);
            _data[_writePos + 5] = static_cast<uint8>(v >> 16);
            _data[_writePos + 6] = static_cast<uint8>(v >> 8);
            _data[_writePos + 7] = static_cast<uint8>(v);
            _writePos += 8;
        }

        /** @brief 写入 float（native → big-endian） */
        void WriteFloat(float v)
        {
            uint32 bits;
            std::memcpy(&bits, &v, sizeof(float));
            WriteUint32(bits);
        }

        /** @brief 写入 double（native → big-endian） */
        void WriteDouble(double v)
        {
            uint64 bits;
            std::memcpy(&bits, &v, sizeof(double));
            WriteUint64(bits);
        }

        /** @brief 写入指定字节数 */
        void WriteBytes(const uint8 *data, size_t count)
        {
            EnsureWrite(count);
            std::memcpy(_data + _writePos, data, count);
            _writePos += count;
        }

        // ── 容量 ──

        /** @brief 当前有效数据长度 */
        size_t Size() const
        {
            return _writePos;
        }

        /** @brief 剩余可读字节数 */
        size_t ReadableBytes() const
        {
            return _writePos - _readPos;
        }

        /** @brief 总容量 */
        size_t Capacity() const
        {
            return _capacity;
        }

        /** @brief 扩容到指定容量（仅 Own 模式有效） */
        void Reserve(size_t capacity);

        // ── 裸指针 ──

        const uint8 *Data() const
        {
            return _data;
        }

        uint8 *Data()
        {
            return _data;
        }

        const uint8 *ReadPtr() const
        {
            return _data + _readPos;
        }

        uint8 *WritePtr()
        {
            return _data + _writePos;
        }

        // ── 位置操作 ──

        /** @brief 跳过读取指定字节数 */
        void SkipRead(size_t bytes)
        {
            CheckRead(bytes);
            _readPos += bytes;
        }

        /** @brief 重置读取位置 */
        void ResetRead()
        {
            _readPos = 0;
        }

        /** @brief 重置读写位置 */
        void ResetWrite()
        {
            _writePos = 0;
            _readPos  = 0;
        }

        /** @brief 手动设置写入位置 */
        void SetWritePos(size_t pos)
        {
            _writePos = pos;
        }

        size_t ReadPos() const
        {
            return _readPos;
        }

        size_t WritePos() const
        {
            return _writePos;
        }

        // ── 状态 ──

        /** @brief 是否拥有内存所有权 */
        bool IsOwner() const
        {
            return _ownsMemory;
        }

    private:
        ByteBuffer(uint8 *data, size_t capacity, size_t writePos, bool owns);

        void Release();
        void CheckRead(size_t count) const;
        void EnsureWrite(size_t count);

        uint8 *_data       = nullptr;
        size_t _capacity   = 0;
        size_t _readPos    = 0;
        size_t _writePos   = 0;
        bool   _ownsMemory = false;
    };

} // namespace MMO
