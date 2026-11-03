#include "traffic-scheduler.h"

#include "monitoring.h"

#include <ns3/applications-module.h>
#include <ns3/letflow-routing.h>
#include <ns3/qbb-net-device.h>
#include <ns3/rdma-client-helper.h>
#include <ns3/rdma-client.h>
#include <ns3/rdma-driver.h>
#include <ns3/settings.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>

namespace nlb {

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NetworkLoadBalanceTraffic");

TrafficScheduler::TrafficScheduler(const ExperimentConfig& config, SimulationState& state,
                                   SimulationMonitor& monitor)
    : config_(config), state_(state), monitor_(monitor) {}

bool TrafficScheduler::LoadFlowFile() {
    flow_file_.open(config_.io.flow_file.c_str());
    if (!flow_file_.is_open()) {
        std::cerr << "Error: cannot open flow file " << config_.io.flow_file << '\n';
        return false;
    }
    flow_file_ >> flow_count_;
    if (!flow_file_) {
        std::cerr << "Error: malformed flow file header in " << config_.io.flow_file << '\n';
        return false;
    }
    target_completion_count_ = CalculateTargetCompletionCount();
    if (config_.traffic.v5_nsub > 1 || PersistentPoolEnabled() || config_.monitoring.native_resource_log) {
        std::cerr << "V5_TARGET_COMPLETION_COUNT\t\t" << target_completion_count_ << "\n";
    }
    return true;
}

void TrafficScheduler::InitializePorts() {
    for (uint32_t i = 0; i < state_.node_num; ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 0) {
            source_ports_[i] = 10000;
            destination_ports_[i] = 100;
        }
    }
    LetflowRouting::ClearPinnedLanes();
    oracle_lane_bytes_.clear();
    oracle_lane_deficits_.clear();
    online_published_capacities_.clear();
    online_published_versions_.clear();
    online_published_epochs_.clear();
    last_probe_epochs_.clear();
    binary_hold_until_ns_.clear();
    persistent_pool_.clear();
    last_persistent_commit_time_ = 0.0;
    persistent_pool_seal_scheduled_ = false;
    host_probe_entry_ = PersistentPoolEntry();
    host_probe_entry_created_ = false;
    host_probe_outstanding_ = false;
    host_probe_epoch_ = 0;
}

void TrafficScheduler::Start() {
    flow_input_.idx = 0;
    if (flow_count_ > 0) {
        ReadFlowInput();
        Simulator::Schedule(Seconds(0), &TrafficScheduler::ScheduleFlowInputs, this);
    }
    if (HostSensingEnabled()) {
        Ptr<RdmaDriver> driver =
            state_.nodes.Get(config_.host_sensing.target_source)->GetObject<RdmaDriver>();
        driver->TraceConnectWithoutContext(
            "WqeEvent", MakeCallback(&TrafficScheduler::HostProbeWqeEvent, this));
        double warmup_start = config_.flowgen_start_time -
                              config_.host_sensing.warmup_us / 1000000.0;
        NS_ASSERT_MSG(warmup_start >= 0.0, "host probe warm-up starts before time zero");
        Simulator::Schedule(Seconds(warmup_start),
                            &TrafficScheduler::ScheduleHostProbeEpoch, this);
        Simulator::Schedule(Seconds(config_.flowgen_stop_time),
                            &TrafficScheduler::SealHostProbe, this);
    }
}

bool TrafficScheduler::PersistentPoolEnabled() const {
    return config_.traffic.v5_qp_pool != 0;
}

bool TrafficScheduler::HostSensingEnabled() const {
    return config_.host_sensing.enable != 0;
}

uint32_t TrafficScheduler::LaneCount() const {
    return PersistentPoolEnabled() ? config_.traffic.v5_qp_pool_size
                                   : config_.traffic.v5_nsub;
}

bool TrafficScheduler::ChunkModeEnabled() const {
    return config_.traffic.v5_chunk_bytes > 0 && LaneCount() > 1;
}

uint64_t TrafficScheduler::ChunkCount(uint32_t bytes) const {
    if (bytes == 0) {
        bytes = 1;
    }
    if (!ChunkModeEnabled()) {
        return std::min<uint32_t>(LaneCount(), bytes);
    }
    if (config_.traffic.v5_size_gate_bytes > 0 &&
        bytes < config_.traffic.v5_size_gate_bytes) {
        return 1;
    }
    return (bytes + config_.traffic.v5_chunk_bytes - 1) / config_.traffic.v5_chunk_bytes;
}

uint64_t TrafficScheduler::CalculateTargetCompletionCount() {
    if (LaneCount() <= 1) {
        return flow_count_;
    }
    std::streampos first_flow_position = flow_file_.tellg();
    uint64_t target = 0;
    for (uint32_t i = 0; i < flow_count_; ++i) {
        uint32_t source = 0;
        uint32_t destination = 0;
        uint32_t priority_group = 0;
        uint32_t target_length = 0;
        double start_time = 0;
        flow_file_ >> source >> destination >> priority_group >> target_length >> start_time;
        if (target_length == 0) {
            target_length = 1;
        }
        target += ChunkCount(target_length);
    }
    flow_file_.clear();
    flow_file_.seekg(first_flow_position);
    return target;
}

uint64_t TrafficScheduler::LaneCounterKey(uint32_t source_tor,
                                          uint32_t destination_tor) const {
    return (static_cast<uint64_t>(source_tor) << 32) | destination_tor;
}

uint32_t TrafficScheduler::LaneOutPort(uint32_t source_tor, uint32_t destination_tor,
                                       uint32_t lane) const {
    auto switch_iterator = state_.tor_by_id.find(source_tor);
    if (switch_iterator == state_.tor_by_id.end()) {
        return static_cast<uint32_t>(-1);
    }
    auto path_iterator =
        switch_iterator->second->m_mmu->m_letflowRouting.m_letflowRoutingTable.find(
            destination_tor);
    if (path_iterator ==
            switch_iterator->second->m_mmu->m_letflowRouting.m_letflowRoutingTable.end() ||
        lane >= path_iterator->second.size()) {
        return static_cast<uint32_t>(-1);
    }
    auto path_id = path_iterator->second.begin();
    std::advance(path_id, lane);
    return LetflowRouting::GetOutPortFromPath(*path_id, 0);
}

