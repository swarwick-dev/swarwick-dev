
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef _AFX
#include <afx.h>
#endif

#ifndef FOCUSTRANSPORT_API
#define FOCUSTRANSPORT_API
#endif

namespace FocusTransport
{
enum class BinaryEndian : uint8_t { Little = 1, Big = 2 };

namespace BinaryDetail
{
    inline constexpr BinaryEndian DefaultEndian = BinaryEndian::Little;

    inline constexpr uint16_t ByteSwap16(uint16_t v)
    {
        return static_cast<uint16_t>(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
    }

    inline constexpr uint32_t ByteSwap32(uint32_t v)
    {
        return ((v & 0x000000FFu) << 24) |
               ((v & 0x0000FF00u) << 8)  |
               ((v & 0x00FF0000u) >> 8)  |
               ((v & 0xFF000000u) >> 24);
    }

    inline constexpr uint64_t ByteSwap64(uint64_t v)
    {
        return ((v & 0x00000000000000FFull) << 56) |
               ((v & 0x000000000000FF00ull) << 40) |
               ((v & 0x0000000000FF0000ull) << 24) |
               ((v & 0x00000000FF000000ull) << 8)  |
               ((v & 0x000000FF00000000ull) >> 8)  |
               ((v & 0x0000FF0000000000ull) >> 24) |
               ((v & 0x00FF000000000000ull) >> 40) |
               ((v & 0xFF00000000000000ull) >> 56);
    }

    inline constexpr bool NeedSwap(BinaryEndian wireEndian)
    {
        if constexpr (std::endian::native == std::endian::little)
            return wireEndian == BinaryEndian::Big;
        else
            return wireEndian == BinaryEndian::Little;
    }

    inline uint16_t ToWire(uint16_t v, BinaryEndian e) { return NeedSwap(e) ? ByteSwap16(v) : v; }
    inline uint32_t ToWire(uint32_t v, BinaryEndian e) { return NeedSwap(e) ? ByteSwap32(v) : v; }
    inline uint64_t ToWire(uint64_t v, BinaryEndian e) { return NeedSwap(e) ? ByteSwap64(v) : v; }

    inline uint16_t FromWire(uint16_t v, BinaryEndian e) { return ToWire(v, e); }
    inline uint32_t FromWire(uint32_t v, BinaryEndian e) { return ToWire(v, e); }
    inline uint64_t FromWire(uint64_t v, BinaryEndian e) { return ToWire(v, e); }

    template <typename T>
    inline constexpr bool IsSupportedPrimitive =
        std::is_same_v<T, bool> ||
        std::is_same_v<T, int8_t> ||
        std::is_same_v<T, uint8_t> ||
        std::is_same_v<T, int16_t> ||
        std::is_same_v<T, uint16_t> ||
        std::is_same_v<T, int32_t> ||
        std::is_same_v<T, uint32_t> ||
        std::is_same_v<T, int64_t> ||
        std::is_same_v<T, uint64_t> ||
        std::is_same_v<T, float> ||
        std::is_same_v<T, double>;
}

class FOCUSTRANSPORT_API BinaryWriter final
{
public:
    explicit BinaryWriter(std::vector<uint8_t>& out, BinaryEndian endian = BinaryDetail::DefaultEndian)
        : out_(out), endian_(endian)
    {
        out_.clear();
    }

    bool Ok() const noexcept { return ok_; }
    size_t Size() const noexcept { return out_.size(); }

    void Reserve(size_t bytes)
    {
        if (ok_) out_.reserve(bytes);
    }

    bool WriteBool(bool v)       { return WriteUInt8(v ? 1u : 0u); }
    bool WriteInt8(int8_t v)     { return WriteByte(static_cast<uint8_t>(v)); }
    bool WriteUInt8(uint8_t v)   { return WriteByte(v); }
    bool WriteInt16(int16_t v)   { return WriteUInt16(static_cast<uint16_t>(v)); }

