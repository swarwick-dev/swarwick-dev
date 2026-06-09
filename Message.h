#pragma once

// FocusMessageV2.h
//
// High-performance binary field message for TIBRV-style message replacement over NATS.
// C++20, Windows-focused, DLL-friendly.
//
// Design goals:
// - Binary NATS payload, no JSON/Base64.
// - Named and/or numeric field identifiers.
// - Typed fields: signed/unsigned ints, floats, double, string, XML, opaque binary.
// - Zero-copy reads via FieldView and payload spans.
// - Hot-path TryGet methods that avoid exceptions.
// - Explicit wire endian handling.
// - Optional internal thread-safety with shared_mutex.
// - Buffer reuse via SerializeInto.
// - Stable C++ API suitable to wrap inside a DLL.
//
// Notes:
// - This class is intended to be used as:
//      Message msg;
//      msg.AddInt32(1001, "Quantity", 42);
//      msg.AddString(1002, "Symbol", "ABC");
//      msg.SerializeInto(buffer);
//      ... send buffer over NATS ...
//      Message rx = Message::Deserialize(bytes, size);
//      int32_t qty{};
//      rx.TryGetInt32(1001, qty);
//
// - Opaque struct support is only safe for trivially-copyable structs.
// - Wire format uses fixed endian integer/float encoding.
// - Duplicate field names/ids are rejected by default; this is usually safer for a wrapper API.
//   If you need exact TIBRV duplicate field behavior, change duplicate policy or store multi-maps.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef FOCUSTRANSPORT_API
    #define FOCUSTRANSPORT_API
#endif

namespace FocusTransport
{
    using FieldId = uint32_t;

    inline constexpr FieldId InvalidFieldId = 0;

    enum class FieldType : uint8_t
    {
        Int8      = 1,
        UInt8     = 2,
        Int16     = 3,
        UInt16    = 4,
        Int32     = 5,
        UInt32    = 6,
        Int64     = 7,
        UInt64    = 8,
        Float32   = 9,
        Float64   = 10,
        String    = 11,
        Xml       = 12,
        Opaque    = 13
    };

    enum class ThreadSafetyMode : uint8_t
    {
        None,
        SharedMutex
    };

    enum class DuplicatePolicy : uint8_t
    {
        Reject,
        Replace
    };

    enum class WireEndian : uint8_t
    {
        Little = 1,
        Big    = 2
    };

    enum class DeserializeResult : uint8_t
    {
        Ok,
        NullData,
        TooSmall,
        BadMagic,
        UnsupportedVersion,
        Truncated,
        InvalidFieldType,
        DuplicateField,
        SizeLimitExceeded
    };

    class FOCUSTRANSPORT_API MessageError final : public std::runtime_error
    {
    public:
        explicit MessageError(const char* message)
            : std::runtime_error(message)
        {
        }