bool TrafficScheduler::GetBadLane(uint32_t source, uint32_t destination, uint32_t* bad_lane,
                                  double decision_time, double* bad_fraction,
                                  double* degrade_time) {
    *bad_lane = static_cast<uint32_t>(-1);
    *bad_fraction = 1.0;
    *degrade_time = std::numeric_limits<double>::max();
    if (config_.traffic.v5_oracle_bad_spine >= state_.nodes.GetN()) {
        return false;
    }

    uint32_t source_tor = Settings::hostIp2SwitchId[state_.server_addresses[source].Get()];
    uint32_t destination_tor =
        Settings::hostIp2SwitchId[state_.server_addresses[destination].Get()];
    bool local_degrade = false;
    double latest_effective_time = -1.0;
    for (const auto& event : config_.failures.link_degrade_events) {
        bool a_to_b_matches =
            event.node_a == source_tor &&
            event.node_b == config_.traffic.v5_oracle_bad_spine;
        bool b_to_a_matches =
            config_.failures.link_degrade_bidirectional && event.node_b == source_tor &&
            event.node_a == config_.traffic.v5_oracle_bad_spine;
        if (a_to_b_matches || b_to_a_matches) {
            local_degrade = true;
            double event_time = config_.flowgen_start_time + event.time_us / 1000000.0;
            if (event_time <= decision_time && event_time >= latest_effective_time) {
                latest_effective_time = event_time;
                *bad_fraction = event.fraction;
                *degrade_time = event_time;
            }
        }
    }
    if (!local_degrade) {
        return false;
    }

    Ptr<Node> source_node = state_.nodes.Get(source_tor);
    Ptr<Node> spine_node = state_.nodes.Get(config_.traffic.v5_oracle_bad_spine);
    if (state_.neighbor_interfaces.find(source_node) == state_.neighbor_interfaces.end() ||
        state_.neighbor_interfaces[source_node].find(spine_node) ==
            state_.neighbor_interfaces[source_node].end()) {
        return false;
    }
    uint32_t bad_output_port = state_.neighbor_interfaces[source_node][spine_node].idx;

    auto switch_iterator = state_.tor_by_id.find(source_tor);
    if (switch_iterator == state_.tor_by_id.end()) {
        return false;
    }
    auto path_iterator = switch_iterator->second->m_mmu->m_letflowRouting.m_letflowRoutingTable.find(
        destination_tor);
    if (path_iterator ==
        switch_iterator->second->m_mmu->m_letflowRouting.m_letflowRoutingTable.end()) {
        return false;
    }

    uint32_t lane = 0;
    for (const auto& path_id : path_iterator->second) {
        if (LetflowRouting::GetOutPortFromPath(path_id, 0) == bad_output_port) {
            *bad_lane = lane;
            return true;
        }
        ++lane;
    }
    return false;
}

std::string TrafficScheduler::EffectivePolicy() const {
    if (!config_.traffic.v5_policy.empty()) {
        return config_.traffic.v5_policy;
    }
    return config_.traffic.v5_oracle_policy;
}

void TrafficScheduler::ApplyQueueEcnBinary(
    uint32_t source_tor, uint32_t destination_tor,
    const std::vector<LaneObservation>& observations, DecisionSignal* signal) {
    uint32_t lane_count = observations.size();
    NS_ASSERT_MSG(lane_count == LaneCount(), "binary observation count must match lanes");
    uint64_t key = LaneCounterKey(source_tor, destination_tor);
    std::vector<uint64_t>& holds = binary_hold_until_ns_[key];
    if (holds.size() != lane_count) {
        holds.assign(lane_count, 0);
    }
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    uint64_t hold_ns = config_.traffic.v5_binary_hold_us * 1000;
    uint32_t available = 0;
    for (uint32_t lane = 0; lane < lane_count; ++lane) {
        const LaneObservation& observation = observations[lane];
        auto threshold = config_.cc.rate_to_kmin.find(observation.nominal_bps);
        uint64_t ecn_kmin = threshold == config_.cc.rate_to_kmin.end()
                                ? 0
                                : threshold->second;
        bool congested = observation.queue_bytes > 0 &&
                         observation.queue_bytes >= ecn_kmin;
        signal->binary_congested[lane] = congested ? 1 : 0;
        if (congested) {
            holds[lane] = std::max(holds[lane], now_ns + hold_ns);
        }
        signal->binary_hold_until_ns[lane] = holds[lane];
        if (now_ns >= holds[lane]) {
            ++available;
            signal->capacities_bps[lane] = observation.nominal_bps;
        } else {
            signal->capacities_bps[lane] = 0.0;
        }
    }
    if (available == 0) {
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            signal->capacities_bps[lane] = observations[lane].nominal_bps;
        }
    }
}