    bool WriteUInt16(uint16_t v)
    {
        const auto wire = BinaryDetail::ToWire(v, endian_);
        return Append(&wire, sizeof(wire));
    }

    bool WriteInt32(int32_t v) { return WriteUInt32(static_cast<uint32_t>(v)); }

    bool WriteUInt32(uint32_t v)
    {
        const auto wire = BinaryDetail::ToWire(v, endian_);
        return Append(&wire, sizeof(wire));
    }

    bool WriteInt64(int64_t v) { return WriteUInt64(static_cast<uint64_t>(v)); }

    bool WriteUInt64(uint64_t v)
    {
        const auto wire = BinaryDetail::ToWire(v, endian_);
        return Append(&wire, sizeof(wire));
    }

    bool WriteFloat(float v)
    {
        uint32_t bits{};
        std::memcpy(&bits, &v, sizeof(bits));
        return WriteUInt32(bits);
    }

    bool WriteDouble(double v)
    {
        uint64_t bits{};
        std::memcpy(&bits, &v, sizeof(bits));
        return WriteUInt64(bits);
    }

    template <typename T>
    bool WritePrimitive(T v)
    {
        static_assert(BinaryDetail::IsSupportedPrimitive<T>, "Unsupported primitive type");

        if constexpr (std::is_same_v<T, bool>) return WriteBool(v);
        else if constexpr (std::is_same_v<T, int8_t>) return WriteInt8(v);
        else if constexpr (std::is_same_v<T, uint8_t>) return WriteUInt8(v);
        else if constexpr (std::is_same_v<T, int16_t>) return WriteInt16(v);
        else if constexpr (std::is_same_v<T, uint16_t>) return WriteUInt16(v);
        else if constexpr (std::is_same_v<T, int32_t>) return WriteInt32(v);
        else if constexpr (std::is_same_v<T, uint32_t>) return WriteUInt32(v);
        else if constexpr (std::is_same_v<T, int64_t>) return WriteInt64(v);
        else if constexpr (std::is_same_v<T, uint64_t>) return WriteUInt64(v);
        else if constexpr (std::is_same_v<T, float>) return WriteFloat(v);
        else if constexpr (std::is_same_v<T, double>) return WriteDouble(v);
    }

    bool WriteBytes(std::span<const uint8_t> bytes)
    {
        if (!WriteUInt32(CheckedSize32(bytes.size()))) return false;
        return Append(bytes.data(), bytes.size());
    }

    bool WriteRawBytes(std::span<const uint8_t> bytes)
    {
        return Append(bytes.data(), bytes.size());
    }

    bool WriteString(std::string_view v)
    {
        if (!WriteUInt32(CheckedSize32(v.size()))) return false;
        return Append(v.data(), v.size());
    }

    bool WriteWString(std::wstring_view v)
    {
        if (!WriteUInt32(CheckedSize32(v.size()))) return false;

        for (wchar_t ch : v)
        {
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (!WriteUInt16(static_cast<uint16_t>(ch))) return false;
            }
            else
            {
                if (!WriteUInt32(static_cast<uint32_t>(ch))) return false;
            }
        }

        return ok_;
    }

    template <typename T>
    bool WriteVector(const std::vector<T>& values)
    {
        static_assert(BinaryDetail::IsSupportedPrimitive<T>, "WriteVector<T> supports primitive T only");

        if (!WriteUInt32(CheckedSize32(values.size()))) return false;

        for (const auto& v : values)
            if (!WritePrimitive<T>(v)) return false;

        return ok_;
    }

    bool WriteStringVector(const std::vector<std::string>& values)
    {
        if (!WriteUInt32(CheckedSize32(values.size()))) return false;

        for (const auto& v : values)
            if (!WriteString(v)) return false;

        return ok_;
    }

    template <typename T, size_t N>
    bool WriteArray(const std::array<T, N>& values)
    {
        static_assert(BinaryDetail::IsSupportedPrimitive<T>, "WriteArray<T> supports primitive T only");

        for (const auto& v : values)
            if (!WritePrimitive<T>(v)) return false;

        return ok_;
    }

#ifdef _AFX
    bool WriteCStringA(const CStringA& v)
    {
        return WriteString(std::string_view(v.GetString(), static_cast<size_t>(v.GetLength())));
    }

