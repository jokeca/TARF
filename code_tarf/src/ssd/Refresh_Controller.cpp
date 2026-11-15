#include "Refresh_Controller.h"
#include "Flash_Block_Manager.h"
#include "NVM_Transaction_Flash_RD.h"
#include "NVM_Transaction_Flash_WR.h"
#include "NVM_Transaction_Flash_ER.h"
#include "TSU_Priority_OutOfOrder.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace TARF {
using namespace SSD_Components;
RefreshController* RefreshController::instance_ = nullptr;
RefreshController::RefreshController(const sim_object_id_type& id, GC_and_WL_Unit_Page_Level* gc)
    : Sim_Object(id), gc_(gc) { instance_ = this; }
RefreshController::~RefreshController() { if (instance_ == this) instance_ = nullptr; }
void RefreshController::Setup_triggers() { Sim_Object::Setup_triggers(); }
void RefreshController::Validate_simulation_config() {
    if (gc_->use_copyback) throw std::runtime_error("TARF experiments require explicit NAND read/program, copyback=false");
    if (!dynamic_cast<TSU_Priority_OutOfOrder*>(gc_->tsu))
        throw std::runtime_error("TARF controller currently requires PRIORITY_OUT_OF_ORDER scheduling");
}
void RefreshController::Start_simulation() {
    const auto& cfg = Config::Get();
    const unsigned chips = gc_->channel_count * gc_->chip_no_per_channel;
    chip_pages_.assign(chips, 0); chip_blocks_.assign(chips, 0);
    chip_wait_since_.assign(chips, 0); chip_waiting_.assign(chips, false);
    chip_last_foreground_completion_.assign(chips, 0);
    // NAND latency depends only on this chip's technology/configuration and page
    // type. Preserve the original serial sum, including its addition order.
    serial_cost_prefix_.assign(chips, std::vector<double>(gc_->pages_no_per_block + 1));
    for (unsigned channel = 0; channel < gc_->channel_count; ++channel)
        for (unsigned chip_id = 0; chip_id < gc_->chip_no_per_channel; ++chip_id) {
            auto* chip = gc_->GC_and_WL_Unit_Base::flash_controller->Get_chip(channel, chip_id);
            auto& prefix = serial_cost_prefix_[channel * gc_->chip_no_per_channel + chip_id];
            prefix[0] = chip->Get_command_execution_latency(CMD_ERASE_BLOCK, 0);
            for (unsigned page = 0; page < gc_->pages_no_per_block; ++page)
                prefix[page + 1] = prefix[page] + (chip->Get_command_execution_latency(CMD_READ_PAGE, page)
                    + chip->Get_command_execution_latency(CMD_PROGRAM_PAGE, page));
        }
    Simulator->Register_sim_event(0, this, nullptr, 0);
    Simulator->Register_sim_event(cfg.horizon_ns, this, nullptr, 2);
    audit_.open(cfg.output_prefix + "_migrations.csv");
    if (!audit_) throw std::runtime_error("Cannot open migration audit output");
    risk_episodes_.open(cfg.output_prefix + "_risk_episodes.csv");
    if (!risk_episodes_) throw std::runtime_error("Cannot open risk episode output");
    events_.open(cfg.output_prefix + "_refresh_events.csv");
    if (!events_) throw std::runtime_error("Cannot open refresh event audit output");
    events_ << "event,time_ns,channel,chip,die,plane,source_block,source_page,destination_block,destination_page\n";
    timeline_.open(cfg.output_prefix + "_background_timeline.csv");
    if (!timeline_) throw std::runtime_error("Cannot open background timeline output");
    timeline_ << "time_ns,temperature_c,live_pages,risky_blocks,eligible_blocks,pending_refresh_blocks,refresh_program_pages,gc_program_pages,foreground_program_pages,risky_block_seconds\n";
    risk_episodes_ << "channel,chip,die,plane,block,first_observed_above_ns,end_ns,observed_wait_ns,censored_at_horizon,scan_resolution_ns\n";
    audit_ << "completion_ns,source_channel,source_chip,source_die,source_plane,source_block,source_page,destination_block,destination_page,destination_pe,program_temp_c,program_ns,initial_age_days,source_age_before_days,source_live_after,destination_live\n";
}
void RefreshController::Execute_simulator_event(MQSimEngine::Sim_Event* ev) {
    const uint64_t now = Simulator->Time();
    if (ev->Type == 2) { Simulator->Stop_simulation(); return; }
    if (now >= Config::Get().horizon_ns) return;
    if (ev->Type == 1) {
        if (now != pump_time_) return;
        pump_time_ = std::numeric_limits<uint64_t>::max(); Pump(); return;
    }
    Sample(now, true);
    const uint64_t interval = Config::Get().scan_ns;
    uint64_t next = interval > Config::Get().horizon_ns - now ? Config::Get().horizon_ns : now + interval;
    if (now < Config::Get().measurement_start_ns)
        next = std::min(next, Config::Get().measurement_start_ns);
    // Temperature changes are sampled and considered at their exact declared times.
    for (const auto& p : Config::Get().temperatures) if (p.first > now && p.first < next) next = p.first;
    if (next < Config::Get().horizon_ns) Simulator->Register_sim_event(next, this, nullptr, 0);
}
bool RefreshController::Trigger(const Address& a, unsigned pe, uint64_t now) const {
    const auto& c = Config::Get();
    if (now < c.measurement_start_ns) return false;
    auto& r = Reliability::Get();
    if (c.policy == "none") return false;
    if (c.policy == "periodic") return r.BlockMaxAgeDays(a, now) >= c.period_days;
    return r.BlockRisk(a, pe, now, c.policy == "age" ? "age" : c.policy == "tempdiff" ? "tempdiff" : "full") > c.threshold;
}
bool RefreshController::SafeSource(const Address& a) const {
    auto* manager = gc_->block_manager;
    auto* plane = manager->Get_plane_bookkeeping_entry(a);
    const auto& block = plane->Blocks[a.BlockID];
    if (!block.Current_page_write_index || block.Holds_mapping_data) return false;
    const bool partial = block.Current_page_write_index < gc_->pages_no_per_block;
    if (partial && !manager->Is_partial_data_frontier(a)) return false;
    // This only permits considering the frontier. Migration atomically replaces
    // it before source protection or any NAND transaction is submitted.
    return gc_->is_safe_gc_wl_candidate(plane, a.BlockID, true, partial);
}
bool RefreshController::Destination(const Address& a, Address& destination, double& benefit) const {
    auto* plane = gc_->block_manager->Get_plane_bookkeeping_entry(a);
    auto& r = Reliability::Get();
    const auto& cfg = Config::Get();
    const double current = r.BlockRisk(a, plane->Blocks[a.BlockID].Erase_count, Simulator->Time());
    const double t = r.TemperatureC(Simulator->Time());
    double best = std::numeric_limits<double>::infinity();
    bool found = false;
    for (const auto& item : plane->Free_block_pool) {
        const auto* block = item.second;
        if (block->Current_page_write_index || block->Has_ongoing_gc_wl) continue;
        const double post = r.PredictNFBC(block->Erase_count, 0, t, t,
                                         a.ChannelID * gc_->chip_no_per_channel + a.ChipID);
        if (post < best || (post == best && (!found || block->BlockID < destination.BlockID))) {
            best = post; destination = a; destination.BlockID = block->BlockID; destination.PageID = 0; found = true;
        }
    }
    benefit = found ? std::max(0.0, current - best) : 0;
    return found && (cfg.policy != "tarf" || benefit > 0);
}
double RefreshController::Cost(const Address& a, uint64_t valid) const {
    auto* phy = gc_->GC_and_WL_Unit_Base::flash_controller;
    auto* chip = phy->Get_chip(a.ChannelID, a.ChipID);
    // Serial sum is a conservative completion-cost estimate, not simulated service time.
    double cost = serial_cost_prefix_.at(ChipIndex(a)).at(valid);
    const uint64_t finish = phy->Expected_finish_time(chip);
    if (phy->GetChipStatus(chip) != ChipStatus::IDLE && finish > Simulator->Time()) cost += finish - Simulator->Time();
    const auto* tsu = dynamic_cast<TSU_Priority_OutOfOrder*>(gc_->tsu);
    cost += tsu->Pending_user_transactions(a.ChannelID, a.ChipID)
          * chip->Get_command_execution_latency(CMD_READ_PAGE, 0);
    return std::max(1.0, cost);
}
void RefreshController::Sample(uint64_t now, bool admit) {
    auto& r = Reliability::Get();
    if (episodes_.empty()) episodes_.resize(r.BlockCount());
    if (sampled_) {
        const long double dt = now - last_sample_;
        risky_ns_ += dt * previous_risky_;
        eligible_ns_ += dt * previous_eligible_;
        ratio_ns_ += previous_eligible_ ? dt * previous_risky_ / previous_eligible_ : 0;
    }
    risky_ = eligible_ = pending_ = 0;
    if (admit) queue_.clear();
    uint64_t live = 0;
    for (size_t i = 0; i < r.BlockCount(); ++i) {
        Address a = r.BlockAddress(i);
        auto* plane = gc_->block_manager->Get_plane_bookkeeping_entry(a);
        auto& block = plane->Blocks[a.BlockID];
        const uint64_t pages = r.LivePageCount(a);
        live += pages;
        const bool eligible = pages && !block.Holds_mapping_data;
        if (eligible) ++eligible_;
        const bool above = eligible && r.BlockRisk(a, block.Erase_count, now) > Config::Get().risk_threshold;
        auto& episode = episodes_[i];
        if (above) {
            ++risky_;
            if (!episode.above) { episode.above = true; episode.since = now; }
            max_wait_ns_ = std::max(max_wait_ns_, now - episode.since);
        } else if (episode.above) {
            max_wait_ns_ = std::max(max_wait_ns_, now - episode.since);
            RecordEpisode(a, episode.since, now, false);
            ++completed_episodes_; episode.above = false;
        }
        if (!eligible || !Trigger(a, block.Erase_count, now)) continue;
        if (!block.Has_ongoing_gc_wl) ++pending_;
        if (!admit || block.Has_ongoing_gc_wl || !SafeSource(a)) continue;
        Candidate candidate; candidate.source = a;
        double benefit = 0;
        if (!Destination(a, candidate.destination, benefit)) continue;
        candidate.score = -static_cast<double>(i);
        if (Config::Get().policy == "tarf") {
            if (Config::Get().selection_rank == "benefit_cost") candidate.score = benefit / Cost(a, pages);
            else if (Config::Get().selection_rank == "risk") candidate.score = r.BlockRisk(a, block.Erase_count, now);
        }
        queue_.push_back(candidate);
    }
    if (!sampled_) { initial_live_ = live; initial_risky_ = risky_; initial_eligible_ = eligible_; }
    sampled_ = true; last_sample_ = now; previous_risky_ = risky_; previous_eligible_ = eligible_;
    // Uncapped scan-level cumulative counters permit full-horizon attribution;
    // the detailed page-event audit intentionally retains its existing cap.
    timeline_ << std::setprecision(17) << now << ',' << r.TemperatureC(now) << ',' << live << ',' << risky_ << ','
        << eligible_ << ',' << pending_ << ',' << refresh_writes_ << ',' << gc_writes_ << ',' << foreground_writes_
        << ',' << double(risky_ns_ / 1e9L) << '\n';
    if (!measurement_captured_ && Config::Get().measurement_start_ns && now == Config::Get().measurement_start_ns)
        CaptureMeasurementStart(now);
    if (admit) {
        std::stable_sort(queue_.begin(), queue_.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        Pump();
    }
}
bool RefreshController::SpaceReady(const Address& a) const {
    const auto& cfg = Config::Get();
    auto* plane = gc_->block_manager->Get_plane_bookkeeping_entry(a);
    const bool partial = plane->Blocks[a.BlockID].Current_page_write_index < gc_->pages_no_per_block;
    const auto free = plane->Free_block_pool.size();
    const unsigned soft = gc_->block_pool_gc_threshold;
    if (free > soft + (partial ? 1 : 0)) return true;
    // GC starts strictly below soft. Legacy refresh stops at soft, which can
    // strand old data indefinitely after host arrivals cease. A closed-source
    // refresh reserves its destination atomically and returns one free source
    // block on erase, so this one-block loan does not consume net block reserve.
    // Partial frontiers need an extra replacement and may not use this path.
    if (cfg.policy != "tarf" || !cfg.refresh_reserve_escape_ns || partial
        || free != soft || soft <= gc_->block_pool_gc_hard_threshold + 1) return false;
    const size_t index = ((((size_t(a.ChannelID) * gc_->chip_no_per_channel + a.ChipID)
        * gc_->die_no_per_chip + a.DieID) * gc_->plane_no_per_die + a.PlaneID)
        * gc_->block_no_per_plane + a.BlockID);
    if (index >= episodes_.size() || !episodes_[index].above) return false;
    const uint64_t since = std::max(episodes_[index].since, cfg.measurement_start_ns);
    return Simulator->Time() >= since && Simulator->Time() - since >= cfg.refresh_reserve_escape_ns;
}
bool RefreshController::Admit(const Candidate& c) {
    auto* plane = gc_->block_manager->Get_plane_bookkeeping_entry(c.source);
    if (!SpaceReady(c.source)
        || plane->Ongoing_erase_operations.size() >= gc_->max_ongoing_gc_reqs_per_plane
        || chip_blocks_[ChipIndex(c.source)] >= Config::Get().refresh_max_blocks_per_chip) return false;
    return ResourceReady(c.source);
}
unsigned RefreshController::ChipIndex(const Address& a) const { return a.ChannelID * gc_->chip_no_per_channel + a.ChipID; }
bool RefreshController::ResourceReady(const Address& a) {
    const auto& cfg = Config::Get();
    if (!cfg.resource_aware || (cfg.policy != "tarf" && !cfg.resource_aware_baselines)) return true;
    auto* phy = gc_->GC_and_WL_Unit_Base::flash_controller;
    auto* chip = phy->Get_chip(a.ChannelID, a.ChipID);
    const unsigned i = ChipIndex(a);
    const bool idle = phy->Get_channel_status(a.ChannelID) == BusChannelStatus::IDLE
        && phy->GetChipStatus(chip) == ChipStatus::IDLE
        && !dynamic_cast<TSU_Priority_OutOfOrder*>(gc_->tsu)->Pending_user_transactions(a.ChannelID, a.ChipID);
    // A currently empty queue can be a short gap inside a foreground burst.
    // Use only completed past I/O, never trace lookahead. At t=0 no prior idle
    // history is assumed. A zero quiet window preserves the previous executor.
    const uint64_t quiet_elapsed = Simulator->Time() - chip_last_foreground_completion_[i];
    const bool quiet = quiet_elapsed >= cfg.refresh_quiet_ns;
    if (idle && quiet) { chip_waiting_[i] = false; return true; }
    if (!chip_waiting_[i]) { chip_waiting_[i] = true; chip_wait_since_[i] = Simulator->Time(); }
    if (Simulator->Time() - chip_wait_since_[i] >= cfg.refresh_admission_max_defer_ns) {
        chip_waiting_[i] = false; ++forced_admissions_; return true;
    }
    uint64_t retry = cfg.refresh_admission_max_defer_ns - (Simulator->Time() - chip_wait_since_[i]);
    if (idle && !quiet) {
        ++quiet_deferred_attempts_;
        retry = std::min(retry, cfg.refresh_quiet_ns - quiet_elapsed);
    }
    SchedulePump(retry);
    return false;
}
void RefreshController::SchedulePump(uint64_t delay) {
    const uint64_t now = Simulator->Time();
    if (delay >= Config::Get().horizon_ns - now) return;
    const uint64_t when = now + std::max(uint64_t(1), delay);
    if (when >= pump_time_) return;
    pump_time_ = when; Simulator->Register_sim_event(when, this, nullptr, 1);
}
void RefreshController::Pump() {
    if (Simulator->Time() >= Config::Get().horizon_ns) return;
    Advance();
    bool room = false;
    for (unsigned count : chip_blocks_) if (count < Config::Get().refresh_max_blocks_per_chip) room = true;
    if (!room) return;
    std::vector<Candidate> postponed;
    std::vector<bool> checked(chip_blocks_.size(), false);
    std::vector<bool> checked_plane(chip_blocks_.size() * gc_->die_no_per_chip * gc_->plane_no_per_die, false);
    for (auto c : queue_) {
        auto* plane = gc_->block_manager->Get_plane_bookkeeping_entry(c.source);
        auto& b = plane->Blocks[c.source.BlockID];
        if (b.Has_ongoing_gc_wl) continue;
        // Most completion callbacks still find the chip busy. Avoid evaluating
        // every queued block's page model until there is an admission opportunity.
        const unsigned ci = ChipIndex(c.source);
        const unsigned pi = (ci * gc_->die_no_per_chip + c.source.DieID) * gc_->plane_no_per_die + c.source.PlaneID;
        if (checked[ci] || checked_plane[pi] || chip_blocks_[ci] >= Config::Get().refresh_max_blocks_per_chip) { postponed.push_back(c); continue; }
        if (!Admit(c)) {
            // Space/erase limits belong to one plane. Only a chip-wide resource
            // admission failure can justify skipping other planes on this chip.
            checked[ci] = SpaceReady(c.source)
                && plane->Ongoing_erase_operations.size() < gc_->max_ongoing_gc_reqs_per_plane;
            // A partial frontier needs one more block than a closed source.
            // Its extra reservation must not prevent a full block on this plane.
            const bool escape_enabled = Config::Get().policy == "tarf" && Config::Get().refresh_reserve_escape_ns;
            checked_plane[pi] = (escape_enabled ? plane->Free_block_pool.size() < gc_->block_pool_gc_threshold
                                               : plane->Free_block_pool.size() <= gc_->block_pool_gc_threshold)
                || plane->Ongoing_erase_operations.size() >= gc_->max_ongoing_gc_reqs_per_plane;
            ++deferred_; postponed.push_back(c); continue;
        }
        if (!Trigger(c.source, b.Erase_count, Simulator->Time())) continue;
        double benefit;
        if (!SafeSource(c.source)
            || !Destination(c.source, c.destination, benefit)) {
            ++deferred_; postponed.push_back(c); continue;
        }
        Migrate(c);
    }
    queue_.swap(postponed);
    Advance();
}
void RefreshController::Migrate(const Candidate& c) {
    auto* manager = gc_->block_manager;
    auto* plane = manager->Get_plane_bookkeeping_entry(c.source);
    auto& block = plane->Blocks[c.source.BlockID];
    const unsigned source_used_pages = block.Current_page_write_index;
    const unsigned valid = block.Current_page_write_index - block.Invalid_page_count;
    if (!valid) return;
    if (plane->Free_block_pool.size() == gc_->block_pool_gc_threshold) {
        if (!SpaceReady(c.source)) throw std::logic_error("Unsafe refresh reserve escape");
        ++reserve_escape_admissions_;
        RecordEvent("soft_reserve_escape", c.source, c.destination);
    }
    if (!manager->Begin_refresh_block(block.Stream_id, c.destination))
        throw std::runtime_error("Refresh destination reservation failed after revalidation");
    if (source_used_pages < gc_->pages_no_per_block) {
        source_tail_sealed_pages_ += manager->Retire_partial_data_frontier(c.source);
        ++partial_frontiers_retired_;
        RecordEvent("partial_frontier_retired", c.source, c.destination);
    }
    manager->GC_WL_started(c.source, false);
    plane->Ongoing_erase_operations.insert(c.source.BlockID);
    auto* erase = new NVM_Transaction_Flash_ER(Transaction_Source_Type::GC_WL, block.Stream_id, c.source);
    erase->Is_refresh = true;
    block.Erase_transaction = erase;
    Migration job; job.source = c.source; job.destination = c.destination; job.source_used_pages = source_used_pages;
    active_[erase] = job;
    RecordEvent("block_started", job.source, job.destination);
    ++chip_blocks_[ChipIndex(c.source)]; ++refresh_started_; ++inflight_;
}
void RefreshController::Advance() {
    auto* manager = gc_->block_manager;
    auto& reliability = Reliability::Get();
    // active_ is keyed by transaction identity for completion callbacks. Heap
    // addresses must not determine NAND submission order: allocator layout can
    // vary with output paths even when every scientific input is identical.
    // Source blocks are unique while active, so this physical key is a strict
    // total order without a pointer tie-break. Map node references remain valid.
    using ScheduledMigration = std::pair<NVM_Transaction_Flash_ER*, Migration*>;
    std::vector<ScheduledMigration> ordered;
    ordered.reserve(active_.size());
    for (auto& entry : active_) ordered.emplace_back(entry.first, &entry.second);
    std::sort(ordered.begin(), ordered.end(), [](const ScheduledMigration& left, const ScheduledMigration& right) {
        const auto& a = left.second->source;
        const auto& b = right.second->source;
        return std::tie(a.ChannelID, a.ChipID, a.DieID, a.PlaneID, a.BlockID)
             < std::tie(b.ChannelID, b.ChipID, b.DieID, b.PlaneID, b.BlockID);
    });
    for (const auto& entry : ordered) {
        auto* erase = entry.first; auto& job = *entry.second;
        if (job.erase_submitted) continue;
        auto* plane = manager->Get_plane_bookkeeping_entry(job.source);
        auto& block = plane->Blocks[job.source.BlockID];
        const unsigned ci = ChipIndex(job.source);
        while (job.next_page < job.source_used_pages && chip_pages_[ci] < Config::Get().refresh_max_pages_per_chip) {
            Address source = job.source; source.PageID = job.next_page;
            if (!manager->Is_page_valid(&block, source.PageID)) { ++job.next_page; ++skipped_invalid_; continue; }
            const auto state = reliability.State(source);
            if (!state.live || state.scheduled) throw std::runtime_error("Refresh source valid page is not committed");
            if (!ResourceReady(source)) { ++deferred_; break; }
            PPA_type actual; page_status_type bitmap;
            gc_->address_mapping_unit->Get_data_mapping_info_for_gc(block.Stream_id, state.lpa, actual, bitmap);
            const auto expected = gc_->address_mapping_unit->Convert_address_to_ppa(source);
            if (actual != expected) throw std::runtime_error("Refresh page mapping changed without invalidation");
            gc_->address_mapping_unit->Set_barrier_for_accessing_lpa(block.Stream_id, state.lpa);
            const Address destination = manager->Allocate_refresh_page(job.destination);
            auto* read = new NVM_Transaction_Flash_RD(Transaction_Source_Type::GC_WL, block.Stream_id,
                gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE, state.lpa, expected, source, nullptr, 0, nullptr, bitmap, INVALID_TIME_STAMP);
            auto* write = new NVM_Transaction_Flash_WR(Transaction_Source_Type::GC_WL, block.Stream_id,
                gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE, state.lpa, NO_PPA, destination,
                nullptr, 0, read, bitmap, INVALID_TIME_STAMP);
            read->Is_refresh = write->Is_refresh = true;
            write->Has_reserved_destination = true; write->Refresh_source_address = source;
            write->ExecutionMode = WriteExecutionModeType::SIMPLE;
            write->RelatedErase = erase; read->RelatedWrite = write;
            erase->Page_movement_activities.push_back(write);
            RecordEvent("read_submitted", source, destination);
            ++job.next_page; ++job.outstanding; ++chip_pages_[ci]; ++reads_submitted_;
            refresh_read_bytes_submitted_ += read->Data_and_metadata_size_in_byte;
            max_chip_pages_ = std::max(max_chip_pages_, uint64_t(chip_pages_[ci]));
            uint64_t total = 0; for (unsigned count : chip_pages_) total += count;
            max_outstanding_pages_ = std::max(max_outstanding_pages_, total);
            gc_->tsu->Prepare_for_transaction_submit(); gc_->tsu->Submit_transaction(read); gc_->tsu->Schedule();
        }
        if (job.next_page == job.source_used_pages && !job.outstanding
            && !reliability.LivePageCount(job.source) && manager->Can_execute_gc_wl(job.source)) {
            sealed_unused_pages_ += manager->Seal_refresh_block(job.destination);
            job.erase_submitted = true; ++erases_submitted_;
            RecordEvent("erase_submitted", job.source, job.destination);
            gc_->tsu->Prepare_for_transaction_submit(); gc_->tsu->Submit_transaction(erase); gc_->tsu->Schedule();
        }
    }
}
bool RefreshController::OwnsRefreshBlock(const Address& address) {
    if (!instance_) return false;
    auto same = [&address](const Address& a) { return a.ChannelID == address.ChannelID && a.ChipID == address.ChipID
        && a.DieID == address.DieID && a.PlaneID == address.PlaneID && a.BlockID == address.BlockID; };
    for (const auto& entry : instance_->active_)
        if (same(entry.second.source) || (!entry.second.erase_submitted && same(entry.second.destination))) return true;
    return false;
}
void RefreshController::WriteSubmitted(NVM_Transaction_Flash* transaction) {
    if (instance_ && transaction->Is_refresh) {
        ++instance_->writes_submitted_;
        auto* write = static_cast<NVM_Transaction_Flash_WR*>(transaction);
        instance_->RecordEvent("write_submitted", write->Refresh_source_address, write->Address);
    }
}
void RefreshController::RecordEvent(const char* event, const Address& source, const Address& destination) {
    if (event_rows_ >= 8192) return;
    ++event_rows_;
    events_ << event << ',' << Simulator->Time() << ',' << source.ChannelID << ',' << source.ChipID << ','
        << source.DieID << ',' << source.PlaneID << ',' << source.BlockID << ',' << source.PageID << ','
        << destination.BlockID << ',' << destination.PageID << '\n';
}
void RefreshController::Observe(NVM_Transaction_Flash* tr) {
    auto* self = instance_;
    if (!self) return;
    auto& r = Reliability::Get();
    const uint64_t now = Simulator->Time();
    if (tr->Source != Transaction_Source_Type::GC_WL && tr->Source != Transaction_Source_Type::MAPPING
        && (tr->Type == Transaction_Type::READ || tr->Type == Transaction_Type::WRITE))
        self->chip_last_foreground_completion_[self->ChipIndex(tr->Address)] = now;
    if (tr->Type == Transaction_Type::WRITE) {
        const uint64_t bytes = self->gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE;
        self->all_program_bytes_ += bytes;
        if (tr->Source == Transaction_Source_Type::MAPPING) self->mapping_program_bytes_ += bytes;
        auto* destination_plane = self->gc_->block_manager->Get_plane_bookkeeping_entry(tr->Address);
        if (!destination_plane->Blocks[tr->Address.BlockID].Holds_mapping_data) {
            auto* wr = static_cast<NVM_Transaction_Flash_WR*>(tr);
            double before_age = tr->Is_refresh ? r.BlockMaxAgeDays(wr->Refresh_source_address, now) : 0;
            r.OnProgramComplete(tr->Address, tr->Stream_id, tr->LPA, now);
            if (tr->Is_refresh && self->audited_pages_++ < 2048) {
                const auto& a = wr->Refresh_source_address;
                const auto& state = r.State(tr->Address);
                auto* plane = self->gc_->block_manager->Get_plane_bookkeeping_entry(tr->Address);
                self->audit_ << std::setprecision(17) << now << ',' << a.ChannelID << ',' << a.ChipID << ',' << a.DieID << ',' << a.PlaneID << ',' << a.BlockID << ',' << a.PageID << ','
                    << tr->Address.BlockID << ',' << tr->Address.PageID << ',' << plane->Blocks[tr->Address.BlockID].Erase_count << ','
                    << state.program_temp_c << ',' << state.program_ns << ',' << state.initial_age_days << ',' << before_age << ','
                    << r.State(a).live << ',' << state.live << '\n';
            }
        }
        if (tr->Is_refresh) {
            ++self->refresh_writes_; self->refresh_program_bytes_ += bytes;
            auto* write = static_cast<NVM_Transaction_Flash_WR*>(tr);
            self->RecordEvent("write_completed", write->Refresh_source_address, write->Address);
            auto& job = self->active_.at(write->RelatedErase);
            --job.outstanding; --self->chip_pages_[self->ChipIndex(job.source)];
            const uint64_t hold = now - tr->Issue_time;
            self->page_hold_ns_ += hold; self->max_page_hold_ns_ = std::max(self->max_page_hold_ns_, hold);
        }
        else if (tr->Source == Transaction_Source_Type::GC_WL) ++self->gc_writes_;
        else if (tr->Source != Transaction_Source_Type::MAPPING) ++self->foreground_writes_;
    } else if (tr->Type == Transaction_Type::READ) {
        const uint64_t bytes = tr->Data_and_metadata_size_in_byte;
        self->all_read_bytes_ += bytes;
        if (tr->Source == Transaction_Source_Type::MAPPING) self->mapping_read_bytes_ += bytes;
        else if (tr->Source != Transaction_Source_Type::GC_WL) self->foreground_read_bytes_ += bytes;
        if (tr->Is_refresh) {
            ++self->refresh_reads_; self->refresh_read_bytes_ += bytes;
            self->RecordEvent("read_completed", tr->Address, static_cast<NVM_Transaction_Flash_RD*>(tr)->RelatedWrite->Address);
        }
        else if (tr->Source == Transaction_Source_Type::GC_WL) { ++self->gc_reads_; self->gc_read_bytes_ += bytes; }
    } else if (tr->Type == Transaction_Type::ERASE) {
        r.OnErase(tr->Address);
        if (tr->Is_refresh) {
            self->RecordEvent("erase_completed", tr->Address, self->active_.at(static_cast<NVM_Transaction_Flash_ER*>(tr)).destination);
            ++self->refresh_erases_; ++self->refresh_completed_; --self->inflight_;
            --self->chip_blocks_[self->ChipIndex(tr->Address)];
            self->active_.erase(static_cast<NVM_Transaction_Flash_ER*>(tr));
        }
        else ++self->gc_erases_;
    }
    if (!self->queue_.empty() || !self->active_.empty()) self->SchedulePump(1);
}
double RefreshController::GCBenefit(const Address& a) {
    if (!instance_ || Simulator->Time() < Config::Get().measurement_start_ns
        || Config::Get().policy != "tarf" || !Config::Get().gc_coordination) return 0;
    auto& r = Reliability::Get();
    auto* plane = instance_->gc_->block_manager->Get_plane_bookkeeping_entry(a);
    auto& b = plane->Blocks[a.BlockID];
    const double risk = r.BlockRisk(a, b.Erase_count, Simulator->Time());
    if (risk <= Config::Get().threshold || b.Holds_mapping_data || b.Stream_id == NO_STREAM) return 0;
    auto* dest = plane->GC_wf[b.Stream_id];
    const double t = r.TemperatureC(Simulator->Time());
    return std::max(0.0, risk - r.PredictNFBC(dest->Erase_count, 0, t, t, a.ChannelID * instance_->gc_->chip_no_per_channel + a.ChipID));
}
void RefreshController::GCSelected(const Address& a) {
    if (instance_ && GCBenefit(a) > 0) ++instance_->gc_shared_;
}
void RefreshController::WriteReport() { if (instance_) instance_->Report(); }
void RefreshController::RecordEpisode(const Address& a, uint64_t start, uint64_t end, bool censored) {
    risk_episodes_ << a.ChannelID << ',' << a.ChipID << ',' << a.DieID << ',' << a.PlaneID << ',' << a.BlockID
        << ',' << start << ',' << end << ',' << end - start << ',' << censored << ',' << Config::Get().scan_ns << '\n';
}
void RefreshController::CaptureMeasurementStart(uint64_t now) {
    auto& r = Reliability::Get();
    std::ofstream blocks(Config::Get().output_prefix + "_measurement_blocks.csv");
    std::ofstream out(Config::Get().output_prefix + "_measurement_start.json");
    if (!blocks || !out) throw std::runtime_error("Cannot write measurement boundary audit");
    blocks << "block_index,pe,used_pages,invalid_pages,live_pages,mapping,ongoing_gc,page_state_fnv64,full_nfbc\n";
    uint64_t live = 0, invalid = 0, ongoing = 0;
    for (size_t i = 0; i < r.BlockCount(); ++i) {
        auto a = r.BlockAddress(i);
        const auto& b = gc_->block_manager->Get_plane_bookkeeping_entry(a)->Blocks[a.BlockID];
        auto pages = r.LivePageCount(a); live += pages; invalid += b.Invalid_page_count; ongoing += b.Has_ongoing_gc_wl;
        blocks << std::setprecision(17) << i << ',' << b.Erase_count << ',' << b.Current_page_write_index << ','
               << b.Invalid_page_count << ',' << pages << ',' << b.Holds_mapping_data << ',' << b.Has_ongoing_gc_wl << ','
               << r.BlockStateDigest(i) << ',' << r.BlockRisk(a, b.Erase_count, now) << '\n';
    }
    out << std::setprecision(17) << "{\n";
#define BOUNDARY(name, value) out << "  \"" name "\": " << (value) << ",\n"
    BOUNDARY("time_ns", now);
    BOUNDARY("live_pages", live); BOUNDARY("invalid_pages", invalid); BOUNDARY("ongoing_gc_blocks", ongoing);
    BOUNDARY("risky_blocks", risky_); BOUNDARY("eligible_blocks", eligible_);
    BOUNDARY("risky_block_seconds", double(risky_ns_ / 1e9L));
    BOUNDARY("total_eligible_block_seconds", double(eligible_ns_ / 1e9L));
    BOUNDARY("risk_exposure_integral_seconds", double(ratio_ns_ / 1e9L));
    BOUNDARY("refresh_started", refresh_started_); BOUNDARY("refresh_write_pages_completed", refresh_writes_);
    BOUNDARY("gc_read_pages_completed", gc_reads_); BOUNDARY("gc_write_pages_completed", gc_writes_);
    BOUNDARY("gc_erase_completed", gc_erases_); BOUNDARY("gc_read_bytes_completed", gc_read_bytes_);
    BOUNDARY("gc_program_bytes_completed", gc_writes_ * gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE);
    BOUNDARY("all_program_bytes", all_program_bytes_); BOUNDARY("all_read_bytes_completed", all_read_bytes_);
    BOUNDARY("foreground_program_pages_completed", foreground_writes_);
    BOUNDARY("foreground_program_bytes_completed", foreground_writes_ * gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE);
    BOUNDARY("foreground_read_bytes_completed", foreground_read_bytes_);
    BOUNDARY("mapping_read_bytes_completed", mapping_read_bytes_); BOUNDARY("mapping_program_bytes_completed", mapping_program_bytes_);
    out << "  \"state_scope\": \"physical page metadata including logical mapping, validity, generation, programming time/temperature/age; per-block PE and allocation counts\"\n}\n";
#undef BOUNDARY
    measurement_captured_ = true;
}
void RefreshController::Report() {
    Sample(Simulator->Time(), false);
    audit_.flush();
    events_.flush();
    timeline_.flush();
    std::ofstream out(Config::Get().output_prefix + "_reliability.json");
    if (!out) throw std::runtime_error("Cannot write reliability results");
    auto& r = Reliability::Get();
    for (size_t i = 0; i < episodes_.size(); ++i)
        if (episodes_[i].above) RecordEpisode(r.BlockAddress(i), episodes_[i].since, Simulator->Time(), true);
    risk_episodes_.flush();
    unsigned min_pe = std::numeric_limits<unsigned>::max(), max_pe = 0;
    uint64_t pending_pages = 0, outstanding_pages = 0, max_observed_page_hold = max_page_hold_ns_;
    for (unsigned count : chip_pages_) outstanding_pages += count;
    for (const auto& job : active_)
        for (auto* write : job.first->Page_movement_activities)
            max_observed_page_hold = std::max(max_observed_page_hold, Simulator->Time() - write->Issue_time);
    for (size_t i = 0; i < r.BlockCount(); ++i) {
        const auto a = r.BlockAddress(i);
        const unsigned pe = gc_->block_manager->Get_plane_bookkeeping_entry(a)->Blocks[a.BlockID].Erase_count;
        min_pe = std::min(min_pe, pe); max_pe = std::max(max_pe, pe);
        if (Trigger(a, pe, Simulator->Time())) pending_pages += r.LivePageCount(a);
    }
    out << std::setprecision(17) << "{\n  \"policy\": \"" << Config::Get().policy << "\",\n";
#define FIELD(name, value) out << "  \"" name "\": " << (value) << ",\n"
    FIELD("model_calibrated", Config::Get().calibrated ? "true" : "false");
    FIELD("horizon_ns", Config::Get().horizon_ns);
    FIELD("measurement_start_ns", Config::Get().measurement_start_ns);
    FIELD("risk_exposure_integral_seconds", double(ratio_ns_ / 1e9L));
    FIELD("actual_sim_time_ns", Simulator->Time());
    FIELD("scan_interval_ns", Config::Get().scan_ns);
    FIELD("wait_resolution_ns", Config::Get().scan_ns);
    FIELD("initial_pe", Config::Get().initial_pe);
    FIELD("initial_age_days", Config::Get().initial_age_days);
    FIELD("threshold", Config::Get().threshold);
    FIELD("risk_threshold", Config::Get().risk_threshold);
    FIELD("initial_live_pages", initial_live_);
    FIELD("initial_risky_blocks", initial_risky_);
    FIELD("initial_eligible_blocks", initial_eligible_);
    FIELD("risk_exposure_time_mean", Config::Get().horizon_ns ? double(ratio_ns_ / Config::Get().horizon_ns) : 0);
    FIELD("risky_block_seconds", double(risky_ns_ / 1e9L));
    FIELD("total_eligible_block_seconds", double(eligible_ns_ / 1e9L));
    FIELD("max_over_threshold_wait_seconds", max_wait_ns_ / 1e9);
    FIELD("completed_risk_episodes", completed_episodes_);
    FIELD("pending_risky_blocks", risky_);
    FIELD("pending_refresh_blocks", pending_);
    FIELD("inflight_refresh_blocks", inflight_);
    FIELD("refresh_started", refresh_started_);
    FIELD("refresh_completed", refresh_completed_);
    FIELD("refresh_read_pages_completed", refresh_reads_);
    FIELD("refresh_write_pages_completed", refresh_writes_);
    FIELD("refresh_erase_completed", refresh_erases_);
    FIELD("refresh_read_pages_submitted", reads_submitted_);
    FIELD("refresh_write_pages_submitted", writes_submitted_);
    FIELD("refresh_erase_submitted", erases_submitted_);
    FIELD("refresh_read_bytes_submitted", refresh_read_bytes_submitted_);
    FIELD("refresh_read_bytes_completed", refresh_read_bytes_);
    FIELD("refresh_program_bytes_submitted", writes_submitted_ * gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE);
    FIELD("refresh_program_bytes_completed", refresh_program_bytes_);
    FIELD("refresh_read_pages_outstanding", reads_submitted_ - refresh_reads_);
    FIELD("refresh_program_pages_outstanding", writes_submitted_ - refresh_writes_);
    FIELD("refresh_migration_pages_outstanding", outstanding_pages);
    FIELD("refresh_migration_pages_peak", max_outstanding_pages_);
    FIELD("refresh_migration_pages_per_chip_peak", max_chip_pages_);
    FIELD("pending_triggered_live_pages", pending_pages);
    FIELD("refresh_skipped_invalid_source_pages", skipped_invalid_);
    FIELD("refresh_forced_progress_admissions", forced_admissions_);
    FIELD("refresh_page_barrier_mean_seconds", refresh_writes_ ? double(page_hold_ns_ / refresh_writes_ / 1e9L) : 0);
    FIELD("refresh_page_barrier_max_seconds", max_page_hold_ns_ / 1e9);
    FIELD("refresh_page_barrier_max_observed_seconds", max_observed_page_hold / 1e9);
    FIELD("refresh_page_barriers_current", outstanding_pages);
    FIELD("refresh_max_pages_per_chip", Config::Get().refresh_max_pages_per_chip);
    FIELD("refresh_max_blocks_per_chip", Config::Get().refresh_max_blocks_per_chip);
    FIELD("refresh_max_defer_ns", Config::Get().refresh_max_defer_ns);
    FIELD("refresh_admission_max_defer_ns", Config::Get().refresh_admission_max_defer_ns);
    FIELD("refresh_quiet_ns", Config::Get().refresh_quiet_ns);
    FIELD("refresh_quiet_deferred_attempts", quiet_deferred_attempts_);
    FIELD("refresh_reserve_escape_ns", Config::Get().refresh_reserve_escape_ns);
    FIELD("refresh_reserve_escape_admissions", reserve_escape_admissions_);
    FIELD("resource_aware_baselines", Config::Get().resource_aware_baselines ? "true" : "false");
    FIELD("gc_respect_configured_threshold", Config::Get().gc_respect_configured_threshold ? "true" : "false");
    FIELD("gc_effective_soft_blocks", gc_->block_pool_gc_threshold);
    FIELD("gc_effective_hard_blocks", gc_->block_pool_gc_hard_threshold);
    FIELD("gc_effective_max_concurrent_per_plane", gc_->max_ongoing_gc_reqs_per_plane);
    FIELD("gc_read_pages_completed", gc_reads_);
    FIELD("gc_write_pages_completed", gc_writes_);
    FIELD("gc_erase_completed", gc_erases_);
    FIELD("gc_read_bytes_completed", gc_read_bytes_);
    FIELD("gc_program_bytes_completed", gc_writes_ * gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE);
    FIELD("all_read_bytes_completed", all_read_bytes_);
    FIELD("foreground_read_bytes_completed", foreground_read_bytes_);
    FIELD("mapping_read_bytes_completed", mapping_read_bytes_);
    FIELD("mapping_program_bytes_completed", mapping_program_bytes_);
    FIELD("refresh_event_audit_limit", 8192);
    FIELD("refresh_event_audit_written", event_rows_);
    FIELD("gc_coordinated_blocks", gc_shared_);
    FIELD("foreground_program_pages_completed", foreground_writes_);
    FIELD("foreground_program_bytes_completed", foreground_writes_ * gc_->sector_no_per_page * SECTOR_SIZE_IN_BYTE);
    FIELD("all_program_bytes", all_program_bytes_);
    FIELD("refresh_program_bytes", refresh_program_bytes_);
    FIELD("admission_deferred_attempts", deferred_);
    FIELD("sealed_unused_pages", sealed_unused_pages_);
    FIELD("partial_frontiers_retired", partial_frontiers_retired_);
    FIELD("source_tail_sealed_pages", source_tail_sealed_pages_);
    FIELD("pe_min", min_pe);
    FIELD("pe_max", max_pe);
    FIELD("lazy_initial_pages", r.lazy_initial_pages);
    FIELD("stale_program_completions", r.stale_program_completions);
    FIELD("erased_live_pages", r.erased_live_pages);
    out << "  \"risk_integration\": \"left-held scan samples, exact declared temperature-change samples; unfinished episodes included\",\n"
        << "  \"read_bytes_accounting\": \"completed transaction Data_and_metadata_size_in_byte; partial pages counted at actual transfer size\",\n"
        << "  \"selection_rank\": \"" << Config::Get().selection_rank << "\",\n"
        << "  \"refresh_executor\": \"bounded per-chip page pipeline; page-local barriers; shared background progress deadline\",\n"
        << "  \"destination_allocation\": \"same-plane free block, per-page reservation; unused tail sealed without NAND programming\"\n}\n";
#undef FIELD
}
}
