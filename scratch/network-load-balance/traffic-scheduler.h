#ifndef NETWORK_LOAD_BALANCE_TRAFFIC_SCHEDULER_H
#define NETWORK_LOAD_BALANCE_TRAFFIC_SCHEDULER_H

#include "experiment-config.h"
#include "simulation-state.h"

#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nlb {

class SimulationMonitor;

class TrafficScheduler {
  public:
    TrafficScheduler(const ExperimentConfig& config, SimulationState& state,
                     SimulationMonitor& monitor);

    bool LoadFlowFile();
    void InitializePorts();
    void Start();
    uint64_t TargetCompletionCount() const { return target_completion_count_; }

  private:
    struct FlowInput {
        uint32_t src = 0;
        uint32_t dst = 0;
        uint32_t pg = 0;
        uint32_t max_packet_count = 0;
        uint32_t port = 0;
        double start_time = 0;
        uint32_t idx = 0;
    };

    struct PersistentPoolEntry {
        uint32_t source = 0;
        uint32_t destination = 0;
        uint16_t pg = 0;
        uint32_t lane = 0;
        uint16_t source_port = 0;
        uint16_t destination_port = 0;
        uint64_t qp_key = 0;
        uint64_t scheduled_wqes = 0;
    };

    enum PublicationSource {
        kHoldDegraded = 0,
        kFreshEstimate = 1,
        kHealthyNominal = 2,
    };

    struct DecisionSignal {
        std::vector<double> capacities_bps;
        std::vector<double> weights;
        std::vector<double> deficits;
        std::vector<uint32_t> binary_congested;
        std::vector<uint64_t> binary_hold_until_ns;
        std::vector<uint64_t> published_versions;
        std::vector<uint32_t> publication_sources;
        std::vector<uint64_t> sample_ages_ns;
        std::vector<uint32_t> degraded_states;
        uint64_t estimate_version = 0;
        uint64_t estimate_age_ns = 0;
        bool publication_changed = false;
        bool is_probe = false;
        uint64_t probe_epoch = 0;
        uint32_t probe_bytes = 0;
        uint32_t probe_lane = static_cast<uint32_t>(-1);
    };

    struct LaneObservation {
        uint32_t out_port = static_cast<uint32_t>(-1);
        uint64_t nominal_bps = 0;
        uint64_t queue_bytes = 0;
        uint64_t sample_age_ns = 0;
        uint64_t valid_sample_count = 0;
        bool degraded_state = false;
    };

    struct ScheduledChunk {
        uint32_t app_id;
        uint32_t pg;
        uint32_t source;
        uint32_t destination;
        uint32_t bytes;
        uint32_t chunk_id;
    };

    bool PersistentPoolEnabled() const;
    uint32_t LaneCount() const;
    bool ChunkModeEnabled() const;
    uint64_t ChunkCount(uint32_t bytes) const;
    uint64_t CalculateTargetCompletionCount();
    uint64_t LaneCounterKey(uint32_t source_tor, uint32_t destination_tor) const;
    uint32_t LaneOutPort(uint32_t source_tor, uint32_t destination_tor,
                         uint32_t lane) const;
    bool GetBadLane(uint32_t source, uint32_t destination, uint32_t* bad_lane,
                    double decision_time, double* bad_fraction, double* degrade_time);
    std::string EffectivePolicy() const;
    void ApplyQueueEcnBinary(uint32_t source_tor, uint32_t destination_tor,
                             const std::vector<LaneObservation>& observations,
                             DecisionSignal* signal);
    void MaybeSelectRecoveryProbe(uint32_t source_tor, uint32_t destination_tor,
                                  uint32_t chunk_size,
                                  const std::vector<LaneObservation>& observations,
                                  DecisionSignal* signal);
    DecisionSignal BuildDecisionSignal(uint32_t source, uint32_t destination,
                                       const std::string& policy, bool has_bad_lane,
                                       uint32_t bad_lane, double bad_fraction,
                                       double decision_time, double degrade_time,
                                       uint32_t chunk_size);
    uint32_t SelectLane(uint32_t source, uint32_t destination, uint32_t chunk_size,
                        DecisionSignal* signal);
    void ScheduleV5Chunk(ScheduledChunk chunk);
    void InstallRdmaSubflow(uint32_t app_id, uint32_t pg, uint32_t source,
                            uint32_t destination, uint32_t bytes, double start_time,
                            uint32_t chunk_id, bool pin_lane, uint32_t lane,
                            const std::string& policy, bool is_bad_lane, bool has_bad_lane,
                            uint32_t bad_lane, double bad_fraction, double degrade_time);
    uint64_t PersistentPoolKey(uint32_t source, uint32_t destination, uint32_t pg,
                               uint32_t lane) const;
    PersistentPoolEntry& GetPersistentPoolEntry(uint32_t source, uint32_t destination,
                                                uint32_t pg, uint32_t lane);
    void CommitPersistentWqe(uint64_t pool_key, uint32_t app_id, uint32_t chunk_id,
                             uint32_t bytes);
    void SealPersistentPools();
    void ReadFlowInput();
    void ScheduleFlowInputs();

    const ExperimentConfig& config_;
    SimulationState& state_;
    SimulationMonitor& monitor_;
    std::ifstream flow_file_;
    FlowInput flow_input_;
    uint32_t flow_count_ = 0;
    uint64_t target_completion_count_ = 0;
    std::unordered_map<uint32_t, uint16_t> source_ports_;
    std::unordered_map<uint32_t, uint16_t> destination_ports_;
    std::map<uint64_t, std::vector<uint64_t>> oracle_lane_bytes_;
    std::map<uint64_t, std::vector<double>> oracle_lane_deficits_;
    std::map<uint64_t, std::vector<double>> online_published_capacities_;
    std::map<uint64_t, std::vector<uint64_t>> online_published_versions_;
    std::map<uint64_t, uint64_t> online_published_epochs_;
    std::map<uint64_t, std::vector<uint64_t>> last_probe_epochs_;
    std::map<uint64_t, std::vector<uint64_t>> binary_hold_until_ns_;
    DecisionSignal current_decision_signal_;
    std::map<uint64_t, PersistentPoolEntry> persistent_pool_;
    double last_persistent_commit_time_ = 0.0;
    bool persistent_pool_seal_scheduled_ = false;
};

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_TRAFFIC_SCHEDULER_H
