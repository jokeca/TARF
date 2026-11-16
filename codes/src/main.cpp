#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "ssd/SSD_Defs.h"
#include "ssd/Reliability.h"
#include "ssd/Refresh_Controller.h"
#include "exec/Execution_Parameter_Set.h"
#include "exec/SSD_Device.h"
#include "exec/Host_System.h"
#include "utils/rapidxml/rapidxml.hpp"

namespace {
std::vector<char> Read_XML(const std::string& path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open configuration file: " + path);
    std::vector<char> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("I/O error reading configuration file: " + path);
    if (data.empty()) throw std::runtime_error("Empty configuration file: " + path);
    data.push_back('\0');
    return data;
}
uint64_t Unsigned_value(const std::string& value, const std::string& name)
{
    if (value.empty()) throw std::runtime_error("Missing integer value: " + name);
    uint64_t result = 0;
    for (char ch : value) {
        if (ch < '0' || ch > '9' || result > (UINT64_MAX - (ch - '0')) / 10)
            throw std::runtime_error("Invalid unsigned integer for " + name + ": " + value);
        result = result * 10 + ch - '0';
    }
    return result;
}
std::string Required(rapidxml::xml_node<>* node, const char* name)
{
    auto* child = node->first_node(name);
    if (!child || !child->value()[0]) throw std::runtime_error(std::string("Required XML field is missing: ") + name);
    if (child->next_sibling(name)) throw std::runtime_error(std::string("Duplicate XML field: ") + name);
    return child->value();
}
std::string Directory(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}
std::string Stem(const std::string& path)
{
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of('/');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(0, dot) : path;
}
void Make_output_parent(const std::string& prefix)
{
    if (prefix.empty() || prefix.back() == '/') throw std::runtime_error("TARF_OUTPUT_PREFIX must include a filename prefix");
    const std::string parent = Directory(prefix);
    for (size_t end = 1; end <= parent.size(); ++end) {
        if (end != parent.size() && parent[end] != '/') continue;
        const std::string part = parent.substr(0, end);
        if (part.empty()) continue;
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
            throw std::runtime_error("Cannot create output directory " + part + ": " + std::strerror(errno));
        struct stat info;
        if (stat(part.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            throw std::runtime_error("Output parent is not a directory: " + part);
    }
}
void Read_configuration(const std::string& path, Execution_Parameter_Set& parameters)
{
    auto data = Read_XML(path);
    rapidxml::xml_document<> document;
    document.parse<0>(data.data());
    auto* root = document.first_node("Execution_Parameter_Set");
    if (!root) throw std::runtime_error("Missing Execution_Parameter_Set in " + path);
    auto* device = root->first_node("Device_Parameter_Set");
    if (!device) throw std::runtime_error("Missing Device_Parameter_Set in " + path);
    const uint64_t queue_depth = Unsigned_value(Required(device, "IO_Queue_Depth"), "IO_Queue_Depth");
    if (queue_depth < 2 || queue_depth > UINT16_MAX)
        throw std::runtime_error("IO_Queue_Depth must be in [2,65535]; silent uint16 truncation is disabled");
    parameters.XML_deserialize(root);
}
std::vector<IO_Flow_Parameter_Set*> Read_workload(const std::string& path)
{
    auto data = Read_XML(path);
    rapidxml::xml_document<> document;
    document.parse<0>(data.data());
    auto* root = document.first_node("MQSim_IO_Scenarios");
    if (!root) throw std::runtime_error("Missing MQSim_IO_Scenarios in " + path);
    auto* scenario = root->first_node("IO_Scenario");
    if (!scenario || scenario->next_sibling("IO_Scenario"))
        throw std::runtime_error("TARF requires exactly one IO_Scenario per process to isolate seeds and state");
    std::vector<IO_Flow_Parameter_Set*> flows;
    for (auto* node = scenario->first_node(); node; node = node->next_sibling()) {
        if (node->type() != rapidxml::node_element) continue;
        if (std::string(node->name()) != "IO_Flow_Parameter_Set_Trace_Based")
            throw std::runtime_error("TARF experiment runner accepts explicit trace-based flows only");
        std::unique_ptr<IO_Flow_Parameter_Set_Trace_Based> flow(new IO_Flow_Parameter_Set_Trace_Based);
        const char* required_common[] = {"Priority_Class", "Device_Level_Data_Caching_Mode", "Channel_IDs",
            "Chip_IDs", "Die_IDs", "Plane_IDs", "Initial_Occupancy_Percentage"};
        for (const char* field : required_common) Required(node, field);
        flow->IO_Flow_Parameter_Set::XML_deserialize(node);
        flow->File_Path = Required(node, "File_Path");
        // Relative trace paths are resolved against the workload XML's directory.
        if (flow->File_Path.front() != '/') flow->File_Path = Directory(path) + "/" + flow->File_Path;
        const uint64_t percentage = Unsigned_value(Required(node, "Percentage_To_Be_Executed"), "Percentage_To_Be_Executed");
        const uint64_t replay = Unsigned_value(Required(node, "Relay_Count"), "Relay_Count");
        if (percentage < 1 || percentage > 100) throw std::runtime_error("Percentage_To_Be_Executed must be in [1,100]");
        if (replay != 1) throw std::runtime_error("TARF requires Relay_Count=1");
        flow->Percentage_To_Be_Executed = static_cast<int>(percentage);
        flow->Relay_Count = 1;
        flow->Total_requests_to_server = 0;
        flow->Choose_type = 0;
        std::string unit = Required(node, "Time_Unit");
        std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) { return std::toupper(ch); });
        if (unit == "NANOSECOND" || unit == "NS") flow->Time_Unit = Trace_Time_Unit::NANOSECOND;
        else if (unit == "MICROSECOND" || unit == "US") flow->Time_Unit = Trace_Time_Unit::MICROSECOND;
        else if (unit == "PICOSECOND" || unit == "PS") flow->Time_Unit = Trace_Time_Unit::PICOSECOND;
        else throw std::runtime_error("Time_Unit must explicitly name nanoseconds, microseconds, or picoseconds");
        flows.push_back(flow.release());
    }
    if (flows.empty()) throw std::runtime_error("IO_Scenario contains no trace-based flow");
    // Legacy lifetime/request-count stopping must not alter a policy's arrival prefix.
    SSD_Components::Stats::choose_type = 0;
    SSD_Components::Stats::turn_on = 0;
    return flows;
}
void Collect_XML(SSD_Device& ssd, Host_System& host, const std::string& output)
{
    Utils::XmlWriter xml;
    if (!xml.Open(output)) throw std::runtime_error("Cannot open XML output: " + output);
    xml.Write_open_tag("MQSim_Results");
    host.Report_results_in_XML("", xml);
    ssd.Report_results_in_XML("", xml);
    xml.Write_close_tag();
    xml.Close();
}
}

