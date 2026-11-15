#include "IO_Flow_Trace_Based.h"
#include "../sim/Engine.h"
#include "../ssd/Stats.h"
#include <algorithm>
#include <cstdlib>
#include <climits>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Host_Components
{
namespace {
uint64_t Parse_unsigned(const std::string& value, const std::string& context)
{
    if (value.empty()) throw std::runtime_error(context + ": expected unsigned decimal integer");
    uint64_t result = 0;
    for (char ch : value) {
        if (ch < '0' || ch > '9' || result > (UINT64_MAX - (ch - '0')) / 10)
            throw std::runtime_error(context + ": invalid or overflowing unsigned decimal integer: " + value);
        result = result * 10 + (ch - '0');
    }
    return result;
}
sim_time_type To_nanoseconds(uint64_t relative_timestamp, Trace_Time_Unit unit, const std::string& context)
{
    switch (unit) {
    case Trace_Time_Unit::NANOSECOND:
        return relative_timestamp;
    case Trace_Time_Unit::MICROSECOND:
        if (relative_timestamp > (MAXIMUM_TIME - 1) / 1000)
            throw std::runtime_error(context + ": microsecond timestamp overflows simulation nanoseconds");
        return relative_timestamp * 1000;
    case Trace_Time_Unit::PICOSECOND:
        if (relative_timestamp % 1000 != 0)
            throw std::runtime_error(context + ": picosecond interval is not exactly representable in nanoseconds");
        return relative_timestamp / 1000;
    default:
        throw std::runtime_error(context + ": unsupported trace time unit");
    }
}
}

IO_Flow_Trace_Based::IO_Flow_Trace_Based(const sim_object_id_type& name, uint16_t flow_id,
    LHA_type start_lsa_on_device, LHA_type end_lsa_on_device, uint16_t io_queue_id,
    uint16_t nvme_submission_queue_size, uint16_t nvme_completion_queue_size,
    IO_Flow_Priority_Class::Priority priority_class, double initial_occupancy_ratio,
    std::string trace_file_path, Trace_Time_Unit time_unit, unsigned int total_replay_count,
    unsigned int percentage_to_be_simulated, HostInterface_Types SSD_device_type,
    PCIe_Root_Complex* pcie_root_complex, SATA_HBA* sata_hba, bool enabled_logging,
    sim_time_type logging_period, std::string logging_file_path)
    : IO_Flow_Base(name, flow_id, start_lsa_on_device, end_lsa_on_device, io_queue_id,
        nvme_submission_queue_size, nvme_completion_queue_size, priority_class, 0,
        initial_occupancy_ratio, 0, SSD_device_type, pcie_root_complex, sata_hba,
        enabled_logging, logging_period, logging_file_path),
      time_unit(time_unit), percentage_to_be_simulated(percentage_to_be_simulated),
      trace_file_path(trace_file_path), total_replay_no(total_replay_count)
{
    if (total_replay_count != 1)
        throw std::runtime_error("TARF requires Relay_Count=1; repeated traces must be explicitly prepared as input");
    if (percentage_to_be_simulated == 0 || percentage_to_be_simulated > 100)
        throw std::runtime_error("Percentage_To_Be_Executed must be in [1,100]");
    if (const char* value = std::getenv("TARF_HORIZON_NS")) {
        horizon_ns = Parse_unsigned(value, "TARF_HORIZON_NS");
        if (horizon_ns == 0 || horizon_ns == MAXIMUM_TIME)
            throw std::runtime_error("TARF_HORIZON_NS must be positive and smaller than UINT64_MAX");
    }
    if (const char* normalized = std::getenv("TARF_TRACE_NORMALIZED")) {
        const std::string value(normalized);
        if (value != "0" && value != "1") throw std::runtime_error("TARF_TRACE_NORMALIZED must be 0 or 1");
        trace_normalized = value == "1";
    }
    To_nanoseconds(0, time_unit, trace_file_path);
}

IO_Flow_Trace_Based::~IO_Flow_Trace_Based() {}

void IO_Flow_Trace_Based::Load_and_validate_trace()
{
    if (trace_loaded) return;
    if (end_lsa_on_device < start_lsa_on_device)
        throw std::runtime_error("Invalid logical-address range for " + ID());
    std::ifstream input(trace_file_path);
    if (!input) throw std::runtime_error("Cannot open trace file: " + trace_file_path);
    std::string line;
    uint64_t previous_timestamp = 0;
    uint64_t line_number = 0;
    const uint64_t max_relative_lba = end_lsa_on_device - start_lsa_on_device;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string context = trace_file_path + ":" + std::to_string(line_number);
        std::istringstream fields(line);
        std::string tokens[5], extra;
        for (unsigned int i = 0; i < 5; ++i)
            if (!(fields >> tokens[i])) throw std::runtime_error(context + ": expected exactly five columns");
        if (fields >> extra) throw std::runtime_error(context + ": expected exactly five columns");
        uint64_t values[5];
        for (unsigned int i = 0; i < 5; ++i) values[i] = Parse_unsigned(tokens[i], context);
        if (values[4] > 1) throw std::runtime_error(context + ": operation must be 0 (write) or 1 (read)");
        if (values[3] == 0 || values[3] > std::numeric_limits<unsigned int>::max())
            throw std::runtime_error(context + ": sector count is zero or exceeds host request representation");
        // MQSim's NVMe command path stores the sector count directly in uint16_t.
        if (SSD_device_type == HostInterface_Types::NVME && values[3] > UINT16_MAX)
            throw std::runtime_error(context + ": sector count exceeds MQSim NVMe command capacity (65535)");
        if (values[2] > max_relative_lba || values[3] - 1 > max_relative_lba - values[2])
            throw std::runtime_error(context + ": request exceeds the flow logical-address range; address folding is disabled");
        if (line_number == 1) {
            first_source_timestamp = values[0];
            origin_timestamp = trace_normalized ? 0 : values[0];
        }
        else if (values[0] < previous_timestamp)
            throw std::runtime_error(context + ": request timestamps must be nondecreasing");
        previous_timestamp = values[0];
        source_duration_ns = To_nanoseconds(values[0] - first_source_timestamp, time_unit, context);
        const sim_time_type arrival_ns = To_nanoseconds(values[0] - origin_timestamp, time_unit, context);
        if (arrival_ns == MAXIMUM_TIME)
            throw std::runtime_error(context + ": timestamp collides with the simulator invalid-time sentinel");
        if (arrival_ns < horizon_ns)
            requests.push_back({arrival_ns, start_lsa_on_device + values[2],
                static_cast<unsigned int>(values[3]), values[4] == 0});
    }
    if (input.bad()) throw std::runtime_error("I/O error reading trace file: " + trace_file_path);
    total_requests_in_file = line_number;
    if (total_requests_in_file == 0) throw std::runtime_error("Trace file is empty: " + trace_file_path);
    const uint64_t percentage_limit = total_requests_in_file / 100 * percentage_to_be_simulated
        + total_requests_in_file % 100 * percentage_to_be_simulated / 100;
    if (requests.size() > percentage_limit) requests.resize(static_cast<size_t>(percentage_limit));
    if (requests.empty()) throw std::runtime_error("Trace selection contains no requests: " + trace_file_path);
    if (requests.size() > UINT_MAX)
        throw std::runtime_error("Trace prefix exceeds MQSim preconditioning request-count representation");
    total_requests_to_be_generated = requests.size();
    SSD_Components::Stats::total_request_in_file = total_requests_in_file;
    trace_loaded = true;
}

