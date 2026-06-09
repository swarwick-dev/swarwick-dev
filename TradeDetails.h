
#pragma once

#include "FocusBinarySerializable.h"

#include <cstdint>
#include <string>
#include <vector>

namespace FocusTransport::Examples
{
class TradeDetails final : public FocusTransport::IBinarySerializable
{
public:
    int32_t TradeId{};
    std::string Symbol;
    double Price{};
    uint32_t Quantity{};
    bool IsBuy{};
    std::vector<int32_t> RouteIds;

    static constexpr uint32_t TypeId = 0x54524431u; // "TRD1"
    static constexpr uint32_t Version1 = 1;
    static constexpr uint32_t Version2 = 2;

    uint32_t BinaryTypeId() const noexcept override
    {
        return TypeId;
    }

    uint32_t BinaryVersion() const noexcept override
    {
        return Version2;
    }

    bool SerializeBinary(FocusTransport::BinaryWriter& writer) const override
    {
        writer.WriteInt32(TradeId);
        writer.WriteString(Symbol);
        writer.WriteDouble(Price);
        writer.WriteUInt32(Quantity);
        writer.WriteBool(IsBuy);

        // Added in v2
        writer.WriteVector<int32_t>(RouteIds);

        return writer.Ok();
    }

    bool DeserializeBinary(FocusTransport::BinaryReader& reader, uint32_t version) override
    {
        if (version != Version1 && version != Version2)
            return false;

        if (!reader.ReadInt32(TradeId)) return false;
        if (!reader.ReadString(Symbol)) return false;
        if (!reader.ReadDouble(Price)) return false;
        if (!reader.ReadUInt32(Quantity)) return false;
        if (!reader.ReadBool(IsBuy)) return false;

        if (version >= Version2)
        {
            if (!reader.ReadVector<int32_t>(RouteIds)) return false;
        }
        else
        {
            RouteIds.clear();
        }

        return reader.Ok();
    }
};
}
