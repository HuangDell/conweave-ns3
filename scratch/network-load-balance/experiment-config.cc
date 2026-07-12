#include "experiment-config.h"

#include <fstream>
#include <iostream>

namespace nlb {

bool LoadExperimentConfig(const std::string& path, ExperimentConfig* config) {
    std::ifstream conf(path.c_str());
    if (!conf.is_open()) {
        std::cerr << "Error: cannot open config file " << path << '\n';
        return false;
    }

    std::string key;
    while (conf >> key) {
        if (key == "FLOW_INPUT_FILE") {
            conf >> config->io.flow_input_file;
            std::cerr << "FLOW_INPUT_FILE\t\t\t" << config->io.flow_input_file << "\n";
        } else if (key == "CNP_OUTPUT_FILE") {
            conf >> config->io.cnp_output_file;
            std::cerr << "CNP_OUTPUT_FILE\t\t\t" << config->io.cnp_output_file << "\n";
        } else if (key == "EST_ERROR_MON_FILE") {
            conf >> config->io.est_error_output_file;
            std::cerr << "EST_ERROR_MON_FILE\t\t\t" << config->io.est_error_output_file << "\n";
        } else if (key == "LB_MODE") {
            conf >> config->lb.mode;
            std::cerr << "LB_MODE\t\t\t" << config->lb.mode << "\n";
        } else if (key == "SW_MONITORING_INTERVAL") {
            conf >> config->monitoring.switch_interval_ns;
            std::cerr << "SW_MONITORING_INTERVAL\t\t\t"
                      << config->monitoring.switch_interval_ns << "\n";
        } else if (key == "SFLOWLET_WEIGHT_MODE") {
            conf >> config->lb.sflowlet_weight_mode;
            std::cerr << "SFLOWLET_WEIGHT_MODE\t\t\t" << config->lb.sflowlet_weight_mode
                      << "\n";
        } else if (key == "SFLOWLET_EST_TIME_US") {
            uint32_t value = 0;
            conf >> value;
            config->lb.sflowlet_est_time = ns3::MicroSeconds(value);
            std::cerr << "SFLOWLET_EST_TIME_US\t\t\t" << config->lb.sflowlet_est_time << "\n";
        } else if (key == "SFLOWLET_FLOWLET_TIMEOUT_US") {
            uint32_t value = 0;
            conf >> value;
            config->lb.sflowlet_flowlet_timeout = ns3::MicroSeconds(value);
            std::cerr << "SFLOWLET_FLOWLET_TIMEOUT_US\t\t"
                      << config->lb.sflowlet_flowlet_timeout << "\n";
        } else if (key == "SFLOWLET_SWITCH_LOG") {
            conf >> config->lb.sflowlet_switch_log;
            std::cerr << "SFLOWLET_SWITCH_LOG\t\t\t" << config->lb.sflowlet_switch_log << "\n";
        } else if (key == "SFLOWLET_OOO_LOG") {
            conf >> config->lb.sflowlet_ooo_log;
            std::cerr << "SFLOWLET_OOO_LOG\t\t\t" << config->lb.sflowlet_ooo_log << "\n";
        } else if (key == "V5_NSUB") {
            uint32_t value = 0;
            conf >> value;
            config->traffic.v5_nsub = value < 1 ? 1 : value;
            std::cerr << "V5_NSUB\t\t\t\t" << config->traffic.v5_nsub << "\n";
        } else if (key == "V5_CHUNK_BYTES") {
            conf >> config->traffic.v5_chunk_bytes;
            std::cerr << "V5_CHUNK_BYTES\t\t\t" << config->traffic.v5_chunk_bytes << "\n";
        } else if (key == "V5_SIZE_GATE_BYTES") {
            conf >> config->traffic.v5_size_gate_bytes;
            std::cerr << "V5_SIZE_GATE_BYTES\t\t" << config->traffic.v5_size_gate_bytes << "\n";
        } else if (key == "V5_ORACLE_POLICY") {
            conf >> config->traffic.v5_oracle_policy;
            std::cerr << "V5_ORACLE_POLICY\t\t" << config->traffic.v5_oracle_policy << "\n";
        } else if (key == "V5_ORACLE_BAD_SPINE") {
            conf >> config->traffic.v5_oracle_bad_spine;
            std::cerr << "V5_ORACLE_BAD_SPINE\t\t" << config->traffic.v5_oracle_bad_spine
                      << "\n";
        } else if (key == "V5_CHUNK_COMMIT_RATE_GBPS") {
            conf >> config->traffic.v5_chunk_commit_rate_gbps;
            std::cerr << "V5_CHUNK_COMMIT_RATE_GBPS\t"
                      << config->traffic.v5_chunk_commit_rate_gbps << "\n";
        } else if (key == "V5_CHUNK_LOG") {
            conf >> config->traffic.v5_chunk_log;
            std::cerr << "V5_CHUNK_LOG\t\t\t" << config->traffic.v5_chunk_log << "\n";
        } else if (key == "V5_QP_POOL") {
            conf >> config->traffic.v5_qp_pool;
            std::cerr << "V5_QP_POOL\t\t\t" << config->traffic.v5_qp_pool << "\n";
        } else if (key == "V5_QP_POOL_SIZE") {
            conf >> config->traffic.v5_qp_pool_size;
            std::cerr << "V5_QP_POOL_SIZE\t\t" << config->traffic.v5_qp_pool_size << "\n";
        } else if (key == "V5_WQE_LOG") {
            conf >> config->traffic.v5_wqe_log;
            std::cerr << "V5_WQE_LOG\t\t\t" << config->traffic.v5_wqe_log << "\n";
        } else if (key == "V5_QP_STATE_LOG") {
            conf >> config->traffic.v5_qp_state_log;
            std::cerr << "V5_QP_STATE_LOG\t\t" << config->traffic.v5_qp_state_log << "\n";
        } else if (key == "FLOWLET_SWITCH_OUTPUT_FILE") {
            conf >> config->io.flowlet_switch_output_file;
            std::cerr << "FLOWLET_SWITCH_OUTPUT_FILE\t\t"
                      << config->io.flowlet_switch_output_file << "\n";
        } else if (key == "OOO_EVENT_OUTPUT_FILE") {
            conf >> config->io.ooo_event_output_file;
            std::cerr << "OOO_EVENT_OUTPUT_FILE\t\t\t" << config->io.ooo_event_output_file
                      << "\n";
        } else if (key == "V5_CHUNK_OUTPUT_FILE") {
            conf >> config->io.v5_chunk_output_file;
            std::cerr << "V5_CHUNK_OUTPUT_FILE\t\t" << config->io.v5_chunk_output_file << "\n";
        } else if (key == "V5_WQE_OUTPUT_FILE") {
            conf >> config->io.v5_wqe_output_file;
            std::cerr << "V5_WQE_OUTPUT_FILE\t\t" << config->io.v5_wqe_output_file << "\n";
        } else if (key == "V5_QP_STATE_OUTPUT_FILE") {
            conf >> config->io.v5_qp_state_output_file;
            std::cerr << "V5_QP_STATE_OUTPUT_FILE\t" << config->io.v5_qp_state_output_file
                      << "\n";
        } else if (key == "CONWEAVE_TX_EXPIRY_TIME") {
            uint32_t value = 0;
            conf >> value;
            config->lb.conweave_tx_expiry_time = ns3::MicroSeconds(value);
            std::cerr << "CONWEAVE_TX_EXPIRY_TIME\t\t\t"
                      << config->lb.conweave_tx_expiry_time << "\n";
        } else if (key == "CONWEAVE_REPLY_TIMEOUT_EXTRA") {
            uint32_t value = 0;
            conf >> value;
            config->lb.conweave_extra_reply_deadline = ns3::MicroSeconds(value);
            std::cerr << "CONWEAVE_REPLY_TIMEOUT_EXTRA\t\t\t"
                      << config->lb.conweave_extra_reply_deadline << "\n";
        } else if (key == "CONWEAVE_EXTRA_VOQ_FLUSH_TIME") {
            uint32_t value = 0;
            conf >> value;
            config->lb.conweave_extra_voq_flush_time = ns3::MicroSeconds(value);
            std::cerr << "CONWEAVE_EXTRA_VOQ_FLUSH_TIME\t\t\t"
                      << config->lb.conweave_extra_voq_flush_time << "\n";
        } else if (key == "CONWEAVE_PATH_PAUSE_TIME") {
            uint32_t value = 0;
            conf >> value;
            config->lb.conweave_path_pause_time = ns3::MicroSeconds(value);
            std::cerr << "CONWEAVE_PATH_PAUSE_TIME\t\t\t"
                      << config->lb.conweave_path_pause_time << "\n";
        } else if (key == "CONWEAVE_DEFAULT_VOQ_WAITING_TIME") {
            uint32_t value = 0;
            conf >> value;
            config->lb.conweave_default_voq_waiting_time = ns3::MicroSeconds(value);
            std::cerr << "CONWEAVE_DEFAULT_VOQ_WAITING_TIME\t\t\t"
                      << config->lb.conweave_default_voq_waiting_time << "\n";
        } else if (key == "LETFLOW_FLOWLET_TIMEOUT_US") {
            uint32_t value = 0;
            conf >> value;
            config->lb.letflow_flowlet_timeout = ns3::MicroSeconds(value);
            std::cerr << "LETFLOW_FLOWLET_TIMEOUT_US\t\t"
                      << config->lb.letflow_flowlet_timeout << "\n";
        } else if (key == "ENABLE_PFC") {
            uint32_t value = 0;
            conf >> value;
            config->cc.enable_pfc = value;
            std::cerr << "ENABLE_PFC\t\t\t" << (config->cc.enable_pfc ? "Yes" : "No") << "\n";
        } else if (key == "ENABLE_QCN") {
            uint32_t value = 0;
            conf >> value;
            config->cc.enable_qcn = value;
            std::cerr << "ENABLE_QCN\t\t\t" << (config->cc.enable_qcn ? "Yes" : "No") << "\n";
        } else if (key == "USE_DYNAMIC_PFC_THRESHOLD") {
            uint32_t value = 0;
            conf >> value;
            config->cc.use_dynamic_pfc_threshold = value;
            std::cerr << "USE_DYNAMIC_PFC_THRESHOLD\t"
                      << (config->cc.use_dynamic_pfc_threshold ? "Yes" : "No") << "\n";
        } else if (key == "CLAMP_TARGET_RATE") {
            uint32_t value = 0;
            conf >> value;
            config->cc.clamp_target_rate = value;
            std::cerr << "CLAMP_TARGET_RATE\t\t"
                      << (config->cc.clamp_target_rate ? "Yes" : "No") << "\n";
        } else if (key == "PAUSE_TIME") {
            conf >> config->cc.pause_time;
            std::cerr << "PAUSE_TIME\t\t\t" << config->cc.pause_time << "\n";
        } else if (key == "DATA_RATE") {
            conf >> config->data_rate;
            std::cerr << "DATA_RATE\t\t\t" << config->data_rate << "\n";
        } else if (key == "LINK_DELAY") {
            conf >> config->link_delay;
            std::cerr << "LINK_DELAY\t\t\t" << config->link_delay << "\n";
        } else if (key == "PACKET_PAYLOAD_SIZE") {
            conf >> config->packet_payload_size;
            std::cerr << "PACKET_PAYLOAD_SIZE\t\t" << config->packet_payload_size << "\n";
        } else if (key == "L2_CHUNK_SIZE") {
            conf >> config->cc.l2_chunk_size;
            std::cerr << "L2_CHUNK_SIZE\t\t\t" << config->cc.l2_chunk_size << "\n";
        } else if (key == "L2_ACK_INTERVAL") {
            conf >> config->cc.l2_ack_interval;
            std::cerr << "L2_ACK_INTERVAL\t\t\t" << config->cc.l2_ack_interval << "\n";
        } else if (key == "L2_BACK_TO_ZERO") {
            uint32_t value = 0;
            conf >> value;
            config->cc.l2_back_to_zero = value;
            std::cerr << "L2_BACK_TO_ZERO\t\t\t"
                      << (config->cc.l2_back_to_zero ? "Yes" : "No") << "\n";
        } else if (key == "TOPOLOGY_FILE") {
            conf >> config->io.topology_file;
            std::cerr << "TOPOLOGY_FILE\t\t\t" << config->io.topology_file << "\n";
        } else if (key == "FLOW_FILE") {
            conf >> config->io.flow_file;
            std::cerr << "FLOW_FILE\t\t\t" << config->io.flow_file << "\n";
        } else if (key == "FLOWGEN_START_TIME") {
            conf >> config->flowgen_start_time;
            config->monitoring.qlen_start = config->flowgen_start_time;
            config->monitoring.qlen_end = config->flowgen_start_time;
            config->monitoring.cnp_start_ns = config->flowgen_start_time;
            config->monitoring.irn_start_ns = config->flowgen_start_time;
            std::cerr << "FLOWGEN_START_TIME\t\t" << config->flowgen_start_time << "\n";
        } else if (key == "FLOWGEN_STOP_TIME") {
            conf >> config->flowgen_stop_time;
            std::cerr << "FLOWGEN_STOP_TIME\t\t" << config->flowgen_stop_time << "\n";
        } else if (key == "ALPHA_RESUME_INTERVAL") {
            conf >> config->cc.alpha_resume_interval;
            std::cerr << "ALPHA_RESUME_INTERVAL\t\t" << config->cc.alpha_resume_interval << "\n";
        } else if (key == "RP_TIMER") {
            conf >> config->cc.rp_timer;
            std::cerr << "RP_TIMER\t\t\t" << config->cc.rp_timer << "\n";
        } else if (key == "EWMA_GAIN") {
            conf >> config->cc.ewma_gain;
            std::cerr << "EWMA_GAIN\t\t\t" << config->cc.ewma_gain << "\n";
        } else if (key == "FAST_RECOVERY_TIMES") {
            conf >> config->cc.fast_recovery_times;
            std::cerr << "FAST_RECOVERY_TIMES\t\t" << config->cc.fast_recovery_times << "\n";
        } else if (key == "RATE_AI") {
            conf >> config->cc.rate_ai;
            std::cerr << "RATE_AI\t\t\t\t" << config->cc.rate_ai << "\n";
        } else if (key == "RATE_HAI") {
            conf >> config->cc.rate_hai;
            std::cerr << "RATE_HAI\t\t\t" << config->cc.rate_hai << "\n";
        } else if (key == "ERROR_RATE_PER_LINK") {
            conf >> config->error_rate_per_link;
            std::cerr << "ERROR_RATE_PER_LINK\t\t" << config->error_rate_per_link << "\n";
        } else if (key == "CC_MODE") {
            conf >> config->cc.mode;
            std::cerr << "CC_MODE\t\t" << config->cc.mode << '\n';
        } else if (key == "RATE_DECREASE_INTERVAL") {
            conf >> config->cc.rate_decrease_interval;
            std::cerr << "RATE_DECREASE_INTERVAL\t\t" << config->cc.rate_decrease_interval << "\n";
        } else if (key == "MIN_RATE") {
            conf >> config->cc.min_rate;
            std::cerr << "MIN_RATE\t\t" << config->cc.min_rate << "\n";
        } else if (key == "FCT_OUTPUT_FILE") {
            conf >> config->io.fct_output_file;
            std::cerr << "FCT_OUTPUT_FILE\t\t" << config->io.fct_output_file << '\n';
        } else if (key == "HAS_WIN") {
            conf >> config->cc.has_win;
            std::cerr << "HAS_WIN\t\t" << config->cc.has_win << "\n";
        } else if (key == "GLOBAL_T") {
            conf >> config->cc.global_t;
            std::cerr << "GLOBAL_T\t\t" << config->cc.global_t << '\n';
        } else if (key == "MI_THRESH") {
            conf >> config->cc.mi_thresh;
            std::cerr << "MI_THRESH\t\t" << config->cc.mi_thresh << '\n';
        } else if (key == "VAR_WIN") {
            uint32_t value = 0;
            conf >> value;
            config->cc.var_win = value;
            std::cerr << "VAR_WIN\t\t" << value << '\n';
        } else if (key == "FAST_REACT") {
            uint32_t value = 0;
            conf >> value;
            config->cc.fast_react = value;
            std::cerr << "FAST_REACT\t\t" << value << '\n';
        } else if (key == "U_TARGET") {
            conf >> config->cc.u_target;
            std::cerr << "U_TARGET\t\t" << config->cc.u_target << '\n';
        } else if (key == "INT_MULTI") {
            conf >> config->cc.int_multi;
            std::cerr << "INT_MULTI\t\t\t\t" << config->cc.int_multi << '\n';
        } else if (key == "RATE_BOUND") {
            uint32_t value = 0;
            conf >> value;
            config->cc.rate_bound = value;
            std::cerr << "RATE_BOUND\t\t" << config->cc.rate_bound << '\n';
        } else if (key == "DCTCP_RATE_AI") {
            conf >> config->cc.dctcp_rate_ai;
            std::cerr << "DCTCP_RATE_AI\t\t\t\t" << config->cc.dctcp_rate_ai << "\n";
        } else if (key == "PFC_OUTPUT_FILE") {
            conf >> config->io.pfc_output_file;
            std::cerr << "PFC_OUTPUT_FILE\t\t\t\t" << config->io.pfc_output_file << '\n';
        } else if (key == "LINK_DOWN") {
            conf >> config->failures.link_down_time_us >> config->failures.link_down_a >>
                config->failures.link_down_b;
            std::cerr << "LINK_DOWN\t\t\t\t" << config->failures.link_down_time_us << ' '
                      << config->failures.link_down_a << ' ' << config->failures.link_down_b << '\n';
        } else if (key == "LINK_DEGRADE") {
            uint32_t event_count = 0;
            conf >> event_count;
            std::cerr << "LINK_DEGRADE\t\t\t\t" << event_count;
            for (uint32_t i = 0; i < event_count; ++i) {
                LinkDegradeEvent event = {0, 0, 0, 0.0};
                conf >> event.time_us >> event.node_a >> event.node_b >> event.fraction;
                config->failures.link_degrade_events.push_back(event);
                std::cerr << " | " << event.time_us << ' ' << event.node_a << ' ' << event.node_b
                          << ' ' << event.fraction;
            }
            std::cerr << '\n';
        } else if (key == "KMAX_MAP") {
            int count = 0;
            conf >> count;
            std::cerr << "KMAX_MAP\t\t\t\t";
            for (int i = 0; i < count; ++i) {
                uint64_t rate = 0;
                uint32_t value = 0;
                conf >> rate >> value;
                config->cc.rate_to_kmax[rate] = value;
                std::cerr << ' ' << rate << ' ' << value;
            }
            std::cerr << '\n';
        } else if (key == "KMIN_MAP") {
            int count = 0;
            conf >> count;
            std::cerr << "KMIN_MAP\t\t\t\t";
            for (int i = 0; i < count; ++i) {
                uint64_t rate = 0;
                uint32_t value = 0;
                conf >> rate >> value;
                config->cc.rate_to_kmin[rate] = value;
                std::cerr << ' ' << rate << ' ' << value;
            }
            std::cerr << '\n';
        } else if (key == "PMAX_MAP") {
            int count = 0;
            conf >> count;
            std::cerr << "PMAX_MAP\t\t\t\t";
            for (int i = 0; i < count; ++i) {
                uint64_t rate = 0;
                double value = 0;
                conf >> rate >> value;
                config->cc.rate_to_pmax[rate] = value;
                std::cerr << ' ' << rate << ' ' << value;
            }
            std::cerr << '\n';
        } else if (key == "BUFFER_SIZE") {
            conf >> config->buffer_size_mb;
            std::cerr << "BUFFER_SIZE\t\t\t\t" << config->buffer_size_mb << '\n';
        } else if (key == "QLEN_MON_FILE") {
            conf >> config->io.qlen_mon_file;
            std::cerr << "QLEN_MON_FILE\t\t\t\t" << config->io.qlen_mon_file << '\n';
        } else if (key == "VOQ_MON_FILE") {
            conf >> config->io.voq_mon_file;
            std::cerr << "VOQ_MON_FILE\t\t\t\t" << config->io.voq_mon_file << '\n';
        } else if (key == "VOQ_MON_DETAIL_FILE") {
            conf >> config->io.voq_mon_detail_file;
            std::cerr << "VOQ_MON_DETAIL_FILE\t\t\t\t" << config->io.voq_mon_detail_file << '\n';
        } else if (key == "UPLINK_MON_FILE") {
            conf >> config->io.uplink_mon_file;
            std::cerr << "UPLINK_MON_FILE\t\t\t\t" << config->io.uplink_mon_file << '\n';
        } else if (key == "CONN_MON_FILE") {
            conf >> config->io.conn_mon_file;
            std::cerr << "CONN_MON_FILE\t\t\t\t" << config->io.conn_mon_file << '\n';
        } else if (key == "PATH_DELAY_MON_FILE") {
            conf >> config->io.path_delay_mon_file;
            std::cerr << "PATH_DELAY_MON_FILE\t\t\t\t" << config->io.path_delay_mon_file << '\n';
        } else if (key == "QLEN_MON_START") {
            conf >> config->monitoring.qlen_start;
            std::cerr << "QLEN_MON_START\t\t\t\t" << config->monitoring.qlen_start << '\n';
        } else if (key == "QLEN_MON_END") {
            conf >> config->monitoring.qlen_end;
            std::cerr << "QLEN_MON_END\t\t\t\t" << config->monitoring.qlen_end << '\n';
        } else if (key == "MULTI_RATE") {
            int value = 0;
            conf >> value;
            config->cc.multi_rate = value;
            std::cerr << "MULTI_RATE\t\t\t\t" << config->cc.multi_rate << '\n';
        } else if (key == "SAMPLE_FEEDBACK") {
            int value = 0;
            conf >> value;
            config->cc.sample_feedback = value;
            std::cerr << "SAMPLE_FEEDBACK\t\t\t\t" << config->cc.sample_feedback << '\n';
        } else if (key == "LOAD") {
            conf >> config->load;
            std::cerr << "LOAD\t\t\t" << config->load << "\n";
        } else if (key == "ENABLE_IRN") {
            bool value = false;
            conf >> value;
            config->cc.enable_irn = value;
            std::cerr << "ENABLE_IRN\t\t" << config->cc.enable_irn << "\n";
        } else if (key == "RANDOM_SEED") {
            conf >> config->random_seed;
            std::cerr << "RANDOM_SEED\t\t\t" << config->random_seed << "\n";
        }

        if (!conf) {
            std::cerr << "Error: malformed value for config key " << key << '\n';
            return false;
        }
    }

    if (!conf.eof()) {
        std::cerr << "Error: failed while reading config file " << path << '\n';
        return false;
    }
    return true;
}

}  // namespace nlb
