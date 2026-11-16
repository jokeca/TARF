#ifndef TARF_REFRESH_CONTROLLER_H
#define TARF_REFRESH_CONTROLLER_H
#include <cstdint>
#include <vector>
#include <fstream>
#include <map>
#include <limits>
#include "../sim/Sim_Object.h"
#include "Reliability.h"
#include "GC_and_WL_Unit_Page_Level.h"

namespace TARF {
// Policy decisions are separate from the shared, real NAND migration path.
class RefreshController : public MQSimEngine::Sim_Object {
public:
    RefreshController(const sim_object_id_type&, SSD_Components::GC_and_WL_Unit_Page_Level*);
    ~RefreshController();
    void Setup_triggers() override;
    void Validate_simulation_config() override;
    void Start_simulation() override;
    void Execute_simulator_event(MQSimEngine::Sim_Event*) override;
    static void Observe(SSD_Components::NVM_Transaction_Flash*);
    static void WriteReport();
    static double GCBenefit(const Address&);
    static void GCSelected(const Address&);
    static bool OwnsRefreshBlock(const Address&);
    static void WriteSubmitted(SSD_Components::NVM_Transaction_Flash*);
private:
    struct Episode { bool above = false; uint64_t since = 0; };
    struct Candidate { Address source, destination; double score; };
    struct Migration {
        Address source, destination;
        unsigned next_page = 0, source_used_pages = 0, outstanding = 0;
        bool erase_submitted = false;
    };
    static RefreshController* instance_;
    SSD_Components::GC_and_WL_Unit_Page_Level* gc_;
    std::vector<Episode> episodes_;
    std::vector<Candidate> queue_;
    uint64_t pump_time_ = std::numeric_limits<uint64_t>::max();
    std::map<SSD_Components::NVM_Transaction_Flash_ER*, Migration> active_;
    std::vector<unsigned> chip_pages_, chip_blocks_;
    std::vector<std::vector<double>> serial_cost_prefix_;
    std::vector<uint64_t> chip_wait_since_;
    std::vector<uint64_t> chip_last_foreground_completion_;
    std::vector<bool> chip_waiting_;
    std::ofstream audit_;
    std::ofstream risk_episodes_;
    std::ofstream events_;
    std::ofstream timeline_;
    uint64_t event_rows_ = 0;
    uint64_t audited_pages_ = 0;
    uint64_t last_sample_ = 0, previous_risky_ = 0, previous_eligible_ = 0;
    uint64_t initial_live_ = 0, initial_risky_ = 0, initial_eligible_ = 0;
    uint64_t risky_ = 0, eligible_ = 0, pending_ = 0, inflight_ = 0;
    uint64_t refresh_started_ = 0, refresh_completed_ = 0, deferred_ = 0;
    uint64_t refresh_reads_ = 0, refresh_writes_ = 0, refresh_erases_ = 0;
    uint64_t gc_reads_ = 0, gc_writes_ = 0, gc_erases_ = 0, gc_shared_ = 0;
    uint64_t foreground_writes_ = 0, all_program_bytes_ = 0, refresh_program_bytes_ = 0;
    uint64_t all_read_bytes_ = 0, foreground_read_bytes_ = 0, mapping_read_bytes_ = 0, mapping_program_bytes_ = 0;
    uint64_t refresh_read_bytes_submitted_ = 0, refresh_read_bytes_ = 0, gc_read_bytes_ = 0;
    uint64_t sealed_unused_pages_ = 0;
    uint64_t partial_frontiers_retired_ = 0, source_tail_sealed_pages_ = 0;
    uint64_t reads_submitted_ = 0, writes_submitted_ = 0, erases_submitted_ = 0;
    uint64_t max_outstanding_pages_ = 0, max_chip_pages_ = 0, skipped_invalid_ = 0;
    uint64_t forced_admissions_ = 0, max_page_hold_ns_ = 0;
    uint64_t quiet_deferred_attempts_ = 0;
    uint64_t reserve_escape_admissions_ = 0;
    long double page_hold_ns_ = 0;
    long double risky_ns_ = 0, eligible_ns_ = 0, ratio_ns_ = 0;
    uint64_t max_wait_ns_ = 0, completed_episodes_ = 0;
    bool sampled_ = false;
    bool measurement_captured_ = false;
    void CaptureMeasurementStart(uint64_t now);
    void Sample(uint64_t now, bool admit);
    void Pump();
    void Advance();
    void SchedulePump(uint64_t delay_ns);
    bool ResourceReady(const Address&);
    unsigned ChipIndex(const Address&) const;
    bool Trigger(const Address&, unsigned pe, uint64_t now) const;
    bool SafeSource(const Address&) const;
    bool Destination(const Address&, Address&, double& benefit) const;
    bool Admit(const Candidate&);
    bool SpaceReady(const Address&) const;
    void Migrate(const Candidate&);
    double Cost(const Address&, uint64_t valid_pages) const;
    void Report();
    void RecordEpisode(const Address&, uint64_t start, uint64_t end, bool censored);
    void RecordEvent(const char*, const Address& source, const Address& destination);
};
}
#endif
