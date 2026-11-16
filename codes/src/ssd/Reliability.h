#ifndef TARF_RELIABILITY_H
#define TARF_RELIABILITY_H

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>
#include "../nvm_chip/flash_memory/Physical_Page_Address.h"
#include "../sim/Sim_Defs.h"

namespace TARF {
using Address = NVM::FlashMemory::Physical_Page_Address;
struct RetentionParameters {
    double chip_bias = 0, t0days = 365, delta = 0.1, gamma = 0.5;
};
struct InitialBlockState {
    unsigned pe = 0;
    double age_days = 0, program_temp_c = 30;
};
struct Config {
    std::string policy = "none", temperature_file, model_file, initial_state_file, initial_read_coverage_file, output_prefix = "tarf";
    bool reject_lazy_initial_reads = false;
    uint64_t horizon_ns = 10000000000ULL, scan_ns = 1000000000ULL;
    // Explicit, common-policy workload pre-run. Zero preserves legacy behavior.
    uint64_t measurement_start_ns = 0;
    unsigned initial_pe = 0;
    double initial_age_days = 0, initial_temp_c = 30;
    unsigned initial_pe_min = 0, initial_pe_max = 0;
    double initial_age_days_min = 0, initial_age_days_max = 0;
    double initial_temp_c_min = 30, initial_temp_c_max = 30;
    std::vector<InitialBlockState> initial_blocks;
    unsigned refresh_max_pages_per_chip = 2, refresh_max_blocks_per_chip = 1;
    uint64_t refresh_max_defer_ns = 100000000ULL;
    // Separate admission from the shared TSU progress deadline. Keeping them
    // coupled makes a refresh-policy sweep silently change natural-GC priority.
    uint64_t refresh_admission_max_defer_ns = 100000000ULL;
    uint64_t refresh_quiet_ns = 0;
    // Optional TARF progress rule: after this observed risk wait, a closed
    // source may temporarily borrow one block at the soft GC reserve boundary.
    // Zero retains legacy admission. The hard reserve is never borrowed.
    uint64_t refresh_reserve_escape_ns = 0;
    std::string selection_rank = "benefit_cost";
    double threshold = 20, risk_threshold = 20, period_days = 30;
    bool resource_aware = true, resource_aware_baselines = false, gc_coordination = true, calibrated = false;
    bool gc_respect_configured_threshold = false;
    double beta0 = -3, beta_pe = 0.3, beta_diff = 2, beta_common = -1;
    RetentionParameters retention;
    std::map<unsigned, RetentionParameters> chip_parameters;
    std::string provenance = "synthetic-regression-only; no device measurements supplied";
    std::vector<std::pair<uint64_t, double>> temperatures;
    static const Config& Get();
};
void Load_from_environment();

struct PageState {
    uint64_t program_ns = 0, generation = 0;
    double initial_age_days = 0, program_temp_c = 30;
    LPA_type lpa = NO_LPA;
    stream_id_type stream = NO_STREAM;
    bool live = false, scheduled = false;
};

class Reliability {
public:
    static Reliability& Get();
    void InitializeGeometry(unsigned channels, unsigned chips, unsigned dies,
                            unsigned planes, unsigned blocks, unsigned pages);
    // Initial data is an explicit t=0 snapshot, including lazily materialized reads.
    void InitializePage(const Address&, stream_id_type, LPA_type, uint64_t origin_ns = 0);
    void OnProgramScheduled(const Address&, stream_id_type, LPA_type);
    bool OnProgramComplete(const Address&, stream_id_type, LPA_type, uint64_t now_ns);
    void OnErase(const Address&);
    double TemperatureC(uint64_t now_ns) const;
    double PredictNFBC(unsigned pe, double age_days, double program_c, double read_c,
                       unsigned chip = 0, const std::string& variant = "full") const;
    double PageRisk(const Address&, unsigned pe, uint64_t now_ns,
                    const std::string& variant = "full") const;
    double BlockRisk(const Address&, unsigned pe, uint64_t now_ns,
                     const std::string& variant = "full") const;
    double BlockMaxAgeDays(const Address&, uint64_t now_ns) const;
    double BlockMaxTemperatureDifference(const Address&, uint64_t now_ns) const;
    uint64_t LivePageCount(const Address&) const;
    const PageState& State(const Address&) const;
    size_t BlockCount() const;
    Address BlockAddress(size_t index) const;
    unsigned PagesPerBlock() const { return pages_; }
    const InitialBlockState& InitialState(const Address&) const;
    uint64_t BlockStateDigest(size_t block_index) const;
    uint64_t initialized_pages = 0, lazy_initial_pages = 0, completed_programs = 0;
    uint64_t stale_program_completions = 0, erased_live_pages = 0;
    void WriteConfiguration(std::ostream&) const;
    void WriteConfigurationFile(const std::string&) const;
private:
    struct LogicalState {
        uint64_t latest_generation = 0, committed_generation = 0;
        size_t live_index = static_cast<size_t>(-1);
    };
    // At fixed P/E, chip and programming temperature, risk is monotone in age.
    // A max-heap retains the exact oldest live page for each temperature group.
    // Offsets use four bytes per initialized/programmed page, not tree nodes.
    struct TemperatureGroup {
        double program_temp_c = 30;
        std::vector<uint32_t> oldest;
    };
    struct BlockSummary {
        uint32_t live_count = 0;
        std::vector<TemperatureGroup> groups;
        uint64_t cached_ns = 0;
        unsigned cached_pe = 0, cached_mask = 0;
        double cached_risk[3] = {0, 0, 0};
    };
    unsigned channels_ = 0, chips_ = 0, dies_ = 0, planes_ = 0, blocks_ = 0, pages_ = 0;
    std::vector<PageState> states_;
    mutable std::vector<BlockSummary> summaries_;
    InitialBlockState uniform_initial_state_;
    std::map<stream_id_type, std::vector<LogicalState>> logical_;
    size_t Index(const Address&) const;
    LogicalState& Logical(stream_id_type, LPA_type);
    double AgeDays(const PageState&, uint64_t now_ns) const;
    void AddLive(size_t index);
    void RemoveLive(size_t index);
    bool AgeLess(size_t base, uint32_t a, uint32_t b) const;
    size_t OldestLive(size_t block, TemperatureGroup&) const;
};
}
#endif
