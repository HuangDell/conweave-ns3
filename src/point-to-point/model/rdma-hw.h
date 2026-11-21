#ifndef RDMA_HW_H
#define RDMA_HW_H

#include <ns3/custom-header.h>
#include <ns3/node.h>
#include <ns3/rdma.h>
#include <ns3/selective-packet-queue.h>

#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "ns3/traced-callback.h"

#include "odin-controller.h"
#include "qbb-net-device.h"
#include "rdma-queue-pair.h"

namespace ns3 {

class qbbHeader;

struct RdmaInterfaceMgr {
    Ptr<QbbNetDevice> dev;
    Ptr<RdmaQueuePairGroup> qpGrp;
    OdinController odin;

    RdmaInterfaceMgr() : dev(NULL), qpGrp(NULL) {}
    RdmaInterfaceMgr(Ptr<QbbNetDevice> _dev) { dev = _dev; }
};

class RdmaHw : public Object {
   public:
    enum PersistentQpEvent {
        PERSISTENT_QP_CREATE = 0,
        PERSISTENT_QP_WAKE = 1,
        PERSISTENT_QP_IDLE = 2,
        PERSISTENT_QP_TEARDOWN = 3,
        PERSISTENT_QP_PACKET = 4,
        PERSISTENT_QP_ACK = 5,
    };

    enum WqeEvent {
        WQE_COMMIT = 0,
        WQE_COMPLETE = 1,
    };

    static TypeId GetTypeId(void);
    RdmaHw();

    Ptr<Node> m_node;
    DataRate m_minRate;  //< Min sending rate
    uint32_t m_mtu;
    uint32_t m_cc_mode;
    double m_nack_interval;
    uint32_t m_chunk;
    uint32_t m_ack_interval;
    bool m_backto0;
    bool m_var_win, m_fast_react;
    bool m_rateBound;
    bool m_odinTraceEnabled = false;
    std::set<int32_t> m_odinTraceFlowIds;
    double m_odinEta, m_odinGamma;
    DataRate m_odinBeta;
    TracedCallback<Ptr<RdmaQueuePair>, const OdinEvent&> m_traceOdin;
    void HandleAckOdin(Ptr<RdmaQueuePair> qp, CustomHeader& ch, Ptr<const Packet> packet);
    void TraceOdin(Ptr<RdmaQueuePair> qp, OdinEvent event);
    bool m_odinResourceFeedback = false;
    bool m_odinAllowPersistent = false;
    uint32_t m_odinGateInflight0 = 0; // 0: off, 1: all, 2: async_only
    uint32_t m_odinIntShareMode = 0;
    uint64_t m_odinIntMinUpdateNs = 8000, m_odinIntWindowNs = 8000;
    uint64_t m_odinIntFreshnessNs = 16000, m_odinIntProbeIntervalNs = 16000;
    uint64_t m_odinIntMaxRttNs = 0;
    FILE* m_odinIntAudit = nullptr;
    uint64_t m_odinResourceStartNs = 0, m_odinResourceVersion = 0;
    uint64_t m_odinResourceWindowNs = 32000, m_odinResourceFreshnessNs = 16000;
    uint32_t m_odinExpectedMappingVersion = 1;
    std::set<uint32_t> m_odinResourceDestinations;
    struct OdinResourceKey {
        uint32_t switch_id;
        uint32_t interface;
        OdinResourceKey() : switch_id(0), interface(0) {}
        OdinResourceKey(uint32_t sw, uint32_t iface) : switch_id(sw), interface(iface) {}
        bool operator<(const OdinResourceKey& other) const {
            return switch_id != other.switch_id ? switch_id < other.switch_id
                                                 : interface < other.interface;
        }
        bool operator==(const OdinResourceKey& other) const {
            return switch_id == other.switch_id && interface == other.interface;
        }
    };
    struct OdinPathSpec {
        uint32_t path_id = 0;
        uint32_t mapping_version = 1;
        std::vector<OdinResourceKey> resources;
    };
    struct OdinCachedResource {
        OdinResourceSample sample;
        uint64_t epoch = 0;
        uint64_t received_ns = 0;
    };
    struct OdinSwitchTboState {
        bool tbo;
        uint64_t epoch;
        uint64_t measured_ns;
        OdinSwitchTboState() : tbo(false), epoch(0), measured_ns(0) {}
        OdinSwitchTboState(bool t, uint64_t e, uint64_t m)
            : tbo(t), epoch(e), measured_ns(m) {}
    };
    std::map<uint64_t, OdinPathSpec> m_odinResourcePaths;
    std::map<OdinResourceKey, OdinCachedResource> m_odinResourceCache;
    std::map<uint32_t, OdinSwitchTboState> m_odinSwitchTboCache;
    typedef Callback<void, uint64_t> OdinResourceDeliveredCallback;
    OdinResourceDeliveredCallback m_odinResourceDeliveredCallback;
    void RegisterOdinResourcePath(uint64_t qp_key, uint32_t path_id, uint32_t mapping_version,
                                  const std::vector<OdinResourceKey>& resources);
    bool QueryOdinResourceSetScore(const std::vector<OdinResourceKey>& resources,
                                   uint32_t mapping_version, uint64_t base_rtt_ns, double* score,
                                   bool* tbo, uint64_t* max_age_ns, uint64_t* min_version,
                                   OdinResourceSample* bottleneck_sample,
                                   const char** reason) const;
    bool QueryOdinIntScore(const std::vector<OdinResourceKey>& resources, uint64_t base_rtt_ns,
                           double* score, bool* tbo, uint64_t* max_age_ns) const;
    uint64_t GetOdinIntAuditAckCount() const { return m_odinIntAuditAckCount; }
    bool QueryOdinPathScore(uint64_t qp_key, uint64_t base_rtt_ns, double* score, bool* tbo,
                            uint64_t* max_age_ns, uint64_t* min_version,
                            OdinResourceSample* bottleneck_sample, const char** reason) const;
    uint32_t ApplyOdinResourceFeedback(uint64_t version,
                                     const std::vector<OdinResourceSample>& samples);
    uint32_t ApplyOdinMultiHopResourceFeedback(uint64_t version,
                                             const std::vector<OdinResourceSample>& samples);
    std::vector<RdmaInterfaceMgr> m_nic;  // list of running nic controlled by this RdmaHw
    std::unordered_map<uint64_t, Ptr<RdmaQueuePair>> m_qpMap;      // mapping from uint64_t to qp
    std::unordered_map<uint64_t, Ptr<RdmaRxQueuePair>> m_rxQpMap;  // mapping from uint64_t to rx qp
    std::unordered_map<uint32_t, std::vector<int>>
        m_rtTable;  // map from ip address (u32) to possible ECMP port (index of dev)

