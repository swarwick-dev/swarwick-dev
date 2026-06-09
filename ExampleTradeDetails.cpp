
#include "FocusMessageV2.h"
#include "FocusBinarySerializable.h"
#include "TradeDetails.h"

#include <iostream>
#include <vector>

// This sample assumes you have added the AddOpaqueSerializable/GetOpaqueSerializable
// methods from FocusBinarySerializable.h comments into FocusMessageV2::Message.

int main()
{
    using namespace FocusTransport;
    using namespace FocusTransport::Examples;

    TradeDetails trade;
    trade.TradeId = 12345;
    trade.Symbol = "VOD.L";
    trade.Price = 72.50;
    trade.Quantity = 1000;
    trade.IsBuy = true;
    trade.RouteIds = {10, 20, 30};

    Message msg;
    msg.ReserveFields(4);

    msg.AddString(1, "MessageType", "TradeDetails");
    msg.AddInt32(2, "RequestId", 999);
    msg.AddOpaqueSerializable(3, "Trade", trade);

    std::vector<uint8_t> natsPayload;
    msg.SerializeInto(natsPayload);

    // natsConnection_Publish(conn, subject.c_str(), natsPayload.data(), natsPayload.size());

    DeserializeResult result{};
    Message received = Message::Deserialize(natsPayload.data(), natsPayload.size(), &result);

    if (result != DeserializeResult::Ok)
    {
        std::cerr << "Deserialize failed\n";
        return 1;
    }

    TradeDetails decoded;
    if (!received.GetOpaqueSerializable(3, decoded))
    {
        std::cerr << "TradeDetails decode failed\n";
        return 1;
    }

    std::cout << "TradeId: " << decoded.TradeId << "\n";
    std::cout << "Symbol: " << decoded.Symbol << "\n";
    std::cout << "Price: " << decoded.Price << "\n";
    std::cout << "Quantity: " << decoded.Quantity << "\n";
    std::cout << "IsBuy: " << decoded.IsBuy << "\n";
    std::cout << "RouteIds: ";

    for (auto id : decoded.RouteIds)
        std::cout << id << " ";

    std::cout << "\n";
    return 0;
}
