#include "host-resource-lb.h"

#include <ns3/simulator.h>

#include <algorithm>
#include <iomanip>
#include <iterator>
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
    if (config_.hrlb.UsesCrossDepth()) {
        cross_audit_ = fopen((prefix + "_out_hrlb_cross_audit.csv").c_str(), "w");
        if (!cross_audit_) return false;
        fprintf(cross_audit_,
                "record_type,time_ns,schedule_seq,host,message_id,chunk_id,host_cqe_count,int_ack_count,"
                "base_choice,final_choice,choice_change,cand_lane,base_eligible,self_key_ok,cand_key,"
                "alt_key,self_hit,n_valid,agree,disagree,neutral,denied,peer_max_age_ns\n");
    }
    return true;
}

void HostResourceLb::ClosePhase2Audit() {
    if (cross_audit_) {
        fclose(cross_audit_);
        cross_audit_ = nullptr;
    }
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
    if (config_.hrlb.UsesCrossDepth()) {
        for (auto key : {"attempt", "candidate", "unchanged", "reassigned", "blocked_no_commit", "denied"}) {
            fprintf(phase2_summary_, "  \"cross_%s\": %llu,\n", key,
                    static_cast<unsigned long long>(cross_counts_[key]));
        }
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
    uint64_t start_ns;
    if (config_.hrlb.svc_obs_mode == "cqe_busy") {
        const uint64_t commit_ns = boundary.commit_time.GetNanoSeconds();
        start_ns = lane.has_prev_complete ? std::max(commit_ns, lane.prev_complete_ns) : commit_ns;
    } else {
        start_ns = boundary.service_start_time.GetNanoSeconds();
    }
    if (complete_ns > start_ns) {
        const double sample = complete_ns - start_ns;
        const double alpha = config_.hrlb.svc_ewma_alpha;
        lane.tau_svc_ns = lane.has_tau_svc ? (1.0 - alpha) * lane.tau_svc_ns + alpha * sample : sample;
        lane.last_svc_ns = sample;
        lane.last_svc_sample_ns = complete_ns;
        lane.has_tau_svc = true;
    }
    lane.prev_complete_ns = complete_ns;
    lane.has_prev_complete = true;
    if (lane.draining_inherited && lane.outstanding_wqes <= 1) {
        if (phase2_summary_) {
            phase2_inherited_drain_ns_.push_back(complete_ns - lane.inherited_since_ns);
        }
        lane.draining_inherited = false;
    }
}

HostResourceLb::PathDirection HostResourceLb::MsgPathDirection(const MessageState& msg,
                                                              const ns3::RdmaHw& hw) const {
    PathDirection result;
    std::map<PathDirection::Key, std::pair<double, uint32_t>> groups;
    bool measured = true;
    result.oldest_sample_ns = std::numeric_limits<uint64_t>::max();
    for (const auto& lane : msg.lanes) {
        PathDirection::Key key;
        if (!hw.QueryOdinLearnedFirstHop(lane.qp_key, &key)) return result;
        result.lanes.push_back(key);
        result.keys.insert(key);
        auto& group = groups[key];
        group.first += lane.tau_svc_ns;
        ++group.second;
        measured = measured && lane.has_tau_svc;
        result.oldest_sample_ns = std::min(result.oldest_sample_ns, lane.last_svc_sample_ns);
    }
    result.key_ok = true;
    result.valid = measured && groups.size() == 2;
    if (!result.valid) return result;
    const auto a = groups.begin();
    const auto b = std::next(a);
    const double mean_a = a->second.first / a->second.second;
    const double mean_b = b->second.first / b->second.second;
    const double ratio = config_.hrlb.cross_dst_ratio;
    if (mean_a > ratio * mean_b || mean_b > ratio * mean_a) {
        result.biased = true;
        result.slow = mean_a > ratio * mean_b ? a->first : b->first;
    }
    return result;
}

void HostResourceLb::ApplyCrossDepth(const MessageState& msg, uint64_t seq, uint64_t now,
                                    const ns3::RdmaHw& hw, const std::vector<uint32_t>& slots,
                                    std::vector<uint32_t>& depth) {
    const auto& h = config_.hrlb;
    const bool self_only = h.dyn_depth_mode == "rel_self";
    uint32_t base_choice = h.k, final_choice = h.k;
    PathDirection self;
    bool checked = false;
    std::ostringstream prefix, candidates;
    if (cross_audit_) {
        prefix << now << ',' << seq << ',' << msg.src << ',' << msg.message_id << ','
               << msg.pending_chunks.front().chunk_id << ',' << host_cqe_counts_[msg.src] << ','
               << hw.GetOdinIntAuditAckCount();
    }
    for (uint32_t off = 0; off < h.k; ++off) {
        const uint32_t l = (msg.target_cursor + off) % h.k;
        if (base_choice == h.k && slots[l] < depth[l]) base_choice = l;
        if (depth[l] == 2 && slots[l] == 1) {
            if (!checked) {
                self = MsgPathDirection(msg, hw);
                checked = true;
            }
            const bool hit = self.valid && self.biased && self.slow == self.lanes[l];
            uint32_t valid = 0, agree = 0, disagree = 0, neutral = 0;
            uint64_t age = 0;
            if (hit && !self_only) {
                for (const auto& entry : messages_) {
                    const auto& peer = entry.second;
                    if (peer.src != msg.src || peer.message_id == msg.message_id || peer.dst == msg.dst ||
                        peer.completed_chunks >= peer.total_chunks) continue;
                    const auto direction = MsgPathDirection(peer, hw);
                    if (!direction.valid || direction.keys != self.keys) continue;
                    ++valid;
                    if (!direction.biased) ++neutral;
                    else if (direction.slow == self.slow) ++agree;
                    else ++disagree;
                    age = std::max(age, now - direction.oldest_sample_ns);
                }
            }
            const bool denied = hit && (self_only ||
                (h.dyn_depth_mode == "rel_peer_avail" ? valid > 0 : 2 * agree > valid));
            if (denied) depth[l] = 1;
            if (cross_audit_) {
                ++cross_counts_["candidate"];
                cross_counts_["denied"] += denied;
                candidates << "candidate," << prefix.str() << ",,,," << l << ",1," << self.key_ok << ',';
                if (self.key_ok && self.keys.size() == 2) {
                    const auto& key = self.lanes[l];
                    const auto& alt = key == *self.keys.begin() ? *self.keys.rbegin() : *self.keys.begin();
                    candidates << key.switch_id << ':' << key.interface << ',' << alt.switch_id << ':' << alt.interface;
                } else {
                    candidates << "NA,NA";
                }
                candidates << ',' << hit << ',';
                if (self_only) candidates << "NA,NA,NA,NA," << denied << ",NA\n";
                else candidates << valid << ',' << agree << ',' << disagree << ',' << neutral << ','
                                << denied << ',' << age << '\n';
            }
        }
        if (final_choice == h.k && slots[l] < depth[l]) final_choice = l;
    }
    if (cross_audit_) {
        const char* change = base_choice == final_choice ? "unchanged" :
            final_choice == h.k ? "blocked_no_commit" : "reassigned";
        ++cross_counts_["attempt"];
        ++cross_counts_[change];
        fprintf(cross_audit_, "attempt,%s,%u,%u,%s,,,,,,,,,,,,\n", prefix.str().c_str(),
                base_choice, final_choice, change);
        fputs(candidates.str().c_str(), cross_audit_);
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
            bool slow = false;
            if (h.dyn_depth_mode == "rel" || h.UsesCrossDepth() || h.dyn_depth_mode == "hybrid") {
                slow = measured >= 2 && tau[l] > h.dyn_depth_ratio * min_tau && tau[l] > 4.0 * base_rtt;
                if (h.dyn_depth_mode == "hybrid") {
                    slow = slow || (measured >= 2 && tau[l] > h.dyn_depth_rtt_mult * base_rtt &&
                                    tau[l] > 1.2 * min_tau);
                }
            } else if (h.dyn_depth_mode == "rtt_abs") {
                slow = tau[l] > h.dyn_depth_rtt_mult * base_rtt;
            }
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
        }
    }
    if (h.UsesCrossDepth()) ApplyCrossDepth(msg, seq, now, hw, slots, depth);
    for (uint32_t off = 0; off < k; ++off) {
        const uint32_t l = (msg.target_cursor + off) % k;
        if (h.dyn_depth_enable) msg.lanes[l].last_d_eff = depth[l];
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
        const bool use_last = h.tail_fast_estimator == "min_ewma_last";
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
                const double fast_tau = use_last ? std::min(tau[j], msg.lanes[j].last_svc_ns) : tau[j];
                raw_rate += chunk / fast_tau;
                raw_bytes += backlog[j];
                const bool healthy = !pressure[j].fresh || pressure[j].psi < h.theta ||
                                     (pressure[l].fresh &&
                                      pressure[j].psi <= pressure[l].psi + h.coord_delta_guard);
                if (healthy) {
                    guarded_rate += chunk / fast_tau;
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
