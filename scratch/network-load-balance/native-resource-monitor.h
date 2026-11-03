#ifndef NLB_NATIVE_RESOURCE_MONITOR_H
#define NLB_NATIVE_RESOURCE_MONITOR_H

#include "experiment-config.h"
#include "simulation-state.h"
#include <ns3/qbb-net-device.h>
#include <ns3/rdma-driver.h>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace nlb {

// Passive observations only: no changes to admission, scheduling, CC or routing.
class NativeResourceMonitor {
  public:
    NativeResourceMonitor(const ExperimentConfig& config, SimulationState& state);
    bool Open();
    void ConnectDevice(ns3::Ptr<ns3::QbbNetDevice> device);
    void ConnectDriver(ns3::Ptr<ns3::RdmaDriver> driver);
    void Sample();
    void Close();

  private:
    struct QpState {
        ns3::Ptr<ns3::RdmaQueuePair> qp;
        bool sent = false;
        bool complete = false;
    };
    struct PauseState {
        uint64_t start_ns = 0;
        uint64_t total_ns = 0;
        bool paused = false;
    };
    using PauseKey = std::tuple<uint32_t, uint32_t, uint32_t>;
    static void Registered(NativeResourceMonitor* self, ns3::Ptr<ns3::RdmaQueuePair> qp);
    static void Completed(NativeResourceMonitor* self, ns3::Ptr<ns3::RdmaQueuePair> qp);
    static void Tx(NativeResourceMonitor* self, ns3::Ptr<ns3::QbbNetDevice> device,
                   ns3::Ptr<const ns3::Packet> packet);
    static void Pause(NativeResourceMonitor* self, ns3::Ptr<ns3::QbbNetDevice> device,
                      uint32_t pg, bool paused);
    void QpEvent(ns3::Ptr<ns3::RdmaQueuePair> qp, const char* event);
    void WritePause(const PauseKey& key, const PauseState& state, bool censored);

    const ExperimentConfig& config_;
    SimulationState& state_;
    std::map<int32_t, QpState> qps_;
    std::map<uint32_t, std::vector<QpState*>> host_qps_;
    std::map<uint32_t, std::set<int32_t>> serviced_;
    std::map<uint32_t, uint64_t> payload_attempt_bytes_;
    std::map<PauseKey, PauseState> pauses_;
    FILE* resources_ = NULL;
    FILE* qp_events_ = NULL;
    FILE* pfc_ = NULL;
    uint64_t last_sample_ns_ = 0;
    bool sampled_ = false;
};

}  // namespace nlb
#endif
