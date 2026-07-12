#include "rdma-driver.h"

namespace ns3 {

/***********************
 * RdmaDriver
 **********************/
TypeId RdmaDriver::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaDriver")
		.SetParent<Object> ()
		.AddTraceSource ("QpComplete", "A qp completes.",
				MakeTraceSourceAccessor (&RdmaDriver::m_traceQpComplete))
		.AddTraceSource ("PersistentQpEvent", "A persistent QP changes lifecycle state.",
				MakeTraceSourceAccessor (&RdmaDriver::m_tracePersistentQpEvent))
		.AddTraceSource ("WqeEvent", "A persistent-QP WQE is committed or completes.",
				MakeTraceSourceAccessor (&RdmaDriver::m_traceWqeEvent))
		;
	return tid;
}

RdmaDriver::RdmaDriver(){
}

void RdmaDriver::Init(void){
	Ptr<Ipv4> ipv4 = m_node->GetObject<Ipv4> ();
	#if 0
	m_rdma->m_nic.resize(ipv4->GetNInterfaces());
	for (uint32_t i = 0; i < m_rdma->m_nic.size(); i++){
		m_rdma->m_nic[i] = CreateObject<RdmaQueuePairGroup>();
		// share the queue pair group with NIC
		if (ipv4->GetNetDevice(i)->IsQbb()){
			DynamicCast<QbbNetDevice>(ipv4->GetNetDevice(i))->m_rdmaEQ->m_qpGrp = m_rdma->m_nic[i];
		}
	}
	#endif
	for (uint32_t i = 0; i < m_node->GetNDevices(); i++){
		Ptr<QbbNetDevice> dev = NULL;
		if (m_node->GetDevice(i)->IsQbb())
			dev = DynamicCast<QbbNetDevice>(m_node->GetDevice(i));
		m_rdma->m_nic.push_back(RdmaInterfaceMgr(dev));
		m_rdma->m_nic.back().qpGrp = CreateObject<RdmaQueuePairGroup>();
	}
	#if 0
	for (uint32_t i = 0; i < ipv4->GetNInterfaces (); i++){
		if (ipv4->GetNetDevice(i)->IsQbb() && ipv4->IsUp(i)){
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(ipv4->GetNetDevice(i));
			// add a new RdmaInterfaceMgr for this device
			m_rdma->m_nic.push_back(RdmaInterfaceMgr(dev));
			m_rdma->m_nic.back().qpGrp = CreateObject<RdmaQueuePairGroup>();
		}
	}
	#endif
	// RdmaHw do setup
	m_rdma->SetNode(m_node);
	m_rdma->Setup(MakeCallback(&RdmaDriver::QpComplete, this),
		MakeCallback(&RdmaDriver::PersistentQpEvent, this),
		MakeCallback(&RdmaDriver::WqeEvent, this));
}

void RdmaDriver::SetNode(Ptr<Node> node){
	m_node = node;
}

void RdmaDriver::SetRdmaHw(Ptr<RdmaHw> rdma){
	m_rdma = rdma;
}

void RdmaDriver::AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address sip, Ipv4Address dip, uint16_t sport, uint16_t dport, uint32_t win, uint64_t baseRtt, int32_t flow_id){
	m_rdma->AddQueuePair(size, pg, sip, dip, sport, dport, win, baseRtt, flow_id);
}

Ptr<RdmaQueuePair> RdmaDriver::AppendPersistentWqe(uint16_t pg, Ipv4Address sip,
	Ipv4Address dip, uint16_t sport, uint16_t dport, uint32_t win, uint64_t baseRtt,
	int32_t app_id, uint32_t chunk_id, uint64_t bytes, uint32_t lane) {
	return m_rdma->AppendPersistentWqe(pg, sip, dip, sport, dport, win, baseRtt, app_id,
		chunk_id, bytes, lane);
}

void RdmaDriver::SealPersistentQueuePair(uint32_t dip, uint16_t sport, uint16_t dport,
	uint16_t pg, uint64_t expected_wqes) {
	m_rdma->SealPersistentQueuePair(dip, sport, dport, pg, expected_wqes);
}

void RdmaDriver::QpComplete(Ptr<RdmaQueuePair> q){
	m_traceQpComplete(q);
}

void RdmaDriver::PersistentQpEvent(Ptr<RdmaQueuePair> q, uint32_t event) {
	m_tracePersistentQpEvent(q, event);
}

void RdmaDriver::WqeEvent(Ptr<RdmaQueuePair> q, uint32_t event,
	RdmaQueuePair::WqeBoundary boundary) {
	m_traceWqeEvent(q, event, boundary);
}

} // namespace ns3