void TrafficScheduler::ApplySeqBalanceBinary(
    uint32_t source, uint32_t destination, uint32_t pg,
    const std::vector<LaneObservation>& observations, DecisionSignal* signal) {
    uint32_t lane_count = observations.size();
    NS_ASSERT_MSG(lane_count == LaneCount(), "SeqBalance observation count must match lanes");
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    uint64_t phi_ns = config_.traffic.v5_seqbalance_phi_us * 1000;
    uint32_t available = 0;
    std::vector<double> nominal_capacities = signal->capacities_bps;
    for (uint32_t lane = 0; lane < lane_count; ++lane) {
        uint64_t cnp_count = 0;
        uint64_t last_cnp_ns = 0;
        auto entry = persistent_pool_.find(PersistentPoolKey(source, destination, pg, lane));
        if (entry != persistent_pool_.end()) {
            Ptr<RdmaDriver> driver = state_.nodes.Get(source)->GetObject<RdmaDriver>();
            Ptr<RdmaQueuePair> qp = driver->m_rdma->GetQp(entry->second.qp_key);
            if (qp) {
                cnp_count = qp->m_v5_ecn_cnp_count;
                last_cnp_ns = qp->m_v5_last_ecn_cnp_ns;
            }
        }
        uint64_t hold_until = last_cnp_ns == 0 ? 0 : last_cnp_ns + phi_ns;
        bool inactive = now_ns < hold_until;
        signal->seqbalance_cnp_counts[lane] = cnp_count;
        signal->seqbalance_last_cnp_ns[lane] = last_cnp_ns;
        signal->binary_congested[lane] = inactive ? 1 : 0;
        signal->binary_hold_until_ns[lane] = hold_until;
        signal->capacities_bps[lane] = inactive ? 0.0 : nominal_capacities[lane];
        available += inactive ? 0 : 1;
    }

    if (available == 0) {
        signal->binary_all_inactive_fallback = true;
        uint64_t earliest = *std::min_element(signal->binary_hold_until_ns.begin(),
                                              signal->binary_hold_until_ns.end());
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            if (signal->binary_hold_until_ns[lane] == earliest) {
                signal->capacities_bps[lane] = nominal_capacities[lane];
            }
        }
    }
}

void TrafficScheduler::MaybeSelectRecoveryProbe(
    uint32_t source_tor, uint32_t destination_tor, uint32_t chunk_size,
    const std::vector<LaneObservation>& observations, DecisionSignal* signal) {
    if (config_.traffic.v5_probe_period_us == 0 ||
        config_.traffic.v5_probe_bytes == 0 || chunk_size == 0) {
        return;
    }
    uint64_t stale_ns = static_cast<uint64_t>(
        config_.lb.sflowlet_est_time.GetNanoSeconds()) *
        config_.traffic.v5_estimator_stale_periods;
    uint64_t estimator_period_ns = static_cast<uint64_t>(
        config_.lb.sflowlet_est_time.GetNanoSeconds());
    uint64_t period_ns = config_.traffic.v5_probe_period_us * 1000;
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    uint64_t probe_epoch = now_ns / period_ns;
    uint64_t time_to_sample_ns = estimator_period_ns - now_ns % estimator_period_ns;
    uint32_t probe_cap = std::min(config_.traffic.v5_probe_bytes,
                                  config_.traffic.v5_chunk_bytes);
    uint64_t key = LaneCounterKey(source_tor, destination_tor);
    std::vector<uint64_t>& last_epochs = last_probe_epochs_[key];
    if (last_epochs.size() != observations.size()) {
        last_epochs.assign(observations.size(), std::numeric_limits<uint64_t>::max());
    }
    for (uint32_t lane = 0; lane < observations.size(); ++lane) {
        const LaneObservation& observation = observations[lane];
        bool stale_degraded = observation.degraded_state &&
                              observation.valid_sample_count > 0 &&
                              observation.sample_age_ns >= stale_ns;
        uint64_t probe_window_ns = observation.nominal_bps == 0
                                       ? 0
                                       : static_cast<uint64_t>(std::ceil(
                                             static_cast<long double>(chunk_size) * 8.0L *
                                             1000000000.0L / observation.nominal_bps));
        if (stale_degraded && last_epochs[lane] != probe_epoch &&
            chunk_size <= probe_cap && time_to_sample_ns <= probe_window_ns) {
            signal->is_probe = true;
            signal->probe_epoch = probe_epoch;
            signal->probe_bytes = chunk_size;
            signal->probe_lane = lane;
            last_epochs[lane] = probe_epoch;
            return;
        }
    }
}

