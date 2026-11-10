#include "odin-resource-feedback.h"
#include <ns3/rdma-driver.h>
#include <ns3/settings.h>
#include <ns3/simulator.h>
#include <iostream>

namespace nlb {
using namespace ns3;

bool OdinResourceFeedback::Start() {
    const auto& c = config_.cc;
    if (!c.odin_resource_feedback) return true;
    if (c.odin_resource_feedback != 1 || c.mode != CC_MODE_ODIN ||
        c.odin_resource_host >= state_.nodes.GetN() || !c.odin_resource_period_ns ||
        c.odin_resource_window_ns < c.odin_resource_period_ns ||
        c.odin_resource_window_ns % c.odin_resource_period_ns ||
        c.odin_resource_window_ns / c.odin_resource_period_ns > 100000 ||
        c.odin_resource_destinations.empty() || !config_.monitoring.odin_log) {
        std::cerr << "Invalid Odin resource feedback configuration\n";
        return false;
    }
    Ptr<Node> host = state_.nodes.Get(c.odin_resource_host);
    Ptr<RdmaDriver> driver = host->GetObject<RdmaDriver>();
    if (!driver) return false;
    for (const auto& neighbor : state_.neighbor_interfaces[host]) {
        Ptr<SwitchNode> candidate = DynamicCast<SwitchNode>(neighbor.first);
        if (candidate) {
            if (tor_) return false;  // Single directly attached ToR only.
            tor_ = candidate;
        }
    }
    if (!tor_) return false;
    hardware_ = driver->m_rdma;
    for (uint32_t destination : c.odin_resource_destinations) {
        if (destination >= state_.nodes.GetN() || destination == c.odin_resource_host) return false;
        Ptr<Node> peer = state_.nodes.Get(destination);
        auto found = state_.neighbor_interfaces[tor_].find(peer);
        if (peer->GetNodeType() != 0 || found == state_.neighbor_interfaces[tor_].end()) return false;
        Port port;
        port.destination_ip = state_.server_addresses[destination].Get();
        port.device = DynamicCast<QbbNetDevice>(tor_->GetDevice(found->second.idx));
        if (!port.device || !found->second.up) return false;
        ports_.push_back(port);
        hardware_->m_odinResourceDestinations.insert(port.destination_ip);
    }
    hardware_->m_odinResourceFeedback = true;
    hardware_->m_odinResourceStartNs = c.odin_resource_start_ns;
    uint64_t start = Seconds(config_.flowgen_start_time).GetNanoSeconds();
    if (c.odin_resource_start_ns < start + c.odin_resource_window_ns ||
        (c.odin_resource_start_ns - start) % c.odin_resource_period_ns) return false;
    output_ = fopen((config_.io.odin_output_file + ".resources.csv").c_str(), "w");
    if (!output_) return false;
    fprintf(output_, "time_ns,event,version,host,switch,interface,destination,window_start_ns,"
                     "measured_ns,delta_wire_bytes,line_bps,queue_bytes,tbo,eligible_qps,"
                     "begin_tx_wire_bytes,end_tx_wire_bytes\n");
    tick_ = Simulator::Schedule(NanoSeconds(start), &OdinResourceFeedback::Tick, this);
    return true;
}

void OdinResourceFeedback::Tick() {
    const auto& c = config_.cc;
    uint64_t now = Simulator::Now().GetNanoSeconds();
    std::vector<OdinResourceSample> samples;
    for (auto& p : ports_) {
        auto& h = p.history;
        h.emplace_back(now, p.device->GetTxDepartedBytes());
        while (h.size() > c.odin_resource_window_ns / c.odin_resource_period_ns + 1)
            h.pop_front();
        if (now < c.odin_resource_start_ns || now - h.front().first < c.odin_resource_window_ns)
            continue;
        OdinResourceSample s;
        s.destination_ip = p.destination_ip;
        s.switch_id = tor_->GetId();
        s.interface = p.device->GetIfIndex();
        s.window_start_ns = h.front().first;
        s.measured_ns = now;
        s.delta_bytes = h.back().second - h.front().second;
        s.begin_tx_bytes = h.front().second;
        s.end_tx_bytes = h.back().second;
        s.line_bps = p.device->GetDataRate().GetBitRate();
        s.queue_bytes = p.device->GetQueue()->GetNBytesTotal();
        s.tbo = tor_->m_mmu->GetUsedBufferTotal() >
                c.odin_tbo_ratio * tor_->m_mmu->GetMmuBufferBytes();
        samples.push_back(s);
    }
    if (!samples.empty()) {
        NS_ABORT_MSG_IF(samples.size() != ports_.size(), "incomplete resource frame");
        Write("generated", ++version_, samples, 0);
        Simulator::Schedule(NanoSeconds(c.odin_resource_delay_ns),
                            &OdinResourceFeedback::Deliver, this, version_, samples);
    }
    tick_ = Simulator::Schedule(NanoSeconds(c.odin_resource_period_ns),
                               &OdinResourceFeedback::Tick, this);
}

void OdinResourceFeedback::Deliver(uint64_t version, std::vector<OdinResourceSample> samples) {
    uint32_t eligible = hardware_->ApplyOdinResourceFeedback(version, samples);
    Write("delivered", version, samples, eligible);
}

void OdinResourceFeedback::Write(const char* event, uint64_t version,
    const std::vector<OdinResourceSample>& samples, uint32_t eligible) {
    for (const auto& s : samples)
        fprintf(output_, "%lu,%s,%lu,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%u,%u,%lu,%lu\n",
                Simulator::Now().GetNanoSeconds(), event, version,
                config_.cc.odin_resource_host, s.switch_id, s.interface,
                Settings::ip_to_node_id(Ipv4Address(s.destination_ip)),
                s.window_start_ns, s.measured_ns, s.delta_bytes, s.line_bps,
                s.queue_bytes, s.tbo ? 1 : 0, eligible, s.begin_tx_bytes, s.end_tx_bytes);
}

void OdinResourceFeedback::Close() {
    if (tick_.IsRunning()) tick_.Cancel();
    if (output_) { fclose(output_); output_ = NULL; }
}
}  // namespace nlb
