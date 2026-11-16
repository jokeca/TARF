#include <stdexcept>
#include <cmath>
#include <vector>
#include <random>
#include <map>
#include <functional>
#include <iterator>
#include "../sim/Sim_Defs.h"
#include "../utils/DistributionTypes.h"
#include "../utils/Helper_Functions.h"
#include "FTL.h"
#include "Stats.h"
#include "Address_Mapping_Unit_Page_Level.h"
#include "Reliability.h"

namespace SSD_Components
{
	FTL::FTL(const sim_object_id_type& id, Data_Cache_Manager_Base* data_cache_manager,
		unsigned int channel_no, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int page_size_in_sectors, 
		sim_time_type avg_flash_read_latency, sim_time_type avg_flash_program_latency, 
		double over_provisioning_ratio, unsigned int max_allowed_block_erase_count, int seed,int setcount) :
		NVM_Firmware(id, data_cache_manager), random_generator(seed),
		channel_no(channel_no), chip_no_per_channel(chip_no_per_channel), die_no_per_chip(die_no_per_chip), plane_no_per_die(plane_no_per_die),
		block_no_per_plane(block_no_per_plane), page_no_per_block(page_no_per_block), page_size_in_sectors(page_size_in_sectors), 
		avg_flash_read_latency(avg_flash_read_latency), avg_flash_program_latency(avg_flash_program_latency),
		over_provisioning_ratio(over_provisioning_ratio), max_allowed_block_erase_count(max_allowed_block_erase_count),
		setcount(setcount)
	{
		Stats::Init_stats(channel_no, chip_no_per_channel, die_no_per_chip, plane_no_per_die, block_no_per_plane, page_no_per_block, max_allowed_block_erase_count,setcount);
	}

	FTL::~FTL()
	{
		Stats::Clear_stats(channel_no, chip_no_per_channel, die_no_per_chip, plane_no_per_die, block_no_per_plane, page_no_per_block, max_allowed_block_erase_count,setcount);
	}

	void FTL::Validate_simulation_config()
	{
		NVM_Firmware::Validate_simulation_config();
		if (this->Data_cache_manager == NULL)
			throw std::logic_error("The cache manager is not set for FTL!");
		if (this->Address_Mapping_Unit == NULL)
			throw std::logic_error("The mapping module is not set for FTL!");
		if (this->BlockManager == NULL)
			throw std::logic_error("The block manager is not set for FTL!");
		if (this->GC_and_WL_Unit == NULL)
			throw std::logic_error("The garbage collector is not set for FTL!");
	}
	void FTL::Perform_precondition(std::vector<Utils::Workload_Statistics*> workload_stats)
	{
		Address_Mapping_Unit->Store_mapping_table_on_flash_at_start();
		auto* mapping = dynamic_cast<Address_Mapping_Unit_Page_Level*>(Address_Mapping_Unit);
		if (!mapping) throw std::runtime_error("TARF initial snapshot requires page-level mapping");
		for (const auto* stat : workload_stats) {
			const LPA_type pages = static_cast<LPA_type>(stat->Initial_occupancy_ratio * mapping->Get_logical_pages_count(stat->Stream_id));
			mapping->Initialize_valid_pages(stat->Stream_id, pages);
			if (!TARF::Config::Get().initial_read_coverage_file.empty()) {
				if (workload_stats.size() != 1) throw std::runtime_error("Explicit read coverage currently requires one input stream");
				mapping->Initialize_valid_pages_from_file(stat->Stream_id, TARF::Config::Get().initial_read_coverage_file);
			}
		}
	}

