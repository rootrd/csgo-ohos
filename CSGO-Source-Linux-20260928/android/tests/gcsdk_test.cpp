// Round-trip GC protobuf messages through the public API without a GC service.
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include "gcsdk/gcclientsdk.h"
#include "tier1/keyvalues.h"

using namespace GCSDK;
extern void EmitJSONString(CUtlBuffer &buffer, const char *value);

static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "GCSDK_FAIL: %s\n", message); std::exit(1); }
}

class Sender : public CProtoBufMsgBase::IProtoBufSendHandler {
public:
    std::vector<uint8> data;
    int calls = 0;
    bool BAsyncSend(MsgType_t, const uint8 *bytes, uint32 size) override {
        ++calls;
        data.assign(bytes, bytes + size);
        return true;
    }
};

static CProtoBufNetPacket *packet(const std::vector<uint8> &bytes) {
    auto *raw = new CNetPacket;
    raw->Init(bytes.size(), bytes.data());
    auto *result = new CProtoBufNetPacket(raw, static_cast<GCProtoBufMsgSrc>(0), CSteamID(), 0, 0, 1001);
    raw->Release();
    return result;
}

int main() {
    {
        KeyValues::AutoDelete empty("LanSearch");
        CUtlBuffer wire;
        require(empty->WriteAsBinary(wire), "serialize empty LAN search");
        const unsigned char expected[] = {KeyValues::TYPE_NONE, 'L', 'a', 'n', 'S', 'e', 'a', 'r', 'c', 'h', 0,
                                         KeyValues::TYPE_NUMTYPES, KeyValues::TYPE_NUMTYPES};
        require(wire.TellPut() == int(sizeof(expected)) && !std::memcmp(wire.Base(), expected, sizeof(expected)),
                "empty KeyValues sections preserve wire terminators");
        KeyValues::AutoDelete tree("root"), decoded("decoded");
        tree->FindKey("empty", true);
        tree->SetInt("value", 42);
        tree->SetWString("message", L"测试文本");
        CUtlBuffer nested;
        require(tree->WriteAsBinary(nested) && decoded->ReadAsBinary(nested), "nested KeyValues round trip");
        require(decoded->FindKey("empty") && decoded->GetInt("value") == 42
                && !std::wcscmp(decoded->GetWString("message"), L"测试文本"),
                "empty children, siblings and 16-bit wire characters survive on wchar_t32 platforms");
    }
    Sender sender;
    {
        CProtoBufMsg<CMsgProtoBufHeader> outgoing(1001);
        outgoing.SetJobIDSource(12345);
        outgoing.Body().set_error_message("payload");
        require(outgoing.BAsyncSend(sender), "serialize message");
        auto *received = packet(sender.data);
        require(received->IsValid(), "parse packet header");
        CProtoBufMsg<CMsgProtoBufHeader> incoming;
        require(incoming.InitFromPacket(received), "parse message body");
        require(incoming.GetJobIDSource() == 12345 && incoming.Body().error_message() == "payload",
                "header and body survive round trip");
        // incoming now holds the only reference. Rebinding must acquire the
        // new reference before releasing the old one, even for the same packet.
        received->Release();
        require(incoming.InitFromPacket(received) && incoming.Body().error_message() == "payload",
                "rebind the same packet without freeing it");
    }
    {
        CProtoBufMsg<CMsgProtoBufHeader> reused(1001);
        require(reused.GetJobIDSource() == k_GIDNil && !reused.Body().has_error_message(),
                "pooled header and body are reset");
    }
    CMsgProtoBufHeader header;
    const uint8 byte = 0;
    const int sent = sender.calls;
    require(!CProtoBufMsgBase::BAsyncSendWithPreSerializedBody(sender, 1001, header, &byte, 0xffffffffu),
            "reject size overflow before copying");
    require(!CProtoBufMsgBase::BAsyncSendWithPreSerializedBody(sender, 1001, header, nullptr, 1),
            "reject missing payload");
    require(sender.calls == sent, "invalid messages never reach the transport");
    sender.data.resize(sizeof(ProtoBufMsgHeader_t) - 1);
    auto *truncated = packet(sender.data);
    require(!truncated->IsValid(), "reject truncated fixed header");
    truncated->Release();
    const char expected[] = "\"line\\n\\\"\\\\\\u0001\"";
    for (int flags : {0, int(CUtlBuffer::TEXT_BUFFER)}) {
        CUtlBuffer json(0, 0, flags);
        EmitJSONString(json, "line\n\"\\\x01");
        require(json.TellPut() == int(sizeof(expected) - 1)
                && !std::memcmp(json.Base(), expected, sizeof(expected) - 1),
                "JSON escaping produces bytes without interior terminators in binary or text buffers");
    }
    std::puts("GCSDK_PASS: KeyValues/protobuf round trips, packet ownership, pool reset, bounds, JSON escaping");
    return 0;
}