TrafficScheduler::DecisionSignal TrafficScheduler::BuildDecisionSignal(
    uint32_t source, uint32_t destination, uint32_t pg, const std::string& policy,
    bool has_bad_lane,
    uint32_t bad_lane, double bad_fraction, double decision_time, double degrade_time,
    uint32_t chunk_size) {
    uint32_t source_tor = Settings::hostIp2SwitchId[state_.server_addresses[source].Get()];
    uint32_t destination_tor =
        Settings::hostIp2SwitchId[state_.server_addresses[destination].Get()];
    uint32_t lane_count = LaneCount();
    DecisionSignal signal;
    signal.capacities_bps.assign(lane_count, 0.0);
    signal.weights.assign(lane_count, 1.0);
    signal.binary_congested.assign(lane_count, 0);
    signal.binary_hold_until_ns.assign(lane_count, 0);
    signal.seqbalance_cnp_counts.assign(lane_count, 0);
    signal.seqbalance_last_cnp_ns.assign(lane_count, 0);
    signal.published_versions.assign(lane_count, 0);
    signal.publication_sources.assign(lane_count, kHealthyNominal);
    signal.sample_ages_ns.assign(lane_count, 0);
    signal.degraded_states.assign(lane_count, 0);
    if (source_tor == destination_tor) {
        signal.weights.assign(lane_count, 1.0 / lane_count);
        return signal;
    }
    auto switch_iterator = state_.tor_by_id.find(source_tor);
    NS_ASSERT_MSG(switch_iterator != state_.tor_by_id.end(), "source ToR is missing");
    ResidualCapacityEstimator& estimator =
        switch_iterator->second->m_mmu->m_residualEstimator;
    std::vector<ResidualCapacityEstimator::PortSnapshot> snapshots;
    snapshots.reserve(lane_count);
    std::vector<LaneObservation> observations;
    observations.reserve(lane_count);
    for (uint32_t lane = 0; lane < lane_count; ++lane) {
        uint32_t out_port = LaneOutPort(source_tor, destination_tor, lane);
        ResidualCapacityEstimator::PortSnapshot snapshot = estimator.GetSnapshot(out_port);
        snapshots.push_back(snapshot);
        Ptr<QbbNetDevice> device = out_port == static_cast<uint32_t>(-1)
                                       ? Ptr<QbbNetDevice>()
                                       : DynamicCast<QbbNetDevice>(
                                             state_.nodes.Get(source_tor)->GetDevice(out_port));
        LaneObservation observation;
        observation.out_port = out_port;
        observation.nominal_bps = snapshot.nominalBps;
        observation.queue_bytes = device && device->GetQueue()
                                      ? device->GetQueue()->GetNBytesTotal()
                                      : 0;
        observation.sample_age_ns = snapshot.sampleAgeNs;
        observation.valid_sample_count = snapshot.validSampleCount;
        observation.degraded_state = snapshot.degradedState;
        observations.push_back(observation);
        signal.published_versions[lane] = snapshot.sampleVersion;
        signal.sample_ages_ns[lane] = snapshot.sampleAgeNs;
        signal.degraded_states[lane] = snapshot.degradedState ? 1 : 0;
        if (snapshot.nominalBps > 0) {
            signal.capacities_bps[lane] = snapshot.nominalBps;
        } else if (policy == "online-proportional" || policy == "queue-ecn-binary") {
            NS_ASSERT_MSG(false, "S4 policy requires every lane to have a registered estimator port");
        } else if (out_port != static_cast<uint32_t>(-1)) {
            signal.capacities_bps[lane] = device ? device->GetDataRate().GetBitRate() : 0;
        }
        signal.estimate_version = std::max(signal.estimate_version,
                                           snapshot.estimateVersion);
        if (snapshot.validSampleCount > 0) {
            signal.estimate_age_ns = std::max(signal.estimate_age_ns,
                                              snapshot.sampleAgeNs);
        }
    }

    bool oracle_proportional = policy == "oracle-proportional" || policy == "proportional";
    bool oracle_avoid = policy == "oracle-avoid" || policy == "avoid";
    if ((oracle_proportional || oracle_avoid) && has_bad_lane && bad_lane < lane_count &&
        decision_time >= degrade_time) {
        signal.capacities_bps[bad_lane] *= oracle_avoid ? 0.0 : std::max(0.0, bad_fraction);
    } else if (policy == "online-proportional") {
        uint64_t stale_ns = static_cast<uint64_t>(
            config_.lb.sflowlet_est_time.GetNanoSeconds()) *
            config_.traffic.v5_estimator_stale_periods;
        uint64_t key = LaneCounterKey(source_tor, destination_tor);
        std::vector<double>& published = online_published_capacities_[key];
        std::vector<uint64_t>& published_versions = online_published_versions_[key];
        if (published.size() != lane_count || published_versions.size() != lane_count) {
            published.assign(lane_count, 0.0);
            published_versions.assign(lane_count, 0);
            for (uint32_t lane = 0; lane < lane_count; ++lane) {
                const auto& snapshot = snapshots[lane];
                published[lane] = snapshot.degradedState && snapshot.validSampleCount > 0
                                      ? std::max(0.0, std::min(
                                            snapshot.ewmaCEffBps,
                                            static_cast<double>(snapshot.nominalBps)))
                                      : snapshot.nominalBps;
                published_versions[lane] = snapshot.degradedState
                                               ? snapshot.sampleVersion
                                               : 0;
                signal.publication_changed |=
                    std::fabs(published[lane] -
                              static_cast<double>(snapshot.nominalBps)) > 0.5;
            }
        }

        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            const auto& snapshot = snapshots[lane];
            double next_capacity = published[lane];
            uint64_t next_version = published_versions[lane];
            uint32_t source = kHoldDegraded;
            if (snapshot.validSampleCount == 0 ||
                (snapshot.sampleAgeNs > stale_ns && !snapshot.degradedState)) {
                next_capacity = snapshot.nominalBps;
                next_version = 0;
                source = kHealthyNominal;
            } else if (snapshot.sampleAgeNs <= stale_ns) {
                next_capacity = std::max(
                    0.0, std::min(snapshot.ewmaCEffBps,
                                  static_cast<double>(snapshot.nominalBps)));
                next_version = snapshot.sampleVersion;
                source = kFreshEstimate;
            }
            if (std::fabs(next_capacity - published[lane]) > 0.5) {
                signal.publication_changed = true;
            }
            published[lane] = next_capacity;
            published_versions[lane] = next_version;
            signal.publication_sources[lane] = source;
        }

        signal.capacities_bps = published;
        signal.published_versions = published_versions;
        uint64_t& published_epoch = online_published_epochs_[key];
        for (uint64_t version : published_versions) {
            published_epoch = std::max(published_epoch, version);
        }
        signal.estimate_version = published_epoch;
    } else if (policy == "queue-ecn-binary") {
        ApplyQueueEcnBinary(source_tor, destination_tor, observations, &signal);
    } else if (policy == "seqbalance-style") {
        ApplySeqBalanceBinary(source, destination, pg, observations, &signal);
    }

    if (policy == "online-proportional") {
        MaybeSelectRecoveryProbe(source_tor, destination_tor, chunk_size,
                                 observations, &signal);
    }

    if (policy == "uniform") {
        signal.weights.assign(lane_count, 1.0 / lane_count);
    } else if (policy == "seqbalance-style") {
        uint32_t active = std::count_if(
            signal.capacities_bps.begin(), signal.capacities_bps.end(),
            [](double capacity) { return capacity > 0.0; });
        NS_ASSERT_MSG(active > 0, "SeqBalance-style must retain a fallback lane");
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            signal.weights[lane] = signal.capacities_bps[lane] > 0.0
                                       ? 1.0 / active
                                       : 0.0;
        }
    } else {
        double capacity_sum = 0.0;
        for (double capacity : signal.capacities_bps) {
            capacity_sum += capacity;
        }
        NS_ASSERT_MSG(capacity_sum > 0.0, "all lane capacities are zero");
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            signal.weights[lane] = signal.capacities_bps[lane] / capacity_sum;
        }
    }
    return signal;
}