	void FTL::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter)
	{
		std::string tmp = name_prefix + ".FTL";
		xmlwriter.Write_start_element_tag(tmp);

		std::string attr = "Issued_Flash_Read_CMD";
		std::string val = std::to_string(Stats::IssuedReadCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Interleaved_Read_CMD";
		val = std::to_string(Stats::IssuedInterleaveReadCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Multiplane_Read_CMD";
		val = std::to_string(Stats::IssuedMultiplaneReadCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Copyback_Read_CMD";
		val = std::to_string(Stats::IssuedCopybackReadCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Multiplane_Copyback_Read_CMD";
		val = std::to_string(Stats::IssuedMultiplaneCopybackReadCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Program_CMD";
		val = std::to_string(Stats::IssuedProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Interleaved_Program_CMD";
		val = std::to_string(Stats::IssuedInterleaveProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Multiplane_Program_CMD";
		val = std::to_string(Stats::IssuedMultiplaneProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Interleaved_Multiplane_Program_CMD";
		val = std::to_string(Stats::IssuedInterleaveMultiplaneProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Copyback_Program_CMD";
		val = std::to_string(Stats::IssuedCopybackProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Multiplane_Copyback_Program_CMD";
		val = std::to_string(Stats::IssuedMultiplaneCopybackProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Erase_CMD";
		val = std::to_string(Stats::IssuedEraseCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Interleaved_Erase_CMD";
		val = std::to_string(Stats::IssuedInterleaveEraseCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Multiplane_Erase_CMD";
		val = std::to_string(Stats::IssuedMultiplaneEraseCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Interleaved_Multiplane_Erase_CMD";
		val = std::to_string(Stats::IssuedInterleaveMultiplaneEraseCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Suspend_Program_CMD";
		val = std::to_string(Stats::IssuedSuspendProgramCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Suspend_Erase_CMD";
		val = std::to_string(Stats::IssuedSuspendEraseCMD);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Read_CMD_For_Mapping";
		val = std::to_string(Stats::Total_flash_reads_for_mapping);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Issued_Flash_Program_CMD_For_Mapping";
		val = std::to_string(Stats::Total_flash_writes_for_mapping);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_Hits";
		val = std::to_string(Stats::CMT_hits);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_Hits_For_Read";
		val = std::to_string(Stats::readTR_CMT_hits);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_Hits_For_Write";
		val = std::to_string(Stats::writeTR_CMT_hits);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_Misses";
		val = std::to_string(Stats::CMT_miss);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_Misses_For_Read";
		val = std::to_string(Stats::readTR_CMT_miss);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_Misses_For_Write";
		val = std::to_string(Stats::writeTR_CMT_miss);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Total_CMT_Queries";
		val = std::to_string(Stats::total_CMT_queries);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Total_CMT_Queries_For_Reads";
		val = std::to_string(Stats::total_readTR_CMT_queries);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Total_CMT_Queries_For_Writes";
		val = std::to_string(Stats::total_writeTR_CMT_queries);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "CMT_hit_ratio";
		val = std::to_string((double)Stats::CMT_hits/Stats::total_CMT_queries);
		xmlwriter.Write_attribute_string_inline(attr, val);
		
		attr = "Total_GC_Executions";
		val = std::to_string(Stats::Total_gc_executions);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Average_Page_Movement_For_GC";
		val = std::to_string(double(Stats::Total_page_movements_for_gc) / double(Stats::Total_gc_executions));
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Total_WL_Executions";
		val = std::to_string(Stats::Total_wl_executions);
		xmlwriter.Write_attribute_string_inline(attr, val);

		attr = "Average_Page_Movement_For_WL";
		val = std::to_string(double(Stats::Total_page_movements_for_wl) / double(Stats::Total_wl_executions));
		xmlwriter.Write_attribute_string_inline(attr, val);


		attr = "CMT_size";
		val = std::to_string(Stats::CMTsize);
		xmlwriter.Write_attribute_string_inline(attr, val);


		attr = "stage1_end";
		val = std::to_string(Stats::stage1_end_count);
		xmlwriter.Write_attribute_string_inline(attr, val);\

		attr = "stage2_end";
		val = std::to_string(Stats::stage2_end_count);
		xmlwriter.Write_attribute_string_inline(attr, val);
		//******

		xmlwriter.Write_end_element_tag();
	}

	void FTL::Start_simulation()
	{
	}

	void FTL::Execute_simulator_event(MQSimEngine::Sim_Event*)
	{
	}

	LPA_type FTL::Convert_host_logical_address_to_device_address(LHA_type lha)
	{
		return lha / page_size_in_sectors;
	}

	page_status_type FTL::Find_NVM_subunit_access_bitmap(LHA_type lha)
	{
		return ((page_status_type)~(0xffffffffffffffff << (int)1)) << (int)(lha % page_size_in_sectors);
	}
}
