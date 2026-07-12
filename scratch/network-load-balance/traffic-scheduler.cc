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
#include <cstdio>
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
    if (config_.traffic.v5_nsub > 1 || PersistentPoolEnabled()) {
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
    persistent_pool_.clear();
    last_persistent_commit_time_ = 0.0;
    persistent_pool_seal_scheduled_ = false;
}

void TrafficScheduler::Start() {
    flow_input_.idx = 0;
    if (flow_count_ > 0) {
        ReadFlowInput();
        Simulator::Schedule(Seconds(0), &TrafficScheduler::ScheduleFlowInputs, this);
    }
}

bool TrafficScheduler::PersistentPoolEnabled() const {
    return config_.traffic.v5_qp_pool != 0;
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

uint64_t TrafficScheduler::LaneCounterKey(uint32_t source_tor, uint32_t destination_tor,
                                          bool post_degrade) const {
    return (static_cast<uint64_t>(source_tor) << 33) |
           (static_cast<uint64_t>(destination_tor) << 1) | (post_degrade ? 1 : 0);
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
                                  double* bad_fraction, double* degrade_time) {
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
    for (const auto& event : config_.failures.link_degrade_events) {
        if ((event.node_a == source_tor &&
             event.node_b == config_.traffic.v5_oracle_bad_spine) ||
            (event.node_b == source_tor &&
             event.node_a == config_.traffic.v5_oracle_bad_spine)) {
            local_degrade = true;
            *bad_fraction = event.fraction;
            *degrade_time = config_.flowgen_start_time + event.time_us / 1000000.0;
            break;
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

std::string TrafficScheduler::ActivePolicy(bool has_bad_lane, double chunk_start_time,
                                           double degrade_time) const {
    if (!has_bad_lane || chunk_start_time < degrade_time) {
        return "uniform";
    }
    return config_.traffic.v5_oracle_policy;
}

uint32_t TrafficScheduler::SelectLane(uint32_t source, uint32_t destination, uint32_t chunk_size,
                                      const std::string& policy, bool has_bad_lane,
                                      uint32_t bad_lane, double bad_fraction,
                                      double chunk_start_time, double degrade_time) {
    uint32_t source_tor = Settings::hostIp2SwitchId[state_.server_addresses[source].Get()];
    uint32_t destination_tor =
        Settings::hostIp2SwitchId[state_.server_addresses[destination].Get()];
    uint32_t lane_count = LaneCount();
    std::vector<double> weights(lane_count, 1.0);
    if (has_bad_lane && bad_lane < lane_count) {
        if (policy == "proportional") {
            weights[bad_lane] = std::max(0.0, bad_fraction);
        } else if (policy == "avoid") {
            weights[bad_lane] = 0.0;
        }
    }

    bool post_degrade =
        has_bad_lane && chunk_start_time >= degrade_time && policy != "uniform";
    uint64_t key = LaneCounterKey(source_tor, destination_tor, post_degrade);
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

void TrafficScheduler::InstallRdmaSubflow(uint32_t pg, uint32_t source, uint32_t destination,
                                          uint32_t bytes, double start_time, uint32_t chunk_id,
                                          bool pin_lane, uint32_t lane,
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
        uint64_t start_ns = static_cast<uint64_t>(start_time * 1000000000.0);
        uint32_t source_tor =
            Settings::hostIp2SwitchId[state_.server_addresses[source].Get()];
        uint32_t destination_tor =
            Settings::hostIp2SwitchId[state_.server_addresses[destination].Get()];
        uint32_t out_port = pin_lane ? LaneOutPort(source_tor, destination_tor, lane)
                                     : static_cast<uint32_t>(-1);
        std::ostringstream capacities;
        std::ostringstream weights;
        for (uint32_t current_lane = 0; current_lane < LaneCount();
             ++current_lane) {
            if (current_lane > 0) {
                capacities << ';';
                weights << ';';
            }
            uint32_t current_port = LaneOutPort(source_tor, destination_tor, current_lane);
            uint64_t capacity_bps = 0;
            if (current_port != static_cast<uint32_t>(-1)) {
                Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(
                    state_.nodes.Get(source_tor)->GetDevice(current_port));
                capacity_bps = device ? device->GetDataRate().GetBitRate() : 0;
            }
            if (has_bad_lane && current_lane == bad_lane && start_time >= degrade_time) {
                capacity_bps = static_cast<uint64_t>(capacity_bps * bad_fraction);
            }
            capacities << capacity_bps;
            double weight = 1.0;
            if (has_bad_lane && current_lane == bad_lane) {
                if (policy == "avoid") {
                    weight = 0.0;
                } else if (policy == "proportional") {
                    weight = bad_fraction;
                }
            }
            weights << weight;
        }
        std::ostringstream deficits;
        if (PersistentPoolEnabled()) {
            bool post_degrade = has_bad_lane && start_time >= degrade_time && policy != "uniform";
            uint64_t deficit_key = LaneCounterKey(source_tor, destination_tor, post_degrade);
            const auto& lane_deficits = oracle_lane_deficits_[deficit_key];
            for (uint32_t current_lane = 0; current_lane < lane_deficits.size(); ++current_lane) {
                if (current_lane > 0) {
                    deficits << ';';
                }
                deficits << lane_deficits[current_lane];
            }
        } else {
            deficits << "na";
        }
        fprintf(chunk_output,
                "%u %u %u %u %u %u %u %lu %u %s %u %u %lu %u %u %u %u %s %s %s\n",
                flow_input_.idx,
                chunk_id, source, destination, source_port, destination_port, bytes, start_ns,
                pin_lane ? lane : static_cast<uint32_t>(-1), policy.c_str(),
                is_bad_lane ? 1 : 0, has_bad_lane ? 1 : 0,
                PersistentPoolEnabled() ? pool_key : qp_key, source_tor,
                destination_tor, out_port, 0, capacities.str().c_str(), weights.str().c_str(),
                deficits.str().c_str());
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
                            pool_key, flow_input_.idx, chunk_id, bytes);
        return;
    }

    RdmaClientHelper client_helper(
        pg, state_.server_addresses[source], state_.server_addresses[destination], source_port,
        destination_port, bytes, window, base_rtt);
    client_helper.SetAttribute("StatFlowID", IntegerValue(flow_input_.idx));
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
        assert(state_.nodes.Get(source)->GetNodeType() == 0 &&
               state_.nodes.Get(destination)->GetNodeType() == 0);

        if (ChunkModeEnabled()) {
            uint32_t bad_lane;
            double bad_fraction;
            double degrade_time;
            bool has_bad_lane =
                GetBadLane(source, destination, &bad_lane, &bad_fraction, &degrade_time);
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
                std::string policy = ActivePolicy(has_bad_lane, chunk_start, degrade_time);
                uint32_t lane = SelectLane(source, destination, chunk_length, policy,
                                           has_bad_lane, bad_lane, bad_fraction, chunk_start,
                                           degrade_time);
                InstallRdmaSubflow(priority_group, source, destination, chunk_length, chunk_start,
                                   chunk_id, true, lane, policy,
                                   has_bad_lane && lane == bad_lane, has_bad_lane, bad_lane,
                                   bad_fraction, degrade_time);
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
            for (uint32_t i = 0; i < subflow_count; ++i) {
                uint32_t subflow_length = base_length + (i == subflow_count - 1 ? remainder : 0);
                InstallRdmaSubflow(priority_group, source, destination, subflow_length,
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