uint32_t TrafficScheduler::SelectLane(uint32_t source, uint32_t destination,
                                      uint32_t chunk_size, DecisionSignal* signal) {
    uint32_t source_tor = Settings::hostIp2SwitchId[state_.server_addresses[source].Get()];
    uint32_t destination_tor =
        Settings::hostIp2SwitchId[state_.server_addresses[destination].Get()];
    uint32_t lane_count = LaneCount();
    const std::vector<double>& weights = signal->weights;
    uint64_t key = LaneCounterKey(source_tor, destination_tor);
    if (PersistentPoolEnabled()) {
        std::vector<double>& deficits = oracle_lane_deficits_[key];
        if (deficits.size() != lane_count) {
            deficits.assign(lane_count, 0.0);
        }
        double weight_sum = 0.0;
        for (double weight : weights) {
            weight_sum += weight;
        }
        NS_ASSERT_MSG(weight_sum > 0.0, "all persistent-pool lane weights are zero");
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            deficits[lane] += weights[lane] / weight_sum * chunk_size;
        }
        if (signal->is_probe) {
            NS_ASSERT_MSG(signal->probe_lane < lane_count,
                          "probe lane must belong to the persistent pool");
            deficits[signal->probe_lane] -= chunk_size;
            signal->deficits = deficits;
            return signal->probe_lane;
        }
        uint32_t best_lane = 0;
        double best_deficit = -std::numeric_limits<double>::max();
        double best_weight = -1.0;
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            if (weights[lane] <= 0.0) {
                continue;
            }
            if (deficits[lane] > best_deficit ||
                (deficits[lane] == best_deficit && weights[lane] > best_weight)) {
                best_deficit = deficits[lane];
                best_weight = weights[lane];
                best_lane = lane;
            }
        }
        deficits[best_lane] -= chunk_size;
        signal->deficits = deficits;
        return best_lane;
    }

    std::vector<uint64_t>& assigned = oracle_lane_bytes_[key];
    if (assigned.size() != lane_count) {
        assigned.assign(lane_count, 0);
    }

    uint32_t best_lane = 0;
    double best_score = std::numeric_limits<double>::max();
    double best_weight = -1.0;
    for (uint32_t lane = 0; lane < lane_count; ++lane) {
        if (weights[lane] <= 0.0) {
            continue;
        }
        double score = static_cast<double>(assigned[lane]) / weights[lane];
        if (score < best_score || (score == best_score && weights[lane] > best_weight)) {
            best_score = score;
            best_weight = weights[lane];
            best_lane = lane;
        }
    }
    assigned[best_lane] += chunk_size;
    return best_lane;
}

void TrafficScheduler::ScheduleV5Chunk(ScheduledChunk chunk) {
    uint32_t app_id = chunk.app_id;
    uint32_t pg = chunk.pg;
    uint32_t source = chunk.source;
    uint32_t destination = chunk.destination;
    uint32_t bytes = chunk.bytes;
    uint32_t chunk_id = chunk.chunk_id;
    std::string policy = EffectivePolicy();
    bool oracle_policy = policy == "oracle-proportional" || policy == "oracle-avoid" ||
                         policy == "proportional" || policy == "avoid";
    uint32_t bad_lane = static_cast<uint32_t>(-1);
    double bad_fraction = 1.0;
    double degrade_time = std::numeric_limits<double>::max();
    bool has_bad_lane = false;
    double decision_time = Simulator::Now().GetSeconds();
    if (oracle_policy) {
        has_bad_lane =
            GetBadLane(source, destination, &bad_lane, decision_time, &bad_fraction,
                       &degrade_time);
    }
    current_decision_signal_ = BuildDecisionSignal(
        source, destination, pg, policy, has_bad_lane, bad_lane, bad_fraction,
        decision_time, degrade_time, bytes);
    uint32_t lane = 0;
    if (HostSensingEnabled()) {
        auto fixed_lane = config_.host_sensing.app_lanes.find(app_id);
        NS_ASSERT_MSG(fixed_lane != config_.host_sensing.app_lanes.end(),
                      "host sensing requires a frozen lane for every app");
        lane = fixed_lane->second;
        NS_ASSERT_MSG(lane < LaneCount(), "frozen app lane is outside the QP pool");
        current_decision_signal_.weights.assign(LaneCount(), 0.0);
        current_decision_signal_.weights[lane] = 1.0;
    } else {
        lane = SelectLane(source, destination, bytes, &current_decision_signal_);
    }
    InstallRdmaSubflow(app_id, pg, source, destination, bytes, decision_time,
                       chunk_id, true, lane, policy,
                       has_bad_lane && lane == bad_lane, has_bad_lane, bad_lane,
                       bad_fraction, degrade_time);
}

