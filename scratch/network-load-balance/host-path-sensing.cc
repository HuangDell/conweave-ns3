#include "host-path-sensing.h"

#include <ns3/custom-header.h>
#include <ns3/letflow-routing.h>
#include <ns3/settings.h>
#include <ns3/simulator.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace nlb {

using namespace ns3;

HostPathSensing::HostPathSensing(const ExperimentConfig& config, SimulationState& state)
    : config_(config), state_(state) {}

bool HostPathSensing::OpenOutputs() {
    estimator_output_ = fopen(config_.io.host_sensing_output_file.c_str(), "w");
    probe_output_ = fopen(config_.io.host_probe_output_file.c_str(), "w");
    truth_output_ = fopen(config_.io.host_path_truth_output_file.c_str(), "w");
    probe_truth_output_ = fopen(config_.io.host_probe_truth_output_file.c_str(), "w");
    if (!estimator_output_ || !probe_output_ || !truth_output_ || !probe_truth_output_) {
        return false;
    }
    fprintf(estimator_output_,
            "time_ns,event,traffic_class,app_id,chunk_or_probe_id,src,dst,qp_key,lane,bytes,"
            "committed_data_bytes,completed_data_bytes,outstanding_data_bytes,"
            "window_min_outstanding_bytes,backlog_threshold_bytes,raw_delivery_rate_bps,"
            "ewma_delivery_rate_bps,sample_valid,last_valid_time_ns,sample_age_ns,"
            "ecn_cnp_delta,retry_delta,ecn_cnp_count,retry_count,dcqcn_rate_bps,"
            "dcqcn_target_rate_bps,dcqcn_alpha\n");
    fprintf(probe_output_,
            "probe_id,traffic_class,bytes,qp_key,lane,probe_post_time_ns,probe_cqe_time_ns,"
            "probe_timeout_time_ns,probe_delay_ns,baseline_probe_delay_ns,"
            "queue_delay_proxy_ns,status,sample_age_ns,ecn_cnp_delta,retry_delta,warmup\n");
    fprintf(truth_output_,
            "time_ns,lane,configured_line_rate_bps,target_unique_payload_departed_bytes,"
            "target_unique_payload_delta_bytes,total_departed_bytes,per_hop_queue_bytes,pfc_state\n");
    fprintf(probe_truth_output_,
            "probe_id,probe_first_tx_time_ns,probe_last_tx_time_ns,host_injection_wait_ns,"
            "forward_queue_wait_ns,reverse_ack_queue_wait_ns,probe_network_queue_delay_ns,"
            "retransmission_count\n");
    return true;
}

void HostPathSensing::CloseOutputs() {
    FlushUnresolvedProbes();
    FILE** files[] = {&estimator_output_, &probe_output_, &truth_output_,
                      &probe_truth_output_};
    for (FILE** file : files) {
        if (*file) {
            fclose(*file);
            *file = NULL;
        }
    }
}

void HostPathSensing::ConnectDevice(Ptr<QbbNetDevice> device) {
    device->TraceConnectWithoutContext(
        "QbbEnqueue", MakeBoundCallback(&HostPathSensing::EnqueueCallback, this, device));
    device->TraceConnectWithoutContext(
        "PhyTxBegin", MakeBoundCallback(&HostPathSensing::TxBeginCallback, this, device));
}

void HostPathSensing::Start() {
    uint64_t start_ns = static_cast<uint64_t>(config_.flowgen_start_time * 1000000000.0);
    window_min_outstanding_bytes_ = OutstandingDataBytes();
    Simulator::Schedule(NanoSeconds(start_ns), &HostPathSensing::FinalizeBaseline, this);
    Simulator::Schedule(NanoSeconds(start_ns), &HostPathSensing::SamplePathTruth, this);
    Simulator::Schedule(NanoSeconds(start_ns) + MicroSeconds(config_.host_sensing.delivery_window_us),
                        &HostPathSensing::SampleDeliveryWindow, this);
}

uint64_t HostPathSensing::QpKey(Ptr<RdmaQueuePair> queue_pair) const {
    return LetflowRouting::GetQpKey(queue_pair->dip.Get(), queue_pair->sport,
                                    queue_pair->dport, queue_pair->m_pg);
}

