#include "IO_Flow_Base.h"
#include "../ssd/Host_Interface_Defs.h"
#include "../sim/Engine.h"
#include "../ssd/Stats.h"
#include "../ssd/Reliability.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace Host_Components
{
	//unsigned int InputStreamBase::lastId = 0;
IO_Flow_Base::IO_Flow_Base(const sim_object_id_type &name, uint16_t flow_id, LHA_type start_lsa_on_device, LHA_type end_lsa_on_device, uint16_t io_queue_id,
						   uint16_t nvme_submission_queue_size, uint16_t nvme_completion_queue_size,
						   IO_Flow_Priority_Class::Priority priority_class, sim_time_type stop_time, double initial_occupancy_ratio, unsigned long long total_requets_to_be_generated,
						   HostInterface_Types SSD_device_type, PCIe_Root_Complex *pcie_root_complex, SATA_HBA *sata_hba,
						   bool enabled_logging, sim_time_type logging_period, std::string logging_file_path) : MQSimEngine::Sim_Object(name), flow_id(flow_id), start_lsa_on_device(start_lsa_on_device), end_lsa_on_device(end_lsa_on_device), io_queue_id(io_queue_id),
																												priority_class(priority_class), stop_time(stop_time), initial_occupancy_ratio(initial_occupancy_ratio), total_requests_to_be_generated(total_requets_to_be_generated), SSD_device_type(SSD_device_type), pcie_root_complex(pcie_root_complex), sata_hba(sata_hba),
																												STAT_generated_request_count(0), STAT_generated_read_request_count(0), STAT_generated_write_request_count(0),
																												STAT_ignored_request_count(0),
																												STAT_serviced_request_count(0), STAT_serviced_read_request_count(0), STAT_serviced_write_request_count(0),
																												STAT_sum_device_response_time(0), STAT_sum_device_response_time_read(0), STAT_sum_device_response_time_write(0),
																												STAT_min_device_response_time(MAXIMUM_TIME), STAT_min_device_response_time_read(MAXIMUM_TIME), STAT_min_device_response_time_write(MAXIMUM_TIME),
																												STAT_max_device_response_time(0), STAT_max_device_response_time_read(0), STAT_max_device_response_time_write(0),
																												STAT_sum_request_delay(0), STAT_sum_request_delay_read(0), STAT_sum_request_delay_write(0),
																												STAT_min_request_delay(MAXIMUM_TIME), STAT_min_request_delay_read(MAXIMUM_TIME), STAT_min_request_delay_write(MAXIMUM_TIME),
																												STAT_max_request_delay(0), STAT_max_request_delay_read(0), STAT_max_request_delay_write(0),
																												STAT_transferred_bytes_total(0), STAT_transferred_bytes_read(0), STAT_transferred_bytes_write(0), progress(0), next_progress_step(0),
																												enabled_logging(enabled_logging), logging_period(logging_period), logging_file_path(logging_file_path)
{
	Host_IO_Request *t = NULL;

	switch (SSD_device_type)
	{
	case HostInterface_Types::NVME:
		for (uint16_t cmdid = 0; cmdid < (uint16_t)(0xffffffff); cmdid++)
		{
			available_command_ids.insert(cmdid);
		}
		for (uint16_t cmdid = 0; cmdid < nvme_submission_queue_size; cmdid++)
		{
			request_queue_in_memory.push_back(t);
		}
		nvme_queue_pair.Submission_queue_size = nvme_submission_queue_size;
		nvme_queue_pair.Submission_queue_head = 0;
		nvme_queue_pair.Submission_queue_tail = 0;
		nvme_queue_pair.Completion_queue_size = nvme_completion_queue_size;
		nvme_queue_pair.Completion_queue_head = 0;
		nvme_queue_pair.Completion_queue_tail = 0;

		//id = 0: admin queues, id = 1 to 8, normal I/O queues
		switch (io_queue_id)
		{
		case 0:
			throw std::logic_error("I/O queue id 0 is reserved for NVMe admin queues and should not be used for I/O flows");
			/*
						nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_0;
						nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_0;
						nvme_queue_pair.Completion_queue_tail = COMPLETION_QUEUE_REGISTER_0;*/
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_REGISTER_0;
			break;
		case 1:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_1;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_1;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_1;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_1;
			break;
		case 2:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_2;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_2;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_2;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_2;
			break;
		case 3:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_3;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_3;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_3;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_3;
			break;
		case 4:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_4;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_4;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_4;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_4;
			break;
		case 5:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_5;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_5;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_5;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_5;
			break;
		case 6:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_6;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_6;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_6;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_6;
			break;
		case 7:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_7;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_7;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_7;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_7;
			break;
		case 8:
			nvme_queue_pair.Submission_queue_memory_base_address = SUBMISSION_QUEUE_MEMORY_8;
			nvme_queue_pair.Submission_tail_register_address_on_device = SUBMISSION_QUEUE_REGISTER_8;
			nvme_queue_pair.Completion_queue_memory_base_address = COMPLETION_QUEUE_MEMORY_8;
			nvme_queue_pair.Completion_head_register_address_on_device = COMPLETION_QUEUE_REGISTER_8;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
		if(SSD_Components::Stats::turn_on==1){
			std::string directory = SSD_Components::Stats::filename; 
			for(int i=0;i<3;i++){
				std::string filename = SSD_Components::Stats::filename+"_latency_" + std::to_string(i) + ".csv";
				latency_files[i].open(directory + "/"+filename);
				if (!latency_files[i].is_open()) {
					std::cerr << "open error" << std::endl;
				}
			}
		}
	}
	
	IO_Flow_Base::~IO_Flow_Base()
	{
		log_file.close();
		for(auto &req : waiting_requests) {
			if (req) {
				delete req;
			}
		}

		switch (SSD_device_type) {
			case HostInterface_Types::NVME:
				for (auto &req : nvme_software_request_queue) {
					if (req.second) {
						delete req.second;
					}
				}
				break;
			case HostInterface_Types::SATA:
				break;
			default:
				PRINT_ERROR("Unsupported host interface type in IO_Flow_Base!")
		}
		if(SSD_Components::Stats::turn_on==1){
			for (int i = 0; i < 3; i++)
				{
					latency_files[i].close();
				}
		}
	}

	void IO_Flow_Base::Start_simulation()
	{
		next_logging_milestone = logging_period;
		if (enabled_logging) {
			log_file.open(logging_file_path, std::ofstream::out);
		}
		/**/log_file << "到达时间(ns)\t" << "排队时间(ns)\t" << "ReponseTime(us)\t" << "EndToEndDelay(us)" << std::endl;

		//log_file << "SimulationTime(us)\t" << "ReponseTime(us)\t" << "EndToEndDelay(us)"<< std::endl;
		STAT_sum_device_response_time_short_term = 0;
		STAT_serviced_request_count_short_term = 0;
		STAT_sum_request_delay_short_term = 0;
	}

	void IO_Flow_Base::SATA_consume_io_request(Host_IO_Request* request)
	{
		Record_completion(request);
		sim_time_type device_response_time = Simulator->Time() - request->Enqueue_time;
		sim_time_type request_delay = Simulator->Time() - request->Arrival_time;
		
		STAT_serviced_request_count++;
		STAT_serviced_request_count_short_term++;
		STAT_sum_device_response_time += device_response_time;
		STAT_sum_device_response_time_short_term += device_response_time;
		STAT_sum_request_delay += request_delay;
		STAT_sum_request_delay_short_term += request_delay;
		if (device_response_time > STAT_max_device_response_time) {
			STAT_max_device_response_time = device_response_time;
		}
		if (device_response_time < STAT_min_device_response_time) {
			STAT_min_device_response_time = device_response_time;
		}
		if (request_delay > STAT_max_request_delay) {
			STAT_max_request_delay = request_delay;
		}
		if (request_delay < STAT_min_request_delay) {
			STAT_min_request_delay = request_delay;
		}
		STAT_transferred_bytes_total += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;

		if (request->Type == Host_IO_Request_Type::READ) {
			STAT_serviced_read_request_count++;
			STAT_sum_device_response_time_read += device_response_time;
			STAT_sum_request_delay_read += request_delay;
			if (device_response_time > STAT_max_device_response_time_read) {
				STAT_max_device_response_time_read = device_response_time;
			}
			if (device_response_time < STAT_min_device_response_time_read) {
				STAT_min_device_response_time_read = device_response_time;
			}
			if (request_delay > STAT_max_request_delay_read) {
				STAT_max_request_delay_read = request_delay;
			}
			if (request_delay < STAT_min_request_delay_read) {
				STAT_min_request_delay_read = request_delay;
			}
			STAT_transferred_bytes_read += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;
		} else {
			STAT_serviced_write_request_count++;
			STAT_sum_device_response_time_write += device_response_time;
			STAT_sum_request_delay_write += request_delay;
			if (device_response_time > STAT_max_device_response_time_write) {
				STAT_max_device_response_time_write = device_response_time;
			}
			if (device_response_time < STAT_min_device_response_time_write) {
				STAT_min_device_response_time_write = device_response_time;
			}
			if (request_delay > STAT_max_request_delay_write) {
				STAT_max_request_delay_write = request_delay;
			}
			if (request_delay < STAT_min_request_delay_write) {
				STAT_min_request_delay_write = request_delay;
			}
			STAT_transferred_bytes_write += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;
		}

		delete request;

		//Announce simulation progress
		if (stop_time > 0) {
			progress = int(Simulator->Time() / (double)stop_time * 100);
		} else {
			progress = int(STAT_serviced_request_count / (double)total_requests_to_be_generated * 100);
		}
		if (progress >= next_progress_step) {
			std::string progress_bar;
			int barWidth = 100;
			progress_bar += "[";
			int pos = progress;
			for (int i = 0; i < barWidth; i += 5) {
				if (i < pos) {
					progress_bar += "=";
				} else if (i == pos) {
					progress_bar += ">";
				} else {
					progress_bar += " ";
				}
			}
			progress_bar += "] ";
			PRINT_MESSAGE(progress_bar << " " << progress << "% progress in " << ID() << std::endl)
				next_progress_step += 5;
		}

		if (Simulator->Time() > next_logging_milestone) {
			log_file << Simulator->Time() / SIM_TIME_TO_MICROSECONDS_COEFF << "\t" << Get_device_response_time_short_term() << "\t" << Get_end_to_end_request_delay_short_term() << std::endl;
			STAT_sum_device_response_time_short_term = 0;
			STAT_sum_request_delay_short_term = 0;
			STAT_serviced_request_count_short_term = 0;
			next_logging_milestone = Simulator->Time() + logging_period;
		}
	}

	void IO_Flow_Base::NVMe_consume_io_request(Completion_Queue_Entry* cqe)
	{
		//Find the request and update statistics
		auto found_request = nvme_software_request_queue.find(cqe->Command_Identifier);
		if (found_request == nvme_software_request_queue.end() || found_request->second == nullptr)
			throw std::runtime_error("NVMe completion has no matching host request");
		Host_IO_Request* request = found_request->second;
		nvme_software_request_queue.erase(found_request);
		available_command_ids.insert(cqe->Command_Identifier);
		Record_completion(request);
		sim_time_type device_response_time = Simulator->Time() - request->Enqueue_time;
		sim_time_type request_delay = Simulator->Time() - request->Arrival_time;

		
		STAT_serviced_request_count++;

		STAT_serviced_request_count_short_term++;

		STAT_sum_device_response_time += device_response_time;
		STAT_sum_device_response_time_short_term += device_response_time;
		STAT_sum_request_delay += request_delay;
		STAT_sum_request_delay_short_term += request_delay;
		if (device_response_time > STAT_max_device_response_time) {
			STAT_max_device_response_time = device_response_time;
		}
		if (device_response_time < STAT_min_device_response_time) {
			STAT_min_device_response_time = device_response_time;
		}
		if (request_delay > STAT_max_request_delay) {
			STAT_max_request_delay = request_delay;
		}
		if (request_delay < STAT_min_request_delay) {
			STAT_min_request_delay = request_delay;
		}
		STAT_transferred_bytes_total += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;

		if(SSD_Components::Stats::turn_on==1){
			latency_files[0] << device_response_time << ",";
		}
		if (request->Type == Host_IO_Request_Type::READ) {
			STAT_serviced_read_request_count++;
			STAT_sum_device_response_time_read += device_response_time;
			STAT_sum_request_delay_read += request_delay;
			if (device_response_time > STAT_max_device_response_time_read) {
				STAT_max_device_response_time_read = device_response_time;
			}
			if (device_response_time < STAT_min_device_response_time_read) {
				STAT_min_device_response_time_read = device_response_time;
			}
			if (request_delay > STAT_max_request_delay_read) {
				STAT_max_request_delay_read = request_delay;
			}
			if (request_delay < STAT_min_request_delay_read) {
				STAT_min_request_delay_read = request_delay;
			}
			STAT_transferred_bytes_read += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;

			if(SSD_Components::Stats::turn_on==1){
				latency_files[1] << device_response_time << ",";
			}

		} else {
			STAT_serviced_write_request_count++;
			STAT_sum_device_response_time_write += device_response_time;
			STAT_sum_request_delay_write += request_delay;
			if (device_response_time > STAT_max_device_response_time_write) {
				STAT_max_device_response_time_write = device_response_time;
			}
			if (device_response_time < STAT_min_device_response_time_write) {
				STAT_min_device_response_time_write = device_response_time;
			}
			if (request_delay > STAT_max_request_delay_write) {
				STAT_max_request_delay_write = request_delay;
			}
			if (request_delay < STAT_min_request_delay_write) {
				STAT_min_request_delay_write = request_delay;
			}
			STAT_transferred_bytes_write += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;

			if(SSD_Components::Stats::turn_on==1){
				latency_files[2] << device_response_time << ",";
			}
		}

		/**/log_file << request->Arrival_time << "\t" << request->Enqueue_time << "\t" << device_response_time / SIM_TIME_TO_MICROSECONDS_COEFF << "\t" << request_delay / SIM_TIME_TO_MICROSECONDS_COEFF << std::endl;


		delete request;

		nvme_queue_pair.Submission_queue_head = cqe->SQ_Head;
		
		//MQSim always assumes that the request is processed correctly, so no need to check cqe->SF_P

		//If the submission queue is not full anymore, then enqueue waiting requests
		while(waiting_requests.size() > 0) {
			if (!NVME_SQ_FULL(nvme_queue_pair) && available_command_ids.size() > 0) {
				Host_IO_Request* new_req = waiting_requests.front();
				waiting_requests.pop_front();
				if (nvme_software_request_queue[*available_command_ids.begin()] != NULL) {
					PRINT_ERROR("Unexpteced situation in IO_Flow_Base! Overwriting a waiting I/O request in the queue!")
				} else {
					new_req->IO_queue_info = *available_command_ids.begin();
					nvme_software_request_queue[*available_command_ids.begin()] = new_req;
					available_command_ids.erase(available_command_ids.begin());
					request_queue_in_memory[nvme_queue_pair.Submission_queue_tail] = new_req;
					NVME_UPDATE_SQ_TAIL(nvme_queue_pair);
				}
				new_req->Enqueue_time = Simulator->Time();
				pcie_root_complex->Write_to_device(nvme_queue_pair.Submission_tail_register_address_on_device, nvme_queue_pair.Submission_queue_tail);//Based on NVMe protocol definition, the updated tail pointer should be informed to the device
			} else {
				break;
			}
		}

		delete cqe;

		//Announce simulation progress
		if (stop_time > 0) {
			progress = int(Simulator->Time() / (double)stop_time * 100);
		} else {
			progress = int(STAT_serviced_request_count / (double)total_requests_to_be_generated * 100);
			// if(progress<=70){
			// 	SSD_Components::Stats::stage=1;
			// }
			// // if(progress>60){
			// // 	SSD_Components::Stats::stage=2;
			// // }
			// if(progress>70&&progress<=95){
			// 	SSD_Components::Stats::stage=2;
			// }
			// if(progress>95){
			// 	SSD_Components::Stats::stage=3;
			// }
		}

		if (progress >= next_progress_step) {
			std::string progress_bar;
			int barWidth = 100;
			progress_bar += "[";
			int pos = progress;
			for (int i = 0; i < barWidth; i += 5) {
				if (i < pos) {
					progress_bar += "=";
				} else if (i == pos) {
					progress_bar += ">";
				} else {
					progress_bar += " ";
				}
			}
			progress_bar += "] ";
			PRINT_MESSAGE(progress_bar << " " << progress << "% progress in " << ID() << std::endl)
			next_progress_step += 5;
		}



		//if (Simulator->Time() > next_logging_milestone) {
		//	log_file << Simulator->Time() / SIM_TIME_TO_MICROSECONDS_COEFF << "\t" << Get_device_response_time_short_term() << "\t" << Get_end_to_end_request_delay_short_term() << std::endl;
		//	STAT_sum_device_response_time_short_term = 0;
		//	STAT_sum_request_delay_short_term = 0;
		//	STAT_serviced_request_count_short_term = 0;
		//	next_logging_milestone = Simulator->Time() + logging_period;
		//}
	}
	
	Submission_Queue_Entry* IO_Flow_Base::NVMe_read_sqe(uint64_t address)
	{
		Submission_Queue_Entry* sqe = new Submission_Queue_Entry;
		Host_IO_Request* request = request_queue_in_memory[(uint16_t)((address - nvme_queue_pair.Submission_queue_memory_base_address) / sizeof(Submission_Queue_Entry))];
		
		if (request == NULL) {
			throw std::invalid_argument(this->ID() + ": Request to access a submission queue entry that does not exist.");
		}

		sqe->Command_Identifier = request->IO_queue_info;
		if (request->Type == Host_IO_Request_Type::READ) {
			sqe->Opcode = NVME_READ_OPCODE;
			sqe->Command_specific[0] = (uint32_t) request->Start_LBA;
			sqe->Command_specific[1] = (uint32_t)(request->Start_LBA >> 32);
			sqe->Command_specific[2] = ((uint32_t)((uint16_t)request->LBA_count)) & (uint32_t)(0x0000ffff);
			sqe->PRP_entry_1 = (DATA_MEMORY_REGION);//Dummy addresses, just to emulate data read/write access
			sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000);//Dummy addresses
		} else {
			sqe->Opcode = NVME_WRITE_OPCODE;
			sqe->Command_specific[0] = (uint32_t)request->Start_LBA;
			sqe->Command_specific[1] = (uint32_t)(request->Start_LBA >> 32);
			sqe->Command_specific[2] = ((uint32_t)((uint16_t)request->LBA_count)) & (uint32_t)(0x0000ffff);
			sqe->PRP_entry_1 = (DATA_MEMORY_REGION);//Dummy addresses, just to emulate data read/write access
			sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000);//Dummy addresses
		}

		return sqe;
	}

	void IO_Flow_Base::Submit_io_request(Host_IO_Request* request)
	{
		const unsigned int type_index = request->Type == Host_IO_Request_Type::READ ? 0 : 1;
		const uint64_t bytes = uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;
		if (generated_bytes[type_index] > UINT64_MAX - bytes)
			throw std::runtime_error("Generated host byte counter overflow");
		generated_bytes[type_index] += bytes;
        if (TARF::Config::Get().measurement_start_ns) {
            const unsigned phase = request->Arrival_time >= TARF::Config::Get().measurement_start_ns ? 1 : 0;
            ++phase_generated[phase][type_index];
            phase_generated_bytes[phase][type_index] += bytes;
        }
		switch (SSD_device_type) {
			case HostInterface_Types::NVME:
				//If either of software or hardware queue is full
				if (NVME_SQ_FULL(nvme_queue_pair) || available_command_ids.size() == 0) {
					waiting_requests.push_back(request);
				} else {
					if (nvme_software_request_queue[*available_command_ids.begin()] != NULL) {
						PRINT_ERROR("Unexpteced situation in IO_Flow_Base! Overwriting an unhandled I/O request in the queue!")
					} else {
						request->IO_queue_info = *available_command_ids.begin();
						nvme_software_request_queue[*available_command_ids.begin()] = request;
						available_command_ids.erase(available_command_ids.begin());
						request_queue_in_memory[nvme_queue_pair.Submission_queue_tail] = request;
						NVME_UPDATE_SQ_TAIL(nvme_queue_pair);
					}
					request->Enqueue_time = Simulator->Time();

					//if (request->Arrival_time == 1455618852660009984) {
					//	std::cout << "追踪事务-Submit_io_request" << std::endl;
					//	std::cout << "地址：" << nvme_queue_pair.Submission_tail_register_address_on_device << "::值：" << nvme_queue_pair.Submission_queue_tail << std::endl;
					//}
					pcie_root_complex->Write_to_device(nvme_queue_pair.Submission_tail_register_address_on_device, nvme_queue_pair.Submission_queue_tail);//Based on NVMe protocol definition, the updated tail pointer should be informed to the device
				}
				break;
			case HostInterface_Types::SATA:
				request->Source_flow_id = flow_id;
				sata_hba->Submit_io_request(request);
				break;
		}
	}

	void IO_Flow_Base::NVMe_update_and_submit_completion_queue_tail()
	{
		nvme_queue_pair.Completion_queue_head++;
		if (nvme_queue_pair.Completion_queue_head == nvme_queue_pair.Completion_queue_size) {
			nvme_queue_pair.Completion_queue_head = 0;
		}
		pcie_root_complex->Write_to_device(nvme_queue_pair.Completion_head_register_address_on_device, nvme_queue_pair.Completion_queue_head);//Based on NVMe protocol definition, the updated head pointer should be informed to the device
	}

	const NVMe_Queue_Pair* IO_Flow_Base::Get_nvme_queue_pair_info()
	{
		return &nvme_queue_pair;
	}

	LHA_type IO_Flow_Base::Get_start_lsa_on_device()
	{
		return start_lsa_on_device;
	}

	LHA_type IO_Flow_Base::Get_end_lsa_address_on_device()
	{
		return end_lsa_on_device;
	}

	uint32_t IO_Flow_Base::Get_generated_request_count()
	{
		return STAT_generated_request_count;
	}

	uint32_t IO_Flow_Base::Get_serviced_request_count()
	{
		return STAT_serviced_request_count;
	}

	uint32_t IO_Flow_Base::Get_device_response_time()
	{
		if (STAT_serviced_request_count == 0) {
			return 0;
		}

		return (uint32_t)(STAT_sum_device_response_time / STAT_serviced_request_count / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_min_device_response_time()
	{
		return (uint32_t)(STAT_min_device_response_time / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_max_device_response_time()
	{
		return (uint32_t)(STAT_max_device_response_time / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_end_to_end_request_delay()
	{
		if (STAT_serviced_request_count == 0) {
			return 0;
		}

		return (uint32_t)(STAT_sum_request_delay / STAT_serviced_request_count / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_min_end_to_end_request_delay()
	{
		return (uint32_t)(STAT_min_request_delay / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_max_end_to_end_request_delay()
	{
		return (uint32_t)(STAT_max_request_delay / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_device_response_time_short_term()
	{
		if (STAT_serviced_request_count_short_term == 0) {
			return 0;
		}

		return (uint32_t)(STAT_sum_device_response_time_short_term / STAT_serviced_request_count_short_term / SIM_TIME_TO_MICROSECONDS_COEFF);
	}

	uint32_t IO_Flow_Base::Get_end_to_end_request_delay_short_term()
	{
		if (STAT_serviced_request_count == 0) {
			return 0;
		}

		return (uint32_t)(STAT_sum_request_delay_short_term / STAT_serviced_request_count_short_term / SIM_TIME_TO_MICROSECONDS_COEFF);
	}
	

    void IO_Flow_Base::Record_completion(const Host_IO_Request* request)
    {
        const sim_time_type now = Simulator->Time();
        if (now < request->Arrival_time || now < request->Enqueue_time || request->Enqueue_time < request->Arrival_time)
            throw std::runtime_error("Host completion has inconsistent arrival/enqueue timestamps");
        const unsigned int index = request->Type == Host_IO_Request_Type::READ ? 0 : 1;
        end_to_end_samples[index].push_back(now - request->Arrival_time);
        device_latency_samples[index].push_back(now - request->Enqueue_time);
        if (TARF::Config::Get().measurement_start_ns) {
            const unsigned phase = request->Arrival_time >= TARF::Config::Get().measurement_start_ns ? 1 : 0;
            completion_phases[index].push_back(phase);
            phase_completed_bytes[phase][index] += uint64_t(request->LBA_count) * SECTOR_SIZE_IN_BYTE;
            if (!phase && now >= TARF::Config::Get().measurement_start_ns) ++warmup_completions_after_boundary;
        }
    }

    void IO_Flow_Base::Write_JSON_string(std::ostream& output, const std::string& text)
    {
        const char hex[] = "0123456789abcdef";
        output << '"';
        for (unsigned char ch : text) {
            if (ch == '"' || ch == '\\') output << '\\' << ch;
            else if (ch < 0x20) output << "\\u00" << hex[ch >> 4] << hex[ch & 15];
            else output << ch;
        }
        output << '"';
    }

    void IO_Flow_Base::Report_input_in_JSON(std::ostream& output) const
    {
        output << "{\"kind\":\"synthetic\"}";
    }

    namespace {
    struct Host_Summary {
        uint64_t generated[2] = {0, 0};
        uint64_t completed[2] = {0, 0};
        uint64_t generated_bytes[2] = {0, 0};
        uint64_t completed_bytes[2] = {0, 0};
        std::vector<sim_time_type> latency[2], device_latency[2];
    };
    void Write_latency(std::ostream& output, std::vector<sim_time_type> samples)
    {
        output << "{\"count\":" << samples.size();
        if (samples.empty()) {
            output << ",\"sum_ns\":0,\"mean\":null,\"p99\":null,\"p99_9\":null,\"min\":null,\"max\":null}";
            return;
        }
        std::sort(samples.begin(), samples.end());
        unsigned __int128 sum = 0;
        for (sim_time_type sample : samples) sum += sample;
        std::string sum_decimal;
        unsigned __int128 remaining = sum;
        do { sum_decimal.push_back('0' + remaining % 10); remaining /= 10; } while (remaining);
        std::reverse(sum_decimal.begin(), sum_decimal.end());
        output << ",\"sum_ns\":" << sum_decimal
            << ",\"mean\":" << std::setprecision(20) << static_cast<long double>(sum) / samples.size()
            << ",\"p99\":" << samples[(samples.size() * 99 + 99) / 100 - 1]
            << ",\"p99_9\":" << samples[(samples.size() * 999 + 999) / 1000 - 1]
            << ",\"min\":" << samples.front() << ",\"max\":" << samples.back() << '}';
    }
    void Write_group(std::ostream& output, const Host_Summary& data, unsigned int first, unsigned int last)
    {
        uint64_t generated = 0, completed = 0, generated_bytes = 0, completed_bytes = 0;
        std::vector<sim_time_type> latency, device_latency;
        for (unsigned int index = first; index < last; ++index) {
            generated += data.generated[index]; completed += data.completed[index];
            generated_bytes += data.generated_bytes[index]; completed_bytes += data.completed_bytes[index];
            latency.insert(latency.end(), data.latency[index].begin(), data.latency[index].end());
            device_latency.insert(device_latency.end(), data.device_latency[index].begin(), data.device_latency[index].end());
        }
        if (completed > generated || completed_bytes > generated_bytes || latency.size() != completed || device_latency.size() != completed)
            throw std::runtime_error("Host completion accounting is inconsistent");
        output << "{\"generated\":" << generated << ",\"completed\":" << completed
            << ",\"pending\":" << generated - completed
            << ",\"generated_bytes\":" << generated_bytes << ",\"completed_bytes\":" << completed_bytes
            << ",\"pending_bytes\":" << generated_bytes - completed_bytes << ",\"latency_ns\":";
        Write_latency(output, std::move(latency));
        output << ",\"device_latency_ns\":";
        Write_latency(output, std::move(device_latency));
        output << '}';
    }
    void Write_summary(std::ostream& output, const Host_Summary& data)
    {
        output << "\"all\":"; Write_group(output, data, 0, 2);
        output << ",\"read\":"; Write_group(output, data, 0, 1);
        output << ",\"write\":"; Write_group(output, data, 1, 2);
    }
    }

    void IO_Flow_Base::Report_results_in_JSON(const std::vector<IO_Flow_Base*>& flows, const std::string& output_file_path)
    {
        std::ofstream output(output_file_path);
        if (!output) throw std::runtime_error("Cannot open host JSON output: " + output_file_path);
        output << "{\"schema_version\":1,\"simulation_time_ns\":" << Simulator->Time()
            << ",\"arrival_window\":\"[0,horizon_ns)\",\"latency_population\":\"completed requests only\","
            << "\"latency_definition\":\"completion minus arrival; includes host queue waiting\","
            << "\"percentile_method\":\"exact nearest rank ceil(p*N)\",\"flows\":[";
        Host_Summary aggregate, phases[2];
        uint64_t warmup_late = 0;
        bool first_flow = true;
        for (IO_Flow_Base* flow : flows) {
            Host_Summary current;
            current.generated[0] = flow->STAT_generated_read_request_count;
            current.generated[1] = flow->STAT_generated_write_request_count;
            current.completed[0] = flow->STAT_serviced_read_request_count;
            current.completed[1] = flow->STAT_serviced_write_request_count;
            current.completed_bytes[0] = flow->STAT_transferred_bytes_read;
            current.completed_bytes[1] = flow->STAT_transferred_bytes_write;
            for (unsigned int index = 0; index < 2; ++index) {
                current.generated_bytes[index] = flow->generated_bytes[index];
                current.latency[index] = flow->end_to_end_samples[index];
                current.device_latency[index] = flow->device_latency_samples[index];
                aggregate.generated[index] += current.generated[index];
                aggregate.completed[index] += current.completed[index];
                aggregate.generated_bytes[index] += current.generated_bytes[index];
                aggregate.completed_bytes[index] += current.completed_bytes[index];
                aggregate.latency[index].insert(aggregate.latency[index].end(), current.latency[index].begin(), current.latency[index].end());
                aggregate.device_latency[index].insert(aggregate.device_latency[index].end(), current.device_latency[index].begin(), current.device_latency[index].end());
            }
            if (TARF::Config::Get().measurement_start_ns) {
                warmup_late += flow->warmup_completions_after_boundary;
                for (unsigned index = 0; index < 2; ++index) {
                    for (unsigned phase = 0; phase < 2; ++phase) {
                        phases[phase].generated[index] += flow->phase_generated[phase][index];
                        phases[phase].generated_bytes[index] += flow->phase_generated_bytes[phase][index];
                        phases[phase].completed_bytes[index] += flow->phase_completed_bytes[phase][index];
                    }
                    if (flow->completion_phases[index].size() != current.latency[index].size())
                        throw std::runtime_error("Host cohort labels do not match completion samples");
                    for (size_t i = 0; i < current.latency[index].size(); ++i) {
                        auto& phase = phases[flow->completion_phases[index][i]];
                        ++phase.completed[index];
                        phase.latency[index].push_back(current.latency[index][i]);
                        phase.device_latency[index].push_back(current.device_latency[index][i]);
                    }
                }
            }
            if (!first_flow) output << ',';
            first_flow = false;
            output << "{\"id\":"; Write_JSON_string(output, flow->ID());
            output << ",\"input\":"; flow->Report_input_in_JSON(output);
            output << ','; Write_summary(output, current); output << '}';
        }
        output << "],\"aggregate\":{"; Write_summary(output, aggregate); output << '}';
        if (TARF::Config::Get().measurement_start_ns) {
            output << ",\"measurement_start_ns\":" << TARF::Config::Get().measurement_start_ns
                << ",\"warmup_completions_after_boundary\":" << warmup_late
                << ",\"phases\":{\"warmup\":{";
            Write_summary(output, phases[0]); output << "},\"measurement\":{";
            Write_summary(output, phases[1]); output << "}}";
        }
        output << "}\n";
        output.close();
        if (!output) throw std::runtime_error("Failed to finish host JSON output: " + output_file_path);
    }

    void IO_Flow_Base::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter)
    {
        xmlwriter.Write_open_tag(name_prefix + ".IO_Flow");
        xmlwriter.Write_attribute_string("Name", ID());
        xmlwriter.Write_attribute_string("Sys_Total_Generate_Requests", std::to_string(STAT_generated_request_count));
        xmlwriter.Write_attribute_string("Sys_Total_Serviced_Requests", std::to_string(STAT_serviced_request_count));
        xmlwriter.Write_attribute_string("Pending_Requests", std::to_string(STAT_generated_request_count - STAT_serviced_request_count));
        xmlwriter.Write_attribute_string("Total_Serviced_Read_Requests", std::to_string(STAT_serviced_read_request_count));
        xmlwriter.Write_attribute_string("Total_Serviced_Write_Requests", std::to_string(STAT_serviced_write_request_count));
        xmlwriter.Write_attribute_string("Completed_Bytes", std::to_string(STAT_transferred_bytes_total));
        xmlwriter.Write_attribute_string("Device_Response_Time", std::to_string(Get_device_response_time()));
        xmlwriter.Write_attribute_string("End_to_End_Request_Delay", std::to_string(Get_end_to_end_request_delay()));
        xmlwriter.Write_close_tag();
    }
}
