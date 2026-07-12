#ifndef RDMA_DRIVER_H
#define RDMA_DRIVER_H

#include <ns3/node.h>
#include <ns3/qbb-net-device.h>
#include <ns3/rdma.h>
#include <ns3/rdma-queue-pair.h>
#include <ns3/rdma-hw.h>
#include <vector>
#include <unordered_map>

namespace ns3 {

class RdmaDriver : public Object {
public:
	Ptr<Node> m_node;
	Ptr<RdmaHw> m_rdma;

	// trace
	TracedCallback<Ptr<RdmaQueuePair> > m_traceQpComplete;
	TracedCallback<Ptr<RdmaQueuePair>, uint32_t> m_tracePersistentQpEvent;
	TracedCallback<Ptr<RdmaQueuePair>, uint32_t, RdmaQueuePair::WqeBoundary> m_traceWqeEvent;

	static TypeId GetTypeId (void);
	RdmaDriver();

	// This function init the m_nic according to the NetDevice
	// So this must be called after all NICs are installed
	void Init(void);

	// Set Node
	void SetNode(Ptr<Node> node);

	// Set RdmaHw
	void SetRdmaHw(Ptr<RdmaHw> rdma);

	// add a queue pair
	void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt, int32_t flow_id);
	Ptr<RdmaQueuePair> AppendPersistentWqe(uint16_t pg, Ipv4Address sip, Ipv4Address dip,
		uint16_t sport, uint16_t dport, uint32_t win, uint64_t baseRtt, int32_t app_id,
		uint32_t chunk_id, uint64_t bytes, uint32_t lane);
	void SealPersistentQueuePair(uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg,
		uint64_t expected_wqes);
	
	void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt) {
		this->AddQueuePair(size, pg, _sip, _dip, _sport, _dport, win, baseRtt, -1);
	}

	// callback when qp completes
	void QpComplete(Ptr<RdmaQueuePair> q);
	void PersistentQpEvent(Ptr<RdmaQueuePair> q, uint32_t event);
	void WqeEvent(Ptr<RdmaQueuePair> q, uint32_t event, RdmaQueuePair::WqeBoundary boundary);
};

} // namespace ns3

#endif /* RDMA_DRIVER_H */
