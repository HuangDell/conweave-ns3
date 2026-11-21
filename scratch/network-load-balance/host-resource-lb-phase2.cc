#include "host-resource-lb.h"

#include <ns3/simulator.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace nlb {
namespace {

const char* const kCounters[] = {
    "cold_start_depth_blocks", "measured_asym_depth_blocks", "inherited_backlog_count",
    "multi_path_slot_opportunities", "int_comparable_opportunities",
    "delta_met_opportunities", "changed_wc_choice", "tail_veto_same_round_redirect",
    "tail_veto_message_hold", "faster_alt_candidates", "int_fresh_evaluable",
    "int_guard_pruned_lanes", "int_guard_flipped_candidate", "int_guard_flipped_action"
};

template <typename T>
std::string Serialize(const std::vector<T>& values) {
    std::ostringstream out;
    out << std::setprecision(17);
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ';';
        out << values[i];
    }
    return out.str();
}

struct IntScore {
    bool fresh = false;
    double psi = 0.0;
};

}  // namespace

bool HostResourceLb::OpenPhase2Audit() {
    if (!config_.hrlb.UsesPhase2() || !config_.hrlb.audit_log) return true;
    const std::string suffix = "_out_hrlb_decisions.csv";
    const auto& path = config_.io.hrlb_decision_output_file;
    if (path.size() < suffix.size() ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string prefix = path.substr(0, path.size() - suffix.size());
    phase2_audit_ = fopen((prefix + "_out_hrlb_p2_audit.csv").c_str(), "w");
    if (!phase2_audit_) return false;
    phase2_summary_ = fopen((prefix + "_out_hrlb_p2_summary.json").c_str(), "w");
    if (!phase2_summary_) return false;
    fprintf(phase2_audit_,
            "time_ns,schedule_seq,trigger,message_id,chunk_id,cursor,completed,pending,base_rtt_ns,"
            "int_ack_count,slots,backlog_bytes,d_eff,tau_svc_ns,int_psi,wc_first_lane,chosen_lane,"
            "unguarded_lane,veto_kind,seqbal_funnel,reason,counts\n");
    return true;
}

void HostResourceLb::ClosePhase2Audit() {
    if (phase2_audit_) {
        fclose(phase2_audit_);
        phase2_audit_ = nullptr;
    }
    if (!phase2_summary_) return;
    fprintf(phase2_summary_, "{\n");
    for (auto key : kCounters) {
        fprintf(phase2_summary_, "  \"%s\": %llu,\n", key,
                static_cast<unsigned long long>(phase2_counts_[key]));
    }
    fprintf(phase2_summary_, "  \"inherited_drain_ns\": [");
    for (size_t i = 0; i < phase2_inherited_drain_ns_.size(); ++i) {
        fprintf(phase2_summary_, "%s%llu", i ? ", " : "",
                static_cast<unsigned long long>(phase2_inherited_drain_ns_[i]));
    }
    fprintf(phase2_summary_, "]\n}\n");
    fclose(phase2_summary_);
    phase2_summary_ = nullptr;
}

void HostResourceLb::ObservePhase2Completion(LaneState& lane,
                                            const ns3::RdmaQueuePair::WqeBoundary& boundary) {
    const uint64_t complete_ns = ns3::Simulator::Now().GetNanoSeconds();
    const uint64_t service_ns = boundary.service_start_time.GetNanoSeconds();
    if (complete_ns > service_ns) {
        const double sample = complete_ns - service_ns;
        const double alpha = config_.hrlb.svc_ewma_alpha;
        lane.tau_svc_ns = lane.has_tau_svc ? (1.0 - alpha) * lane.tau_svc_ns + alpha * sample : sample;
        lane.has_tau_svc = true;
    }
    if (lane.draining_inherited && lane.outstanding_wqes <= 1) {
        if (phase2_summary_) {
            phase2_inherited_drain_ns_.push_back(complete_ns - lane.inherited_since_ns);
        }
        lane.draining_inherited = false;
    }
}

uint32_t HostResourceLb::ChoosePhase2Lane(MessageState& msg, uint64_t seq, const char* trigger,
                                         uint64_t base_rtt, const ns3::RdmaHw& hw,
                                         const char** reason) {
    const auto& h = config_.hrlb;
    const uint32_t k = h.k;
    const uint64_t now = ns3::Simulator::Now().GetNanoSeconds();
    std::vector<uint32_t> slots(k), depth(k, h.depth), free;
    std::vector<double> tau(k, 0.0);
    std::vector<uint64_t> backlog(k, 0);
    std::vector<IntScore> pressure(k);
    std::map<std::string, uint64_t> counts;
    const auto count = [&](const char* key) {
        if (phase2_audit_) ++counts[key];
    };
    uint32_t measured = 0;
    double min_tau = std::numeric_limits<double>::infinity();
    for (uint32_t l = 0; l < k; ++l) {
        const auto& lane = msg.lanes[l];
        tau[l] = lane.tau_svc_ns;
        if (lane.has_tau_svc) {
            ++measured;
            min_tau = std::min(min_tau, tau[l]);
        }
    }
    for (uint32_t off = 0; off < k; ++off) {
        const uint32_t l = (msg.target_cursor + off) % k;
        auto& lane = msg.lanes[l];
        slots[l] = lane.outstanding_wqes;
        if (h.tail_drain_enable || phase2_audit_) {
            backlog[l] = lane.qp->GetBytesLeft() + lane.qp->GetOnTheFly();
        }
        if (h.dyn_depth_enable) {
            const bool slow = measured >= 2 && tau[l] > h.dyn_depth_ratio * min_tau &&
                              tau[l] > 4.0 * base_rtt;
            if (!lane.has_tau_svc || slow) {
                depth[l] = 1;
                if (slots[l] >= 1) {
                    count(lane.has_tau_svc ? "measured_asym_depth_blocks" : "cold_start_depth_blocks");
                }
                if (lane.last_d_eff == 2 && slots[l] == 2 && !lane.draining_inherited) {
                    count("inherited_backlog_count");
                    lane.draining_inherited = true;
                    lane.inherited_since_ns = now;
                }
            }
            lane.last_d_eff = depth[l];
        }
        if (slots[l] < depth[l]) free.push_back(l);
    }

    const auto score = [&](uint32_t l) {
        bool tbo;
        uint64_t age;
        pressure[l].fresh = hw.QueryOdinIntScore(msg.lanes[l].div_resources, base_rtt,
                                                &pressure[l].psi, &tbo, &age);
    };
    const uint32_t wc = free.empty() ? k : free.front();
    uint32_t chosen = wc, raw = wc;
    unsigned seqbal_funnel = 0;
    const char* veto = "none";
    *reason = nullptr;
    if (wc != k && h.policy == "seqbal_int") {
        std::vector<uint32_t> paths;
        std::set<uint32_t> seen;
        for (auto l : free) {
            if (seen.insert(msg.lanes[l].spec.path_id).second) paths.push_back(l);
        }
        *reason = "seqbal_int_single_path";
        if (paths.size() >= 2) {
            count("multi_path_slot_opportunities");
            seqbal_funnel = 1;
            *reason = "seqbal_int_fallback";
            std::vector<uint32_t> comparable;
            for (auto l : paths) {
                score(l);
                if (pressure[l].fresh) comparable.push_back(l);
            }
            if (comparable.size() >= 2) {
                count("int_comparable_opportunities");
                seqbal_funnel = 2;
                std::stable_sort(comparable.begin(), comparable.end(), [&](uint32_t a, uint32_t b) {
                    return pressure[a].psi < pressure[b].psi;
                });
                if (pressure[comparable[1]].psi - pressure[comparable[0]].psi >= h.coord_delta_guard) {
                    count("delta_met_opportunities");
                    seqbal_funnel = 3;
                    chosen = comparable.front();
                    *reason = "seqbal_int_min_psi";
                    if (chosen != wc) {
                        count("changed_wc_choice");
                        seqbal_funnel = 4;
                    }
                }
            }
        }
    } else if (wc != k && h.tail_drain_enable) {
        if (h.tail_int_guard) {
            for (uint32_t l = 0; l < k; ++l) {
                if (msg.lanes[l].has_tau_svc) score(l);
            }
        }
        chosen = raw = k;
        const double chunk = h.chunk_bytes;
        for (auto l : free) {
            double raw_rate = 0.0, guarded_rate = 0.0;
            uint64_t raw_bytes = 0, guarded_bytes = 0;
            for (uint32_t j = 0; j < k; ++j) {
                if (!(tau[j] > 0.0 && tau[j] < tau[l])) continue;
                if (chosen == k) {
                    count("faster_alt_candidates");
                    if (pressure[j].fresh) count("int_fresh_evaluable");
                }
                raw_rate += chunk / tau[j];
                raw_bytes += backlog[j];
                const bool healthy = !pressure[j].fresh || pressure[j].psi < h.theta ||
                                     (pressure[l].fresh &&
                                      pressure[j].psi <= pressure[l].psi + h.coord_delta_guard);
                if (healthy) {
                    guarded_rate += chunk / tau[j];
                    guarded_bytes += backlog[j];
                } else if (chosen == k) {
                    count("int_guard_pruned_lanes");
                }
            }
            const double remaining = msg.pending_chunks.size() * chunk;
            const double candidate = (backlog[l] / chunk + 1.0) * tau[l];
            const auto hold = [&](double rate, uint64_t bytes) {
                return rate > 0.0 && candidate > (1.0 + h.tail_drain_eps) * ((bytes + remaining) / rate);
            };
            const bool raw_hold = hold(raw_rate, raw_bytes);
            const bool guarded_hold = hold(guarded_rate, guarded_bytes);
            if (chosen == k) {
                if (guarded_hold != raw_hold) count("int_guard_flipped_candidate");
                if (!guarded_hold) chosen = l;
            }
            if (raw == k && !raw_hold) raw = l;
            if (chosen != k && raw != k) break;
        }
        if (chosen != raw) count("int_guard_flipped_action");
        if (chosen == k) {
            count("tail_veto_message_hold");
            veto = "message_hold";
            *reason = "wc_tail_drain_hold";
        } else if (chosen != wc) {
            count("tail_veto_same_round_redirect");
            veto = "same_round_redirect";
            *reason = "wc_tail_veto_redirect";
        }
    }
    if (chosen != k && !*reason) *reason = h.dyn_depth_enable ? "wc_dyn_depth_slot" : "wc_slot";

    if (phase2_audit_) {
        std::vector<double> psi;
        for (const auto& value : pressure) {
            psi.push_back(value.fresh ? value.psi : std::numeric_limits<double>::quiet_NaN());
        }
        std::vector<uint64_t> event_counts;
        for (auto key : kCounters) {
            event_counts.push_back(counts[key]);
            phase2_counts_[key] += counts[key];
        }
        fprintf(phase2_audit_, "%llu,%llu,%s,%u,%u,%u,%u,%zu,%llu,%llu,%s,%s,%s,%s,%s,%u,%u,%u,%s,%u,%s,%s\n",
                static_cast<unsigned long long>(now), static_cast<unsigned long long>(seq), trigger,
                msg.message_id, msg.pending_chunks.front().chunk_id, msg.target_cursor,
                msg.completed_chunks, msg.pending_chunks.size(), static_cast<unsigned long long>(base_rtt),
                static_cast<unsigned long long>(hw.GetOdinIntAuditAckCount()), Serialize(slots).c_str(),
                Serialize(backlog).c_str(), Serialize(depth).c_str(), Serialize(tau).c_str(),
                Serialize(psi).c_str(), wc, chosen, raw, veto, seqbal_funnel,
                *reason ? *reason : "full", Serialize(event_counts).c_str());
    }
    return chosen;
}

}  // namespace nlb