        explicit MessageError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    namespace Detail
    {
        inline constexpr uint32_t WireMagic = 0x46544D32u; // "FTM2"
        inline constexpr uint16_t WireVersion = 2;
        inline constexpr WireEndian DefaultWireEndian = WireEndian::Little;

        template <typename T>
        [[nodiscard]] constexpr bool IsSupportedNumberV =
            std::is_same_v<T, int8_t>   ||
            std::is_same_v<T, uint8_t>  ||
            std::is_same_v<T, int16_t>  ||
            std::is_same_v<T, uint16_t> ||
            std::is_same_v<T, int32_t>  ||
            std::is_same_v<T, uint32_t> ||
            std::is_same_v<T, int64_t>  ||
            std::is_same_v<T, uint64_t> ||
            std::is_same_v<T, float>    ||
            std::is_same_v<T, double>;

        template <typename T>
        [[nodiscard]] constexpr FieldType FieldTypeOf()
        {
            static_assert(IsSupportedNumberV<T>, "Unsupported numeric field type");

            if constexpr (std::is_same_v<T, int8_t>)   return FieldType::Int8;
            if constexpr (std::is_same_v<T, uint8_t>)  return FieldType::UInt8;
            if constexpr (std::is_same_v<T, int16_t>)  return FieldType::Int16;
            if constexpr (std::is_same_v<T, uint16_t>) return FieldType::UInt16;
            if constexpr (std::is_same_v<T, int32_t>)  return FieldType::Int32;
            if constexpr (std::is_same_v<T, uint32_t>) return FieldType::UInt32;
            if constexpr (std::is_same_v<T, int64_t>)  return FieldType::Int64;
            if constexpr (std::is_same_v<T, uint64_t>) return FieldType::UInt64;
            if constexpr (std::is_same_v<T, float>)    return FieldType::Float32;
            if constexpr (std::is_same_v<T, double>)   return FieldType::Float64;
        }

        [[nodiscard]] inline constexpr bool IsValidFieldType(FieldType type)
        {
            switch (type)
            {
                case FieldType::Int8:
                case FieldType::UInt8:
                case FieldType::Int16:
                case FieldType::UInt16:
                case FieldType::Int32:
                case FieldType::UInt32:
                case FieldType::Int64:
                case FieldType::UInt64:
                case FieldType::Float32:
                case FieldType::Float64:
                case FieldType::String:
                case FieldType::Xml:
                case FieldType::Opaque:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] inline constexpr uint32_t ByteSwap32(uint32_t v)
        {
            return ((v & 0x000000FFu) << 24) |
                   ((v & 0x0000FF00u) << 8)  |
                   ((v & 0x00FF0000u) >> 8)  |
                   ((v & 0xFF000000u) >> 24);
        }

        [[nodiscard]] inline constexpr uint16_t ByteSwap16(uint16_t v)
        {
            return static_cast<uint16_t>(((v & 0x00FFu) << 8) |
                                         ((v & 0xFF00u) >> 8));
        }

        [[nodiscard]] inline constexpr uint64_t ByteSwap64(uint64_t v)
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

        [[nodiscard]] inline constexpr bool NeedSwap(WireEndian wireEndian)
        {
            if constexpr (std::endian::native == std::endian::little)
            {
                return wireEndian == WireEndian::Big;
            }
            else
            {
                return wireEndian == WireEndian::Little;
            }
        }

        [[nodiscard]] inline uint16_t ToWire(uint16_t v, WireEndian endian)
        {
            return NeedSwap(endian) ? ByteSwap16(v) : v;
        }

        [[nodiscard]] inline uint32_t ToWire(uint32_t v, WireEndian endian)
        {
            return NeedSwap(endian) ? ByteSwap32(v) : v;
        }

        [[nodiscard]] inline uint64_t ToWire(uint64_t v, WireEndian endian)
        {
            return NeedSwap(endian) ? ByteSwap64(v) : v;
        }

        [[nodiscard]] inline uint16_t FromWire(uint16_t v, WireEndian endian)
        {
            return ToWire(v, endian);
        }

        [[nodiscard]] inline uint32_t FromWire(uint32_t v, WireEndian endian)
        {
            return ToWire(v, endian);
        }

        [[nodiscard]] inline uint64_t FromWire(uint64_t v, WireEndian endian)
        {
            return ToWire(v, endian);
        }

        template <typename UInt>
        inline void AppendUnsigned(std::vector<uint8_t>& out, UInt value, WireEndian endian)
        {
            static_assert(std::is_unsigned_v<UInt>);
            UInt wire = value;

            if constexpr (sizeof(UInt) == 2)
                wire = static_cast<UInt>(ToWire(static_cast<uint16_t>(value), endian));
            else if constexpr (sizeof(UInt) == 4)
                wire = static_cast<UInt>(ToWire(static_cast<uint32_t>(value), endian));
            else if constexpr (sizeof(UInt) == 8)
                wire = static_cast<UInt>(ToWire(static_cast<uint64_t>(value), endian));

            const auto* p = reinterpret_cast<const uint8_t*>(&wire);
            out.insert(out.end(), p, p + sizeof(UInt));
        }

        inline void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t size)
        {
            if (size == 0)
                return;

            const auto* p = static_cast<const uint8_t*>(data);
            out.insert(out.end(), p, p + size);
        }

        template <typename UInt>
        [[nodiscard]] inline bool ReadUnsigned(const uint8_t*& p, const uint8_t* end, UInt& out, WireEndian endian)
        {
            static_assert(std::is_unsigned_v<UInt>);

            if (p > end || static_cast<size_t>(end - p) < sizeof(UInt))
                return false;

            UInt wire{};
            std::memcpy(&wire, p, sizeof(UInt));
            p += sizeof(UInt);

            if constexpr (sizeof(UInt) == 1)
            {
                out = wire;
            }
            else if constexpr (sizeof(UInt) == 2)
            {
                out = static_cast<UInt>(FromWire(static_cast<uint16_t>(wire), endian));
            }
            else if constexpr (sizeof(UInt) == 4)
            {
                out = static_cast<UInt>(FromWire(static_cast<uint32_t>(wire), endian));
            }
            else if constexpr (sizeof(UInt) == 8)
            {
                out = static_cast<UInt>(FromWire(static_cast<uint64_t>(wire), endian));
            }

            return true;
        }

        template <typename T>
        inline void EncodeNumberPayload(std::vector<uint8_t>& out, T value, WireEndian endian)
        {
            static_assert(IsSupportedNumberV<T>);

            if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>)
            {
                out.push_back(static_cast<uint8_t>(value));
            }
            else if constexpr (std::is_same_v<T, int16_t>)
            {
                AppendUnsigned(out, static_cast<uint16_t>(value), endian);
            }
            else if constexpr (std::is_same_v<T, uint16_t>)
            {
                AppendUnsigned(out, value, endian);
            }
            else if constexpr (std::is_same_v<T, int32_t>)
            {
                AppendUnsigned(out, static_cast<uint32_t>(value), endian);
            }
            else if constexpr (std::is_same_v<T, uint32_t>)
            {
                AppendUnsigned(out, value, endian);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                AppendUnsigned(out, static_cast<uint64_t>(value), endian);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                AppendUnsigned(out, value, endian);
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                static_assert(sizeof(float) == sizeof(uint32_t));
                uint32_t bits{};
                std::memcpy(&bits, &value, sizeof(bits));
                AppendUnsigned(out, bits, endian);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                static_assert(sizeof(double) == sizeof(uint64_t));
                uint64_t bits{};
                std::memcpy(&bits, &value, sizeof(bits));
                AppendUnsigned(out, bits, endian);
            }
        }