void TrafficScheduler::InstallRdmaSubflow(uint32_t app_id, uint32_t pg,
                                          uint32_t source, uint32_t destination,
                                          uint32_t bytes, double start_time,
                                          uint32_t chunk_id, bool pin_lane, uint32_t lane,
                                          const std::string& policy, bool is_bad_lane,
                                          bool has_bad_lane, uint32_t bad_lane,
                                          double bad_fraction, double degrade_time) {
    uint64_t pool_key = 0;
    uint32_t source_port = 0;
    uint32_t destination_port = 0;
    uint64_t qp_key = 0;
    if (PersistentPoolEnabled()) {
        NS_ASSERT_MSG(pin_lane, "persistent QP pool requires an explicitly pinned lane");
        PersistentPoolEntry& entry =
            GetPersistentPoolEntry(source, destination, pg, lane);
        pool_key = PersistentPoolKey(source, destination, pg, lane);
        source_port = entry.source_port;
        destination_port = entry.destination_port;
        qp_key = entry.qp_key;
        ++entry.scheduled_wqes;
        last_persistent_commit_time_ = std::max(last_persistent_commit_time_, start_time);
    } else {
        source_port = source_ports_[source]++;
        destination_port = destination_ports_[destination]++;
        qp_key = LetflowRouting::GetQpKey(state_.server_addresses[destination].Get(),
                                          source_port, destination_port, pg);
    }

    if (pin_lane) {
        LetflowRouting::RegisterPinnedLane(state_.server_addresses[source].Get(), qp_key, lane);
    }

    FILE* chunk_output = monitor_.V5ChunkOutput();
    if (chunk_output) {
        // The decision executes at Simulator::Now(). Avoid converting the scheduling
        // helper's double timestamp: values on an exact ns boundary can truncate 1ns
        // early and falsely make an estimate version appear to come from the future.
        uint64_t start_ns = Simulator::Now().GetNanoSeconds();
        uint32_t source_tor =
            Settings::hostIp2SwitchId[state_.server_addresses[source].Get()];
        uint32_t destination_tor =
            Settings::hostIp2SwitchId[state_.server_addresses[destination].Get()];
        uint32_t out_port = pin_lane ? LaneOutPort(source_tor, destination_tor, lane)
                                     : static_cast<uint32_t>(-1);
        std::ostringstream capacities;
        std::ostringstream weights;
        capacities << std::setprecision(17);
        weights << std::setprecision(17);
        for (uint32_t current_lane = 0; current_lane < LaneCount(); ++current_lane) {
            if (current_lane > 0) {
                capacities << ';';
                weights << ';';
            }
            capacities << current_decision_signal_.capacities_bps[current_lane];
            weights << current_decision_signal_.weights[current_lane];
        }
        std::ostringstream deficits;
        if (PersistentPoolEnabled()) {
            const auto& lane_deficits = current_decision_signal_.deficits;
            for (uint32_t current_lane = 0; current_lane < lane_deficits.size(); ++current_lane) {
                if (current_lane > 0) {
                    deficits << ';';
                }
                deficits << lane_deficits[current_lane];
            }
        } else {
            deficits << "na";
        }
        std::ostringstream binary_congested;
        std::ostringstream binary_hold_until;
        std::ostringstream published_versions;
        std::ostringstream publication_sources;
        std::ostringstream sample_ages;
        std::ostringstream degraded_states;
        std::ostringstream seqbalance_cnp_counts;
        std::ostringstream seqbalance_last_cnp;
        for (uint32_t current_lane = 0; current_lane < LaneCount(); ++current_lane) {
            if (current_lane > 0) {
                binary_congested << ';';
                binary_hold_until << ';';
                published_versions << ';';
                publication_sources << ';';
                sample_ages << ';';
                degraded_states << ';';
                seqbalance_cnp_counts << ';';
                seqbalance_last_cnp << ';';
            }
            binary_congested << current_decision_signal_.binary_congested[current_lane];
            binary_hold_until << current_decision_signal_.binary_hold_until_ns[current_lane];
            published_versions << current_decision_signal_.published_versions[current_lane];
            publication_sources << current_decision_signal_.publication_sources[current_lane];
            sample_ages << current_decision_signal_.sample_ages_ns[current_lane];
            degraded_states << current_decision_signal_.degraded_states[current_lane];
            seqbalance_cnp_counts << current_decision_signal_.seqbalance_cnp_counts[current_lane];
            seqbalance_last_cnp << current_decision_signal_.seqbalance_last_cnp_ns[current_lane];
        }
        fprintf(chunk_output,
                "%u %u %u %u %u %u %u %lu %u %s %u %u %lu %u %u %u %lu %s %s %s %lu "
                "%u %lu %u %u %s %s %s %s %u %s %s %s %s %u\n",
                app_id, chunk_id, source, destination, source_port, destination_port,
                bytes, start_ns,
                pin_lane ? lane : static_cast<uint32_t>(-1), policy.c_str(),
                is_bad_lane ? 1 : 0, has_bad_lane ? 1 : 0,
                PersistentPoolEnabled() ? pool_key : qp_key, source_tor,
                destination_tor, out_port, current_decision_signal_.estimate_version,
                capacities.str().c_str(), weights.str().c_str(), deficits.str().c_str(),
                current_decision_signal_.estimate_age_ns,
                current_decision_signal_.is_probe ? 1 : 0,
                current_decision_signal_.probe_epoch,
                current_decision_signal_.probe_bytes,
                current_decision_signal_.probe_lane,
                binary_congested.str().c_str(), binary_hold_until.str().c_str(),
                published_versions.str().c_str(), publication_sources.str().c_str(),
                current_decision_signal_.publication_changed ? 1 : 0,
                sample_ages.str().c_str(), degraded_states.str().c_str(),
                seqbalance_cnp_counts.str().c_str(), seqbalance_last_cnp.str().c_str(),
                current_decision_signal_.binary_all_inactive_fallback ? 1 : 0);
        fflush(chunk_output);
    }

    if (state_.pair_rtt.find(state_.nodes.Get(source)) == state_.pair_rtt.end() ||
        state_.pair_rtt[state_.nodes.Get(source)].find(state_.nodes.Get(destination)) ==
            state_.pair_rtt[state_.nodes.Get(source)].end()) {
        std::cerr << "pairRtt src: " << source << " -> dst: " << destination
                  << " ==> cannot be found from database" << std::endl;
        assert(false);
    }

    uint32_t window =
        config_.cc.has_win
            ? (config_.cc.global_t == 1
                   ? state_.max_bdp
                   : state_.pair_bdp[state_.nodes.Get(source)][state_.nodes.Get(destination)])
            : 0;
    uint64_t base_rtt =
        config_.cc.global_t == 1
            ? state_.max_rtt
            : state_.pair_rtt[state_.nodes.Get(source)][state_.nodes.Get(destination)];

    if (PersistentPoolEnabled()) {
        double start_delay = std::max(0.0, start_time - Simulator::Now().GetSeconds());
        Simulator::Schedule(Seconds(start_delay), &TrafficScheduler::CommitPersistentWqe, this,
                            pool_key, app_id, chunk_id, bytes);
        return;
    }

    RdmaClientHelper client_helper(
        pg, state_.server_addresses[source], state_.server_addresses[destination], source_port,
        destination_port, bytes, window, base_rtt);
    client_helper.SetAttribute("StatFlowID", IntegerValue(app_id));
    ApplicationContainer applications = client_helper.Install(state_.nodes.Get(source));
    double start_delay = std::max(0.0, start_time - Simulator::Now().GetSeconds());
    applications.Start(Seconds(start_delay));
    applications.Stop(Seconds(100.0));
}