void HostPathSensing::RecordWqeEvent(Ptr<RdmaQueuePair> queue_pair, uint32_t event,
                                     RdmaQueuePair::WqeBoundary boundary) {
    uint32_t source = Settings::ip_to_node_id(queue_pair->sip);
    uint32_t destination = Settings::ip_to_node_id(queue_pair->dip);
    bool target = queue_pair->m_v5_traffic_class == RdmaQueuePair::V5_DATA &&
                  boundary.app_id == config_.host_sensing.target_app_id;
    if (target) {
        NS_ASSERT_MSG(source == config_.host_sensing.target_source &&
                          destination == config_.host_sensing.target_destination &&
                          queue_pair->m_pg == config_.host_sensing.target_pg &&
                          queue_pair->m_v5_lane == config_.host_sensing.target_lane,
                      "target app/QP/lane mapping changed");
        if (!target_qp_.valid) {
            target_qp_ = {source, destination, queue_pair->m_pg, queue_pair->sport,
                          queue_pair->dport, queue_pair->m_v5_lane, QpKey(queue_pair),
                          queue_pair, true};
        } else {
            NS_ASSERT_MSG(target_qp_.qp_key == QpKey(queue_pair),
                          "target data QP changed during its active lifetime");
        }
        if (event == RdmaHw::WQE_COMMIT) {
            committed_data_bytes_ += boundary.bytes;
        } else {
            NS_ASSERT_MSG(completed_data_bytes_ + boundary.bytes <= committed_data_bytes_,
                          "target completion exceeded committed data bytes");
            completed_data_bytes_ += boundary.bytes;
        }
        window_min_outstanding_bytes_ =
            std::min(window_min_outstanding_bytes_, OutstandingDataBytes());
        return;
    }

    if (queue_pair->m_v5_traffic_class != RdmaQueuePair::V5_PROBE) {
        return;
    }
    NS_ASSERT_MSG(source == config_.host_sensing.target_source &&
                      destination == config_.host_sensing.target_destination &&
                      queue_pair->m_pg == config_.host_sensing.target_pg &&
                      queue_pair->m_v5_lane == config_.host_sensing.target_lane,
                  "probe QP is not pinned to the target path");
    if (!probe_qp_.valid) {
        probe_qp_ = {source, destination, queue_pair->m_pg, queue_pair->sport,
                     queue_pair->dport, queue_pair->m_v5_lane, QpKey(queue_pair),
                     queue_pair, true};
    } else {
        NS_ASSERT_MSG(probe_qp_.qp_key == QpKey(queue_pair),
                      "probe QP changed during the experiment");
    }
    NS_ASSERT_MSG(!target_qp_.valid || target_qp_.qp_key != probe_qp_.qp_key,
                  "probe must use a QP independent from target data");

    ProbeState& probe = probes_[boundary.chunk_id];
    if (event == RdmaHw::WQE_COMMIT) {
        NS_ASSERT_MSG(probe.post_time_ns == 0, "probe epoch posted more than once");
        probe.probe_id = boundary.chunk_id;
        probe.post_time_ns = Simulator::Now().GetNanoSeconds();
        probe.sequence_end = boundary.cumulative_end_seq;
        probe.sequence_start = boundary.cumulative_end_seq - boundary.bytes;
        probe.start_cnp = queue_pair->m_v5_ecn_cnp_count;
        probe.start_retry = queue_pair->m_v5_retry_count;
        ++active_posted_probes_;
        Simulator::Schedule(MicroSeconds(config_.host_sensing.probe_timeout_us),
                            &HostPathSensing::CheckProbeTimeout, this, probe.probe_id);
        return;
    }

    NS_ASSERT_MSG(probe.post_time_ns > 0 && !probe.terminal,
                  "probe completion does not match one outstanding post");
    probe.cqe_time_ns = Simulator::Now().GetNanoSeconds();
    probe.end_cnp = queue_pair->m_v5_ecn_cnp_count;
    probe.end_retry = queue_pair->m_v5_retry_count;
    probe.terminal = true;
    if (!probe.transmitted_packets.empty()) {
        uint32_t last_sequence = probe.transmitted_packets.rbegin()->first;
        probe.forward_queue_wait_ns = probe.forward_wait_by_sequence[last_sequence];
    }
    probe.reverse_ack_queue_wait_ns =
        probe.ack_wait_by_sequence[static_cast<uint32_t>(probe.sequence_end)];
    --active_posted_probes_;
    uint64_t delay_ns = probe.cqe_time_ns - probe.post_time_ns;
    uint64_t start_ns = static_cast<uint64_t>(config_.flowgen_start_time * 1000000000.0);
    if (probe.post_time_ns < start_ns && probe.cqe_time_ns <= start_ns &&
        !probe.timed_out) {
        warmup_delays_ns_.push_back(delay_ns);
    }
    EmitProbeTruth(probe);
    if (baseline_frozen_) {
        EmitProbe(&probe);
    }
}