        template <typename T>
        [[nodiscard]] inline bool DecodeNumberPayload(std::span<const uint8_t> payload, T& out, WireEndian endian)
        {
            static_assert(IsSupportedNumberV<T>);

            if (payload.size() != sizeof(T))
                return false;

            const uint8_t* p = payload.data();
            const uint8_t* end = p + payload.size();

            if constexpr (std::is_same_v<T, int8_t>)
            {
                out = static_cast<int8_t>(*p);
                return true;
            }
            else if constexpr (std::is_same_v<T, uint8_t>)
            {
                out = *p;
                return true;
            }
            else if constexpr (std::is_same_v<T, int16_t>)
            {
                uint16_t v{};
                if (!ReadUnsigned(p, end, v, endian)) return false;
                out = static_cast<int16_t>(v);
                return true;
            }
            else if constexpr (std::is_same_v<T, uint16_t>)
            {
                uint16_t v{};
                if (!ReadUnsigned(p, end, v, endian)) return false;
                out = v;
                return true;
            }
            else if constexpr (std::is_same_v<T, int32_t>)
            {
                uint32_t v{};
                if (!ReadUnsigned(p, end, v, endian)) return false;
                out = static_cast<int32_t>(v);
                return true;
            }
            else if constexpr (std::is_same_v<T, uint32_t>)
            {
                uint32_t v{};
                if (!ReadUnsigned(p, end, v, endian)) return false;
                out = v;
                return true;
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                uint64_t v{};
                if (!ReadUnsigned(p, end, v, endian)) return false;
                out = static_cast<int64_t>(v);
                return true;
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                uint64_t v{};
                if (!ReadUnsigned(p, end, v, endian)) return false;
                out = v;
                return true;
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                uint32_t bits{};
                if (!ReadUnsigned(p, end, bits, endian)) return false;
                std::memcpy(&out, &bits, sizeof(out));
                return true;
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                uint64_t bits{};
                if (!ReadUnsigned(p, end, bits, endian)) return false;
                std::memcpy(&out, &bits, sizeof(out));
                return true;
            }
        }
    }

    struct FOCUSTRANSPORT_API FieldView final
    {
        FieldId id = InvalidFieldId;
        std::string_view name;
        FieldType type = FieldType::Opaque;
        std::span<const uint8_t> payload;

        [[nodiscard]] bool Empty() const noexcept
        {
            return payload.empty();
        }

