#ifndef NETWORK_LOAD_BALANCE_MONITORING_H
#define NETWORK_LOAD_BALANCE_MONITORING_H

#include "experiment-config.h"
#include "simulation-state.h"

#include <ns3/qbb-net-device.h>
#include <ns3/residual-capacity-estimator.h>
#include <ns3/rdma-driver.h>
#include <ns3/rdma-hw.h>

#include <cstdio>

namespace nlb {

class SimulationMonitor {
  public:
    SimulationMonitor(const ExperimentConfig& config, SimulationState& state);
    ~SimulationMonitor();

    bool OpenCoreOutputs();
    bool OpenLateOutputs();
    void CloseOutputs();

    void ConnectPfcTrace(ns3::Ptr<ns3::QbbNetDevice> device);
    void ConnectQpComplete(ns3::Ptr<ns3::RdmaDriver> driver);
    void StartCnpMonitoring(ns3::Ptr<ns3::RdmaHw> hardware);
    void StartPeriodicMonitoring();
    void StartCompletionMonitoring(uint64_t target_completion_count);
    void ScheduleModeHistory();

    FILE* V5ChunkOutput() const { return v5_chunk_output_; }
    void RecordEstimatorBatch(
        uint64_t time_ns, uint32_t switch_id,
        const std::vector<ns3::ResidualCapacityEstimator::PortSnapshot>& snapshots);

  private:
    static void PfcTraceCallback(SimulationMonitor* monitor,
                                 ns3::Ptr<ns3::QbbNetDevice> device, uint32_t type);
    static void QpCompleteCallback(SimulationMonitor* monitor,
                                   ns3::Ptr<ns3::RdmaQueuePair> queue_pair);
    static void PersistentQpEventCallback(SimulationMonitor* monitor,
                                          ns3::Ptr<ns3::RdmaQueuePair> queue_pair,
                                          uint32_t event);
    static void WqeEventCallback(SimulationMonitor* monitor,
                                 ns3::Ptr<ns3::RdmaQueuePair> queue_pair, uint32_t event,
                                 ns3::RdmaQueuePair::WqeBoundary boundary);
    void RecordPfc(ns3::Ptr<ns3::QbbNetDevice> device, uint32_t type);
    void RecordQpFinish(ns3::Ptr<ns3::RdmaQueuePair> queue_pair);
    void RecordPersistentQpEvent(ns3::Ptr<ns3::RdmaQueuePair> queue_pair, uint32_t event);
    void RecordQpState(ns3::Ptr<ns3::RdmaQueuePair> queue_pair, const char* event);
    void RecordWqeEvent(ns3::Ptr<ns3::RdmaQueuePair> queue_pair, uint32_t event,
                        ns3::RdmaQueuePair::WqeBoundary boundary);
    void MonitorCnp(ns3::Ptr<ns3::RdmaHw> hardware);
    void MonitorPeriodic();
    void MonitorCompletion();

    void PrintModeHistory();
    void PrintCongaHistory();
    void PrintLetflowHistory();
    void PrintConweaveHistory();
    void PrintSflowletHistory();
    void CloseFile(FILE** file);

    const ExperimentConfig& config_;
    SimulationState& state_;
    uint64_t target_completion_count_ = 0;
    bool history_printed_ = false;

    FILE* pfc_output_ = NULL;
    FILE* fct_output_ = NULL;
    FILE* flow_input_output_ = NULL;
    FILE* cnp_output_ = NULL;
    FILE* voq_output_ = NULL;
    FILE* voq_detail_output_ = NULL;
    FILE* uplink_output_ = NULL;
    FILE* conn_output_ = NULL;
    FILE* path_delay_output_ = NULL;
    FILE* ooo_event_output_ = NULL;
    FILE* v5_chunk_output_ = NULL;
    FILE* v5_wqe_output_ = NULL;
    FILE* v5_qp_state_output_ = NULL;
    FILE* v5_estimator_output_ = NULL;
};

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_MONITORING_H
