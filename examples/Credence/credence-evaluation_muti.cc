/*
 Vamsi
 Created: 24 Aug 01:04
*/

#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <fstream>
#include <iomanip>
#include <map>
#include <ctime>
#include <set>
#include <unordered_map>
// #include <pybind11/embed.h>
// #include <pybind11/numpy.h>
// #include <pybind11/stl.h>

#include "ns3/core-module.h"
#include "ns3/applications-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/gen-queue-disc.h"
#include "ns3/red-queue-disc.h"
#include "ns3/fq-pie-queue-disc.h"
#include "ns3/fq-codel-queue-disc.h"
#include "ns3/shared-memory.h"
#include "ns3/bufferlog-tag.h"

# define PACKET_SIZE 1400
# define GIGA 1000000000

/*Buffer Management Algorithms*/
# define DT 101
# define FAB 102
# define CS 103
# define IB 104
# define ABM 110
# define LQD 111
# define FOLLOWLQD 112
# define CREDENCE 666


/*Congestion Control Algorithms*/
# define RENO 0
# define CUBIC 1
# define DCTCP 2
# define HPCC 3
# define POWERTCP 4
# define HOMA 5 // HOMA is not supported at the moment.
# define TIMELY 6
# define THETAPOWERTCP 7

#define PORT_END 65530

extern "C"
{
#include "cdf.h"
}


using namespace ns3;
// namespace py = pybind11;
// using namespace pybind11::literals;

NS_LOG_COMPONENT_DEFINE ("CREDENCE_EVALUATION");

uint32_t PORT_START[512] = {4444};

double alpha_values[8] = {1};

Ptr<OutputStreamWrapper> fctOutput;
AsciiTraceHelper asciiTraceHelper;

Ptr<OutputStreamWrapper> torStats;
AsciiTraceHelper torTraceHelper;

Ptr<OutputStreamWrapper> lqdstats[10];
AsciiTraceHelper lqdTraceHelper[10];

Ptr<SharedMemoryBuffer> sharedMemoryLeaf[10];
QueueDiscContainer northQueues[10];
QueueDiscContainer ToRQueueDiscs[10];

uint32_t enableStats;

double START_TIME;
double FLOW_LAUNCH_END_TIME;
double END_TIME;

double poission_gen_interval(double avg_rate)
{
	if (avg_rate > 0)
		return -logf(1.0 - (double)rand() / RAND_MAX) / avg_rate;
	else
		return 0;
}

template<typename T>
T rand_range (T min, T max)
{
	return min + ((double)max - min) * rand () / RAND_MAX;
}

double baseRTTNano;
double nicBw;
void TraceMsgFinish (Ptr<OutputStreamWrapper> stream, double size, double start, bool incast, uint32_t prior )
{
	double fct, standalone_fct, slowdown;
	fct = Simulator::Now().GetNanoSeconds() - start;
	standalone_fct = baseRTTNano + size * 8.0 / nicBw;
	slowdown = fct / standalone_fct;

	*stream->GetStream ()
	        << Simulator::Now().GetSeconds()
	        << " " << size
	        << " " << fct
	        << " " << standalone_fct
	        << " " << slowdown
	        << " " << baseRTTNano / 1e3
	        << " " << (start / 1e3 - Seconds(10).GetMicroSeconds())
	        << " " << prior
	        << " " << incast
	        << std::endl;
}

void TraceLQD(Ptr<OutputStreamWrapper> stream, uint32_t qlen, uint32_t avglen, uint32_t occ, uint avgocc, uint32_t drop){
		*stream->GetStream()
			<< qlen << " "
			<< avglen << " "
			<< occ << " "
			<< avgocc << " "
			<< drop << std::endl;
}

void
InvokeToRStats(Ptr<OutputStreamWrapper> stream, uint32_t BufferSize, uint32_t leafId, double nanodelay) {
	Ptr<SharedMemoryBuffer> sm = sharedMemoryLeaf[leafId];
	QueueDiscContainer queues = northQueues[leafId];
	double totalThroughput = 0;
	for (uint32_t i = 0; i < queues.GetN(); i++) {
		Ptr<GenQueueDisc> genDisc = DynamicCast<GenQueueDisc>(queues.Get(i));
		totalThroughput += genDisc->GetThroughputPort(nanodelay);
	}
	totalThroughput = totalThroughput / queues.GetN();

	*stream->GetStream()
	        << Simulator::Now().GetSeconds()
	        << " " << leafId
	        << " " << double(BufferSize) / 1e6
	        << " " << 100 * double(sm->GetOccupiedBuffer()) / BufferSize
	        << " " << 100 * totalThroughput
	        << " " << 100 * double(sm->GetPerPriorityOccupied(0)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(1)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(2)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(3)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(4)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(5)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(6)) / BufferSize
	        << " " << 100 * double(sm->GetPerPriorityOccupied(7)) / BufferSize
	        << std::endl;

	if (Simulator::Now() < Seconds(FLOW_LAUNCH_END_TIME)){
		Simulator::Schedule(NanoSeconds(nanodelay), InvokeToRStats, stream, BufferSize, leafId, nanodelay);
	}
}

void
InvokePerPortToRStats(Ptr<OutputStreamWrapper> stream, uint32_t BufferSize, uint32_t leafId, double nanodelay) {
	Ptr<SharedMemoryBuffer> sm = sharedMemoryLeaf[leafId];
	QueueDiscContainer queues = northQueues[leafId];
	for (uint32_t i = 0; i < queues.GetN(); i++) {
		Ptr<GenQueueDisc> genDisc = DynamicCast<GenQueueDisc>(queues.Get(i));
		double totalThroughput = genDisc->GetThroughputPort(nanodelay);
		*stream->GetStream()
		        << Simulator::Now().GetSeconds()
		        << " " << leafId
		        << " " << genDisc->getPortId()
		        << " " << double(BufferSize) / 1e6
		        << " " << 100 * double(genDisc->GetCurrentSize().GetValue()) / BufferSize
		        << " " << 100 * totalThroughput
		        << std::endl;
	}
	if (Simulator::Now() < Seconds(FLOW_LAUNCH_END_TIME)){
		Simulator::Schedule(NanoSeconds(nanodelay), InvokePerPortToRStats, stream, BufferSize, leafId, nanodelay);
	}
}