    bool WriteCStringW(const CStringW& v)
    {
        return WriteWString(std::wstring_view(v.GetString(), static_cast<size_t>(v.GetLength())));
    }

    bool WriteCString(const CString& v)
    {
#ifdef UNICODE
        return WriteCStringW(v);
#else
        return WriteCStringA(v);
#endif
    }
#endif

private:
    bool WriteByte(uint8_t v)
    {
        if (!ok_) return false;
        out_.push_back(v);
        return true;
    }

    bool Append(const void* data, size_t size)
    {
        if (!ok_) return false;

        if (data == nullptr && size != 0)
        {
            ok_ = false;
            return false;
        }

        const auto* p = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), p, p + size);
        return true;
    }

    uint32_t CheckedSize32(size_t size)
    {
        if (size > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
        {
            ok_ = false;
            return 0;
        }

        return static_cast<uint32_t>(size);
    }

private:
    std::vector<uint8_t>& out_;
    BinaryEndian endian_;
    bool ok_ = true;
};

class FOCUSTRANSPORT_API BinaryReader final
{
public:
    explicit BinaryReader(std::span<const uint8_t> input, BinaryEndian endian = BinaryDetail::DefaultEndian)
        : input_(input), endian_(endian)
    {
    }

    bool Ok() const noexcept { return ok_; }
    bool End() const noexcept { return offset_ == input_.size(); }
    size_t Remaining() const noexcept { return input_.size() - offset_; }
    size_t Offset() const noexcept { return offset_; }

    bool ReadBool(bool& v)
    {
        uint8_t raw{};
        if (!ReadUInt8(raw)) return false;
        v = raw != 0;
        return true;
    }

    bool ReadInt8(int8_t& v)
    {
        uint8_t raw{};
        if (!ReadUInt8(raw)) return false;
        v = static_cast<int8_t>(raw);
        return true;
    }

    bool ReadUInt8(uint8_t& v) { return ReadRaw(&v, sizeof(v)); }

    bool ReadInt16(int16_t& v)
    {
        uint16_t raw{};
        if (!ReadUInt16(raw)) return false;
        v = static_cast<int16_t>(raw);
        return true;
    }

    bool ReadUInt16(uint16_t& v)
    {
        uint16_t raw{};
        if (!ReadRaw(&raw, sizeof(raw))) return false;
        v = BinaryDetail::FromWire(raw, endian_);
        return true;
    }

    bool ReadInt32(int32_t& v)
    {
        uint32_t raw{};
        if (!ReadUInt32(raw)) return false;
        v = static_cast<int32_t>(raw);
        return true;
    }

    bool ReadUInt32(uint32_t& v)
    {
        uint32_t raw{};
        if (!ReadRaw(&raw, sizeof(raw))) return false;
        v = BinaryDetail::FromWire(raw, endian_);
        return true;
    }

    bool ReadInt64(int64_t& v)
    {
        uint64_t raw{};
        if (!ReadUInt64(raw)) return false;
        v = static_cast<int64_t>(raw);
        return true;
    }

    bool ReadUInt64(uint64_t& v)
    {
        uint64_t raw{};
        if (!ReadRaw(&raw, sizeof(raw))) return false;
        v = BinaryDetail::FromWire(raw, endian_);
        return true;
    }

    bool ReadFloat(float& v)
    {
        uint32_t bits{};
        if (!ReadUInt32(bits)) return false;
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }

    bool ReadDouble(double& v)
    {
        uint64_t bits{};
        if (!ReadUInt64(bits)) return false;
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }

    template <typename T>
    bool ReadPrimitive(T& v)
    {
        static_assert(BinaryDetail::IsSupportedPrimitive<T>, "Unsupported primitive type");

        if constexpr (std::is_same_v<T, bool>) return ReadBool(v);
        else if constexpr (std::is_same_v<T, int8_t>) return ReadInt8(v);
        else if constexpr (std::is_same_v<T, uint8_t>) return ReadUInt8(v);
        else if constexpr (std::is_same_v<T, int16_t>) return ReadInt16(v);
        else if constexpr (std::is_same_v<T, uint16_t>) return ReadUInt16(v);
        else if constexpr (std::is_same_v<T, int32_t>) return ReadInt32(v);
        else if constexpr (std::is_same_v<T, uint32_t>) return ReadUInt32(v);
        else if constexpr (std::is_same_v<T, int64_t>) return ReadInt64(v);
        else if constexpr (std::is_same_v<T, uint64_t>) return ReadUInt64(v);
        else if constexpr (std::is_same_v<T, float>) return ReadFloat(v);
        else if constexpr (std::is_same_v<T, double>) return ReadDouble(v);
    }

    bool ReadBytes(std::vector<uint8_t>& v, uint32_t maxBytes = DefaultMaxBlobBytes)
    {
        std::span<const uint8_t> view;
        if (!ReadBytesView(view, maxBytes)) return false;
        v.assign(view.begin(), view.end());
        return true;
    }

    bool ReadBytesView(std::span<const uint8_t>& v, uint32_t maxBytes = DefaultMaxBlobBytes)
    {
        uint32_t size{};
        if (!ReadUInt32(size)) return false;

        if (size > maxBytes || Remaining() < size)
        {
            ok_ = false;
            return false;
        }

        v = std::span<const uint8_t>(input_.data() + offset_, size);
        offset_ += size;
        return true;
    }

    bool ReadString(std::string& v, uint32_t maxBytes = DefaultMaxStringBytes)
    {
        std::string_view view;
        if (!ReadStringView(view, maxBytes)) return false;
        v.assign(view.data(), view.size());
        return true;
    }

    bool ReadStringView(std::string_view& v, uint32_t maxBytes = DefaultMaxStringBytes)
    {
        std::span<const uint8_t> bytes;
        if (!ReadBytesView(bytes, maxBytes)) return false;

        v = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    bool ReadWString(std::wstring& v, uint32_t maxChars = DefaultMaxStringBytes / sizeof(wchar_t))
    {
        uint32_t chars{};
        if (!ReadUInt32(chars)) return false;

        if (chars > maxChars)
        {
            ok_ = false;
            return false;
        }

        v.clear();
        v.reserve(chars);

        for (uint32_t i = 0; i < chars; ++i)
        {
            if constexpr (sizeof(wchar_t) == 2)
            {
                uint16_t ch{};
                if (!ReadUInt16(ch)) return false;
                v.push_back(static_cast<wchar_t>(ch));
            }
            else
            {
                uint32_t ch{};
                if (!ReadUInt32(ch)) return false;
                v.push_back(static_cast<wchar_t>(ch));
            }
        }

        return true;
    }

    template <typename T>
    bool ReadVector(std::vector<T>& values, uint32_t maxElements = DefaultMaxVectorElements)
    {
        static_assert(BinaryDetail::IsSupportedPrimitive<T>, "ReadVector<T> supports primitive T only");

        uint32_t count{};
        if (!ReadUInt32(count)) return false;

        if (count > maxElements)
        {
            ok_ = false;
            return false;
        }

        values.clear();
        values.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            T value{};
            if (!ReadPrimitive<T>(value)) return false;
            values.push_back(value);
        }

        return true;
    }

    bool ReadStringVector(std::vector<std::string>& values, uint32_t maxElements = DefaultMaxVectorElements)
    {
        uint32_t count{};
        if (!ReadUInt32(count)) return false;

        if (count > maxElements)
        {
            ok_ = false;
            return false;
        }

        values.clear();
        values.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            std::string value;
            if (!ReadString(value)) return false;
            values.push_back(std::move(value));
        }

        return true;
    }

    template <typename T, size_t N>
    bool ReadArray(std::array<T, N>& values)
    {
        static_assert(BinaryDetail::IsSupportedPrimitive<T>, "ReadArray<T> supports primitive T only");

        for (auto& value : values)
            if (!ReadPrimitive<T>(value)) return false;

        return true;
    }

