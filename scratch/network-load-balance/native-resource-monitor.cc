#include "native-resource-monitor.h"
#include <ns3/custom-header.h>
#include <ns3/flow-id-num-tag.h>
#include <ns3/rdma-hw.h>
#include <ns3/settings.h>
#include <ns3/simulator.h>
#include <ns3/switch-node.h>

namespace nlb {
using namespace ns3;

NativeResourceMonitor::NativeResourceMonitor(const ExperimentConfig& config, SimulationState& state)
    : config_(config), state_(state) {}

bool NativeResourceMonitor::Open() {
    resources_ = fopen(config_.io.native_resource_output_file.c_str(), "w");
    qp_events_ = fopen(config_.io.native_qp_output_file.c_str(), "w");
    pfc_ = fopen(config_.io.native_pfc_output_file.c_str(), "w");
    if (!resources_ || !qp_events_ || !pfc_) {
        Close();
        return false;
    }
    fprintf(resources_, "time_ns,node,kind,interface,established_qps,ever_used_qps,"
            "unsent_qps,unfinished_qps,unfinished_peers,serviced_qps_in_window,"
            "payload_attempt_bytes,tx_started_wire_bytes,tx_reserved_busy_ns,"
            "queue_bytes,mmu_used_bytes,pg3_pause_ns,rate_bps,window_start_ns,tx_elapsed_busy_ns\n");
    fprintf(qp_events_, "time_ns,event,flow_id,src,dst,sport,dport,pg,bytes,recovery_count,generated_payload_bytes\n");
    fprintf(pfc_, "node,interface,pg,start_ns,end_ns,right_censored\n");
    return true;
}

void NativeResourceMonitor::ConnectDevice(Ptr<QbbNetDevice> device) {
    device->TraceConnectWithoutContext("PfcState", MakeBoundCallback(&NativeResourceMonitor::Pause, this, device));
    if (device->GetNode()->GetNodeType() == 0) {
        device->TraceConnectWithoutContext("PhyTxBegin", MakeBoundCallback(&NativeResourceMonitor::Tx, this, device));
    }
}

void NativeResourceMonitor::ConnectDriver(Ptr<RdmaDriver> driver) {
    driver->m_rdma->TraceConnectWithoutContext("QpRegistered", MakeBoundCallback(&NativeResourceMonitor::Registered, this));
    driver->TraceConnectWithoutContext("QpComplete", MakeBoundCallback(&NativeResourceMonitor::Completed, this));
}

void NativeResourceMonitor::QpEvent(Ptr<RdmaQueuePair> qp, const char* event) {
    NS_ASSERT_MSG(qp_events_, "native resources are not open");
    fprintf(qp_events_, "%lu,%s,%d,%u,%u,%u,%u,%u,%lu,%lu,%lu\n",
            Simulator::Now().GetNanoSeconds(), event, qp->m_flow_id,
            Settings::ip_to_node_id(qp->sip), Settings::ip_to_node_id(qp->dip),
            qp->sport, qp->dport, qp->m_pg, qp->m_size, qp->m_v5_retry_count, qp->stat.txTotalBytes);
}

void NativeResourceMonitor::Registered(NativeResourceMonitor* self, Ptr<RdmaQueuePair> qp) {
    NS_ASSERT_MSG(qp->m_flow_id >= 0 && !qp->m_persistent, "resources require native flow IDs");
    NS_ASSERT_MSG(self->qps_.find(qp->m_flow_id) == self->qps_.end(), "duplicate native flow ID");
    QpState state;
    state.qp = qp;
    self->qps_[qp->m_flow_id] = state;
    self->host_qps_[Settings::ip_to_node_id(qp->sip)].push_back(&self->qps_[qp->m_flow_id]);
    self->QpEvent(qp, "create");
}

void NativeResourceMonitor::Completed(NativeResourceMonitor* self, Ptr<RdmaQueuePair> qp) {
    auto found = self->qps_.find(qp->m_flow_id);
    NS_ASSERT_MSG(found != self->qps_.end() && !found->second.complete, "unknown/duplicate native completion");
    found->second.complete = true;
    self->QpEvent(qp, "complete");
}

void NativeResourceMonitor::Tx(NativeResourceMonitor* self, Ptr<QbbNetDevice> device,
                               Ptr<const Packet> packet) {
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(ch);
    if (ch.l3Prot != 0x11) {
        return;
    }
    FlowIDNUMTag tag;
    bool has_tag = packet->PeekPacketTag(tag);
    NS_ABORT_MSG_IF(!has_tag, "native data packet lacks flow ID");
    auto found = self->qps_.find(tag.GetId());
    NS_ASSERT_MSG(found != self->qps_.end(), "NIC service precedes QP registration");
    uint32_t node = device->GetNode()->GetId();
    self->serviced_[node].insert(tag.GetId());
    uint32_t header = CustomHeader::GetStaticWholeHeaderSize();
    NS_ASSERT_MSG(packet->GetSize() >= header, "invalid native data packet size");
    self->payload_attempt_bytes_[node] += packet->GetSize() - header;
    if (!found->second.sent) {
        found->second.sent = true;
        self->QpEvent(found->second.qp, "first_tx");
    }
}

void NativeResourceMonitor::Pause(NativeResourceMonitor* self, Ptr<QbbNetDevice> device,
                                  uint32_t pg, bool paused) {
    PauseKey key(device->GetNode()->GetId(), device->GetIfIndex(), pg);
    PauseState& state = self->pauses_[key];
    NS_ASSERT_MSG(state.paused != paused, "duplicate pause state transition");
    if (paused) {
        state.start_ns = Simulator::Now().GetNanoSeconds();
    } else {
        self->WritePause(key, state, false);
        state.total_ns += Simulator::Now().GetNanoSeconds() - state.start_ns;
    }
    state.paused = paused;
}

void NativeResourceMonitor::WritePause(const PauseKey& key, const PauseState& state, bool censored) {
    if (pfc_) {
        fprintf(pfc_, "%u,%u,%u,%lu,%lu,%u\n", std::get<0>(key), std::get<1>(key),
                std::get<2>(key), state.start_ns, Simulator::Now().GetNanoSeconds(), censored ? 1 : 0);
    }
}

void NativeResourceMonitor::Sample() {
    if (!resources_) {
        return;
    }
    uint64_t now = Simulator::Now().GetNanoSeconds();
    if (sampled_ && now == last_sample_ns_) {
        return;
    }
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        Ptr<Node> node = state_.nodes.Get(i);
        bool host = node->GetNodeType() == 0;
        uint64_t established = 0, used = 0, unsent = 0, unfinished = 0;
        std::set<uint32_t> peers;
        if (host) {
            for (const QpState* entry : host_qps_[i]) {
                const QpState& q = *entry;
                ++established;
                used += q.sent ? 1 : 0;
                unsent += (!q.complete && q.qp->snd_nxt < q.qp->m_size) ? 1 : 0;
                if (!q.complete) {
                    ++unfinished;
                    peers.insert(Settings::ip_to_node_id(q.qp->dip));
                }
            }
        }
        uint64_t mmu = host ? 0 : DynamicCast<SwitchNode>(node)->m_mmu->GetUsedBufferTotal();
        for (uint32_t port = 0; port < node->GetNDevices(); ++port) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(node->GetDevice(port));
            if (!dev) continue;
            PauseState& pause = pauses_[PauseKey(i, port, 3)];
            uint64_t pause_ns = pause.total_ns + (pause.paused ? now - pause.start_ns : 0);
            uint64_t queue = host ? 0 : dev->GetQueue()->GetNBytesTotal();
            fprintf(resources_, "%lu,%u,%s,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                    now, i, host ? "host" : "switch", port, established, used, unsent, unfinished,
                    static_cast<uint64_t>(peers.size()), static_cast<uint64_t>(serviced_[i].size()),
                    payload_attempt_bytes_[i], dev->GetTxDepartedBytes(), dev->GetTxBusyTimeNs(),
                    queue, mmu, pause_ns, dev->GetDataRate().GetBitRate(),
                    sampled_ ? last_sample_ns_ : now, dev->GetTxElapsedBusyTimeNs());
        }
    }
    serviced_.clear();
    last_sample_ns_ = now;
    sampled_ = true;
}

void NativeResourceMonitor::Close() {
    if (resources_) {
        Sample();
        for (const auto& entry : pauses_) {
            if (entry.second.paused) WritePause(entry.first, entry.second, true);
        }
    }
    for (FILE** file : {&resources_, &qp_events_, &pfc_}) {
        if (*file) {
            fclose(*file);
            *file = NULL;
        }
    }
}
}  // namespace nlb