int tar = 0;
int get_target_leaf(int leafCount) {
	tar += 1;
	if (tar == leafCount) {
		tar = 0;
		return tar;
	}
	return tar;
}

void install_applications_incast (int incastLeaf, NodeContainer* servers, double requestRate, uint32_t requestSize, struct cdf_table *cdfTable,
                                  long &flowCount, int SERVER_COUNT, int LEAF_COUNT, double START_TIME, double END_TIME, double FLOW_LAUNCH_END_TIME, int numPrior,int cc)
{
	int fan = SERVER_COUNT;
	uint64_t flowSize = double(requestSize) / double(fan);

	uint32_t prior = rand_range(1, numPrior - 1);

	for (int incastServer = 0; incastServer < SERVER_COUNT; incastServer++)
	{
		double startTime = START_TIME + poission_gen_interval (requestRate);
		while (startTime < FLOW_LAUNCH_END_TIME && startTime > START_TIME)
		{
			// Permutation demand matrix
			int txLeaf = incastLeaf + 1;
			if (txLeaf == LEAF_COUNT) {
				txLeaf = 0;
			}
			// int txLeaf=incastLeaf;
			// while (txLeaf==incastLeaf){
			//     txLeaf = get_target_leaf(LEAF_COUNT);
			// }

			for (uint32_t txServer = 0; txServer < fan; txServer++) {

				uint16_t port = PORT_START[ incastLeaf * SERVER_COUNT + incastServer]++;
				if (port > PORT_END) {
					port = 4444;
					PORT_START[incastLeaf * SERVER_COUNT + incastServer] = 4444;
				}
				Time startApp = (NanoSeconds (150) + MilliSeconds(rand_range(50, 1000)));
				Ptr<Node> rxNode = servers[incastLeaf].Get (incastServer);
				Ptr<Ipv4> ipv4 = rxNode->GetObject<Ipv4> ();
				Ipv4InterfaceAddress rxInterface = ipv4->GetAddress (1, 0);
				Ipv4Address rxAddress = rxInterface.GetLocal ();

				InetSocketAddress ad (rxAddress, port);
				Address sinkAddress(ad);
				Ptr<BulkSendApplication> bulksend = CreateObject<BulkSendApplication>();
				bulksend->SetAttribute("Protocol", TypeIdValue(TcpSocketFactory::GetTypeId()));
				bulksend->SetAttribute ("SendSize", UintegerValue (flowSize));
				bulksend->SetAttribute ("MaxBytes", UintegerValue(flowSize));
				bulksend->SetAttribute("FlowId", UintegerValue(flowCount++));
				bulksend->SetAttribute("priorityCustom", UintegerValue(prior));//设置myPriorityTag
				bulksend->SetAttribute("ccCustom", UintegerValue(cc));//设置myPriorityTag
				bulksend->SetAttribute("Remote", AddressValue(sinkAddress));
				bulksend->SetAttribute("InitialCwnd", UintegerValue (flowSize / PACKET_SIZE + 1));
				bulksend->SetAttribute("priority", UintegerValue(prior));
				bulksend->SetAttribute("sendAt", TimeValue(Seconds (startTime)));
				bulksend->SetStartTime (startApp);
				bulksend->SetStopTime (Seconds (END_TIME));
				servers[txLeaf].Get (txServer)->AddApplication(bulksend);

				PacketSinkHelper sink ("ns3::TcpSocketFactory", InetSocketAddress (Ipv4Address::GetAny (), port));
				ApplicationContainer sinkApp = sink.Install (servers[incastLeaf].Get(incastServer));
				sinkApp.Get(0)->SetAttribute("TotalQueryBytes", UintegerValue(flowSize));
				sinkApp.Get(0)->SetAttribute("recvAt", TimeValue(Seconds(startTime)));
				sinkApp.Get(0)->SetAttribute("priority", UintegerValue(0)); // ack packets are prioritized
				sinkApp.Get(0)->SetAttribute("priorityCustom", UintegerValue(0)); // ack packets are prioritized
				sinkApp.Get(0)->SetAttribute("ccCustom", UintegerValue(cc)); // ack packets are prioritized
				sinkApp.Get(0)->SetAttribute("senderPriority", UintegerValue(prior));
				sinkApp.Get(0)->SetAttribute("flowId", UintegerValue(flowCount));
				flowCount += 1;
				sinkApp.Start (startApp);
				sinkApp.Stop (Seconds (END_TIME));
				// if (enableStats){
					sinkApp.Get(0)->TraceConnectWithoutContext("FlowFinish", MakeBoundCallback(&TraceMsgFinish, fctOutput));
				// }
			}
			startTime += poission_gen_interval (requestRate);
		}
	}
}

