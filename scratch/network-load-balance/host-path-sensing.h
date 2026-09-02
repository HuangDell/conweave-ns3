#ifndef NETWORK_LOAD_BALANCE_HOST_PATH_SENSING_H
#define NETWORK_LOAD_BALANCE_HOST_PATH_SENSING_H

#include "experiment-config.h"
#include "simulation-state.h"

#include <ns3/qbb-net-device.h>
#include <ns3/rdma-hw.h>

#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace nlb {

class HostPathSensing {
  public:
    HostPathSensing(const ExperimentConfig& config, SimulationState& state);

    bool OpenOutputs();
    void CloseOutputs();
    void ConnectDevice(ns3::Ptr<ns3::QbbNetDevice> device);
    void Start();

    void RecordWqeEvent(ns3::Ptr<ns3::RdmaQueuePair> queue_pair, uint32_t event,
                        ns3::RdmaQueuePair::WqeBoundary boundary);
    void RecordQpEvent(ns3::Ptr<ns3::RdmaQueuePair> queue_pair, uint32_t event);
    void RecordPfc(ns3::Ptr<ns3::QbbNetDevice> device, uint32_t type);
    void RecordProbeSkipped(uint32_t probe_id);

    bool ReadyToStop() const;
    uint64_t CommittedDataBytes() const { return committed_data_bytes_; }
    uint64_t CompletedDataBytes() const { return completed_data_bytes_; }
    uint64_t OutstandingDataBytes() const {
        return committed_data_bytes_ - completed_data_bytes_;
    }
    uint64_t BacklogThresholdBytes() const { return backlog_threshold_bytes_; }
    uint64_t WindowMinOutstandingBytes() const { return window_min_outstanding_bytes_; }

  private:
    struct QpIdentity {
        uint32_t source = 0;
        uint32_t destination = 0;
        uint16_t pg = 0;
        uint16_t sport = 0;
        uint16_t dport = 0;
        uint32_t lane = 0;
        uint64_t qp_key = 0;
        ns3::Ptr<ns3::RdmaQueuePair> queue_pair;
        bool valid = false;
    };

    struct ProbeState {
        uint32_t probe_id = 0;
        uint64_t post_time_ns = 0;
        uint64_t cqe_time_ns = 0;
        uint64_t timeout_time_ns = 0;
        uint64_t sequence_start = 0;
        uint64_t sequence_end = 0;
        uint64_t start_cnp = 0;
        uint64_t start_retry = 0;
        uint64_t end_cnp = 0;
        uint64_t end_retry = 0;
        uint64_t first_tx_time_ns = 0;
        uint64_t last_tx_time_ns = 0;
        uint64_t host_injection_wait_ns = 0;
        uint64_t forward_queue_wait_ns = 0;
        uint64_t reverse_ack_queue_wait_ns = 0;
        uint64_t retransmission_count = 0;
        bool timed_out = false;
        bool terminal = false;
        bool emitted = false;
        std::set<std::pair<uint32_t, uint32_t>> transmitted_packets;
        std::map<uint32_t, uint64_t> forward_wait_by_sequence;
        std::map<uint32_t, uint64_t> ack_wait_by_sequence;
    };

    struct SkippedProbe {
        uint32_t probe_id = 0;
        uint64_t time_ns = 0;
        bool emitted = false;
    };

    enum PacketClass {
        kOtherPacket,
        kTargetDataPacket,
        kProbeDataPacket,
        kProbeAckPacket,
    };

    struct ParsedPacket {
        PacketClass packet_class = kOtherPacket;
        uint32_t sequence = 0;
        uint32_t payload_bytes = 0;
        uint32_t probe_id = 0;
        bool has_probe_id = false;
    };

    static void EnqueueCallback(HostPathSensing* sensing,
                                ns3::Ptr<ns3::QbbNetDevice> device,
                                ns3::Ptr<const ns3::Packet> packet, uint32_t queue);
    static void TxBeginCallback(HostPathSensing* sensing,
                                ns3::Ptr<ns3::QbbNetDevice> device,
                                ns3::Ptr<const ns3::Packet> packet);

    void RecordEnqueue(ns3::Ptr<ns3::QbbNetDevice> device,
                       ns3::Ptr<const ns3::Packet> packet);
    void RecordTxBegin(ns3::Ptr<ns3::QbbNetDevice> device,
                       ns3::Ptr<const ns3::Packet> packet);
    ParsedPacket ParsePacket(ns3::Ptr<const ns3::Packet> packet) const;
    bool PacketMatchesForward(const QpIdentity& identity,
                              const ns3::CustomHeader& header) const;
    bool PacketMatchesAck(const QpIdentity& identity,
                          const ns3::CustomHeader& header) const;
    const ProbeState* ProbeForDataSequence(uint32_t sequence) const;
    const ProbeState* ProbeForAckSequence(uint32_t sequence) const;

    void CheckProbeTimeout(uint32_t probe_id);
    void FinalizeBaseline();
    void SampleDeliveryWindow();
    void SamplePathTruth();
    void EmitProbe(ProbeState* probe);
    void EmitSkippedProbe(SkippedProbe* skipped);
    void EmitProbeTruth(const ProbeState& probe);
    void FlushUnresolvedProbes();
    uint64_t QpKey(ns3::Ptr<ns3::RdmaQueuePair> queue_pair) const;
    ns3::Ptr<ns3::QbbNetDevice> TruthDevice() const;

    const ExperimentConfig& config_;
    SimulationState& state_;
    FILE* estimator_output_ = NULL;
    FILE* probe_output_ = NULL;
    FILE* truth_output_ = NULL;
    FILE* probe_truth_output_ = NULL;

    QpIdentity target_qp_;
    QpIdentity probe_qp_;
    std::map<uint32_t, ProbeState> probes_;
    std::vector<SkippedProbe> skipped_probes_;
    std::vector<uint64_t> warmup_delays_ns_;
    std::map<std::tuple<uint64_t, uint32_t, uint32_t>, uint64_t> enqueue_times_ns_;
    std::map<std::pair<uint32_t, uint32_t>, bool> pfc_state_;
    std::map<std::pair<uint32_t, uint32_t>, ns3::Ptr<ns3::QbbNetDevice>>
        forward_path_devices_;
    std::set<std::pair<uint32_t, uint32_t>> target_departures_;

    uint64_t committed_data_bytes_ = 0;
    uint64_t completed_data_bytes_ = 0;
    uint64_t previous_completed_data_bytes_ = 0;
    uint64_t window_min_outstanding_bytes_ = 0;
    uint64_t backlog_threshold_bytes_ = 0;
    uint64_t baseline_probe_delay_ns_ = 0;
    uint64_t last_valid_time_ns_ = 0;
    uint64_t last_probe_success_time_ns_ = 0;
    uint64_t previous_cnp_count_ = 0;
    uint64_t previous_retry_count_ = 0;
    uint64_t target_unique_payload_departed_bytes_ = 0;
    uint64_t previous_target_unique_payload_departed_bytes_ = 0;
    double ewma_delivery_rate_bps_ = 0.0;
    bool ewma_initialized_ = false;
    bool baseline_frozen_ = false;
    bool window_disrupted_ = false;
    uint64_t active_posted_probes_ = 0;
};

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_HOST_PATH_SENSING_H