Host_IO_Request* IO_Flow_Trace_Based::Generate_next_request()
{
    Load_and_validate_trace();
    if (STAT_generated_request_count >= requests.size()) return NULL;
    const Trace_Request& input = requests[STAT_generated_request_count];
    Host_IO_Request* request = new Host_IO_Request{};
    request->Type = input.is_write ? Host_IO_Request_Type::WRITE : Host_IO_Request_Type::READ;
    request->LBA_count = input.sector_count;
    request->Start_LBA = input.start_lba;
    request->Arrival_time = input.arrival_ns;
    if (input.is_write) ++STAT_generated_write_request_count;
    else ++STAT_generated_read_request_count;
    ++STAT_generated_request_count;
    return request;
}

void IO_Flow_Trace_Based::NVMe_consume_io_request(Completion_Queue_Entry* request)
{
    IO_Flow_Base::NVMe_consume_io_request(request);
    IO_Flow_Base::NVMe_update_and_submit_completion_queue_tail();
}
void IO_Flow_Trace_Based::SATA_consume_io_request(Host_IO_Request* request)
{
    IO_Flow_Base::SATA_consume_io_request(request);
}
void IO_Flow_Trace_Based::Validate_simulation_config() { Load_and_validate_trace(); }
void IO_Flow_Trace_Based::Start_simulation()
{
    IO_Flow_Base::Start_simulation();
    Load_and_validate_trace();
    Simulator->Register_sim_event(requests.front().arrival_ns, this);
}
void IO_Flow_Trace_Based::Execute_simulator_event(MQSimEngine::Sim_Event*)
{
    Host_IO_Request* request = Generate_next_request();
    if (request) Submit_io_request(request);
    if (STAT_generated_request_count < requests.size())
        Simulator->Register_sim_event(requests[STAT_generated_request_count].arrival_ns, this);
}

