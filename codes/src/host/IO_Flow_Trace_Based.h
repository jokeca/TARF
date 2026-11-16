#ifndef IO_FLOW_TRACE_BASED_H
#define IO_FLOW_TRACE_BASED_H

#include <string>
#include <iostream>
#include <fstream>
#include "IO_Flow_Base.h"
#include "ASCII_Trace_Definition.h"

namespace Host_Components
{
class IO_Flow_Trace_Based : public IO_Flow_Base
{
public:
	IO_Flow_Trace_Based(const sim_object_id_type &name, uint16_t flow_id, LHA_type start_lsa_on_device, LHA_type end_lsa_on_device, uint16_t io_queue_id,
						uint16_t nvme_submission_queue_size, uint16_t nvme_completion_queue_size, IO_Flow_Priority_Class::Priority priority_class, double initial_occupancy_ratio,
						std::string trace_file_path, Trace_Time_Unit time_unit, unsigned int total_replay_count, unsigned int percentage_to_be_simulated,
						HostInterface_Types SSD_device_type, PCIe_Root_Complex *pcie_root_complex, SATA_HBA *sata_hba,
						bool enabled_logging, sim_time_type logging_period, std::string logging_file_path);
	~IO_Flow_Trace_Based();
	Host_IO_Request *Generate_next_request();
	void NVMe_consume_io_request(Completion_Queue_Entry *);
	void SATA_consume_io_request(Host_IO_Request *);
	void Start_simulation();
	void Report_input_in_JSON(std::ostream& output) const override;
	void Validate_simulation_config();
	void Execute_simulator_event(MQSimEngine::Sim_Event *);
	void Get_statistics(Utils::Workload_Statistics &stats, LPA_type (*Convert_host_logical_address_to_device_address)(LHA_type lha),
						page_status_type (*Find_NVM_subunit_access_bitmap)(LHA_type lha));

private:
	Trace_Time_Unit time_unit;
	unsigned int percentage_to_be_simulated;
	std::string trace_file_path;
	unsigned int total_replay_no;
	uint64_t total_requests_in_file = 0;
	uint64_t origin_timestamp = 0;
	uint64_t first_source_timestamp = 0;
	bool trace_normalized = false;
	sim_time_type source_duration_ns = 0;
	sim_time_type horizon_ns = MAXIMUM_TIME;
	bool trace_loaded = false;
	struct Trace_Request {
		sim_time_type arrival_ns;
		LHA_type start_lba;
		unsigned int sector_count;
		bool is_write;
	};
	std::vector<Trace_Request> requests;
	void Load_and_validate_trace();

};
} // namespace Host_Components

#endif // !IO_FLOW_TRACE_BASED_H