#ifdef _AFX
    bool ReadCStringA(CStringA& v, uint32_t maxBytes = DefaultMaxStringBytes)
    {
        std::string s;
        if (!ReadString(s, maxBytes)) return false;
        v = s.c_str();
        return true;
    }

    bool ReadCStringW(CStringW& v, uint32_t maxChars = DefaultMaxStringBytes / sizeof(wchar_t))
    {
        std::wstring s;
        if (!ReadWString(s, maxChars)) return false;
        v = s.c_str();
        return true;
    }

    bool ReadCString(CString& v)
    {
#ifdef UNICODE
        return ReadCStringW(v);
#else
        return ReadCStringA(v);
#endif
    }
#endif

private:
    bool ReadRaw(void* dest, size_t size)
    {
        if (!ok_) return false;

        if (dest == nullptr && size != 0)
        {
            ok_ = false;
            return false;
        }

        if (Remaining() < size)
        {
            ok_ = false;
            return false;
        }

        if (size != 0)
            std::memcpy(dest, input_.data() + offset_, size);

        offset_ += size;
        return true;
    }

private:
    static constexpr uint32_t DefaultMaxStringBytes = 16u * 1024u * 1024u;
    static constexpr uint32_t DefaultMaxBlobBytes = 64u * 1024u * 1024u;
    static constexpr uint32_t DefaultMaxVectorElements = 4u * 1024u * 1024u;

    std::span<const uint8_t> input_;
    BinaryEndian endian_;
    size_t offset_ = 0;
    bool ok_ = true;
};

class FOCUSTRANSPORT_API IBinarySerializable
{
public:
    virtual ~IBinarySerializable() = default;

    virtual uint32_t BinaryTypeId() const noexcept = 0;
    virtual uint32_t BinaryVersion() const noexcept = 0;

    virtual bool SerializeBinary(BinaryWriter& writer) const = 0;
    virtual bool DeserializeBinary(BinaryReader& reader, uint32_t version) = 0;

    bool SerializeBinary(std::vector<uint8_t>& out) const
    {
        BinaryWriter writer(out);
        writer.WriteUInt32(BinaryTypeId());
        writer.WriteUInt32(BinaryVersion());

        if (!SerializeBinary(writer))
            return false;

        return writer.Ok();
    }

    bool DeserializeBinary(std::span<const uint8_t> in)
    {
        BinaryReader reader(in);

        uint32_t typeId{};
        uint32_t version{};

        if (!reader.ReadUInt32(typeId) || !reader.ReadUInt32(version))
            return false;

        if (typeId != BinaryTypeId())
            return false;

        if (!DeserializeBinary(reader, version))
            return false;

        return reader.Ok() && reader.End();
    }
};

// Add these as member methods in FocusMessageV2::Message:
/*
bool AddOpaqueSerializable(FieldId id, std::string_view name, const IBinarySerializable& object)
{
    std::vector<uint8_t> bytes;
    if (!object.SerializeBinary(bytes))
        return false;

    return AddOpaque(id, name, bytes.data(), bytes.size());
}

bool GetOpaqueSerializable(FieldId id, IBinarySerializable& object) const
{
    std::span<const uint8_t> bytes;
    if (!TryGetOpaqueView(id, bytes))
        return false;

    return object.DeserializeBinary(bytes);
}

template <typename T>
bool AddOpaqueSerializable(FieldId id, std::string_view name, const T& object)
{
    static_assert(std::is_base_of_v<IBinarySerializable, T>);
    return AddOpaqueSerializable(id, name, static_cast<const IBinarySerializable&>(object));
}

template <typename T>
bool GetOpaqueSerializable(FieldId id, T& object) const
{
    static_assert(std::is_base_of_v<IBinarySerializable, T>);
    return GetOpaqueSerializable(id, static_cast<IBinarySerializable&>(object));
}
*/
}