void install_applications (int txLeaf, NodeContainer* servers, double requestRate, struct cdf_table *cdfTable,
                           long &flowCount, int SERVER_COUNT, int LEAF_COUNT, double START_TIME, double END_TIME, double FLOW_LAUNCH_END_TIME, int numPrior,int cc)
{
	uint64_t flowSize;

	uint32_t prior = rand_range(1, numPrior - 1);

	for (int txServer = 0; txServer < SERVER_COUNT; txServer++)
	{
		double startTime = START_TIME + poission_gen_interval (requestRate);
		while (startTime < FLOW_LAUNCH_END_TIME && startTime > START_TIME)
		{
			// Permutation demand matrix
			int rxLeaf = txLeaf + 1;
			if (rxLeaf == LEAF_COUNT) {
				rxLeaf = 0;
			}
			// int rxLeaf=txLeaf;
			// while (txLeaf==rxLeaf){
			//     rxLeaf = get_target_leaf(LEAF_COUNT);
			// }

			uint32_t rxServer = rand_range(0, SERVER_COUNT);

			uint16_t port = PORT_START[rxLeaf * SERVER_COUNT + rxServer]++;
			if (port > PORT_END) {
				port = 4444;
				PORT_START[rxLeaf * SERVER_COUNT + rxServer] = 4444;
			}

			uint64_t flowSize = gen_random_cdf (cdfTable);
			while (flowSize == 0) {
				flowSize = gen_random_cdf (cdfTable);
			}

			Ptr<Node> rxNode = servers[rxLeaf].Get (rxServer);
			Ptr<Ipv4> ipv4 = rxNode->GetObject<Ipv4> ();
			Ipv4InterfaceAddress rxInterface = ipv4->GetAddress (1, 0);
			Ipv4Address rxAddress = rxInterface.GetLocal ();

			InetSocketAddress ad (rxAddress, port);
			Address sinkAddress(ad);
			Ptr<BulkSendApplication> bulksend = CreateObject<BulkSendApplication>();
			bulksend->SetAttribute("Protocol", TypeIdValue(TcpSocketFactory::GetTypeId()));
			bulksend->SetAttribute ("SendSize", UintegerValue (flowSize));
			bulksend->SetAttribute ("MaxBytes", UintegerValue(flowSize));
			bulksend->SetAttribute("FlowId", UintegerValue(flowCount++));
			bulksend->SetAttribute("priorityCustom", UintegerValue(prior));
			bulksend->SetAttribute("ccCustom", UintegerValue(cc));
			bulksend->SetAttribute("Remote", AddressValue(sinkAddress));
			bulksend->SetAttribute("InitialCwnd", UintegerValue (22));
			bulksend->SetAttribute("priority", UintegerValue(prior));
			bulksend->SetStartTime (Seconds(startTime));
			bulksend->SetStopTime (Seconds (END_TIME));
			servers[txLeaf].Get (txServer)->AddApplication(bulksend);

			PacketSinkHelper sink ("ns3::TcpSocketFactory", InetSocketAddress (Ipv4Address::GetAny (), port));
			ApplicationContainer sinkApp = sink.Install (servers[rxLeaf].Get(rxServer));
			sinkApp.Get(0)->SetAttribute("TotalQueryBytes", UintegerValue(flowSize));
			sinkApp.Get(0)->SetAttribute("priority", UintegerValue(0)); // ack packets are prioritized
			sinkApp.Get(0)->SetAttribute("priorityCustom", UintegerValue(0)); // ack packets are prioritized
			sinkApp.Get(0)->SetAttribute("ccCustom", UintegerValue(cc));
			sinkApp.Get(0)->SetAttribute("flowId", UintegerValue(flowCount));
			sinkApp.Get(0)->SetAttribute("senderPriority", UintegerValue(prior));
			flowCount += 1;
			sinkApp.Start (Seconds(startTime));
			sinkApp.Stop (Seconds (END_TIME));
			// if (enableStats){
				sinkApp.Get(0)->TraceConnectWithoutContext("FlowFinish", MakeBoundCallback(&TraceMsgFinish, fctOutput));
			// }
			startTime += poission_gen_interval (requestRate);
		}
	}
	// std::cout << "Finished installation of applications from leaf-"<< fromLeafId << std::endl;
}

// void getPrediction(py::object rf, py::object castInteger, uint32_t queueLength, uint32_t averageQueueLength, uint32_t sharedOccupancy,  uint32_t averageOccupancy, int &drop){
// 	if (Simulator::Now().GetSeconds()>=START_TIME){
// 		int pred = castInteger.attr("getPrediction")(rf, queueLength, averageQueueLength, sharedOccupancy, averageOccupancy).cast<int>();
// 		if (pred ==0 || pred==1){
// 			drop = pred;
// 			// std::cout << "COUT out is: " << drop << std::endl;
// 		}
// 		else{
// 			drop = 0;
// 		}
// 	}
// 	else
// 		drop = 0;
// }

// void getRecord(py::object rf, py::object castInteger, uint32_t queueLength, uint32_t averageQueueLength, uint32_t sharedOccupancy,  uint32_t averageOccupancy, int &drop){
// 	 std::cout << "queueLength: " << queueLength << std::endl;
// }