uint64_t TrafficScheduler::PersistentPoolKey(uint32_t source, uint32_t destination, uint32_t pg,
                                              uint32_t lane) const {
    NS_ASSERT_MSG(source <= 0xffff && destination <= 0xffff && pg <= 0xffff && lane <= 0xffff,
                  "persistent pool key field overflow");
    return (static_cast<uint64_t>(source) << 48) |
           (static_cast<uint64_t>(destination) << 32) |
           (static_cast<uint64_t>(pg) << 16) | lane;
}

TrafficScheduler::PersistentPoolEntry& TrafficScheduler::GetPersistentPoolEntry(
    uint32_t source, uint32_t destination, uint32_t pg, uint32_t lane) {
    uint64_t key = PersistentPoolKey(source, destination, pg, lane);
    auto inserted = persistent_pool_.emplace(key, PersistentPoolEntry());
    PersistentPoolEntry& entry = inserted.first->second;
    if (inserted.second) {
        entry.source = source;
        entry.destination = destination;
        entry.pg = static_cast<uint16_t>(pg);
        entry.lane = lane;
        entry.source_port = source_ports_[source]++;
        entry.destination_port = destination_ports_[destination]++;
        entry.qp_key = LetflowRouting::GetQpKey(
            state_.server_addresses[destination].Get(), entry.source_port,
            entry.destination_port, entry.pg);
        LetflowRouting::RegisterPinnedLane(state_.server_addresses[source].Get(), entry.qp_key,
                                           lane);
    }
    return entry;
}

void TrafficScheduler::CommitPersistentWqe(uint64_t pool_key, uint32_t app_id,
                                           uint32_t chunk_id, uint32_t bytes) {
    auto entry_iterator = persistent_pool_.find(pool_key);
    NS_ASSERT_MSG(entry_iterator != persistent_pool_.end(), "unknown persistent pool entry");
    const PersistentPoolEntry& entry = entry_iterator->second;
    uint32_t source = entry.source;
    uint32_t destination = entry.destination;
    uint32_t window =
        config_.cc.has_win
            ? (config_.cc.global_t == 1
                   ? state_.max_bdp
                   : state_.pair_bdp[state_.nodes.Get(source)][state_.nodes.Get(destination)])
            : 0;
    uint64_t base_rtt =
        config_.cc.global_t == 1
            ? state_.max_rtt
            : state_.pair_rtt[state_.nodes.Get(source)][state_.nodes.Get(destination)];
    Ptr<RdmaDriver> driver = state_.nodes.Get(source)->GetObject<RdmaDriver>();
    driver->AppendPersistentWqe(
        entry.pg, state_.server_addresses[source], state_.server_addresses[destination],
        entry.source_port, entry.destination_port, window, base_rtt,
        static_cast<int32_t>(app_id), chunk_id, bytes, entry.lane);
}

void TrafficScheduler::SealPersistentPools() {
    for (const auto& item : persistent_pool_) {
        const PersistentPoolEntry& entry = item.second;
        Ptr<RdmaDriver> driver = state_.nodes.Get(entry.source)->GetObject<RdmaDriver>();
        driver->SealPersistentQueuePair(state_.server_addresses[entry.destination].Get(),
                                        entry.source_port, entry.destination_port, entry.pg,
                                        entry.scheduled_wqes);
    }
}

void TrafficScheduler::ScheduleHostProbeEpoch() {
    NS_ASSERT_MSG(HostSensingEnabled(), "host probe scheduled while sensing is disabled");
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    uint64_t stop_ns = static_cast<uint64_t>(config_.flowgen_stop_time * 1000000000.0);
    if (now_ns >= stop_ns) {
        return;
    }

    uint32_t probe_id = host_probe_epoch_++;
    if (host_probe_outstanding_) {
        monitor_.RecordHostProbeSkipped(probe_id);
    } else {
        if (!host_probe_entry_created_) {
            host_probe_entry_created_ = true;
            host_probe_entry_.source = config_.host_sensing.target_source;
            host_probe_entry_.destination = config_.host_sensing.target_destination;
            host_probe_entry_.pg = static_cast<uint16_t>(config_.host_sensing.target_pg);
            host_probe_entry_.lane = config_.host_sensing.target_lane;
            host_probe_entry_.source_port = source_ports_[host_probe_entry_.source]++;
            host_probe_entry_.destination_port =
                destination_ports_[host_probe_entry_.destination]++;
            host_probe_entry_.qp_key = LetflowRouting::GetQpKey(
                state_.server_addresses[host_probe_entry_.destination].Get(),
                host_probe_entry_.source_port, host_probe_entry_.destination_port,
                host_probe_entry_.pg);
            LetflowRouting::RegisterPinnedLane(
                state_.server_addresses[host_probe_entry_.source].Get(),
                host_probe_entry_.qp_key, host_probe_entry_.lane);
        }

        uint32_t source = host_probe_entry_.source;
        uint32_t destination = host_probe_entry_.destination;
        uint32_t window =
            config_.cc.has_win
                ? (config_.cc.global_t == 1
                       ? state_.max_bdp
                       : state_.pair_bdp[state_.nodes.Get(source)][state_.nodes.Get(destination)])
                : 0;
        uint64_t base_rtt =
            config_.cc.global_t == 1
                ? state_.max_rtt
                : state_.pair_rtt[state_.nodes.Get(source)][state_.nodes.Get(destination)];
        host_probe_outstanding_ = true;
        ++host_probe_entry_.scheduled_wqes;
        Ptr<RdmaDriver> driver = state_.nodes.Get(source)->GetObject<RdmaDriver>();
        driver->AppendPersistentWqe(
            host_probe_entry_.pg, state_.server_addresses[source],
            state_.server_addresses[destination], host_probe_entry_.source_port,
            host_probe_entry_.destination_port, window, base_rtt,
            static_cast<int32_t>(config_.host_sensing.target_app_id), probe_id,
            config_.host_sensing.probe_bytes, host_probe_entry_.lane,
            RdmaQueuePair::V5_PROBE);
    }

    Simulator::Schedule(MicroSeconds(config_.host_sensing.probe_period_us),
                        &TrafficScheduler::ScheduleHostProbeEpoch, this);
}

