#ifndef NLB_HOST_RESOURCE_LB_H
#define NLB_HOST_RESOURCE_LB_H

#include "experiment-config.h"
#include "simulation-state.h"

#include <ns3/qbb-net-device.h>
#include <ns3/rdma-driver.h>
#include <ns3/rdma-hw.h>

#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace nlb {

class HostResourceLb {
  public:
    HostResourceLb(const ExperimentConfig& config, SimulationState& state);
    ~HostResourceLb();

    bool LoadTupleSpec();
    bool Initialize();
    void Close();

    bool IsEnabled() const { return config_.hrlb.enable != 0; }
    bool IsTargetFlow(uint32_t flow_id) const;
    bool GetBackgroundTuple(uint32_t flow_id, uint16_t* sport, uint16_t* dport) const;
    uint64_t TargetChunkCount(uint32_t bytes) const;
    const std::set<ns3::RdmaHw::OdinResourceKey>& MonitoredResources() const {
        return monitored_resources_;
    }
    const std::vector<uint32_t>& ResourceHosts() const {
        return resource_hosts_;
    }

    void AddPendingTargetMessage(uint32_t flow_id, uint32_t src, uint32_t dst, uint32_t pg,
                                 uint64_t bytes, double start_s);
    void StartAddedMessages();
    void OnResourceFrameDelivered(uint32_t host, uint64_t epoch);

  private:
    struct BgTupleSpec {
        uint32_t flow_id = 0;
        uint32_t src = 0;
        uint32_t dst = 0;
        uint32_t pg = 0;
        uint16_t sport = 0;
        uint16_t dport = 0;
        std::vector<uint32_t> expected_hops;
    };

    struct TgTupleSpec {
        uint32_t flow_id = 0;
        uint32_t message_id = 0;
        uint32_t lane = 0;
        uint32_t src = 0;
        uint32_t dst = 0;
        uint32_t pg = 0;
        uint16_t sport = 0;
        uint16_t dport = 0;
        uint32_t path_id = 0;
        std::vector<uint32_t> expected_hops;
        std::vector<ns3::RdmaHw::OdinResourceKey> resources;
    };

    struct PendingChunk {
        uint32_t chunk_id;
        uint64_t offset;
        uint32_t bytes;
        PendingChunk() : chunk_id(0), offset(0), bytes(0) {}
        PendingChunk(uint32_t c, uint64_t o, uint32_t b)
            : chunk_id(c), offset(o), bytes(b) {}
    };

    struct LaneState {
        TgTupleSpec spec;
        uint64_t qp_key = 0;
        ns3::Ptr<ns3::RdmaQueuePair> qp;
        std::vector<ns3::RdmaHw::OdinResourceKey> div_resources;
        uint32_t outstanding_wqes = 0;
        uint64_t scheduled_wqes = 0;
    };

    struct MessageState {
        uint32_t message_id = 0;
        uint32_t flow_id = 0;
        uint32_t src = 0;
        uint32_t dst = 0;
        uint32_t pg = 0;
        uint64_t total_bytes = 0;
        uint64_t start_ns = 0;
        uint64_t finish_ns = 0;
        uint32_t total_chunks = 0;
        uint32_t completed_chunks = 0;
        uint64_t completed_bytes = 0;
        uint32_t target_cursor = 0;
        bool sealed = false;
        uint64_t held_schedule_seq = 0;
        uint64_t logged_hold_seq = 0;
        std::set<uint32_t> observed_tail_holds;
        std::deque<PendingChunk> pending_chunks;
        std::vector<LaneState> lanes;
    };

    struct ExpectedFlowPath {
        std::set<std::pair<uint32_t, uint32_t>> allowed_switch_ports;
        bool is_target = false;
    };

    using FiveTupleKey = std::tuple<uint32_t, uint32_t, uint16_t, uint16_t, uint16_t>;

    static void WqeEventCallback(HostResourceLb* self, ns3::Ptr<ns3::RdmaQueuePair> qp,
                                 uint32_t event, ns3::RdmaQueuePair::WqeBoundary boundary);
    static void SwitchTxCallback(HostResourceLb* self, ns3::Ptr<ns3::QbbNetDevice> device,
                                 ns3::Ptr<const ns3::Packet> packet);
    static void ResourceDeliveredCallback(HostResourceLb* self, uint32_t host, uint64_t epoch);

    void OnWqeComplete(ns3::Ptr<ns3::RdmaQueuePair> qp, ns3::RdmaQueuePair::WqeBoundary boundary);
    void OnPostAck(uint32_t host);
    void RunSchedule(const char* trigger, uint32_t host);
    void SampleLanes();
    std::string DecisionState(const MessageState& msg) const;
    bool TryCommitOneChunk(MessageState& msg, const char* trigger, uint64_t seq);
    void SealMessage(MessageState& msg);
    void VerifyTuplePath(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint16_t pg,
                         const std::vector<uint32_t>& expected_hops,
                         std::vector<ns3::RdmaHw::OdinResourceKey>* out_resources,
                         ExpectedFlowPath* out_flow_path) const;
    void LogDecision(uint64_t seq, const char* trigger, const char* event,
                     const MessageState& msg, const PendingChunk& chunk, uint32_t nominal,
                     uint32_t chosen, bool is_redirect, const std::vector<bool>& fresh,
                     const std::vector<double>& scores, const std::vector<uint64_t>& versions,
                     const std::vector<uint64_t>& ages, uint32_t slots_before, uint32_t slots_after,
                     uint32_t cursor_before, uint32_t cursor_after, const char* reason);

    const ExperimentConfig& config_;
    SimulationState& state_;
    std::map<uint32_t, BgTupleSpec> bg_specs_;
    std::map<uint32_t, uint32_t> target_flow_to_msg_;
    std::map<uint32_t, std::vector<TgTupleSpec>> tg_specs_by_msg_;
    std::set<ns3::RdmaHw::OdinResourceKey> monitored_resources_;
    std::vector<uint32_t> resource_hosts_;
    std::map<FiveTupleKey, ExpectedFlowPath> expected_flow_paths_;
    std::map<uint32_t, MessageState> messages_;
    std::vector<uint32_t> active_message_ids_;
    std::vector<uint32_t> pending_start_msgs_;
    std::set<uint32_t> connected_hosts_;
    std::map<uint32_t, size_t> msg_rr_cursors_;
    std::map<uint32_t, uint64_t> delivered_epochs_;
    uint64_t schedule_seq_ = 0;
    std::set<uint32_t> post_ack_hosts_;
    ns3::EventId lane_sample_event_;
    std::string decision_state_;
    FILE* lane_output_ = NULL;
    FILE* tail_output_ = NULL;
    FILE* decision_output_ = NULL;
    FILE* message_output_ = NULL;
};

}  // namespace nlb

#endif  // NLB_HOST_RESOURCE_LB_H
