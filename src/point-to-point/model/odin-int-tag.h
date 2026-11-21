#ifndef ODIN_INT_TAG_H
#define ODIN_INT_TAG_H

#include "ns3/int-header.h"
#include "ns3/tag.h"

namespace ns3 {

// Simulation audit metadata. This tag is never serialized onto the link.
class OdinIntTag : public Tag {
public:
    static TypeId GetTypeId() {
        static TypeId tid = TypeId("ns3::OdinIntTag").SetParent<Tag>().AddConstructor<OdinIntTag>();
        return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return sizeof(measured_ns); }
    void Serialize(TagBuffer buffer) const override {
        for (uint64_t ns : measured_ns) buffer.WriteU64(ns);
    }
    void Deserialize(TagBuffer buffer) override {
        for (auto& ns : measured_ns) ns = buffer.ReadU64();
    }
    void Print(std::ostream&) const override {}

    uint64_t measured_ns[IntHeader::maxHop] = {};
};

} // namespace ns3

#endif