void IO_Flow_Trace_Based::Get_statistics(Utils::Workload_Statistics& stats,
    LPA_type (*convert_address)(LHA_type), page_status_type (*access_bitmap)(LHA_type))
{
    Load_and_validate_trace();
    stats = Utils::Workload_Statistics{};
    stats.Type = Utils::Workload_Type::TRACE_BASED;
    stats.Stream_id = io_queue_id - 1;
    stats.Min_LHA = start_lsa_on_device;
    stats.Max_LHA = end_lsa_on_device;
    stats.Write_arrival_time.assign(MAX_ARRIVAL_TIME_HISTOGRAM + 1, 0);
    stats.Read_arrival_time.assign(MAX_ARRIVAL_TIME_HISTOGRAM + 1, 0);
    stats.Write_size_histogram.assign(MAX_REQSIZE_HISTOGRAM_ITEMS + 1, 0);
    stats.Read_size_histogram.assign(MAX_REQSIZE_HISTOGRAM_ITEMS + 1, 0);
    uint64_t total_sectors = 0;
    sim_time_type previous_ns = 0;
    for (const Trace_Request& request : requests) {
        const auto interval_bin = std::min<sim_time_type>((request.arrival_ns - previous_ns) / 1000,
            MAX_ARRIVAL_TIME_HISTOGRAM);
        previous_ns = request.arrival_ns;
        const auto size_bin = std::min<unsigned int>(request.sector_count, MAX_REQSIZE_HISTOGRAM_ITEMS);
        if (request.is_write) {
            ++stats.Write_arrival_time[interval_bin];
            ++stats.Write_size_histogram[size_bin];
        } else {
            ++stats.Read_arrival_time[interval_bin];
            ++stats.Read_size_histogram[size_bin];
        }
        total_sectors += request.sector_count;
        if (total_sectors > UINT_MAX)
            throw std::runtime_error("Trace prefix exceeds MQSim preconditioning accessed-sector count representation");
        for (uint64_t offset = 0; offset < request.sector_count; ++offset) {
            const LHA_type address = request.start_lba + offset;
            const LPA_type page = convert_address(address);
            auto& own_map = request.is_write ? stats.Write_address_access_pattern : stats.Read_address_access_pattern;
            const auto& other_map = request.is_write ? stats.Read_address_access_pattern : stats.Write_address_access_pattern;
            auto found = own_map.find(page);
            if (found == own_map.end()) own_map[page] = {1, access_bitmap(address)};
            else {
                if (found->second.Access_count == INT_MAX)
                    throw std::runtime_error("Preconditioning address access counter overflow");
                ++found->second.Access_count;
                found->second.Accessed_sub_units |= access_bitmap(address);
            }
            if (other_map.count(page)) stats.Write_read_shared_addresses.insert(page);
        }
    }
    stats.Total_generated_requests = static_cast<unsigned int>(requests.size());
    stats.Total_accessed_lbas = static_cast<unsigned int>(total_sectors);
    stats.Average_request_size_sector = static_cast<unsigned int>(total_sectors / requests.size());
    stats.Average_inter_arrival_time_nano_sec = requests.back().arrival_ns / requests.size();
    stats.Initial_occupancy_ratio = initial_occupancy_ratio;
    stats.Replay_no = 1;
}

void IO_Flow_Trace_Based::Report_input_in_JSON(std::ostream& output) const
{
    output << "{\"kind\":\"trace\",\"trace_path\":";
    Write_JSON_string(output, trace_file_path);
    output << ",\"time_unit\":\"" << (time_unit == Trace_Time_Unit::NANOSECOND ? "NANOSECOND" :
        time_unit == Trace_Time_Unit::MICROSECOND ? "MICROSECOND" : "PICOSECOND")
        << "\",\"origin_timestamp\":" << origin_timestamp
        << ",\"first_source_timestamp\":" << first_source_timestamp
        << ",\"timestamp_origin_mode\":\"" << (trace_normalized ? "already_relative_to_simulation_zero" : "subtract_first_timestamp") << "\""
        << ",\"source_request_count\":" << total_requests_in_file
        << ",\"selected_request_count\":" << requests.size()
        << ",\"source_duration_ns\":" << source_duration_ns
        << ",\"selected_last_arrival_ns\":" << (requests.empty() ? 0 : requests.back().arrival_ns)
        << ",\"horizon_ns\":";
    if (horizon_ns == MAXIMUM_TIME) output << "null"; else output << horizon_ns;
    output << ",\"percentage\":" << percentage_to_be_simulated
        << ",\"replay_count\":1,\"address_transform\":\"flow_offset_only\"}";
}
} // namespace Host_Components