    // qp complete callback
    typedef Callback<void, Ptr<RdmaQueuePair>> QpCompleteCallback;
    typedef Callback<void, Ptr<RdmaQueuePair>, uint32_t> PersistentQpEventCallback;
    typedef Callback<void, Ptr<RdmaQueuePair>, uint32_t, RdmaQueuePair::WqeBoundary>
        WqeEventCallback;
    TracedCallback<Ptr<RdmaQueuePair>> m_traceQpRegistered;
    QpCompleteCallback m_qpCompleteCallback;
    PersistentQpEventCallback m_persistentQpEventCallback;
    WqeEventCallback m_wqeEventCallback;

    void SetNode(Ptr<Node> node);
    void Setup(QpCompleteCallback qp_complete_cb, PersistentQpEventCallback qp_event_cb,
               WqeEventCallback wqe_event_cb);

    /* Akashic Record of finished QP */
    std::unordered_set<uint64_t> akashic_Qp;    // instance for each src
    std::unordered_set<uint64_t> akashic_RxQp;  // instance for each dst
    static uint64_t nAllPkts;                   // number of total packets

    /* TxQpeueuPair */
    static uint64_t GetQpKey(uint32_t dip, uint16_t sport, uint16_t dport,
                             uint16_t pg);          // get the lookup key for m_qpMap
    Ptr<RdmaQueuePair> GetQp(uint64_t key);         // get the qp
    uint32_t GetNicIdxOfQp(Ptr<RdmaQueuePair> qp);  // get the NIC index of the qp
    void DeleteQueuePair(Ptr<RdmaQueuePair> qp);    // delete TxQP