int
main (int argc, char *argv[])
{
	// py::scoped_interpreter guard{};
	// py::object joblib = py::module_::import("joblib");
	// py::object castInteger = py::module_::import("cast-to-integer");

	double loadCubic = 0.5;
	double loadDctcp = 0.1;
	double loadPower = 0.0;

	// py::object rf = joblib.attr("load")(py::str("/home/vamsi/src/phd/codebase/ns3-datacenter/simulator/ns-3.39/examples/Credence/rf_models/model-2-0.8-0.875-2-WS-2.joblib"));
	// int drop = 1;
	// py::object *a = &rf;
	// py::object *b = &castInteger;
	// getPrediction(a,b,1,2,3,4,drop);
	// return 0;
	CommandLine cmd;

	START_TIME = 10;
	FLOW_LAUNCH_END_TIME = 13;
	END_TIME = 20;
	cmd.AddValue ("StartTime", "Start time of the simulation", START_TIME);
	cmd.AddValue ("EndTime", "End time of the simulation", END_TIME);
	cmd.AddValue ("FlowLaunchEndTime", "End time of the flow launch period", FLOW_LAUNCH_END_TIME);

	unsigned randomSeed = 8;
	cmd.AddValue ("randomSeed", "Random seed, 0 for random generated", randomSeed);

	double load = 0.0;
	cmd.AddValue ("load", "Load of the network, 0.0 - 1.0", load);

	uint32_t SERVER_COUNT = 16;
	uint32_t SPINE_COUNT = 4;
	uint32_t LEAF_COUNT = 2;
	uint32_t LINK_COUNT = 1;
	uint64_t spineLeafCapacity = 10; //Gbps
	uint64_t leafServerCapacity = 10; //Gbps
	double linkLatency = 3;
	cmd.AddValue ("serverCount", "The Server count", SERVER_COUNT);
	cmd.AddValue ("spineCount", "The Spine count", SPINE_COUNT);
	cmd.AddValue ("leafCount", "The Leaf count", LEAF_COUNT);
	cmd.AddValue ("linkCount", "The Link count", LINK_COUNT);
	cmd.AddValue ("spineLeafCapacity", "Spine <-> Leaf capacity in Gbps", spineLeafCapacity);
	cmd.AddValue ("leafServerCapacity", "Leaf <-> Server capacity in Gbps", leafServerCapacity);
	cmd.AddValue ("linkLatency", "linkLatency in microseconds", linkLatency);

	uint32_t TcpProt = CUBIC;
	cmd.AddValue("TcpProt", "Tcp protocol", TcpProt);

	uint32_t BufferSize = (5.12*1024)*(SERVER_COUNT+SPINE_COUNT*LINK_COUNT)*(spineLeafCapacity);
	double statBuf = 0; // fraction of buffer that is reserved
	cmd.AddValue ("BufferSize", "BufferSize in Bytes", BufferSize);
	cmd.AddValue ("statBuf", "staticBuffer in fraction of Total buffersize", statBuf);

	uint32_t algorithm = LQD;
	cmd.AddValue ("algorithm", "Buffer Management algorithm", algorithm);

	/*RED Parameters*/
	uint32_t RedMinTh = 65;
	uint32_t RedMaxTh = 65;
	uint32_t UseEcn = 0;
	std::string ecnEnabled = "EcnDisabled";
	cmd.AddValue("RedMinTh", "Min Threshold for RED in packets", RedMinTh);
	cmd.AddValue("RedMaxTh", "Max Threshold for RED in packets", RedMaxTh);
	cmd.AddValue("UseEcn", "Ecn Enabled", UseEcn);

	std::string sched = "roundRobin";
	cmd.AddValue ("sched", "scheduling", sched);

	uint32_t requestSize = 0.5 * BufferSize;
	double queryRequestRate = 2; // at each server (per second)
	cmd.AddValue ("request", "Query Size in Bytes", requestSize);
	cmd.AddValue("queryRequestRate", "Query request rate (poisson arrivals)", queryRequestRate);

	uint32_t nPrior = 2; // number queues in switch ports
	cmd.AddValue ("nPrior", "number of priorities", nPrior);

	std::string alphasFile = "/home/vamsi/src/phd/codebase/ns3-datacenter/simulator/ns-3.39/examples/Credence/alphas"; // On lakewood
	std::string cdfFileName = "/home/vamsi/src/phd/codebase/ns3-datacenter/simulator/ns-3.39/examples/Credence/websearch.csv";
	std::string cdfName = "WS";
	cmd.AddValue ("alphasFile", "alpha values file (should be exactly nPrior lines)", alphasFile);
	cmd.AddValue ("cdfFileName", "File name for flow distribution", cdfFileName);
	cmd.AddValue ("cdfName", "Name for flow distribution", cdfName);

	uint32_t printDelay = 30 * 1e3;
	cmd.AddValue("printDelay", "printDelay in NanoSeconds", printDelay);

	double alphaUpdateInterval = 1;
	cmd.AddValue("alphaUpdateInterval", "(Number of Rtts) update interval for alpha values in ABM", alphaUpdateInterval);


	std::string fctOutFile = "./fcts.txt";
	cmd.AddValue ("fctOutFile", "File path for FCTs", fctOutFile);

	std::string torOutFile = "./tor.txt";
	cmd.AddValue ("torOutFile", "File path for ToR statistic", torOutFile);

	std::string lqdOutFile = "./examples/Credence/lqd-traces/WS-";
	cmd.AddValue("lqdOutFile","path to lqd trace",lqdOutFile);
	double averageIntervalNano = 1; // default 1 rtt
	cmd.AddValue("averageIntervalNano", "interval for EWMA in shared memory. Specifiy as as a factor of RTT", averageIntervalNano);

	uint32_t enableLqdTracing = 0;
	cmd.AddValue("enableLqdTracing", "enable tracing Lqd events", enableLqdTracing);

	std::string rfModelFile = "/home/vamsi/src/phd/codebase/ns3-datacenter/simulator/ns-3.39/examples/Credence/rf_models/model-2-0.8-0.75-2-WS-";
	cmd.AddValue("rfModelFile", "path to rf model file. Note that the last part of the string is removed (index by switch id)", rfModelFile);

	double errorProb = 0;
	cmd.AddValue("errorProb", "insert some error 0-1", errorProb); 

	uint32_t rto = 10 * 1000; // in MicroSeconds, 5 milliseconds.
	cmd.AddValue ("rto", "min Retransmission timeout value in MicroSeconds", rto);

	uint32_t torPrintall = 0;
	cmd.AddValue ("torPrintall", "torPrintall", torPrintall);

	enableStats = 0;
	cmd.AddValue("enableStats", "enable stats output from tors, fcts",enableStats);

	/*Parse CMD*/
	cmd.Parse (argc, argv);

	fctOutput = asciiTraceHelper.CreateFileStream (fctOutFile);

	*fctOutput->GetStream ()
	        << "time "
	        << "flowsize "
	        << "fct "
	        << "basefct "
	        << "slowdown "
	        << "basertt "
	        <<  "flowstart "
	        << "priority "
			<< "cc"
	        << "incast "
	        << std::endl;

	if (enableStats){
		torStats = torTraceHelper.CreateFileStream (torOutFile);

		if (!torPrintall) {
			*torStats->GetStream ()
			        << "time "
			        << "tor "
			        << "bufferSizeMB "
			        << "occupiedBufferPct "
			        << "uplinkThroughput "
			        << "priority0 "
			        << "priority1 "
			        << "priority2 "
			        << "priority3 "
			        << "priority4 "
			        << "priority5 "
			        << "priority6 "
			        << "priority7 "
			        << std::endl;
		}
		else {
			*torStats->GetStream()
			        << "time "
			        << "tor "
			        << "portId "
			        << "bufferSizeMB "
			        << "PortOccBuffer "
			        << "PortThroughput "
			        << std::endl;
		}
	}

	if (enableLqdTracing){
		if (algorithm == LQD){
			for (uint32_t i = 0; i < (LEAF_COUNT+SPINE_COUNT); i++){
				lqdstats[i] = lqdTraceHelper[i].CreateFileStream(lqdOutFile+std::to_string(i)+".csv");
				*lqdstats[i]->GetStream()
					<< "queueLength "
					<< "averageQueueLength "
					<< "sharedOccupancy "
					<< "averageOccupancy "
					<< "drop"
					<< std::endl;
			}
		}
	}

	uint32_t staticBuffer = (double) BufferSize * statBuf / (SERVER_COUNT + SPINE_COUNT * LINK_COUNT);
	BufferSize = BufferSize - staticBuffer; // BufferSize is the buffer pool which is available for sharing
	if (UseEcn) {
		ecnEnabled = "EcnEnabled";
	}
	else {
		ecnEnabled = "EcnDisabled";
	}

	/*Reading alpha values from file*/
	std::string line;
	std::fstream aFile;
	aFile.open(alphasFile);
	uint32_t p = 0;
	while ( getline( aFile, line ) && p < 8 ) { // hard coded to read only 8 alpha values.
		std::istringstream iss( line );
		double a;
		iss >> a;
		alpha_values[p] = a;
		// std::cout << "Alpha-"<< p << " = "<< a << " " << alpha_values[p]<< std::endl;
		p++;
	}
	aFile.close();


	uint64_t SPINE_LEAF_CAPACITY = spineLeafCapacity * GIGA;
	uint64_t LEAF_SERVER_CAPACITY = leafServerCapacity * GIGA;
	Time LINK_LATENCY = MicroSeconds(linkLatency);

	double RTTBytes = (LEAF_SERVER_CAPACITY * 1e-6) * linkLatency; // 8 links visited in roundtrip according to the topology, divided by 8 for bytes
	uint32_t RTTPackets = RTTBytes / PACKET_SIZE + 1;
	baseRTTNano = linkLatency * 8 * 1e3;
	nicBw = leafServerCapacity;
	std::cout << "bandwidth " << spineLeafCapacity << "gbps" <<  " rtt " << linkLatency*8 << "us" << " BDP " << RTTBytes << std::endl;

	if (load < 0.0)
	{
		NS_LOG_ERROR ("Illegal Load value");
		return 0;
	}

	RedMinTh = RedMaxTh = std::max( uint32_t(10), uint32_t((RTTBytes/7.0)/double(PACKET_SIZE)) ); // min must be at least the default initial cwnd value.

	Config::SetDefault("ns3::GenQueueDisc::updateInterval", UintegerValue(alphaUpdateInterval * linkLatency * 8 * 1000));
	Config::SetDefault("ns3::GenQueueDisc::staticBuffer", UintegerValue(staticBuffer));
	Config::SetDefault("ns3::GenQueueDisc::BufferAlgorithm", UintegerValue(algorithm));
	Config::SetDefault("ns3::SharedMemoryBuffer::BufferSize", UintegerValue(BufferSize));
	Config::SetDefault ("ns3::FifoQueueDisc::MaxSize", QueueSizeValue (QueueSize ("100MB")));

	TrafficControlHelper tc;
	uint16_t handle;
	TrafficControlHelper::ClassIdList cid;

	/*General TCP Socket settings. Mostly used by various congestion control algorithms in common*/
	Config::SetDefault ("ns3::TcpSocket::ConnTimeout", TimeValue (MilliSeconds (10))); // syn retry interval
	Config::SetDefault ("ns3::TcpSocketBase::MinRto", TimeValue (MicroSeconds (rto)) );  //(MilliSeconds (5))
	Config::SetDefault ("ns3::TcpSocketBase::RTTBytes", UintegerValue ( RTTBytes ));  //(MilliSeconds (5))
	Config::SetDefault ("ns3::TcpSocketBase::ClockGranularity", TimeValue (NanoSeconds (10))); //(MicroSeconds (100))
	Config::SetDefault ("ns3::RttEstimator::InitialEstimation", TimeValue (MicroSeconds (200))); //TimeValue (MicroSeconds (80))
	Config::SetDefault ("ns3::TcpSocket::SndBufSize", UintegerValue (1073725440)); //1073725440
	Config::SetDefault ("ns3::TcpSocket::RcvBufSize", UintegerValue (1073725440));
	Config::SetDefault ("ns3::TcpSocket::ConnCount", UintegerValue (6));  // Syn retry count
	Config::SetDefault ("ns3::TcpSocketBase::Timestamp", BooleanValue (true));
	Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (PACKET_SIZE));
	Config::SetDefault ("ns3::TcpSocket::DelAckCount", UintegerValue (0));
	Config::SetDefault ("ns3::TcpSocket::PersistTimeout", TimeValue (Seconds (20)));


	Config::SetDefault ("ns3::RedQueueDisc::UseEcn", BooleanValue (true));
	Config::SetDefault ("ns3::RedQueueDisc::QW", DoubleValue (1.0));
	Config::SetDefault ("ns3::RedQueueDisc::MinTh", DoubleValue (RedMinTh * PACKET_SIZE));
	Config::SetDefault ("ns3::RedQueueDisc::MaxTh", DoubleValue (RedMaxTh * PACKET_SIZE));
	Config::SetDefault ("ns3::RedQueueDisc::MaxSize", QueueSizeValue (QueueSize ("100MB"))); // This is just for initialization. The buffer management algorithm will take care of the rest.
	Config::SetDefault ("ns3::RedQueueDisc::LInterm", DoubleValue (0.0));
	Config::SetDefault ("ns3::RedQueueDisc::UseHardDrop", BooleanValue (false));
	Config::SetDefault ("ns3::RedQueueDisc::Gentle", BooleanValue (false));
	Config::SetDefault ("ns3::RedQueueDisc::MeanPktSize", UintegerValue (PACKET_SIZE));


	Config::SetDefault("ns3::GenQueueDisc::nPrior", UintegerValue(nPrior));
	Config::SetDefault("ns3::GenQueueDisc::RoundRobin", UintegerValue(1));
	Config::SetDefault("ns3::GenQueueDisc::StrictPriority", UintegerValue(0));
	handle = tc.SetRootQueueDisc ("ns3::GenQueueDisc");
	cid = tc.AddQueueDiscClasses (handle, 8 , "ns3::QueueDiscClass");
	tc.AddChildQueueDisc (handle, cid[0], "ns3::FifoQueueDisc"); // acks go here
	tc.AddChildQueueDisc (handle, cid[1], "ns3::FifoQueueDisc"); // Cubic goes here
	tc.AddChildQueueDisc (handle, cid[2], "ns3::RedQueueDisc"); // Dctcp goes here
	tc.AddChildQueueDisc (handle, cid[3], "ns3::FifoQueueDisc"); // ThetaPowerTcp goes here
	tc.AddChildQueueDisc (handle, cid[4], "ns3::FifoQueueDisc");
	tc.AddChildQueueDisc (handle, cid[5], "ns3::FifoQueueDisc");
	tc.AddChildQueueDisc (handle, cid[6], "ns3::FifoQueueDisc");
	tc.AddChildQueueDisc (handle, cid[7], "ns3::FifoQueueDisc");



	NodeContainer spines;
	spines.Create (SPINE_COUNT);
	NodeContainer leaves;
	leaves.Create (LEAF_COUNT);
	NodeContainer servers[LEAF_COUNT];
	Ipv4InterfaceContainer serverIpv4[LEAF_COUNT];

	NodeContainer serversCubic[LEAF_COUNT];
	NodeContainer serversDctcp[LEAF_COUNT];
	NodeContainer serversPower[LEAF_COUNT];

	InternetStackHelper internet;
	Ipv4GlobalRoutingHelper globalRoutingHelper;
	internet.SetRoutingHelper (globalRoutingHelper);

	/*In all the leaves 0 to SERVER_COUNT/3 use Cubic, SERVER_COUNT/3 to 2*SERVER_COUNT/3 use Dctcp and the rest use ThetaPowerTcp*/
	for (uint32_t i = 0; i < LEAF_COUNT; i++) {
		servers[i].Create (SERVER_COUNT);
		internet.Install(servers[i]);
		uint32_t start = 0;
		for (uint32_t j = start; j < 16; j++) { // Make sure that SERVER_COUNT is divisible by 3.
			serversCubic[i].Add(servers[i].Get(j));
			TypeId tid = TypeId::LookupByName ("ns3::TcpCubic");
			std::stringstream nodeId;
			nodeId << servers[i].Get (j)->GetId ();
			std::string specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpL4Protocol/SocketType";
			Config::Set (specificNode, TypeIdValue(ns3::TcpCubic::GetTypeId()));
			// std::cout << Config::SetFailSafe (specificNode, TypeIdValue (ns3::TcpCubic::GetTypeId())) << " " << nodeId.str() << std::endl;
		}
		start += 16;
		for (uint32_t j = start; j < 24; j++) { // Make sure that SERVER_COUNT is divisible by 3.
			serversDctcp[i].Add(servers[i].Get(j));
			TypeId tid = TypeId::LookupByName ("ns3::TcpDctcp");
			std::stringstream nodeId;
			nodeId << servers[i].Get (j)->GetId ();
			std::string specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpL4Protocol/SocketType";
			// std::cout << Config::SetFailSafe (specificNode, TypeIdValue (ns3::TcpDctcp::GetTypeId())) << " " << nodeId.str() << std::endl;
			Config::Set (specificNode, TypeIdValue (ns3::TcpDctcp::GetTypeId()));
			// std::cout << Config::SetFailSafe (specificNode, TypeIdValue (tid)) << " " << nodeId.str() << std::endl;
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketBase/UseEcn";
			Config::Set (specificNode, StringValue ("On"));
		}
		start += 8;
		for (uint32_t j = start; j < 32; j++) { // Make sure that SERVER_COUNT is divisible by 3.
			serversPower[i].Add(servers[i].Get(j));
			TypeId tid = TypeId::LookupByName ("ns3::TcpAdvanced");
			std::stringstream nodeId;
			nodeId << servers[i].Get (j)->GetId ();
			std::string specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpL4Protocol/SocketType";
			Config::Set (specificNode, TypeIdValue (ns3::TcpAdvanced::GetTypeId()));

			// specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketBase/Sack";
			// Config::Set (specificNode, BooleanValue(false));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/initCCRate";
			Config::Set (specificNode, DataRateValue(DataRate(LEAF_SERVER_CAPACITY)));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/minCCRate";
			Config::Set (specificNode, DataRateValue(DataRate("100Mbps")));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/maxCCRate";
			Config::Set (specificNode, DataRateValue(DataRate(LEAF_SERVER_CAPACITY)));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/AI";
			Config::Set (specificNode, DataRateValue(DataRate("100Mbps")));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/mThreshHpcc";
			Config::Set (specificNode, UintegerValue(5));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/fastReactHpcc";
			Config::Set (specificNode, BooleanValue(true));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/sampleFeedbackHpcc";
			Config::Set (specificNode, BooleanValue(false));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/useThetaPowerTcp";
			Config::Set (specificNode, BooleanValue(true));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/multipleRateHpcc";
			Config::Set (specificNode, BooleanValue(false));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/targetUtil";
			Config::Set (specificNode, DoubleValue(0.95));
			specificNode = "$ns3::NodeListPriv/NodeList/" + nodeId.str () + "/$ns3::TcpSocketState/baseRtt";
			Config::Set (specificNode, TimeValue(MicroSeconds(linkLatency * 4 * 2 + 2 * double(PACKET_SIZE * 8) / (LEAF_SERVER_CAPACITY))));
		}
	}

	PointToPointHelper p2p;
	Ipv4AddressHelper ipv4;
	Ipv4InterfaceContainer serverNics[LEAF_COUNT][SERVER_COUNT];

	//py::object rf[LEAF_COUNT+SPINE_COUNT];

	for (uint32_t leaf = 0; leaf < LEAF_COUNT; leaf++) {
		sharedMemoryLeaf[leaf] = CreateObject<SharedMemoryBuffer>();
		sharedMemoryLeaf[leaf]->SetAttribute("BufferSize", UintegerValue(BufferSize));
		sharedMemoryLeaf[leaf]->SetSharedBufferSize(BufferSize);
	}

	Ptr<SharedMemoryBuffer> sharedMemorySpine[SPINE_COUNT];
	for (uint32_t spine = 0; spine < SPINE_COUNT; spine++) {
		sharedMemorySpine[spine] = CreateObject<SharedMemoryBuffer>();
		sharedMemorySpine[spine]->SetAttribute("BufferSize", UintegerValue(BufferSize));
		sharedMemorySpine[spine]->SetSharedBufferSize(BufferSize);
	}




	uint32_t leafPortId[LEAF_COUNT] = {0};
	uint32_t spinePortId[SPINE_COUNT] = {0};


	/*Server <--> Leaf*/
	ipv4.SetBase ("10.1.0.0", "255.255.252.0");
	p2p.SetDeviceAttribute ("DataRate", DataRateValue (DataRate (LEAF_SERVER_CAPACITY)));
	p2p.SetChannelAttribute ("Delay", TimeValue(LINK_LATENCY));
	p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("1p"));

	for (uint32_t leaf = 0; leaf < LEAF_COUNT; leaf++) {
		ipv4.NewNetwork ();
		for (uint32_t server = 0; server < SERVER_COUNT; server++) {
			NodeContainer nodeContainer = NodeContainer (leaves.Get (leaf), servers[leaf].Get (server));
			NetDeviceContainer netDeviceContainer = p2p.Install (nodeContainer);
			DynamicCast<PointToPointNetDevice>(netDeviceContainer.Get(1))->GetQueue()->SetMaxSize(QueueSize("100MB"));
			DynamicCast<PointToPointNetDevice>(netDeviceContainer.Get(0))->GetQueue()->SetMaxSize(QueueSize("1p"));
			QueueDiscContainer queueDiscs;
			queueDiscs = tc.Install(netDeviceContainer.Get(0));
			ToRQueueDiscs[leaf].Add(queueDiscs.Get(0));
			Ptr<GenQueueDisc> genDisc = DynamicCast<GenQueueDisc> (queueDiscs.Get (0));
			genDisc->SetPortId(leafPortId[leaf]);
			sharedMemoryLeaf[leaf]->addQueuePtr(genDisc,leafPortId[leaf]);
			leafPortId[leaf]++;
			switch (algorithm) {
			case DT:
				genDisc->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(DT);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
				}
				break;
			case FAB:
				genDisc->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(FAB);
				genDisc->SetFabWindow(MicroSeconds(5000));
				genDisc->SetFabThreshold(15 * PACKET_SIZE);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
				}
				break;
			case CS:
				genDisc->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(CS);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
				}
				break;
			case IB:
				genDisc->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(IB);
				genDisc->SetAfdWindow(MicroSeconds(50));
				genDisc->SetDppWindow(MicroSeconds(5000));
				genDisc->SetDppThreshold(RTTPackets);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
					genDisc->SetQrefAfd(n, uint32_t(RTTBytes));
				}
				break;
			case ABM:
				genDisc->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(ABM);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
				}
				break;
			case LQD:
				genDisc->setNPrior(nPrior); // Not needed, but anyway copy pasting :P IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(LQD);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
				}
				if (enableLqdTracing){
					genDisc->TraceConnectWithoutContext("traceLQD", MakeBoundCallback(&TraceLQD, lqdstats[leaf]));
				}
				break;
			case CREDENCE:
				genDisc->setNPrior(nPrior); // Not needed, but anyway copy pasting :P IMPORTANT. This will also trigger "alphas = new ..."
				genDisc->setPortBw(leafServerCapacity);
				genDisc->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc->SetBufferAlgorithm(CREDENCE);
				for (uint32_t n = 0; n < nPrior; n++) {
					genDisc->alphas[n] = alpha_values[n];
				}
				genDisc->setErr(errorProb);
				genDisc->SetAttribute("predict",BooleanValue(true));
				//genDisc->TraceConnectWithoutContext("getPrediction", MakeBoundCallback(&getPrediction, rf[leaf], castInteger));
				//genDisc->TraceConnectWithoutContext("getRecord", MakeBoundCallback(&getRecord, rf[leaf], castInteger));

				break;
			default:
				std::cout << "Error in buffer management configuration. Exiting!";
				return 0;
			}
			serverNics[leaf][server] = ipv4.Assign(netDeviceContainer.Get(1));
			ipv4.Assign(netDeviceContainer.Get(0));
		}
	}

	std::vector<InetSocketAddress> clients[LEAF_COUNT];
	for (uint32_t leaf = 0; leaf < LEAF_COUNT; leaf++) {
		for (uint32_t leafnext = 0; leafnext < LEAF_COUNT ; leafnext++) {
			if (leaf == leafnext) {
				continue;
			}
			for (uint32_t server = 0; server < SERVER_COUNT; server++) {
				clients[leaf].push_back(InetSocketAddress (serverNics[leafnext][server].GetAddress (0), 1000 + leafnext * LEAF_COUNT + server));
			}
		}
	}

	/*Leaf <--> Spine*/
	p2p.SetDeviceAttribute ("DataRate", DataRateValue (DataRate (SPINE_LEAF_CAPACITY)));
	p2p.SetChannelAttribute ("Delay", TimeValue(LINK_LATENCY));
	p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("1p"));
	for (uint32_t leaf = 0; leaf < LEAF_COUNT; leaf++) {
		for (uint32_t spine = 0; spine < SPINE_COUNT; spine++) {
			for (uint32_t link = 0; link < LINK_COUNT; link++) {
				ipv4.NewNetwork ();
				NodeContainer nodeContainer = NodeContainer (leaves.Get (leaf), spines.Get (spine));
				NetDeviceContainer netDeviceContainer = p2p.Install (nodeContainer);
				QueueDiscContainer queueDiscs = tc.Install(netDeviceContainer);
				northQueues[leaf].Add(queueDiscs.Get(0));
				ToRQueueDiscs[leaf].Add(queueDiscs.Get(0));
				Ptr<GenQueueDisc> genDisc[2];
				genDisc[0] = DynamicCast<GenQueueDisc> (queueDiscs.Get (0));
				genDisc[0]->SetSharedMemory(sharedMemoryLeaf[leaf]);
				genDisc[1] = DynamicCast<GenQueueDisc> (queueDiscs.Get (1));
				genDisc[1]->SetSharedMemory(sharedMemorySpine[spine]);
				genDisc[0]->SetPortId(leafPortId[leaf]);
				sharedMemoryLeaf[leaf]->addQueuePtr(genDisc[0],leafPortId[leaf]);
				leafPortId[leaf]++;
				genDisc[1]->SetPortId(spinePortId[spine]);
				sharedMemorySpine[spine]->addQueuePtr(genDisc[1],spinePortId[spine]);
				spinePortId[spine]++;
				DynamicCast<PointToPointNetDevice>(netDeviceContainer.Get(0))->GetQueue()->SetMaxSize(QueueSize("1p"));
				DynamicCast<PointToPointNetDevice>(netDeviceContainer.Get(1))->GetQueue()->SetMaxSize(QueueSize("1p"));
				if (algorithm == LQD && enableLqdTracing){
					genDisc[0]->TraceConnectWithoutContext("traceLQD", MakeBoundCallback(&TraceLQD, lqdstats[leaf]));
					genDisc[1]->TraceConnectWithoutContext("traceLQD", MakeBoundCallback(&TraceLQD, lqdstats[LEAF_COUNT+spine]));
				}
				if (algorithm == CREDENCE){
					genDisc[0]->SetAttribute("predict",BooleanValue(true));
					genDisc[1]->SetAttribute("predict",BooleanValue(true));
					genDisc[0]->setErr(errorProb);
					genDisc[1]->setErr(errorProb);
					//genDisc[0]->TraceConnectWithoutContext("getPrediction", MakeBoundCallback(&getPrediction, rf[leaf], castInteger));
					//genDisc[1]->TraceConnectWithoutContext("getPrediction", MakeBoundCallback(&getPrediction, rf[LEAF_COUNT+spine], castInteger));
					//genDisc[0]->TraceConnectWithoutContext("getRecord", MakeBoundCallback(&getRecord, rf[leaf], castInteger));
					//genDisc[1]->TraceConnectWithoutContext("getRecord", MakeBoundCallback(&getRecord, rf[LEAF_COUNT+spine], castInteger));
				}

				for (uint32_t i = 0; i < 2; i++) {
					switch (algorithm) {
					case DT:
						genDisc[i]->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(DT);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
						}
						break;
					case FAB:
						genDisc[i]->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(FAB);
						genDisc[i]->SetFabWindow(MicroSeconds(5000));
						genDisc[i]->SetFabThreshold(15 * PACKET_SIZE);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
						}
						break;
					case CS:
						genDisc[i]->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(CS);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
						}
						break;
					case IB:
						genDisc[i]->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(IB);
						genDisc[i]->SetAfdWindow(MicroSeconds(50));
						genDisc[i]->SetDppWindow(MicroSeconds(5000));
						genDisc[i]->SetDppThreshold(RTTPackets);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
							genDisc[i]->SetQrefAfd(n, uint32_t(RTTBytes));
						}
						break;
					case ABM:
						genDisc[i]->setNPrior(nPrior); // IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(ABM);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
						}
						break;
					case LQD:
						genDisc[i]->setNPrior(nPrior); // Not needed, but anyway copy pasting :P IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(LQD);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
						}
						break;
					case CREDENCE:
						genDisc[i]->setNPrior(nPrior); // Not needed, but anyway copy pasting :P IMPORTANT. This will also trigger "alphas = new ..."
						genDisc[i]->setPortBw(spineLeafCapacity);
						genDisc[i]->SetBufferAlgorithm(CREDENCE);
						for (uint32_t n = 0; n < nPrior; n++) {
							genDisc[i]->alphas[n] = alpha_values[n];
						}
						break;
					default:
						std::cout << "Error in buffer management configuration. Exiting!";
						return 0;
					}
				}
				Ipv4InterfaceContainer interfaceContainer = ipv4.Assign (netDeviceContainer);
			}
		}
	}

	// double oversub_wrt_each = static_cast<double>(SERVER_COUNT * LEAF_SERVER_CAPACITY) / (SPINE_LEAF_CAPACITY * SPINE_COUNT * LINK_COUNT)/3;

	struct cdf_table* cdfTable = new cdf_table ();
	init_cdf (cdfTable);
	load_cdf (cdfTable, cdfFileName.c_str ());
	NS_LOG_INFO ("Calculating request rate");

	double requestRate_Cubic = loadCubic * SPINE_LEAF_CAPACITY * SPINE_COUNT * LINK_COUNT / (8 * avg_cdf (cdfTable)) / (serversCubic[0].GetN());
	double requestRate_Dctcp = loadDctcp * SPINE_LEAF_CAPACITY * SPINE_COUNT * LINK_COUNT / (8 * avg_cdf (cdfTable)) / (serversDctcp[0].GetN());
	double requestRate_Power = loadPower * SPINE_LEAF_CAPACITY * SPINE_COUNT * LINK_COUNT / (8 * avg_cdf (cdfTable)) / (serversPower[0].GetN());

	if (randomSeed == 0)
	{
		srand ((unsigned)time (NULL));
	}
	else
	{
		srand (randomSeed);
	}
	double QUERY_START_TIME = START_TIME;

	long flowCount = 0;

	for (uint32_t i = 0; i < SERVER_COUNT * LEAF_COUNT; i++)
		PORT_START[i] = 4444;

	for (int fromLeafId = 0; fromLeafId < 2; fromLeafId ++)
	{
		install_applications(fromLeafId, serversCubic, requestRate_Cubic, cdfTable, flowCount, serversCubic[0].GetN(), LEAF_COUNT, START_TIME, END_TIME, FLOW_LAUNCH_END_TIME, 1,0);
	}
	for (int fromLeafId = 0; fromLeafId < 2; fromLeafId ++)
	{
		install_applications(fromLeafId, serversDctcp, requestRate_Dctcp, cdfTable, flowCount, serversDctcp[0].GetN(), LEAF_COUNT, START_TIME, END_TIME, FLOW_LAUNCH_END_TIME, 2,1);
	}
	for (int fromLeafId = 0; fromLeafId < 2; fromLeafId ++)
	{
		install_applications(fromLeafId, serversPower, requestRate_Power, cdfTable, flowCount, serversPower[0].GetN(), LEAF_COUNT, START_TIME, END_TIME, FLOW_LAUNCH_END_TIME, 3,1);
		if (queryRequestRate > 0 && requestSize > 0) {
			install_applications_incast(fromLeafId, serversPower, queryRequestRate, requestSize, cdfTable, flowCount, serversPower[0].GetN(), LEAF_COUNT, QUERY_START_TIME, END_TIME, FLOW_LAUNCH_END_TIME, 3,0);
		}
	}

	// std::cout << "Total flows: " << flowCount/2 << std::endl;

	if (enableStats){
		if (!torPrintall) {
			Simulator::Schedule(Seconds(START_TIME), InvokeToRStats, torStats, BufferSize, 0, printDelay);
		}
		else {
			Simulator::Schedule(Seconds(START_TIME), InvokePerPortToRStats, torStats, BufferSize, 0, printDelay);
		}
	}


	// AsciiTraceHelper ascii;
//    p2p.EnableAsciiAll (ascii.CreateFileStream ("eval.tr"));
	Ipv4GlobalRoutingHelper::PopulateRoutingTables ();
	// NS_LOG_UNCOND("Running the Simulation...!");
	std::cout << "Running the Simulation...!" << std::endl;
	Simulator::Stop (Seconds (END_TIME));
	Simulator::Run ();
	Simulator::Destroy ();
	free_cdf (cdfTable);
	return 0;
}