void HostPathSensing::RecordQpEvent(Ptr<RdmaQueuePair> queue_pair, uint32_t event) {
    if (event != RdmaHw::PERSISTENT_QP_TEARDOWN) {
        return;
    }
    uint64_t key = QpKey(queue_pair);
    if (target_qp_.valid && key == target_qp_.qp_key) {
        window_disrupted_ = true;
    }
}

void HostPathSensing::RecordPfc(Ptr<QbbNetDevice> device, uint32_t type) {
    pfc_state_[{device->GetNode()->GetId(), device->GetIfIndex()}] = type != 0;
}

void HostPathSensing::RecordProbeSkipped(uint32_t probe_id) {
    SkippedProbe skipped;
    skipped.probe_id = probe_id;
    skipped.time_ns = Simulator::Now().GetNanoSeconds();
    skipped_probes_.push_back(skipped);
    if (baseline_frozen_) {
        EmitSkippedProbe(&skipped_probes_.back());
    }
}

void HostPathSensing::CheckProbeTimeout(uint32_t probe_id) {
    auto probe = probes_.find(probe_id);
    if (probe != probes_.end() && !probe->second.terminal) {
        probe->second.timed_out = true;
        probe->second.timeout_time_ns = Simulator::Now().GetNanoSeconds();
        probe->second.end_cnp =
            probe_qp_.valid ? probe_qp_.queue_pair->m_v5_ecn_cnp_count : 0;
        probe->second.end_retry =
            probe_qp_.valid ? probe_qp_.queue_pair->m_v5_retry_count : 0;
    }
}

void HostPathSensing::FinalizeBaseline() {
    NS_ABORT_MSG_IF(warmup_delays_ns_.size() < config_.host_sensing.min_baseline_samples,
                    "host probe baseline has fewer than the required successful samples");
    baseline_probe_delay_ns_ =
        *std::min_element(warmup_delays_ns_.begin(), warmup_delays_ns_.end());
    long double numerator = static_cast<long double>(config_.host_sensing.nominal_rate_bps) *
                            baseline_probe_delay_ns_;
    backlog_threshold_bytes_ = static_cast<uint64_t>(std::ceil(numerator / 8000000000.0L));
    baseline_frozen_ = true;
    for (auto& item : probes_) {
        if (item.second.terminal) {
            EmitProbe(&item.second);
        }
    }
    for (auto& skipped : skipped_probes_) {
        EmitSkippedProbe(&skipped);
    }
}