int main(int argc, char* argv[])
{
    try {
        // Legacy parser errors may call cin.get(); this runner is strictly noninteractive.
        std::cin.setstate(std::ios::eofbit);
        if (argc != 5) throw std::runtime_error("Usage: ./MQSim -i path/to/ssd.xml -w path/to/workload.xml");
        std::string configuration_path, workload_path;
        for (int argument = 1; argument < argc; argument += 2) {
            const std::string option = argv[argument];
            if (option == "-i" && configuration_path.empty()) configuration_path = argv[argument + 1];
            else if (option == "-w" && workload_path.empty()) workload_path = argv[argument + 1];
            else throw std::runtime_error("Unknown or repeated command-line option: " + option);
        }
        if (configuration_path.empty() || workload_path.empty()) throw std::runtime_error("Both -i and -w are required");
        const char* environment_prefix = std::getenv("TARF_OUTPUT_PREFIX");
        const std::string output_prefix = environment_prefix ? environment_prefix : Stem(workload_path);
        Make_output_parent(output_prefix);
        if (setenv("TARF_OUTPUT_PREFIX", output_prefix.c_str(), 1) != 0)
            throw std::runtime_error("Cannot set output prefix for TARF model");
        Execution_Parameter_Set parameters;
        Read_configuration(configuration_path, parameters);
        parameters.Host_Configuration.IO_Flow_Definitions = Read_workload(workload_path);
        parameters.Host_Configuration.Input_file_path = output_prefix;
        Simulator->Reset();
        // Load the model and initial-state contract before constructing the SSD.
        TARF::Load_from_environment();
        if (!std::getenv("TARF_HORIZON_NS")) {
            const std::string horizon = std::to_string(TARF::Config::Get().horizon_ns);
            if (setenv("TARF_HORIZON_NS", horizon.c_str(), 1) != 0)
                throw std::runtime_error("Cannot propagate TARF horizon to host trace flows");
        }
        const auto start = std::chrono::steady_clock::now();
        SSD_Device ssd(&parameters.SSD_Device_Configuration, &parameters.Host_Configuration.IO_Flow_Definitions);
        Host_System host(&parameters.Host_Configuration, parameters.SSD_Device_Configuration.Enabled_Preconditioning, ssd.Host_interface);
        host.Attach_ssd_device(&ssd);
        Simulator->Start_simulation(); // The reliability controller owns the common horizon stop event.
        SSD_Components::Stats::Total_Simulation_Time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        TARF::RefreshController::WriteReport();
        const auto flows = host.Get_io_flows();
        uint64_t total_completed = 0;
        for (auto* flow : flows) total_completed += flow->Get_serviced_request_count();
        SSD_Components::Stats::Total_Requests_Serviced = total_completed;
        Host_Components::IO_Flow_Base::Report_results_in_JSON(flows, output_prefix + "_host.json");
        TARF::Reliability::Get().WriteConfigurationFile(output_prefix + "_model.json");
        Collect_XML(ssd, host, output_prefix + ".xml");
        std::cout << "Simulation complete at " << Simulator->Time() << " ns; results: " << output_prefix << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TARF error: " << error.what() << '\n';
    } catch (const char* error) {
        std::cerr << "TARF error: " << error << '\n';
    } catch (const std::string& error) {
        std::cerr << "TARF error: " << error << '\n';
    } catch (...) {
        std::cerr << "TARF error: unknown exception\n";
    }
    return 1;
}