        [[nodiscard]] std::string_view AsStringView() const noexcept
        {
            return std::string_view(
                reinterpret_cast<const char*>(payload.data()),
                payload.size());
        }

        [[nodiscard]] std::span<const std::byte> AsByteSpan() const noexcept
        {
            return std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(payload.data()),
                payload.size());
        }
    };

    struct FOCUSTRANSPORT_API MessageLimits final
    {
        uint32_t maxFields = 65535;
        uint32_t maxNameBytes = 65535;
        uint32_t maxPayloadBytes = 64u * 1024u * 1024u;
        uint32_t maxMessageBytes = 256u * 1024u * 1024u;
    };

    class FOCUSTRANSPORT_API Message final
    {
    public:
        Message(
            ThreadSafetyMode threadSafety = ThreadSafetyMode::None,
            DuplicatePolicy duplicatePolicy = DuplicatePolicy::Reject,
            WireEndian wireEndian = Detail::DefaultWireEndian)
            : threadSafety_(threadSafety),
              duplicatePolicy_(duplicatePolicy),
              wireEndian_(wireEndian)
        {
        }

        [[nodiscard]] static Message CreateThreadSafe(
            DuplicatePolicy duplicatePolicy = DuplicatePolicy::Reject,
            WireEndian wireEndian = Detail::DefaultWireEndian)
        {
            return Message(ThreadSafetyMode::SharedMutex, duplicatePolicy, wireEndian);
        }

        // Move-only. Copying a message with mutex state is deliberately avoided.
        Message(const Message&) = delete;
        Message& operator=(const Message&) = delete;

        Message(Message&& other) noexcept
        {
            MoveFrom(std::move(other));
        }

        Message& operator=(Message&& other) noexcept
        {
            if (this != &other)
            {
                Clear();
                MoveFrom(std::move(other));
            }
            return *this;
        }

        void Clear()
        {
            auto lock = WriteLock();
            fields_.clear();
            byId_.clear();
            byName_.clear();
        }

        void ReserveFields(size_t count)
        {
            auto lock = WriteLock();
            fields_.reserve(count);
            byId_.reserve(count);
            byName_.reserve(count);
        }

        [[nodiscard]] size_t FieldCount() const
        {
            auto lock = ReadLock();
            return fields_.size();
        }

        [[nodiscard]] bool Empty() const
        {
            return FieldCount() == 0;
        }

        [[nodiscard]] bool Contains(FieldId id) const
        {
            auto lock = ReadLock();
            return byId_.find(id) != byId_.end();
        }

        [[nodiscard]] bool Contains(std::string_view name) const
        {
            auto lock = ReadLock();
            return byName_.find(std::string(name)) != byName_.end();
        }

        template <typename T>
        bool AddNumber(FieldId id, std::string_view name, T value)
        {
            static_assert(Detail::IsSupportedNumberV<T>);
            std::vector<uint8_t> payload;
            payload.reserve(sizeof(T));
            Detail::EncodeNumberPayload(payload, value, wireEndian_);
            return AddField(id, name, Detail::FieldTypeOf<T>(), std::move(payload));
        }

        bool AddInt8(FieldId id, std::string_view name, int8_t value)       { return AddNumber(id, name, value); }
        bool AddUInt8(FieldId id, std::string_view name, uint8_t value)     { return AddNumber(id, name, value); }
        bool AddInt16(FieldId id, std::string_view name, int16_t value)     { return AddNumber(id, name, value); }
        bool AddUInt16(FieldId id, std::string_view name, uint16_t value)   { return AddNumber(id, name, value); }
        bool AddInt32(FieldId id, std::string_view name, int32_t value)     { return AddNumber(id, name, value); }
        bool AddUInt32(FieldId id, std::string_view name, uint32_t value)   { return AddNumber(id, name, value); }
        bool AddInt64(FieldId id, std::string_view name, int64_t value)     { return AddNumber(id, name, value); }
        bool AddUInt64(FieldId id, std::string_view name, uint64_t value)   { return AddNumber(id, name, value); }
        bool AddFloat(FieldId id, std::string_view name, float value)       { return AddNumber(id, name, value); }
        bool AddDouble(FieldId id, std::string_view name, double value)     { return AddNumber(id, name, value); }

        bool AddString(FieldId id, std::string_view name, std::string_view value)
        {
            return AddRaw(id, name, FieldType::String, value.data(), value.size());
        }

        bool AddXml(FieldId id, std::string_view name, std::string_view value)
        {
            return AddRaw(id, name, FieldType::Xml, value.data(), value.size());
        }

        bool AddOpaque(FieldId id, std::string_view name, const void* data, size_t size)
        {
            return AddRaw(id, name, FieldType::Opaque, data, size);
        }

        bool AddOpaque(FieldId id, std::string_view name, std::span<const uint8_t> bytes)
        {
            return AddOpaque(id, name, bytes.data(), bytes.size());
        }

        template <typename T>
        bool AddOpaqueStruct(FieldId id, std::string_view name, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "Opaque struct must be trivially-copyable");
            return AddOpaque(id, name, &value, sizeof(T));
        }

        [[nodiscard]] std::optional<FieldView> Find(FieldId id) const
        {
            auto lock = ReadLock();

            const auto it = byId_.find(id);
            if (it == byId_.end())
                return std::nullopt;

            return MakeView(fields_[it->second]);
        }

        [[nodiscard]] std::optional<FieldView> Find(std::string_view name) const
        {
            auto lock = ReadLock();

            const auto it = byName_.find(std::string(name));
            if (it == byName_.end())
                return std::nullopt;

            return MakeView(fields_[it->second]);
        }

        [[nodiscard]] std::optional<FieldView> FieldAt(size_t index) const
        {
            auto lock = ReadLock();

            if (index >= fields_.size())
                return std::nullopt;

            return MakeView(fields_[index]);
        }

        template <typename T>
        [[nodiscard]] bool TryGetNumber(FieldId id, T& out) const
        {
            static_assert(Detail::IsSupportedNumberV<T>);

            auto view = Find(id);
            if (!view || view->type != Detail::FieldTypeOf<T>())
                return false;

            return Detail::DecodeNumberPayload<T>(view->payload, out, wireEndian_);
        }

        template <typename T>
        [[nodiscard]] bool TryGetNumber(std::string_view name, T& out) const
        {
            static_assert(Detail::IsSupportedNumberV<T>);

            auto view = Find(name);
            if (!view || view->type != Detail::FieldTypeOf<T>())
                return false;

            return Detail::DecodeNumberPayload<T>(view->payload, out, wireEndian_);
        }

        [[nodiscard]] bool TryGetInt8(FieldId id, int8_t& out) const       { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetUInt8(FieldId id, uint8_t& out) const     { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetInt16(FieldId id, int16_t& out) const     { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetUInt16(FieldId id, uint16_t& out) const   { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetInt32(FieldId id, int32_t& out) const     { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetUInt32(FieldId id, uint32_t& out) const   { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetInt64(FieldId id, int64_t& out) const     { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetUInt64(FieldId id, uint64_t& out) const   { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetFloat(FieldId id, float& out) const       { return TryGetNumber(id, out); }
        [[nodiscard]] bool TryGetDouble(FieldId id, double& out) const     { return TryGetNumber(id, out); }

        [[nodiscard]] bool TryGetStringView(FieldId id, std::string_view& out) const
        {
            auto view = Find(id);
            if (!view || view->type != FieldType::String)
                return false;

            out = view->AsStringView();
            return true;
        }

        [[nodiscard]] bool TryGetXmlView(FieldId id, std::string_view& out) const
        {
            auto view = Find(id);
            if (!view || view->type != FieldType::Xml)
                return false;

            out = view->AsStringView();
            return true;
        }

        [[nodiscard]] bool TryGetOpaqueView(FieldId id, std::span<const uint8_t>& out) const
        {
            auto view = Find(id);
            if (!view || view->type != FieldType::Opaque)
                return false;

            out = view->payload;
            return true;
        }

        template <typename T>
        [[nodiscard]] bool TryGetOpaqueStruct(FieldId id, T& out) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "Opaque struct must be trivially-copyable");

            std::span<const uint8_t> bytes;
            if (!TryGetOpaqueView(id, bytes) || bytes.size() != sizeof(T))
                return false;

            std::memcpy(&out, bytes.data(), sizeof(T));
            return true;
        }

        // Throwing convenience getters. Prefer TryGet* in hot paths.
        [[nodiscard]] int32_t GetInt32(FieldId id) const
        {
            int32_t value{};
            if (!TryGetInt32(id, value))
                throw MessageError("Field not found or wrong type: Int32");
            return value;
        }

        [[nodiscard]] std::string_view GetStringView(FieldId id) const
        {
            std::string_view value;
            if (!TryGetStringView(id, value))
                throw MessageError("Field not found or wrong type: String");
            return value;
        }

        [[nodiscard]] std::span<const uint8_t> GetOpaqueView(FieldId id) const
        {
            std::span<const uint8_t> value;
            if (!TryGetOpaqueView(id, value))
                throw MessageError("Field not found or wrong type: Opaque");
            return value;
        }

        [[nodiscard]] size_t CalculateSerializedSize() const
        {
            auto lock = ReadLock();
            return CalculateSerializedSizeUnlocked();
        }

        [[nodiscard]] std::vector<uint8_t> Serialize() const
        {
            std::vector<uint8_t> out;
            SerializeInto(out);
            return out;
        }

        void SerializeInto(std::vector<uint8_t>& out) const
        {
            auto lock = ReadLock();

            out.clear();
            out.reserve(CalculateSerializedSizeUnlocked());

            // Header:
            // uint32 magic
            // uint16 version
            // uint8  endian marker
            // uint8  reserved
            // uint32 field count

            Detail::AppendUnsigned(out, Detail::WireMagic, wireEndian_);
            Detail::AppendUnsigned(out, Detail::WireVersion, wireEndian_);
            out.push_back(static_cast<uint8_t>(wireEndian_));
            out.push_back(0);
            Detail::AppendUnsigned(out, static_cast<uint32_t>(fields_.size()), wireEndian_);

            for (const auto& field : fields_)
            {
                Detail::AppendUnsigned(out, field.id, wireEndian_);
                Detail::AppendUnsigned(out, static_cast<uint16_t>(field.name.size()), wireEndian_);
                Detail::AppendBytes(out, field.name.data(), field.name.size());
                out.push_back(static_cast<uint8_t>(field.type));
                out.push_back(0); // field flags/reserved for future use
                Detail::AppendUnsigned(out, static_cast<uint32_t>(field.payload.size()), wireEndian_);
                Detail::AppendBytes(out, field.payload.data(), field.payload.size());
            }
        }

        [[nodiscard]] static Message Deserialize(
            const void* data,
            size_t size,
            DeserializeResult* result = nullptr,
            MessageLimits limits = {},
            ThreadSafetyMode threadSafety = ThreadSafetyMode::None,
            DuplicatePolicy duplicatePolicy = DuplicatePolicy::Reject)
        {
            Message msg(threadSafety, duplicatePolicy);

            auto setResult = [&](DeserializeResult r)
            {
                if (result)
                    *result = r;
            };

            if (data == nullptr && size != 0)
            {
                setResult(DeserializeResult::NullData);
                return msg;
            }

            if (size < 12)
            {
                setResult(DeserializeResult::TooSmall);
                return msg;
            }

            if (size > limits.maxMessageBytes)
            {
                setResult(DeserializeResult::SizeLimitExceeded);
                return msg;
            }

            const auto* p = static_cast<const uint8_t*>(data);
            const auto* end = p + size;

            // Magic is always read using default endian first. For same-platform Windows,
            // little-endian is fastest and expected. If this fails, try big-endian magic.
            uint32_t magicLE{};
            {
                const uint8_t* probe = p;
                if (!Detail::ReadUnsigned(probe, end, magicLE, WireEndian::Little))
                {
                    setResult(DeserializeResult::TooSmall);
                    return msg;
                }
            }

            WireEndian wireEndian = WireEndian::Little;
            if (magicLE == Detail::WireMagic)
            {
                wireEndian = WireEndian::Little;
            }
            else
            {
                uint32_t magicBE{};
                const uint8_t* probe = p;
                if (!Detail::ReadUnsigned(probe, end, magicBE, WireEndian::Big))
                {
                    setResult(DeserializeResult::TooSmall);
                    return msg;
                }

                if (magicBE != Detail::WireMagic)
                {
                    setResult(DeserializeResult::BadMagic);
                    return msg;
                }

                wireEndian = WireEndian::Big;
            }

            msg.wireEndian_ = wireEndian;

            uint32_t magic{};
            uint16_t version{};
            uint8_t endianMarker{};
            uint8_t reserved{};
            uint32_t fieldCount{};

            if (!Detail::ReadUnsigned(p, end, magic, wireEndian) ||
                !Detail::ReadUnsigned(p, end, version, wireEndian) ||
                !Detail::ReadUnsigned(p, end, endianMarker, wireEndian) ||
                !Detail::ReadUnsigned(p, end, reserved, wireEndian) ||
                !Detail::ReadUnsigned(p, end, fieldCount, wireEndian))
            {
                setResult(DeserializeResult::Truncated);
                return Message(threadSafety, duplicatePolicy, wireEndian);
            }

            (void)reserved;

            if (magic != Detail::WireMagic)
            {
                setResult(DeserializeResult::BadMagic);
                return Message(threadSafety, duplicatePolicy, wireEndian);
            }

            if (version != Detail::WireVersion)
            {
                setResult(DeserializeResult::UnsupportedVersion);
                return Message(threadSafety, duplicatePolicy, wireEndian);
            }

            if (endianMarker != static_cast<uint8_t>(wireEndian))
            {
                setResult(DeserializeResult::BadMagic);
                return Message(threadSafety, duplicatePolicy, wireEndian);
            }

            if (fieldCount > limits.maxFields)
            {
                setResult(DeserializeResult::SizeLimitExceeded);
                return Message(threadSafety, duplicatePolicy, wireEndian);
            }

            msg.ReserveFields(fieldCount);

            for (uint32_t i = 0; i < fieldCount; ++i)
            {
                Field field;

                uint16_t nameSize{};
                uint8_t rawType{};
                uint8_t fieldFlags{};
                uint32_t payloadSize{};

                if (!Detail::ReadUnsigned(p, end, field.id, wireEndian) ||
                    !Detail::ReadUnsigned(p, end, nameSize, wireEndian))
                {
                    setResult(DeserializeResult::Truncated);
                    return Message(threadSafety, duplicatePolicy, wireEndian);
                }

                if (nameSize > limits.maxNameBytes ||
                    static_cast<size_t>(end - p) < nameSize)
                {
                    setResult(DeserializeResult::Truncated);
                    return Message(threadSafety, duplicatePolicy, wireEndian);
                }

                field.name.assign(reinterpret_cast<const char*>(p), nameSize);
                p += nameSize;

                if (!Detail::ReadUnsigned(p, end, rawType, wireEndian) ||
                    !Detail::ReadUnsigned(p, end, fieldFlags, wireEndian) ||
                    !Detail::ReadUnsigned(p, end, payloadSize, wireEndian))
                {
                    setResult(DeserializeResult::Truncated);
                    return Message(threadSafety, duplicatePolicy, wireEndian);
                }

                (void)fieldFlags;

                field.type = static_cast<FieldType>(rawType);

                if (!Detail::IsValidFieldType(field.type))
                {
                    setResult(DeserializeResult::InvalidFieldType);
                    return Message(threadSafety, duplicatePolicy, wireEndian);
                }

                if (payloadSize > limits.maxPayloadBytes ||
                    static_cast<size_t>(end - p) < payloadSize)
                {
                    setResult(DeserializeResult::Truncated);
                    return Message(threadSafety, duplicatePolicy, wireEndian);
                }

                field.payload.resize(payloadSize);
                if (payloadSize != 0)
                    std::memcpy(field.payload.data(), p, payloadSize);

                p += payloadSize;

                if (!msg.AddFieldUnlocked(std::move(field)))
                {
                    setResult(DeserializeResult::DuplicateField);
                    return Message(threadSafety, duplicatePolicy, wireEndian);
                }
            }

            setResult(DeserializeResult::Ok);
            return msg;
        }

    private:
        struct Field final
        {
            FieldId id = InvalidFieldId;
            std::string name;
            FieldType type = FieldType::Opaque;
            std::vector<uint8_t> payload;
        };

        struct EmptyLock final
        {
            EmptyLock() = default;
        };

        using UniqueLock = std::unique_lock<std::shared_mutex>;
        using SharedLock = std::shared_lock<std::shared_mutex>;

        [[nodiscard]] auto WriteLock() const
        {
            if (threadSafety_ == ThreadSafetyMode::SharedMutex)
                return UniqueLock(mutex_);

            return UniqueLock{};
        }

        [[nodiscard]] auto ReadLock() const
        {
            if (threadSafety_ == ThreadSafetyMode::SharedMutex)
                return SharedLock(mutex_);

            return SharedLock{};
        }

        void MoveFrom(Message&& other) noexcept
        {
            threadSafety_ = other.threadSafety_;
            duplicatePolicy_ = other.duplicatePolicy_;
            wireEndian_ = other.wireEndian_;
            fields_ = std::move(other.fields_);
            byId_ = std::move(other.byId_);
            byName_ = std::move(other.byName_);
        }

        bool AddRaw(FieldId id, std::string_view name, FieldType type, const void* data, size_t size)
        {
            if (data == nullptr && size != 0)
                return false;

            std::vector<uint8_t> payload;
            payload.resize(size);

            if (size != 0)
                std::memcpy(payload.data(), data, size);

            return AddField(id, name, type, std::move(payload));
        }

        bool AddField(FieldId id, std::string_view name, FieldType type, std::vector<uint8_t>&& payload)
        {
            auto lock = WriteLock();

            Field field;
            field.id = id;
            field.name.assign(name.data(), name.size());
            field.type = type;
            field.payload = std::move(payload);

            return AddFieldUnlocked(std::move(field));
        }

        bool AddFieldUnlocked(Field&& field)
        {
            if (field.id == InvalidFieldId && field.name.empty())
                return false;

            if (duplicatePolicy_ == DuplicatePolicy::Reject)
            {
                if (field.id != InvalidFieldId && byId_.find(field.id) != byId_.end())
                    return false;

                if (!field.name.empty() && byName_.find(field.name) != byName_.end())
                    return false;
            }
            else if (duplicatePolicy_ == DuplicatePolicy::Replace)
            {
                size_t existingIndex = std::numeric_limits<size_t>::max();

                if (field.id != InvalidFieldId)
                {
                    auto idIt = byId_.find(field.id);
                    if (idIt != byId_.end())
                        existingIndex = idIt->second;
                }

                if (existingIndex == std::numeric_limits<size_t>::max() && !field.name.empty())
                {
                    auto nameIt = byName_.find(field.name);
                    if (nameIt != byName_.end())
                        existingIndex = nameIt->second;
                }

                if (existingIndex != std::numeric_limits<size_t>::max())
                {
                    const auto& old = fields_[existingIndex];
                    if (old.id != InvalidFieldId)
                        byId_.erase(old.id);
                    if (!old.name.empty())
                        byName_.erase(old.name);

                    fields_[existingIndex] = std::move(field);

                    const auto& replaced = fields_[existingIndex];
                    if (replaced.id != InvalidFieldId)
                        byId_[replaced.id] = existingIndex;
                    if (!replaced.name.empty())
                        byName_[replaced.name] = existingIndex;

                    return true;
                }
            }

            const size_t index = fields_.size();
            fields_.push_back(std::move(field));

            const auto& added = fields_.back();

            if (added.id != InvalidFieldId)
                byId_[added.id] = index;

            if (!added.name.empty())
                byName_[added.name] = index;

            return true;
        }

        [[nodiscard]] static FieldView MakeView(const Field& field)
        {
            return FieldView
            {
                field.id,
                std::string_view(field.name),
                field.type,
                std::span<const uint8_t>(field.payload.data(), field.payload.size())
            };
        }

        [[nodiscard]] size_t CalculateSerializedSizeUnlocked() const
        {
            size_t total = 0;

            total += sizeof(uint32_t); // magic
            total += sizeof(uint16_t); // version
            total += sizeof(uint8_t);  // endian
            total += sizeof(uint8_t);  // reserved
            total += sizeof(uint32_t); // field count

            for (const auto& field : fields_)
            {
                total += sizeof(uint32_t); // id
                total += sizeof(uint16_t); // name length
                total += field.name.size();
                total += sizeof(uint8_t);  // type
                total += sizeof(uint8_t);  // flags/reserved
                total += sizeof(uint32_t); // payload length
                total += field.payload.size();
            }

            return total;
        }

    private:
        ThreadSafetyMode threadSafety_ = ThreadSafetyMode::None;
        DuplicatePolicy duplicatePolicy_ = DuplicatePolicy::Reject;
        WireEndian wireEndian_ = Detail::DefaultWireEndian;

        mutable std::shared_mutex mutex_;

        std::vector<Field> fields_;
        std::unordered_map<FieldId, size_t> byId_;
        std::unordered_map<std::string, size_t> byName_;
    };
}