void HostPathSensing::SampleDeliveryWindow() {
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    uint64_t start_ns = static_cast<uint64_t>(config_.flowgen_start_time * 1000000000.0);
    uint64_t stop_ns = static_cast<uint64_t>(config_.flowgen_stop_time * 1000000000.0);
    uint64_t delta_completed = completed_data_bytes_ - previous_completed_data_bytes_;
    bool valid = baseline_frozen_ && target_qp_.valid && !window_disrupted_ &&
                 window_min_outstanding_bytes_ >= backlog_threshold_bytes_;
    double raw_rate_bps = static_cast<double>(delta_completed) * 8.0 * 1000000.0 /
                          config_.host_sensing.delivery_window_us;
    if (valid) {
        if (!ewma_initialized_) {
            ewma_delivery_rate_bps_ = raw_rate_bps;
            ewma_initialized_ = true;
        } else {
            ewma_delivery_rate_bps_ =
                (1.0 - config_.host_sensing.ewma_beta) * ewma_delivery_rate_bps_ +
                config_.host_sensing.ewma_beta * raw_rate_bps;
        }
        last_valid_time_ns_ = now_ns;
    }
    uint64_t sample_age_ns = last_valid_time_ns_ == 0
                                 ? now_ns - start_ns
                                 : now_ns - last_valid_time_ns_;
    uint64_t cnp_count = target_qp_.valid ? target_qp_.queue_pair->m_v5_ecn_cnp_count : 0;
    uint64_t retry_count = target_qp_.valid ? target_qp_.queue_pair->m_v5_retry_count : 0;
    uint64_t cnp_delta = cnp_count - previous_cnp_count_;
    uint64_t retry_delta = retry_count - previous_retry_count_;
    fprintf(estimator_output_,
            "%lu,window,data,%u,,%u,%u,%lu,%u,%lu,%lu,%lu,%lu,%lu,%lu,%.17g,%.17g,"
            "%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.17g\n",
            now_ns, config_.host_sensing.target_app_id, config_.host_sensing.target_source,
            config_.host_sensing.target_destination,
            target_qp_.valid ? target_qp_.qp_key : 0, config_.host_sensing.target_lane,
            delta_completed, committed_data_bytes_, completed_data_bytes_,
            OutstandingDataBytes(), window_min_outstanding_bytes_, backlog_threshold_bytes_,
            raw_rate_bps, ewma_initialized_ ? ewma_delivery_rate_bps_ : 0.0,
            valid ? 1 : 0, last_valid_time_ns_, sample_age_ns, cnp_delta, retry_delta,
            cnp_count, retry_count,
            target_qp_.valid ? target_qp_.queue_pair->m_rate.GetBitRate() : 0,
            target_qp_.valid ? target_qp_.queue_pair->mlx.m_targetRate.GetBitRate() : 0,
            target_qp_.valid ? target_qp_.queue_pair->mlx.m_alpha : 0.0);
    fflush(estimator_output_);

    previous_completed_data_bytes_ = completed_data_bytes_;
    previous_cnp_count_ = cnp_count;
    previous_retry_count_ = retry_count;
    window_min_outstanding_bytes_ = OutstandingDataBytes();
    window_disrupted_ = false;
    if (now_ns + config_.host_sensing.delivery_window_us * 1000 <= stop_ns) {
        Simulator::Schedule(MicroSeconds(config_.host_sensing.delivery_window_us),
                            &HostPathSensing::SampleDeliveryWindow, this);
    }
}

Ptr<QbbNetDevice> HostPathSensing::TruthDevice() const {
    if (config_.host_sensing.truth_node_a >= state_.nodes.GetN() ||
        config_.host_sensing.truth_node_b >= state_.nodes.GetN()) {
        return NULL;
    }
    Ptr<Node> a = state_.nodes.Get(config_.host_sensing.truth_node_a);
    Ptr<Node> b = state_.nodes.Get(config_.host_sensing.truth_node_b);
    auto node = state_.neighbor_interfaces.find(a);
    if (node == state_.neighbor_interfaces.end()) {
        return NULL;
    }
    auto neighbor = node->second.find(b);
    if (neighbor == node->second.end()) {
        return NULL;
    }
    return DynamicCast<QbbNetDevice>(a->GetDevice(neighbor->second.idx));
}

