#include "odin-controller.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ns3 {
void OdinController::Register(uint64_t key, uint64_t rate, uint64_t capacity) {
    if (!capacity || !rate || rate > capacity || (capacity_ && capacity_ != capacity) ||
        rates_.count(key) || sum_ > std::numeric_limits<uint64_t>::max() - rate)
        throw std::logic_error("invalid Odin QP registration/capacity");
    capacity_ = capacity;
    rates_[key] = rate;
    sum_ += rate;
}

void OdinController::Remove(uint64_t key) {
    auto it = rates_.find(key);
    if (it == rates_.end()) throw std::logic_error("unknown Odin QP removal");
    sum_ -= it->second;
    rates_.erase(it);
}

OdinController::Update OdinController::OnFeedback(
    uint64_t key, double psi, bool tbo, double eta, double gamma,
    uint64_t beta, uint64_t minimum) {
    auto it = rates_.find(key);
    if (it == rates_.end() || !std::isfinite(psi) || psi <= 0 ||
        !(eta > 0 && eta <= 1) || !(gamma > 0 && gamma <= 1) ||
        !minimum || minimum > capacity_)
        throw std::logic_error("invalid Odin feedback/parameters");
    Update r;
    r.psi = psi;
    r.tbo = tbo;
    r.old_rate = it->second;
    r.sum_before = sum_;
    r.u_before = u_;
    r.beta = tbo && psi < 1.2 ? 0 : beta;
    r.temporary = eta * r.old_rate / psi + r.beta;
    // Do not clamp the intermediate rate or smooth U a second time.
    r.u_after = gamma * ((double(sum_ - r.old_rate) + r.temporary) / capacity_) +
                (1 - gamma) * u_;
    double rate = r.temporary;
    if (r.u_after > 1) {
        double scale = 1 - gamma + gamma / r.u_after;
        if (psi > 1 && r.u_after > psi) {
            rate *= scale;
            r.branch = "allocation";
        } else if (psi < 1) {
            rate = rate * scale + beta;
            r.branch = "allocation_compensation";
        }
    } else if (r.u_after < 0.9) {
        rate += beta;
        r.branch = "ingress_compensation";
    }
    if (!std::isfinite(rate) || !std::isfinite(r.u_after))
        throw std::logic_error("non-finite Odin control state");
    r.rate = static_cast<uint64_t>(std::max(double(minimum),
                                           std::min(double(capacity_), rate)));
    sum_ = sum_ - r.old_rate + r.rate;
    it->second = r.rate;
    u_ = r.u_after;
    r.sum_after = sum_;
    return r;
}
}  // namespace ns3
