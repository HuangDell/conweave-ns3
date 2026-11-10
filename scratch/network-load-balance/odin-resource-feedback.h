#ifndef NLB_ODIN_RESOURCE_FEEDBACK_H
#define NLB_ODIN_RESOURCE_FEEDBACK_H

#include "experiment-config.h"
#include "simulation-state.h"
#include <ns3/rdma-hw.h>
#include <cstdio>
#include <deque>

namespace nlb {

// Experimental same-ToR, ideal control channel. Explicit propagation/age, zero
// wire overhead. Measurements use actual port counters, never workload/oracle
// budgets. Disabled experiments create neither a file nor a periodic event.
class OdinResourceFeedback {
 public:
    OdinResourceFeedback(const ExperimentConfig& config, SimulationState& state)
        : config_(config), state_(state) {}
    bool Start();
    void Close();

 private:
    struct Port {
        uint32_t destination_ip = 0;
        ns3::Ptr<ns3::QbbNetDevice> device;
        std::deque<std::pair<uint64_t, uint64_t>> history;
    };
    void Tick();
    void Deliver(uint64_t version, std::vector<ns3::OdinResourceSample> samples);
    void Write(const char* event, uint64_t version,
               const std::vector<ns3::OdinResourceSample>& samples, uint32_t eligible);
    const ExperimentConfig& config_;
    SimulationState& state_;
    ns3::Ptr<ns3::SwitchNode> tor_;
    ns3::Ptr<ns3::RdmaHw> hardware_;
    std::vector<Port> ports_;
    ns3::EventId tick_;
    uint64_t version_ = 0;
    FILE* output_ = NULL;
};
}  // namespace nlb
#endif