void HostPathSensing::SamplePathTruth() {
    Ptr<QbbNetDevice> device = TruthDevice();
    NS_ASSERT_MSG(device, "host sensing truth link is missing from the topology");
    uint32_t node = device->GetNode()->GetId();
    uint32_t interface = device->GetIfIndex();
    uint64_t delta_unique = target_unique_payload_departed_bytes_ -
                            previous_target_unique_payload_departed_bytes_;
    forward_path_devices_[{node, interface}] = device;
    std::ostringstream queue_state;
    std::ostringstream pfc_state;
    bool first = true;
    for (const auto& item : forward_path_devices_) {
        if (!first) {
            queue_state << ';';
            pfc_state << ';';
        }
        first = false;
        uint64_t queue_bytes = item.second->GetQueue()
                                   ? item.second->GetQueue()->GetNBytesTotal()
                                   : 0;
        queue_state << item.first.first << ':' << item.first.second << ':' << queue_bytes;
        pfc_state << item.first.first << ':' << item.first.second << ':'
                  << (pfc_state_[item.first] ? 1 : 0);
    }
    fprintf(truth_output_, "%lu,%u,%lu,%lu,%lu,%lu,%s,%s\n",
            Simulator::Now().GetNanoSeconds(), config_.host_sensing.target_lane,
            device->GetDataRate().GetBitRate(), target_unique_payload_departed_bytes_,
            delta_unique, device->GetTxDepartedBytes(), queue_state.str().c_str(),
            pfc_state.str().c_str());
    fflush(truth_output_);
    previous_target_unique_payload_departed_bytes_ = target_unique_payload_departed_bytes_;
    uint64_t stop_ns = static_cast<uint64_t>(config_.flowgen_stop_time * 1000000000.0);
    if (Simulator::Now().GetNanoSeconds() +
            config_.host_sensing.delivery_window_us * 1000 <= stop_ns) {
        Simulator::Schedule(MicroSeconds(config_.host_sensing.delivery_window_us),
                            &HostPathSensing::SamplePathTruth, this);
    }
}

void HostPathSensing::EmitProbe(ProbeState* probe) {
    if (!probe_output_ || probe->emitted || !probe->terminal || !baseline_frozen_) {
        return;
    }
    uint64_t delay = probe->cqe_time_ns - probe->post_time_ns;
    int64_t queue_proxy = static_cast<int64_t>(delay) -
                          static_cast<int64_t>(baseline_probe_delay_ns_);
    uint64_t sample_age = last_probe_success_time_ns_ == 0
                              ? 0
                              : probe->cqe_time_ns - last_probe_success_time_ns_;
    last_probe_success_time_ns_ = probe->cqe_time_ns;
    uint64_t start_ns = static_cast<uint64_t>(config_.flowgen_start_time * 1000000000.0);
    fprintf(probe_output_,
            "%u,probe,%u,%lu,%u,%lu,%lu,%lu,%lu,%lu,%ld,%s,%lu,%lu,%lu,%u\n",
            probe->probe_id, config_.host_sensing.probe_bytes, probe_qp_.qp_key,
            config_.host_sensing.target_lane, probe->post_time_ns, probe->cqe_time_ns,
            probe->timeout_time_ns, delay, baseline_probe_delay_ns_, queue_proxy,
            probe->timed_out ? "late_success" : "success", sample_age,
            probe->end_cnp - probe->start_cnp, probe->end_retry - probe->start_retry,
            probe->post_time_ns < start_ns ? 1 : 0);
    fflush(probe_output_);
    probe->emitted = true;
}

void HostPathSensing::EmitSkippedProbe(SkippedProbe* skipped) {
    if (!probe_output_ || skipped->emitted || !baseline_frozen_) {
        return;
    }
    uint64_t sample_age =
        last_probe_success_time_ns_ == 0 || skipped->time_ns <= last_probe_success_time_ns_
            ? 0
            : skipped->time_ns - last_probe_success_time_ns_;
    uint64_t start_ns = static_cast<uint64_t>(config_.flowgen_start_time * 1000000000.0);
    fprintf(probe_output_, "%u,probe,%u,%lu,%u,0,0,0,0,%lu,0,skipped_busy,%lu,0,0,%u\n",
            skipped->probe_id, config_.host_sensing.probe_bytes,
            probe_qp_.valid ? probe_qp_.qp_key : 0, config_.host_sensing.target_lane,
            baseline_probe_delay_ns_, sample_age, skipped->time_ns < start_ns ? 1 : 0);
    fflush(probe_output_);
    skipped->emitted = true;
}

void HostPathSensing::EmitProbeTruth(const ProbeState& probe) {
    if (!probe_truth_output_) {
        return;
    }
    fprintf(probe_truth_output_, "%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
            probe.probe_id, probe.first_tx_time_ns, probe.last_tx_time_ns,
            probe.host_injection_wait_ns, probe.forward_queue_wait_ns,
            probe.reverse_ack_queue_wait_ns,
            probe.forward_queue_wait_ns + probe.reverse_ack_queue_wait_ns,
            probe.retransmission_count);
    fflush(probe_truth_output_);
}

