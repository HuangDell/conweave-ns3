#ifndef NLB_ODIN_RESOURCE_FEEDBACK_H
#define NLB_ODIN_RESOURCE_FEEDBACK_H

#include "experiment-config.h"
#include "simulation-state.h"
#include <ns3/rdma-hw.h>
#include <cstdio>
#include <deque>
#include <set>

namespace nlb {

// Experimental ideal control channel. Explicit propagation/age, zero
// wire overhead. Measurements use actual port counters, never workload/oracle
// budgets. Disabled experiments create neither a file nor a periodic event.
class OdinResourceFeedback {
 public:
    OdinResourceFeedback(const ExperimentConfig& config, SimulationState& state)
        : config_(config), state_(state) {}
    void SetMultiHopResources(const std::set<ns3::RdmaHw::OdinResourceKey>& resources) {
        multihop_resources_ = resources;
    }
    void SetMultiHopHosts(const std::vector<uint32_t>& hosts) {
        multihop_hosts_ = hosts;
    }
    bool Start();
    void Close();

 private:
    struct Port {
        uint32_t destination_ip = 0;
        ns3::Ptr<ns3::SwitchNode> sw;
        ns3::Ptr<ns3::QbbNetDevice> device;
        std::deque<std::pair<uint64_t, uint64_t>> history;
    };
    void Tick();
    void Deliver(uint64_t version, std::vector<ns3::OdinResourceSample> samples);
    void WriteForHost(const char* event, uint32_t host_id, uint64_t version,
                      const std::vector<ns3::OdinResourceSample>& samples, uint32_t eligible);
    const ExperimentConfig& config_;
    SimulationState& state_;
    std::set<ns3::RdmaHw::OdinResourceKey> multihop_resources_;
    std::vector<uint32_t> multihop_hosts_;
    std::vector<std::pair<uint32_t, ns3::Ptr<ns3::RdmaHw>>> multihop_hardwares_;
    ns3::Ptr<ns3::SwitchNode> tor_;
    ns3::Ptr<ns3::RdmaHw> hardware_;
    std::vector<Port> ports_;
    ns3::EventId tick_;
    uint64_t version_ = 0;
    FILE* output_ = NULL;
};
}  // namespace nlb
#endif