    void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip,
                      uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt,
                      int32_t flow_id);  // add a nw qp (new send)
    void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip,
                      uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt) {
        this->AddQueuePair(size, pg, _sip, _dip, _sport, _dport, win, baseRtt, -1);
    }
    Ptr<RdmaQueuePair> AppendPersistentWqe(
        uint16_t pg, Ipv4Address sip, Ipv4Address dip, uint16_t sport, uint16_t dport,
        uint32_t win, uint64_t baseRtt, int32_t app_id, uint32_t chunk_id, uint64_t bytes,
        uint32_t lane, uint32_t traffic_class, bool notify_nic = true);
    void NotifyPersistentQps(const std::vector<Ptr<RdmaQueuePair>>& qps);
    void SealPersistentQueuePair(uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg,
                                 uint64_t expected_wqes);

    /* RxQueuePair */
    static uint64_t GetRxQpKey(uint32_t dip, uint16_t dport, uint16_t sport, uint16_t pg);
    Ptr<RdmaRxQueuePair> GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
                                 uint16_t pg, bool create);  // get a rxQp
    uint32_t GetNicIdxOfRxQp(RdmaRxQueuePair& q);             // get the NIC index of the rxQp
    void DeleteRxQp(uint32_t dip, uint16_t dport, uint16_t sport, uint16_t pg);  // delete RxQP

    int ReceiveUdp(Ptr<Packet> p, CustomHeader &ch);
    int ReceiveCnp(Ptr<Packet> p, CustomHeader &ch);
    int ReceiveAck(Ptr<Packet> p, CustomHeader &ch);  // handle both ACK and NACK
    int Receive(Ptr<Packet> p,
                CustomHeader &
                    ch);  // callback function that the QbbNetDevice should use when receive
                          // packets. Only NIC can call this function. And do not call this upon PFC

    void CheckandSendQCN(Ptr<RdmaRxQueuePair> q);
    int ReceiverCheckSeq(uint32_t seq, Ptr<RdmaRxQueuePair> q, uint32_t size, bool &cnp);
    void AddHeader(Ptr<Packet> p, uint16_t protocolNumber);
    static uint16_t EtherToPpp(uint16_t protocol);

    void RecoverQueue(Ptr<RdmaQueuePair> qp);
    void QpComplete(Ptr<RdmaQueuePair> qp);
    void SetLinkDown(Ptr<QbbNetDevice> dev);

    // call this function after the NIC is setup
    void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
    void ClearTable();
    void RedistributeQp();

    Ptr<Packet> GetNxtPacket(Ptr<RdmaQueuePair> qp);  // get next packet to send, inc snd_nxt
    void PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap);
    void UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size);
    void ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate);

    void HandleTimeout(Ptr<RdmaQueuePair> qp, Time rto);

    /* statistics */
    uint32_t cnp_by_ecn;
    uint32_t cnp_by_ooo;
    uint32_t cnp_total;
    bool m_persistentPacketEvents;

    /* G3: per-event OoO log (set by scenario harness; NULL = disabled) */
    static FILE* s_oooEventLog;
    size_t getIrnBufferOverhead();  // get buffer overhead for IRN

    /******************************
     * Mellanox's version of DCQCN
     *****************************/
    double m_g;               // feedback weight
    double m_rateOnFirstCNP;  // the fraction of line rate to set on first CNP
    bool m_EcnClampTgtRate;
    double m_rpgTimeReset;
    double m_rateDecreaseInterval;
    uint32_t m_rpgThreshold;
    double m_alpha_resume_interval;
    DataRate m_rai;   //< Rate of additive increase
    DataRate m_rhai;  //< Rate of hyper-additive increase

    // the Mellanox's version of alpha update:
    // every fixed time slot, update alpha.
    void UpdateAlphaMlx(Ptr<RdmaQueuePair> q);
    void ScheduleUpdateAlphaMlx(Ptr<RdmaQueuePair> q);

    // Mellanox's version of CNP receive
    void cnp_received_mlx(Ptr<RdmaQueuePair> q);

    // Mellanox's version of rate decrease
    // It checks every m_rateDecreaseInterval if CNP arrived (m_decrease_cnp_arrived).
    // If so, decrease rate, and reset all rate increase related things
    void CheckRateDecreaseMlx(Ptr<RdmaQueuePair> q);
    void ScheduleDecreaseRateMlx(Ptr<RdmaQueuePair> q, uint32_t delta);

    // Mellanox's version of rate increase
    void RateIncEventTimerMlx(Ptr<RdmaQueuePair> q);
    void RateIncEventMlx(Ptr<RdmaQueuePair> q);
    void FastRecoveryMlx(Ptr<RdmaQueuePair> q);
    void ActiveIncreaseMlx(Ptr<RdmaQueuePair> q);
    void HyperIncreaseMlx(Ptr<RdmaQueuePair> q);

    // Implement Timeout according to IB Spec Vol. 1 C9-139.
    // For an HCA requester using Reliable Connection service, to detect missing responses,
    // every Send queue is required to implement a Transport Timer to time outstanding requests.
    Time m_waitAckTimeout;

    /***********************
     * High Precision CC
     ***********************/
    double m_targetUtil;
    double m_utilHigh;
    uint32_t m_miThresh;
    bool m_multipleRate;
    bool m_sampleFeedback;  // only react to feedback every RTT, or qlen > 0
    void HandleAckHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
    void UpdateRateHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
    void UpdateRateHpTest(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
    void FastReactHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

    /**********************
     * TIMELY
     *********************/
    double m_tmly_alpha, m_tmly_beta;
    uint64_t m_tmly_TLow, m_tmly_THigh, m_tmly_minRtt;
    void HandleAckTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
    void UpdateRateTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool us);
    void FastReactTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

    /**********************
     * DCTCP
     *********************/
    DataRate m_dctcp_rai;
    void HandleAckDctcp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

    /**********************
     * IRN
     *********************/
    bool m_irn;
    Time m_irn_rtoLow;
    Time m_irn_rtoHigh;
    uint32_t m_irn_bdp;

   private:
    struct OdinIntSample {
        IntHop hop;
        uint64_t rx_ns;
        uint64_t true_measured_ns;
        uint64_t time_ns;
        bool tbo;
    };
    typedef std::vector<OdinResourceKey> OdinIntPath;
    std::map<OdinResourceKey, std::deque<OdinIntSample>> m_odinIntCache;
    uint64_t m_odinIntAuditAckCount = 0;
    const char* ReadOdinIntHop(const OdinResourceKey& resource, uint64_t now, uint64_t base_rtt_ns,
                               double* score, bool* tbo, uint64_t* rx, uint64_t* dt,
                               uint64_t* measured) const;
    std::map<uint64_t, OdinIntPath> m_odinIntPaths;
    std::map<OdinResourceKey, std::set<uint64_t>> m_odinIntSubscribers;
    std::map<OdinIntPath, uint64_t> m_odinIntLastProbe;
    EventId m_odinIntProbeCheck;

    void HandleSharedIntAck(Ptr<RdmaQueuePair> qp, uint64_t key, Ptr<const Packet> packet,
                            const CustomHeader& ch);
    void ForgetSharedIntPath(uint64_t key);
    bool ApplyIntFeedback(Ptr<RdmaQueuePair> qp, uint64_t key, uint64_t interval_ns,
                          double psi, bool tbo, OdinEvent& event, bool is_async_trigger = false);
    bool TrySharedIntFeedback(Ptr<RdmaQueuePair> qp, uint64_t key, uint64_t ack_uid,
                              const char* trigger, uint32_t seq, bool apply);
    void TraceInt(const char* event, uint64_t ack_uid, uint64_t key, const char* reason,
                  OdinResourceKey resource = OdinResourceKey(), uint64_t rx_ns = 0,
                  uint64_t dt_ns = 0, uint64_t measured_ns = 0, uint64_t value = 0,
                  const IntHop* hop = nullptr, bool tbo = false) const;
    void ArmIntProbeCheck();
    void CheckIntProbes();
    Ptr<Packet> BuildRdmaUdp(Ptr<RdmaQueuePair> qp, uint32_t bytes, uint32_t seq, bool probe);
    Ptr<Packet> BuildRdmaAck(Ptr<const Packet> packet, const CustomHeader& ch,
                            const qbbHeader& ack, uint8_t protocol, uint16_t ipid);
    Ptr<RdmaQueuePair> CreateQueuePair(uint64_t size, uint16_t pg, Ipv4Address sip,
                                       Ipv4Address dip, uint16_t sport, uint16_t dport,
                                       uint32_t win, uint64_t baseRtt, int32_t flow_id);
    void RegisterQueuePair(Ptr<RdmaQueuePair> qp, bool notify_nic = true);
    void CompletePersistentWqes(Ptr<RdmaQueuePair> qp);
};

} /* namespace ns3 */

#endif /* RDMA_HW_H */