void TrafficScheduler::SealHostProbe() {
    if (!host_probe_entry_created_) {
        return;
    }
    Ptr<RdmaDriver> driver =
        state_.nodes.Get(host_probe_entry_.source)->GetObject<RdmaDriver>();
    driver->SealPersistentQueuePair(
        state_.server_addresses[host_probe_entry_.destination].Get(),
        host_probe_entry_.source_port, host_probe_entry_.destination_port,
        host_probe_entry_.pg, host_probe_entry_.scheduled_wqes);
}

void TrafficScheduler::HostProbeWqeEvent(Ptr<RdmaQueuePair> queue_pair, uint32_t event,
                                         RdmaQueuePair::WqeBoundary boundary) {
    (void)boundary;
    if (queue_pair->m_v5_traffic_class != RdmaQueuePair::V5_PROBE ||
        event != RdmaHw::WQE_COMPLETE) {
        return;
    }
    NS_ASSERT_MSG(host_probe_outstanding_, "probe completed without an outstanding epoch");
    host_probe_outstanding_ = false;
}

void TrafficScheduler::ReadFlowInput() {
    if (flow_input_.idx < flow_count_) {
        flow_file_ >> flow_input_.src >> flow_input_.dst >> flow_input_.pg >>
            flow_input_.max_packet_count >> flow_input_.start_time;
        assert(state_.nodes.Get(flow_input_.src)->GetNodeType() == 0 &&
               state_.nodes.Get(flow_input_.dst)->GetNodeType() == 0);
    } else {
        std::cout << "*** input flow is over the prefixed number -- flow number : " << flow_count_
                  << std::endl;
        std::cout << "*** flow_input.idx : " << flow_input_.idx << std::endl;
        std::cout << "*** THIS IS THE LAST FLOW TO SEND :) " << std::endl;
    }
}

void TrafficScheduler::ScheduleFlowInputs() {
    NS_LOG_DEBUG("ScheduleFlowInputs at " << Simulator::Now());
    while (flow_input_.idx < flow_count_ &&
           Seconds(flow_input_.start_time) == Simulator::Now()) {
        uint32_t target_length = flow_input_.max_packet_count;
        if (target_length == 0) {
            target_length = 1;
        }
        uint32_t source = flow_input_.src;
        uint32_t destination = flow_input_.dst;
        uint32_t priority_group = flow_input_.pg;
        uint32_t app_id = flow_input_.idx;
        assert(state_.nodes.Get(source)->GetNodeType() == 0 &&
               state_.nodes.Get(destination)->GetNodeType() == 0);

        if (ChunkModeEnabled()) {
            double bytes_per_second =
                config_.traffic.v5_chunk_commit_rate_gbps > 0.0
                    ? config_.traffic.v5_chunk_commit_rate_gbps * 1000000000.0 / 8.0
                    : std::numeric_limits<double>::max();
            uint32_t remaining = target_length;
            uint32_t chunk_id = 0;
            uint64_t committed_bytes = 0;
            bool no_split = config_.traffic.v5_size_gate_bytes > 0 &&
                            target_length < config_.traffic.v5_size_gate_bytes;
            while (remaining > 0) {
                uint32_t chunk_length =
                    no_split ? remaining
                             : std::min<uint32_t>(remaining, config_.traffic.v5_chunk_bytes);
                double chunk_start =
                    flow_input_.start_time + static_cast<double>(committed_bytes) / bytes_per_second;
                double start_delay = std::max(0.0, chunk_start - Simulator::Now().GetSeconds());
                ScheduledChunk scheduled_chunk = {
                    app_id, priority_group, source, destination, chunk_length, chunk_id};
                Simulator::Schedule(Seconds(start_delay), &TrafficScheduler::ScheduleV5Chunk,
                                    this, scheduled_chunk);
                last_persistent_commit_time_ =
                    std::max(last_persistent_commit_time_, chunk_start);
                remaining -= chunk_length;
                committed_bytes += chunk_length;
                ++chunk_id;
            }
        } else {
            uint32_t subflow_count = LaneCount();
            if (subflow_count > target_length) {
                subflow_count = target_length;
            }
            uint32_t base_length = target_length / subflow_count;
            uint32_t remainder = target_length % subflow_count;
            current_decision_signal_ = BuildDecisionSignal(
                source, destination, priority_group, "uniform", false,
                static_cast<uint32_t>(-1), 1.0,
                Simulator::Now().GetSeconds(), std::numeric_limits<double>::max(), 0);
            for (uint32_t i = 0; i < subflow_count; ++i) {
                uint32_t subflow_length = base_length + (i == subflow_count - 1 ? remainder : 0);
                InstallRdmaSubflow(
                    app_id, priority_group, source, destination, subflow_length,
                    flow_input_.start_time, i, PersistentPoolEnabled(), i,
                    PersistentPoolEnabled() ? "uniform" : "legacy", false, false,
                    static_cast<uint32_t>(-1), 1.0,
                    std::numeric_limits<double>::max());
            }
        }
        ++flow_input_.idx;
        ReadFlowInput();
    }

    if (flow_input_.idx < flow_count_) {
        Simulator::Schedule(Seconds(flow_input_.start_time) - Simulator::Now(),
                            &TrafficScheduler::ScheduleFlowInputs, this);
    } else {
        flow_file_.close();
        if (PersistentPoolEnabled() && !persistent_pool_seal_scheduled_) {
            persistent_pool_seal_scheduled_ = true;
            double seal_time = std::max(Simulator::Now().GetSeconds(),
                                        last_persistent_commit_time_) + 1e-9;
            Simulator::Schedule(Seconds(seal_time) - Simulator::Now(),
                                &TrafficScheduler::SealPersistentPools, this);
        }
    }
}

}  // namespace nlb
