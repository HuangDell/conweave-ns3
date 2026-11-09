#ifndef ODIN_CONTROLLER_H
#define ODIN_CONTROLLER_H

#include <cstdint>
#include <map>

namespace ns3 {

// Paper Algorithm 1 only. Transport, sampling and execution live in RdmaHw.
class OdinController {
 public:
    struct Update {
        const char* branch = "none";
        double psi = 0, beta = 0, temporary = 0, u_before = 1, u_after = 1;
        bool tbo = false;
        uint64_t old_rate = 0, rate = 0, sum_before = 0, sum_after = 0;
    };
    void Register(uint64_t key, uint64_t rate, uint64_t capacity);
    void Remove(uint64_t key);
    Update OnFeedback(uint64_t key, double psi, bool tbo, double eta,
                      double gamma, uint64_t beta, uint64_t minimum);
    uint64_t Sum() const { return sum_; }
    uint64_t Capacity() const { return capacity_; }
    double Utilization() const { return u_; }
    uint32_t Active() const { return rates_.size(); }

 private:
    uint64_t capacity_ = 0, sum_ = 0;
    double u_ = 1;  // Explicit reproduction convention, not specified in the paper.
    std::map<uint64_t, uint64_t> rates_;
};

// Passive trace payload, emitted synchronously; no observation events are scheduled.
struct OdinEvent {
    const char* event = "skip";
    const char* reason = "none";
    uint32_t nic = 0, active = 0, ack_seq = 0;
    uint64_t capacity = 0, base_rtt_ns = 0, window = 0;
    int64_t next_before_ns = 0, next_after_ns = 0;
    uint32_t last_packet_bytes = 0;
    uint64_t inflight_bytes = 0;
    uint32_t sample_hop = 0, sample_queue_bytes = 0;
    uint64_t sample_dt_ns = 0, sample_delta_bytes = 0, sample_line_bps = 0;
    OdinController::Update control;
};
}  // namespace ns3
#endif