void HostPathSensing::FlushUnresolvedProbes() {
    if (!probe_output_) {
        return;
    }
    for (auto& item : probes_) {
        ProbeState& probe = item.second;
        if (probe.terminal || probe.emitted || probe.post_time_ns == 0) {
            continue;
        }
        uint64_t timeout_time_ns = probe.timeout_time_ns > 0
                                       ? probe.timeout_time_ns
                                       : probe.post_time_ns +
                                             config_.host_sensing.probe_timeout_us * 1000;
        if (probe_qp_.valid) {
            probe.end_cnp = std::max(probe.end_cnp,
                                     probe_qp_.queue_pair->m_v5_ecn_cnp_count);
            probe.end_retry = std::max(probe.end_retry,
                                       probe_qp_.queue_pair->m_v5_retry_count);
        }
        uint64_t sample_age =
            last_probe_success_time_ns_ == 0 || timeout_time_ns <= last_probe_success_time_ns_
                ? 0
                : timeout_time_ns - last_probe_success_time_ns_;
        fprintf(probe_output_,
                "%u,probe,%u,%lu,%u,%lu,0,%lu,0,%lu,0,timeout,%lu,%lu,%lu,%u\n",
                probe.probe_id, config_.host_sensing.probe_bytes,
                probe_qp_.valid ? probe_qp_.qp_key : 0, config_.host_sensing.target_lane,
                probe.post_time_ns, timeout_time_ns, baseline_probe_delay_ns_,
                sample_age,
                probe.end_cnp - probe.start_cnp, probe.end_retry - probe.start_retry,
                probe.post_time_ns <
                        static_cast<uint64_t>(config_.flowgen_start_time * 1000000000.0)
                    ? 1
                    : 0);
        EmitProbeTruth(probe);
        probe.emitted = true;
    }
    fflush(probe_output_);
}

bool HostPathSensing::PacketMatchesForward(const QpIdentity& identity,
                                           const CustomHeader& header) const {
    return identity.valid && header.l3Prot == 0x11 &&
           header.sip == state_.server_addresses[identity.source].Get() &&
           header.dip == state_.server_addresses[identity.destination].Get() &&
           header.udp.sport == identity.sport && header.udp.dport == identity.dport &&
           header.udp.pg == identity.pg;
}

bool HostPathSensing::PacketMatchesAck(const QpIdentity& identity,
                                       const CustomHeader& header) const {
    return identity.valid && (header.l3Prot == 0xFC || header.l3Prot == 0xFD) &&
           header.sip == state_.server_addresses[identity.destination].Get() &&
           header.dip == state_.server_addresses[identity.source].Get() &&
           header.ack.sport == identity.dport && header.ack.dport == identity.sport &&
           header.ack.pg == identity.pg;
}

const HostPathSensing::ProbeState* HostPathSensing::ProbeForDataSequence(
    uint32_t sequence) const {
    for (const auto& item : probes_) {
        if (sequence >= item.second.sequence_start && sequence < item.second.sequence_end) {
            return &item.second;
        }
    }
    return nullptr;
}

const HostPathSensing::ProbeState* HostPathSensing::ProbeForAckSequence(
    uint32_t sequence) const {
    for (const auto& item : probes_) {
        if (sequence > item.second.sequence_start && sequence <= item.second.sequence_end) {
            return &item.second;
        }
    }
    return nullptr;
}

HostPathSensing::ParsedPacket HostPathSensing::ParsePacket(Ptr<const Packet> packet) const {
    ParsedPacket parsed;
    CustomHeader header(CustomHeader::L2_Header | CustomHeader::L3_Header |
                        CustomHeader::L4_Header);
    header.getInt = 1;
    if (packet->PeekHeader(header) == 0) {
        return parsed;
    }
    if (PacketMatchesForward(target_qp_, header)) {
        parsed.packet_class = kTargetDataPacket;
        parsed.sequence = header.udp.seq;
        parsed.payload_bytes = packet->GetSize() - header.GetSerializedSize();
    } else if (PacketMatchesForward(probe_qp_, header)) {
        parsed.packet_class = kProbeDataPacket;
        parsed.sequence = header.udp.seq;
        parsed.payload_bytes = packet->GetSize() - header.GetSerializedSize();
        const ProbeState* probe = ProbeForDataSequence(parsed.sequence);
        if (probe) {
            parsed.probe_id = probe->probe_id;
            parsed.has_probe_id = true;
        }
    } else if (PacketMatchesAck(probe_qp_, header)) {
        parsed.packet_class = kProbeAckPacket;
        parsed.sequence = header.ack.seq;
        const ProbeState* probe = ProbeForAckSequence(parsed.sequence);
        if (probe) {
            parsed.probe_id = probe->probe_id;
            parsed.has_probe_id = true;
        }
    }
    return parsed;
}

