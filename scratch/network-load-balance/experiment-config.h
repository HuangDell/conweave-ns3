#ifndef NETWORK_LOAD_BALANCE_EXPERIMENT_CONFIG_H
#define NETWORK_LOAD_BALANCE_EXPERIMENT_CONFIG_H

#include <ns3/core-module.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nlb {

namespace LbMode {
constexpr uint32_t kEcmp = 0;
constexpr uint32_t kDrill = 2;
constexpr uint32_t kConga = 3;
constexpr uint32_t kLetflow = 6;
constexpr uint32_t kConweave = 9;
constexpr uint32_t kSflowlet = 11;
}  // namespace LbMode

struct LoadBalancerConfig {
    uint32_t mode = LbMode::kEcmp;

    ns3::Time conga_flowlet_timeout = ns3::MicroSeconds(100);
    ns3::Time conga_dre_time = ns3::MicroSeconds(50);
    ns3::Time conga_aging_time = ns3::MicroSeconds(500);
    uint32_t conga_quantize_bit = 3;
    double conga_alpha = 0.2;

    ns3::Time letflow_flowlet_timeout = ns3::MicroSeconds(100);
    ns3::Time letflow_aging_time = ns3::MilliSeconds(2);

    ns3::Time conweave_extra_reply_deadline = ns3::MicroSeconds(4);
    ns3::Time conweave_path_pause_time = ns3::MicroSeconds(8);
    ns3::Time conweave_tx_expiry_time = ns3::MicroSeconds(1000);
    ns3::Time conweave_extra_voq_flush_time = ns3::MicroSeconds(32);
    ns3::Time conweave_default_voq_waiting_time = ns3::MicroSeconds(500);
    bool conweave_path_aware_rerouting = true;

    ns3::Time sflowlet_est_time = ns3::MicroSeconds(200);
    double sflowlet_ewma_beta = 1.0 / 64.0;
    uint32_t sflowlet_persist_windows = 3;
    double sflowlet_degrade_ratio = 0.85;
    uint64_t sflowlet_backlog_thresh_bytes = 0;
    uint32_t sflowlet_weight_mode = 1;
    ns3::Time sflowlet_flowlet_timeout = ns3::MicroSeconds(100);
    uint32_t sflowlet_switch_log = 0;
    uint32_t sflowlet_ooo_log = 0;
};

struct TrafficConfig {
    uint32_t v5_nsub = 1;
    uint32_t v5_chunk_bytes = 0;
    uint32_t v5_size_gate_bytes = 104000;
    std::string v5_oracle_policy = "uniform";
    uint32_t v5_oracle_bad_spine = 0;
    double v5_chunk_commit_rate_gbps = 100.0;
    uint32_t v5_chunk_log = 0;
    uint32_t v5_qp_pool = 0;
    uint32_t v5_qp_pool_size = 4;
    uint32_t v5_wqe_log = 0;
    uint32_t v5_qp_state_log = 0;
};

struct CongestionControlConfig {
    uint32_t mode = 1;
    bool enable_qcn = true;
    bool enable_pfc = true;
    bool use_dynamic_pfc_threshold = true;
    double pause_time = 5;

    double alpha_resume_interval = 55;
    double rp_timer = 300;
    double ewma_gain = 0.0;
    double rate_decrease_interval = 4;
    uint32_t fast_recovery_times = 1;
    std::string rate_ai;
    std::string rate_hai;
    std::string min_rate = "100Mb/s";
    std::string dctcp_rate_ai = "1000Mb/s";

    bool clamp_target_rate = false;
    bool l2_back_to_zero = false;
    uint32_t l2_chunk_size = 0;
    uint32_t l2_ack_interval = 0;
    uint32_t has_win = 1;
    uint32_t global_t = 1;
    uint32_t mi_thresh = 5;
    bool var_win = false;
    bool fast_react = true;
    bool multi_rate = true;
    bool sample_feedback = false;
    double u_target = 0.95;
    uint32_t int_multi = 1;
    bool rate_bound = true;
    int enable_irn = 0;

    std::unordered_map<uint64_t, uint32_t> rate_to_kmax;
    std::unordered_map<uint64_t, uint32_t> rate_to_kmin;
    std::unordered_map<uint64_t, double> rate_to_pmax;
};

struct MonitoringConfig {
    uint64_t qlen_start = 0;
    uint64_t qlen_end = 0;
    uint32_t switch_interval_ns = 10000;
    uint64_t cnp_start_ns = 0;
    uint64_t cnp_bucket_ns = 100000;
    uint64_t irn_start_ns = 0;
    uint64_t irn_bucket_ns = 100000;
};

struct IoConfig {
    std::string topology_file;
    std::string flow_file;
    std::string flow_input_file = "flow.txt";
    std::string fct_output_file = "fct.txt";
    std::string pfc_output_file = "pfc.txt";
    std::string cnp_output_file = "cnp.txt";
    std::string qlen_mon_file = "qlen.txt";
    std::string voq_mon_file = "voq.txt";
    std::string voq_mon_detail_file = "voq_detail.txt";
    std::string uplink_mon_file = "uplink.txt";
    std::string conn_mon_file = "conn.txt";
    std::string path_delay_mon_file = "path_delay.txt";
    std::string est_error_output_file = "est_error.txt";
    std::string flowlet_switch_output_file = "flowlet_switch.txt";
    std::string ooo_event_output_file = "ooo_events.txt";
    std::string v5_chunk_output_file = "v5_chunk.txt";
    std::string v5_wqe_output_file = "v5_wqe.txt";
    std::string v5_qp_state_output_file = "v5_qp_state.txt";
};

struct LinkDegradeEvent {
    uint64_t time_us;
    uint32_t node_a;
    uint32_t node_b;
    double fraction;
};

struct FailureConfig {
    uint64_t link_down_time_us = 0;
    uint32_t link_down_a = 0;
    uint32_t link_down_b = 0;
    std::vector<LinkDegradeEvent> link_degrade_events;
};

struct ExperimentConfig {
    LoadBalancerConfig lb;
    TrafficConfig traffic;
    CongestionControlConfig cc;
    MonitoringConfig monitoring;
    IoConfig io;
    FailureConfig failures;

    uint64_t one_hop_delay_ns = 1000;
    std::string data_rate;
    std::string link_delay;
    uint32_t packet_payload_size = 1000;
    double flowgen_start_time = 2.0;
    double flowgen_stop_time = 2.5;
    double simulator_extra_time = 0.1;
    double error_rate_per_link = 0.0;
    uint32_t buffer_size_mb = 0;
    double load = 10.0;
    int random_seed = 1;
};

bool LoadExperimentConfig(const std::string& path, ExperimentConfig* config);

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_EXPERIMENT_CONFIG_H
