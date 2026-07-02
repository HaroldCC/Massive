/**
 * @file ByteBuffer.cpp
 * @brief 容量管理、所有权管理
 */

#include "Common/Core/ByteBuffer.h"
#include "Common/Core/MassiveAssert.h"

#include <algorithm>
#include <cstdlib>

namespace MMO
{

    // ── 工厂方法 ──

    /**
     * @brief 创建拥有内存的缓冲区
     * @param initialCapacity  初始容量，0 则使用默认值 256
     * @return ByteBuffer（Own 模式）
     */
    ByteBuffer ByteBuffer::Own(size_t initialCapacity)
    {
        if (initialCapacity == 0)
        {
            initialCapacity = 256;
        }

        auto *data = static_cast<uint8 *>(std::malloc(initialCapacity));
        MASSIVE_ASSERT(data, "ByteBuffer OOM: failed to allocate Own buffer");

        if (!data)
        {
            return ByteBuffer {};
        }

        return ByteBuffer(data, initialCapacity, 0, true);
    }

    /**
     * @brief 借用外部只读内存
     * @param data  外部数据指针
     * @param len   数据长度
     * @return ByteBuffer（Wrap 模式，零 copy）
     */
    ByteBuffer ByteBuffer::Wrap(const uint8 *data, size_t len)
    {
        return ByteBuffer(const_cast<uint8 *>(data), len, len, false);
    }

    /**
     * @brief 借用外部可写内存
     * @param data  外部数据指针
     * @param len   数据长度
     * @return ByteBuffer（Wrap 模式，零 copy）
     */
    ByteBuffer ByteBuffer::Wrap(uint8 *data, size_t len)
    {
        return ByteBuffer(data, len, len, false);
    }

    /**
     * @brief 拷贝外部数据，拥有所有权
     * @param data  源数据指针
     * @param len   数据长度
     * @return ByteBuffer（Own 模式）
     */
    ByteBuffer ByteBuffer::Copy(const uint8 *data, size_t len)
    {
        auto buf = Own(len);
        std::memcpy(buf._data, data, len);
        buf._writePos = len;
        return buf;
    }

    // ── 私有构造 ──

    /**
     * @brief 内部构造，直接接管内存
     * @param data       数据指针
     * @param capacity   总容量
     * @param writePos   初始写入位置
     * @param owns       是否拥有所有权
     */
    ByteBuffer::ByteBuffer(uint8 *data, size_t capacity, size_t writePos, bool owns)
        : _data(data)
        , _capacity(capacity)
        , _readPos(0)
        , _writePos(writePos)
        , _ownsMemory(owns)
    {
    }

    /**
     * @brief 仅释放拥有的内存
     */
    void ByteBuffer::Release()
    {
        if (_ownsMemory && _data)
        {
            std::free(_data);
        }
        _data       = nullptr;
        _capacity   = 0;
        _readPos    = 0;
        _writePos   = 0;
        _ownsMemory = false;
    }

    /**
     * @brief 扩容（仅 Own 模式有效）
     * 策略：2x 扩容，最小保证 capacity
     * @param capacity  目标容量
     */
    void ByteBuffer::Reserve(size_t capacity)
    {
        if (!_ownsMemory)
        {
            return;
        }

        if (_capacity >= capacity)
        {
            return;
        }

        size_t newCapacity = std::max(_capacity * 2, capacity);
        auto  *newData     = static_cast<uint8 *>(std::realloc(_data, newCapacity));
        MASSIVE_ASSERT(newData, "ByteBuffer OOM: realloc failed");

        if (!newData)
        {
            return;
        }

        _data     = newData;
        _capacity = newCapacity;
    }

    /**
     * @brief 越界读断言
     * @param count  读取字节数
     */
    void ByteBuffer::CheckRead(size_t count) const
    {
        MASSIVE_ASSERT(_readPos + count <= _writePos, "ByteBuffer: read beyond write position");
    }

    /**
     * @brief 写前自动扩容（仅 Own 模式）
     * @param count  需写入字节数
     */
    void ByteBuffer::EnsureWrite(size_t count)
    {
        MASSIVE_ASSERT(_ownsMemory, "ByteBuffer: write on non-owned buffer (Wrap mode is read-only)");

        if (_writePos + count > _capacity)
        {
            Reserve(_writePos + count);
        }
    }

} // namespace MMO