void HostPathSensing::EnqueueCallback(HostPathSensing* sensing,
                                      Ptr<QbbNetDevice> device,
                                      Ptr<const Packet> packet, uint32_t queue) {
    (void)queue;
    sensing->RecordEnqueue(device, packet);
}

void HostPathSensing::TxBeginCallback(HostPathSensing* sensing,
                                      Ptr<QbbNetDevice> device,
                                      Ptr<const Packet> packet) {
    sensing->RecordTxBegin(device, packet);
}

void HostPathSensing::RecordEnqueue(Ptr<QbbNetDevice> device, Ptr<const Packet> packet) {
    if (device->GetNode()->GetNodeType() != 1) {
        return;
    }
    ParsedPacket parsed = ParsePacket(packet);
    if (parsed.packet_class != kProbeDataPacket && parsed.packet_class != kProbeAckPacket) {
        return;
    }
    enqueue_times_ns_[{packet->GetUid(), device->GetNode()->GetId(), device->GetIfIndex()}] =
        Simulator::Now().GetNanoSeconds();
}

void HostPathSensing::RecordTxBegin(Ptr<QbbNetDevice> device, Ptr<const Packet> packet) {
    ParsedPacket parsed = ParsePacket(packet);
    uint32_t node = device->GetNode()->GetId();
    uint32_t interface = device->GetIfIndex();
    if ((parsed.packet_class == kTargetDataPacket ||
         parsed.packet_class == kProbeDataPacket) &&
        device->GetNode()->GetNodeType() == 1) {
        forward_path_devices_[{node, interface}] = device;
    }
    if (parsed.packet_class == kTargetDataPacket) {
        Ptr<QbbNetDevice> truth = TruthDevice();
        if (truth == device) {
            std::pair<uint32_t, uint32_t> packet_id = {parsed.sequence,
                                                       parsed.payload_bytes};
            if (target_departures_.insert(packet_id).second) {
                target_unique_payload_departed_bytes_ += parsed.payload_bytes;
            }
        }
        return;
    }
    if (!parsed.has_probe_id) {
        return;
    }
    ProbeState& probe = probes_.at(parsed.probe_id);
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    if (parsed.packet_class == kProbeDataPacket &&
        node == config_.host_sensing.target_source) {
        if (probe.first_tx_time_ns == 0) {
            probe.first_tx_time_ns = now_ns;
            probe.host_injection_wait_ns = now_ns - probe.post_time_ns;
        }
        probe.last_tx_time_ns = now_ns;
        if (!probe.transmitted_packets.insert({parsed.sequence, parsed.payload_bytes}).second) {
            ++probe.retransmission_count;
            probe.forward_wait_by_sequence[parsed.sequence] = 0;
        }
    }
    if (device->GetNode()->GetNodeType() != 1) {
        return;
    }
    auto enqueued = enqueue_times_ns_.find({packet->GetUid(), node, interface});
    if (enqueued == enqueue_times_ns_.end()) {
        return;
    }
    uint64_t wait_ns = now_ns - enqueued->second;
    if (parsed.packet_class == kProbeDataPacket) {
        probe.forward_wait_by_sequence[parsed.sequence] += wait_ns;
    } else if (parsed.packet_class == kProbeAckPacket) {
        probe.ack_wait_by_sequence[parsed.sequence] += wait_ns;
    }
    enqueue_times_ns_.erase(enqueued);
}

bool HostPathSensing::ReadyToStop() const {
    if (active_posted_probes_ == 0) {
        return true;
    }
    uint64_t force_ns = static_cast<uint64_t>(config_.flowgen_stop_time * 1000000000.0) +
                        config_.host_sensing.probe_timeout_us * 1000;
    return Simulator::Now().GetNanoSeconds() >= force_ns;
}

}  // namespace nlb
