#ifndef NETWORK_LOAD_BALANCE_SIMULATION_STATE_H
#define NETWORK_LOAD_BALANCE_SIMULATION_STATE_H

#include <ns3/internet-module.h>
#include <ns3/switch-node.h>

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nlb {

struct Interface {
    uint32_t idx = 0;
    bool up = false;
    uint64_t delay = 0;
    uint64_t bw = 0;
};

struct SimulationState {
    uint32_t node_num = 0;
    uint32_t switch_num = 0;
    uint32_t link_num = 0;

    ns3::NodeContainer nodes;
    std::vector<ns3::Ipv4Address> server_addresses;
    std::vector<std::pair<uint32_t, uint32_t>> link_pairs;

    std::map<ns3::Ptr<ns3::Node>, std::map<ns3::Ptr<ns3::Node>, Interface>> neighbor_interfaces;
    std::map<ns3::Ptr<ns3::Node>,
             std::map<ns3::Ptr<ns3::Node>, std::vector<ns3::Ptr<ns3::Node>>>>
        next_hops;
    std::map<ns3::Ptr<ns3::Node>, std::map<ns3::Ptr<ns3::Node>, uint64_t>> pair_delay;
    std::map<ns3::Ptr<ns3::Node>, std::map<ns3::Ptr<ns3::Node>, uint64_t>> pair_tx_delay;
    std::map<ns3::Ptr<ns3::Node>, std::map<ns3::Ptr<ns3::Node>, uint64_t>> pair_bw;
    std::map<ns3::Ptr<ns3::Node>, std::map<ns3::Ptr<ns3::Node>, uint64_t>> pair_bdp;
    std::map<ns3::Ptr<ns3::Node>, std::map<ns3::Ptr<ns3::Node>, uint64_t>> pair_rtt;

    std::map<uint32_t, std::vector<uint32_t>> tor_uplink_interfaces;
    std::map<uint32_t, std::vector<uint32_t>> tor_downlink_interfaces;
    std::unordered_map<uint32_t, ns3::Ptr<ns3::SwitchNode>> tor_by_id;

    uint64_t max_rtt = 0;
    uint64_t max_bdp = 0;
};

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_SIMULATION_STATE_H
