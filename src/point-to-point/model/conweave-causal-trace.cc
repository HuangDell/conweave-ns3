#include "conweave-causal-trace.h"

#include <cstdio>
#include <inttypes.h>
#include <unordered_map>
#include "ns3/abort.h"
#include "ns3/conweave-routing.h"
#include "ns3/settings.h"
#include "ns3/simulator.h"

namespace ns3 {
namespace {
FILE* output = nullptr;
uint64_t eventId = 0, voqId = 0;
struct Row {
    uint32_t node = 0, sip = 0, dip = 0, sport = 0, dport = 0, pg = 0;
    uint32_t seq = 0, bytes = 0, epoch = 0, phase = 0, flag = 0, path = 0;
    uint64_t uid = 0, tx = 0, tail = 0, voq = 0, deadline = 0, p0tx = 0, p0rx = 0;
    const char* reason = "none";
};
std::unordered_map<uint64_t, Row> queues;

void Write(const char* event, const Row& r, uint64_t value = 0, uint64_t aux = 0) {
    fprintf(output,
            "%" PRIu64 ",%" PRIu64 ",%s,%u,%u,%u,%u,%u,%u,%u,%u,%" PRIu64
            ",%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 "\n",
            ++eventId, static_cast<uint64_t>(Simulator::Now().GetNanoSeconds()), event,
            r.node, r.sip, r.dip, r.sport, r.dport, r.pg, r.seq, r.bytes, r.uid,
            r.epoch, r.phase, r.flag, r.path, r.tx, r.tail, r.voq, r.deadline,
            r.p0tx, r.p0rx, r.reason, value, aux);
}

bool Read(uint32_t node, Ptr<const ns3::Packet> packet, const CustomHeader& h, Row& r) {
    if (h.l3Prot != 0x11) return false;
    ConWeaveDataTag tag;
    if (!packet->PeekPacketTag(tag)) return false;  // excludes intra-ToR data
    r.node = node; r.sip = h.sip; r.dip = h.dip;
    r.sport = h.udp.sport; r.dport = h.udp.dport; r.pg = h.udp.pg;
    r.seq = h.udp.seq; r.bytes = packet->GetSize() - h.GetSerializedSize();
    r.uid = packet->GetUid(); r.epoch = tag.GetEpoch(); r.phase = tag.GetPhase();
    r.flag = tag.GetFlagData(); r.path = tag.GetPathId();
    r.tx = tag.GetTimestampTx(); r.tail = tag.GetTimestampTail();
    return true;
}
}  // namespace

bool ConWeaveCausalTrace::Enabled() { return output != nullptr; }

bool ConWeaveCausalTrace::Open(const std::string& path) {
    Close();
    output = fopen(path.c_str(), "w");
    if (!output) return false;
    eventId = voqId = 0;
    fprintf(output, "event_id,time_ns,event,node,src_ip,dst_ip,sport,dport,pg,seq,bytes,uid,"
                    "epoch,phase,flag,path,tx_ns,tail_ns,voq_id,deadline_ns,phase0_tx_ns,"
                    "phase0_rx_ns,reason,value,aux\n");
    Write("trace_open", Row());
    return true;
}

void ConWeaveCausalTrace::Close() {
    if (!output) return;
    Write("trace_close", Row(), queues.size(), eventId);
    bool failed = ferror(output) != 0;
    failed = (fclose(output) != 0) || failed;
    output = nullptr;
    queues.clear();
    NS_ABORT_MSG_IF(failed, "ConWeave causal trace write failed");
}

void ConWeaveCausalTrace::Packet(const char* event, uint32_t node,
                                Ptr<const ns3::Packet> packet, const CustomHeader& header,
                                uint64_t voq, uint64_t value, uint64_t aux, const char* reason) {
    if (!output) return;
    Row r;
    if (!Read(node, packet, header, r)) return;
    r.voq = voq; r.reason = reason;
    Write(event, r, value, aux);
}

void ConWeaveCausalTrace::Wire(uint32_t node, Ptr<const ns3::Packet> packet) {
    if (!output) return;
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(h);
    if (h.l3Prot != 0x11) return;
    auto dst = Settings::hostIp2SwitchId.find(h.dip);
    if (dst != Settings::hostIp2SwitchId.end() && dst->second == node)
        Packet("downlink_tx", node, packet, h);
}

void ConWeaveCausalTrace::Drop(uint32_t node, Ptr<const ns3::Packet> packet, const char* reason) {
    if (!output) return;
    CustomHeader h(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(h);
    Packet("drop", node, packet, h, 0, 0, 0, reason);
}

void ConWeaveCausalTrace::Sender(const char* event, uint32_t node, Ptr<RdmaQueuePair> qp,
                                uint64_t value, uint64_t aux) {
    if (!output) return;
    auto src = Settings::hostIp2SwitchId.find(qp->sip.Get());
    auto dst = Settings::hostIp2SwitchId.find(qp->dip.Get());
    if (src == Settings::hostIp2SwitchId.end() || dst == Settings::hostIp2SwitchId.end() ||
        src->second == dst->second) return;
    Row r;
    r.node = node; r.sip = qp->sip.Get(); r.dip = qp->dip.Get();
    r.sport = qp->sport; r.dport = qp->dport; r.pg = qp->m_pg;
    r.seq = qp->snd_nxt;
    Write(event, r, value, aux);
}

uint64_t ConWeaveCausalTrace::CreateVoq(uint32_t node, Ptr<const ns3::Packet> packet,
                                       const CustomHeader& header, uint64_t deadline,
                                       uint64_t phase0Tx, uint64_t phase0Rx, const char* reason) {
    if (!output) return 0;
    Row r;
    if (!Read(node, packet, header, r)) return 0;
    r.voq = ++voqId; r.deadline = deadline;
    r.p0tx = phase0Tx; r.p0rx = phase0Rx; r.reason = reason;
    queues.emplace(r.voq, r);
    Write("voq_create", r);
    return r.voq;
}

void ConWeaveCausalTrace::UpdateVoq(uint64_t voq, uint64_t deadline, uint64_t phase0Tx,
                                   uint64_t phase0Rx, const char* reason, uint32_t packets,
                                   uint32_t triggerEpoch) {
    if (!output) return;
    auto& r = queues.at(voq);
    r.deadline = deadline; r.p0tx = phase0Tx; r.p0rx = phase0Rx; r.reason = reason;
    Write("voq_update", r, packets, triggerEpoch);
}

void ConWeaveCausalTrace::Voq(const char* event, uint64_t voq, uint32_t packets) {
    if (output) Write(event, queues.at(voq), packets);
}

void ConWeaveCausalTrace::DeleteVoq(uint64_t voq) {
    if (output) queues.erase(voq);
}
}  // namespace ns3
