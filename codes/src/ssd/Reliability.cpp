#include "Reliability.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace TARF {
namespace {
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string env(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}
double number(const std::string& s) {
    size_t used = 0;
    double value = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(value)) throw std::runtime_error("Invalid TARF number: " + s);
    return value;
}
uint64_t integer(const std::string& s) {
    if (s.empty() || s[0] == '-') throw std::runtime_error("Invalid TARF unsigned integer: " + s);
    size_t used = 0;
    uint64_t value = std::stoull(s, &used);
    if (used != s.size()) throw std::runtime_error("Invalid TARF integer: " + s);
    return value;
}
bool boolean(const std::string& s) {
    if (s == "1" || s == "true") return true;
    if (s == "0" || s == "false") return false;
    throw std::runtime_error("Invalid TARF boolean: " + s);
}
unsigned unsigned_number(const std::string& s) {
    const uint64_t value = integer(s);
    if (value > std::numeric_limits<unsigned>::max()) throw std::runtime_error("TARF integer exceeds unsigned range: " + s);
    return static_cast<unsigned>(value);
}
void load_initial_state(Config& c) {
    c.initial_pe_min = c.initial_pe_max = c.initial_pe;
    c.initial_age_days_min = c.initial_age_days_max = c.initial_age_days;
    c.initial_temp_c_min = c.initial_temp_c_max = c.initial_temp_c;
    if (c.initial_state_file.empty()) return;
    std::ifstream in(c.initial_state_file);
    if (!in) throw std::runtime_error("Cannot open TARF_INITIAL_STATE_FILE: " + c.initial_state_file);
    std::map<uint64_t, InitialBlockState> rows;
    std::string line; bool header = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (!header) {
            if (line != "block_index,pe,age_days,program_temp_c")
                throw std::runtime_error("Initial state CSV requires header block_index,pe,age_days,program_temp_c");
            header = true; continue;
        }
        std::vector<std::string> fields;
        size_t start = 0;
        for (;;) {
            const size_t comma = line.find(',', start);
            fields.push_back(trim(line.substr(start, comma == std::string::npos ? comma : comma - start)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (fields.size() != 4) throw std::runtime_error("Initial state CSV requires exactly four columns: " + line);
        const uint64_t index = integer(fields[0]);
        InitialBlockState state;
        state.pe = unsigned_number(fields[1]); state.age_days = number(fields[2]); state.program_temp_c = number(fields[3]);
        if (state.age_days < 0 || state.program_temp_c <= -273.15)
            throw std::runtime_error("Initial state requires age >= 0 and temperature above absolute zero: " + line);
        if (!rows.emplace(index, state).second) throw std::runtime_error("Duplicate initial block index: " + fields[0]);
    }
    if (rows.empty()) throw std::runtime_error("Initial state CSV has no block records");
    c.initial_blocks.reserve(rows.size());
    uint64_t next = 0;
    for (const auto& row : rows) {
        if (row.first != next++) throw std::runtime_error("Initial block indices must cover 0..block_count-1 without gaps");
        c.initial_blocks.push_back(row.second);
        const auto& s = row.second;
        if (row.first == 0) {
            c.initial_pe_min = c.initial_pe_max = s.pe;
            c.initial_age_days_min = c.initial_age_days_max = s.age_days;
            c.initial_temp_c_min = c.initial_temp_c_max = s.program_temp_c;
        } else {
            c.initial_pe_min = std::min(c.initial_pe_min, s.pe); c.initial_pe_max = std::max(c.initial_pe_max, s.pe);
            c.initial_age_days_min = std::min(c.initial_age_days_min, s.age_days); c.initial_age_days_max = std::max(c.initial_age_days_max, s.age_days);
            c.initial_temp_c_min = std::min(c.initial_temp_c_min, s.program_temp_c); c.initial_temp_c_max = std::max(c.initial_temp_c_max, s.program_temp_c);
        }
    }
}
std::string quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}
void retention_value(RetentionParameters& p, const std::string& key, double value) {
    if (key == "chip_bias") p.chip_bias = value;
    else if (key == "t0days") p.t0days = value;
    else if (key == "delta") p.delta = value;
    else if (key == "gamma") p.gamma = value;
    else throw std::runtime_error("Unknown TARF retention parameter: " + key);
}
void validate_retention(const RetentionParameters& p) {
    if (p.t0days <= 0 || p.gamma < 0 || p.delta < 0)
        throw std::runtime_error("TARF requires t0days > 0 and delta/gamma >= 0");
}
Config load_config() {
    Config c;
    c.policy = env("TARF_POLICY", c.policy);
    const std::set<std::string> policies = {"none", "periodic", "age", "tempdiff", "tarf"};
    if (!policies.count(c.policy)) throw std::runtime_error("Unknown TARF_POLICY: " + c.policy);
    c.horizon_ns = integer(env("TARF_HORIZON_NS", std::to_string(c.horizon_ns)));
    c.scan_ns = integer(env("TARF_SCAN_NS", std::to_string(c.scan_ns)));
    const uint64_t pe = integer(env("TARF_INITIAL_PE", "0"));
    if (pe > std::numeric_limits<unsigned>::max()) throw std::runtime_error("Initial P/E exceeds unsigned range");
    c.initial_pe = static_cast<unsigned>(pe);
    c.initial_age_days = number(env("TARF_INITIAL_AGE_DAYS", "0"));
    c.initial_temp_c = number(env("TARF_INITIAL_TEMP_C", "30"));
    c.initial_state_file = env("TARF_INITIAL_STATE_FILE", "");
    c.initial_read_coverage_file = env("TARF_INITIAL_READ_COVERAGE_FILE", "");
    c.reject_lazy_initial_reads = boolean(env("TARF_REJECT_LAZY_INITIAL_READS", "false"));
    c.refresh_max_pages_per_chip = unsigned_number(env("TARF_REFRESH_MAX_PAGES_PER_CHIP", "2"));
    c.refresh_max_blocks_per_chip = unsigned_number(env("TARF_REFRESH_MAX_BLOCKS_PER_CHIP", "1"));
    c.refresh_max_defer_ns = integer(env("TARF_REFRESH_MAX_DEFER_NS", "100000000"));
    c.refresh_admission_max_defer_ns = integer(env("TARF_REFRESH_ADMISSION_MAX_DEFER_NS", std::to_string(c.refresh_max_defer_ns)));
    c.refresh_quiet_ns = integer(env("TARF_REFRESH_QUIET_NS", "0"));
    c.refresh_reserve_escape_ns = integer(env("TARF_REFRESH_RESERVE_ESCAPE_NS", "0"));
    c.selection_rank = env("TARF_SELECTION_RANK", "benefit_cost");
    if (c.selection_rank != "benefit_cost" && c.selection_rank != "risk" && c.selection_rank != "fifo")
        throw std::runtime_error("Unknown TARF_SELECTION_RANK: " + c.selection_rank);
    c.measurement_start_ns = integer(env("TARF_MEASUREMENT_START_NS", "0"));
    if (c.measurement_start_ns >= c.horizon_ns)
        throw std::runtime_error("Measurement start must precede horizon");
    c.threshold = number(env("TARF_THRESHOLD", "20"));
    c.risk_threshold = number(env("TARF_RISK_THRESHOLD", "20"));
    c.period_days = number(env("TARF_PERIOD_DAYS", "30"));
    c.resource_aware = boolean(env("TARF_RESOURCE_AWARE", "true"));
    c.resource_aware_baselines = boolean(env("TARF_RESOURCE_AWARE_BASELINES", "false"));
    c.gc_coordination = boolean(env("TARF_GC_COORDINATION", "true"));
    c.gc_respect_configured_threshold = boolean(env("TARF_GC_RESPECT_CONFIGURED_THRESHOLD", "false"));
    c.output_prefix = env("TARF_OUTPUT_PREFIX", c.output_prefix);
    c.model_file = env("TARF_MODEL_FILE", "");
    c.temperature_file = env("TARF_TEMPERATURE_FILE", "");
    if (!c.horizon_ns || !c.scan_ns || c.initial_age_days < 0 || c.threshold <= 0 || c.risk_threshold <= 0 || c.period_days <= 0)
        throw std::runtime_error("TARF time intervals/thresholds must be positive and initial age nonnegative");
    if (!c.refresh_max_pages_per_chip || !c.refresh_max_blocks_per_chip || !c.refresh_max_defer_ns || !c.refresh_admission_max_defer_ns)
        throw std::runtime_error("TARF refresh limits and maximum defer time must be positive");
    if (c.initial_temp_c <= -273.15) throw std::runtime_error("Initial temperature must be above absolute zero");
    load_initial_state(c);
    if (!c.model_file.empty()) {
        std::ifstream in(c.model_file);
        if (!in) throw std::runtime_error("Cannot open TARF_MODEL_FILE: " + c.model_file);
        std::map<std::string, std::string> values;
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) throw std::runtime_error("Model line must be key=value: " + line);
            const std::string key = trim(line.substr(0, eq));
            if (!values.emplace(key, trim(line.substr(eq + 1))).second) throw std::runtime_error("Duplicate model key: " + key);
        }
        // A supplied model is complete: missing coefficients never silently fall back.
        for (const char* key : {"beta0", "beta_pe", "beta_diff", "beta_common", "t0days", "delta", "gamma", "calibrated", "provenance"})
            if (!values.count(key)) throw std::runtime_error(std::string("Model missing key: ") + key);
        for (const auto& entry : values) {
            const std::string& key = entry.first;
            if (key.compare(0, 5, "chip.") == 0) continue;
            if (key == "calibrated") c.calibrated = boolean(entry.second);
            else if (key == "provenance") c.provenance = entry.second;
            else if (key == "beta0") c.beta0 = number(entry.second);
            else if (key == "beta_pe") c.beta_pe = number(entry.second);
            else if (key == "beta_diff") c.beta_diff = number(entry.second);
            else if (key == "beta_common") c.beta_common = number(entry.second);
            else retention_value(c.retention, key, number(entry.second));
        }
        for (const auto& entry : values) if (entry.first.compare(0, 5, "chip.") == 0) {
            const size_t dot = entry.first.find('.', 5);
            if (dot == std::string::npos) throw std::runtime_error("Invalid chip parameter key: " + entry.first);
            const uint64_t chip = integer(entry.first.substr(5, dot - 5));
            if (chip > std::numeric_limits<unsigned>::max()) throw std::runtime_error("Chip identifier too large");
            auto inserted = c.chip_parameters.emplace(static_cast<unsigned>(chip), c.retention);
            retention_value(inserted.first->second, entry.first.substr(dot + 1), number(entry.second));
        }
        if (c.provenance.empty()) throw std::runtime_error("Model provenance must not be empty");
    }
    validate_retention(c.retention);
    for (const auto& p : c.chip_parameters) validate_retention(p.second);
    if (c.temperature_file.empty()) c.temperatures.push_back({0, c.initial_temp_c});
    else {
        std::ifstream in(c.temperature_file);
        if (!in) throw std::runtime_error("Cannot open TARF_TEMPERATURE_FILE: " + c.temperature_file);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line == "time_ns,temp_C" || line == "time_ns,temp_c") continue;
            const size_t comma = line.find(',');
            if (comma == std::string::npos) throw std::runtime_error("Temperature line must be time_ns,temp_C: " + line);
            const uint64_t time = integer(trim(line.substr(0, comma)));
            const double temp = number(trim(line.substr(comma + 1)));
            if (temp <= -273.15) throw std::runtime_error("Temperature trace is at or below absolute zero");
            if ((!c.temperatures.empty() && time <= c.temperatures.back().first) || (c.temperatures.empty() && time != 0))
                throw std::runtime_error("Temperature trace must start at 0 and be strictly increasing");
            c.temperatures.push_back({time, temp});
        }
        if (c.temperatures.empty()) throw std::runtime_error("Empty temperature trace");
    }
    return c;
}
}

const Config& Config::Get() { static const Config config = load_config(); return config; }
void Load_from_environment() { (void)Config::Get(); }
Reliability& Reliability::Get() { static Reliability model; return model; }

void Reliability::InitializeGeometry(unsigned channels, unsigned chips, unsigned dies,
                                     unsigned planes, unsigned blocks, unsigned pages) {
    if (!channels || !chips || !dies || !planes || !blocks || !pages) throw std::runtime_error("Empty TARF geometry");
    channels_ = channels; chips_ = chips; dies_ = dies; planes_ = planes; blocks_ = blocks; pages_ = pages;
    size_t total = channels;
    for (unsigned n : {chips, dies, planes, blocks, pages}) {
        if (total > std::numeric_limits<size_t>::max() / n) throw std::runtime_error("TARF geometry overflow");
        total *= n;
    }
    const Config& c = Config::Get();
    if (!c.initial_state_file.empty() && c.initial_blocks.size() != total / pages)
        throw std::runtime_error("Initial state CSV row count must equal complete physical block geometry: expected "
                                 + std::to_string(total / pages) + ", got " + std::to_string(c.initial_blocks.size()));
    uniform_initial_state_.pe = c.initial_pe;
    uniform_initial_state_.age_days = c.initial_age_days;
    uniform_initial_state_.program_temp_c = c.initial_temp_c;
    states_.assign(total, PageState());
    summaries_.clear(); summaries_.resize(total / pages);
    logical_.clear();
    initialized_pages = lazy_initial_pages = completed_programs = stale_program_completions = erased_live_pages = 0;
}
const InitialBlockState& Reliability::InitialState(const Address& a) const {
    const size_t block = Index(a) / pages_;
    const auto& profile = Config::Get().initial_blocks;
    return profile.empty() ? uniform_initial_state_ : profile[block];
}
uint64_t Reliability::BlockStateDigest(size_t block_index) const {
    // Serialize fields explicitly; exclude struct padding and pointer values.
    static_assert(sizeof(double) == sizeof(uint64_t), "Snapshot audit requires 64-bit doubles");
    uint64_t hash = 14695981039346656037ULL;
    auto add = [&hash](uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) { hash ^= value & 255; hash *= 1099511628211ULL; value >>= 8; }
    };
    for (size_t i = block_index * pages_; i < (block_index + 1) * pages_; ++i) {
        const auto& p = states_.at(i);
        add(p.program_ns); add(p.generation); add(p.lpa); add(p.stream); add(p.live); add(p.scheduled);
        uint64_t bits; std::memcpy(&bits, &p.initial_age_days, sizeof(bits)); add(bits);
        std::memcpy(&bits, &p.program_temp_c, sizeof(bits)); add(bits);
    }
    return hash;
}
size_t Reliability::Index(const Address& a) const {
    if (a.ChannelID >= channels_ || a.ChipID >= chips_ || a.DieID >= dies_ || a.PlaneID >= planes_ || a.BlockID >= blocks_ || a.PageID >= pages_)
        throw std::runtime_error("TARF physical address outside initialized geometry");
    return (((((size_t(a.ChannelID) * chips_ + a.ChipID) * dies_ + a.DieID) * planes_ + a.PlaneID) * blocks_ + a.BlockID) * pages_ + a.PageID);
}
Reliability::LogicalState& Reliability::Logical(stream_id_type stream, LPA_type lpa) {
    if (stream == NO_STREAM || lpa == NO_LPA || lpa >= states_.size()) throw std::runtime_error("Invalid TARF logical address");
    auto& v = logical_[stream];
    if (v.size() <= lpa) v.resize(size_t(lpa) + 1);
    return v[size_t(lpa)];
}
void Reliability::InitializePage(const Address& a, stream_id_type stream, LPA_type lpa, uint64_t origin_ns) {
    const size_t index = Index(a);
    auto& logical = Logical(stream, lpa);
    if (logical.live_index != static_cast<size_t>(-1)) throw std::runtime_error("Initial TARF LPA already exists");
    auto& p = states_[index];
    if (p.live || p.scheduled || p.generation) throw std::runtime_error("Initial TARF physical page is not erased/empty");
    const auto& initial = InitialState(a);
    p.program_ns = origin_ns; p.initial_age_days = initial.age_days;
    p.program_temp_c = initial.program_temp_c; p.stream = stream; p.lpa = lpa;
    p.generation = ++logical.latest_generation; p.live = true;
    logical.committed_generation = p.generation; logical.live_index = index;
    AddLive(index);
    ++initialized_pages;
}
void Reliability::OnProgramScheduled(const Address& a, stream_id_type stream, LPA_type lpa) {
    auto& p = states_[Index(a)];
    if (p.live || p.scheduled || p.generation) throw std::runtime_error("TARF program destination not erased/free");
    auto& logical = Logical(stream, lpa);
    p = PageState(); p.stream = stream; p.lpa = lpa;
    p.generation = ++logical.latest_generation; p.scheduled = true;
}
bool Reliability::OnProgramComplete(const Address& a, stream_id_type stream, LPA_type lpa, uint64_t now) {
    const size_t index = Index(a);
    auto& p = states_[index];
    if (!p.scheduled || p.stream != stream || p.lpa != lpa) throw std::runtime_error("TARF completion lacks matching scheduled program");
    p.scheduled = false; p.program_ns = now; p.initial_age_days = 0; p.program_temp_c = TemperatureC(now);
    ++completed_programs;
    auto& logical = Logical(stream, lpa);
    if (p.generation <= logical.committed_generation) { ++stale_program_completions; return false; }
    if (logical.live_index != static_cast<size_t>(-1)) RemoveLive(logical.live_index);
    logical.live_index = index; logical.committed_generation = p.generation; p.live = true;
    AddLive(index);
    return true;
}
void Reliability::OnErase(const Address& a) {
    Address first(a); first.PageID = 0;
    const size_t begin = Index(first);
    for (unsigned i = 0; i < pages_; ++i) {
        const auto& p = states_[begin + i];
        if (p.live || p.scheduled) {
            if (p.live) ++erased_live_pages;
            std::ostringstream message;
            message << "TARF refuses erase before replacement commit: channel=" << first.ChannelID
                    << " chip=" << first.ChipID << " die=" << first.DieID << " plane=" << first.PlaneID
                    << " block=" << first.BlockID << " page=" << i << " stream=" << p.stream
                    << " lpa=" << p.lpa << " live=" << p.live << " scheduled=" << p.scheduled
                    << " generation=" << p.generation << " program_ns=" << p.program_ns;
            throw std::runtime_error(message.str());
        }
    }
    for (unsigned i = 0; i < pages_; ++i) states_[begin + i] = PageState();
    summaries_[begin / pages_] = BlockSummary();
}
double Reliability::TemperatureC(uint64_t now) const {
    const auto& t = Config::Get().temperatures;
    auto it = std::upper_bound(t.begin(), t.end(), std::make_pair(now, std::numeric_limits<double>::infinity()));
    return (it == t.begin() ? t.front() : *--it).second;
}
double Reliability::PredictNFBC(unsigned pe, double age, double pt, double rt, unsigned chip, const std::string& variant) const {
    if (age < 0 || !std::isfinite(age) || !std::isfinite(pt) || !std::isfinite(rt)) throw std::runtime_error("Invalid NFBC input");
    const Config& c = Config::Get();
    const auto found = c.chip_parameters.find(chip);
    const auto& r = found == c.chip_parameters.end() ? c.retention : found->second;
    const double pe_term = std::log1p(double(pe));
    long double log_nfbc = static_cast<long double>(c.beta0) + static_cast<long double>(c.beta_pe) * pe_term + r.chip_bias;
    if (variant == "full" || variant == "tempdiff") log_nfbc += static_cast<long double>(c.beta_diff) * std::abs(static_cast<long double>(pt) - rt) / 100.0L;
    if (variant == "full") log_nfbc += static_cast<long double>(c.beta_common) * (((static_cast<long double>(pt) + rt) / 2.0L - 30.0L) / 100.0L);
    if (variant != "full" && variant != "tempdiff" && variant != "age") throw std::runtime_error("Unknown NFBC variant: " + variant);
    // Paper Eq. 3--5. All retention times are days; no accelerated clock or Arrhenius factor.
    // log(1 + age/t_scale), evaluated without constructing a vanishing t_scale.
    if (age > 0 && r.gamma > 0) {
        const long double x = std::log(static_cast<long double>(age)) - std::log(static_cast<long double>(r.t0days))
                            + static_cast<long double>(r.delta) * pe_term;
        const long double growth = std::max(0.0L, x) + std::log1p(std::exp(-std::abs(x)));
        log_nfbc += static_cast<long double>(r.gamma) * growth;
    }
    // Keep extreme but finite configured models usable in comparisons and JSON.
    // Saturation never turns a live page into zero risk.
    static const long double maximum_log = std::log(static_cast<long double>(std::numeric_limits<double>::max()));
    static const long double minimum_log = std::log(static_cast<long double>(std::numeric_limits<double>::denorm_min()));
    if (log_nfbc >= maximum_log) return std::numeric_limits<double>::max();
    if (log_nfbc <= minimum_log) return std::numeric_limits<double>::denorm_min();
    return static_cast<double>(std::exp(log_nfbc));
}
double Reliability::AgeDays(const PageState& p, uint64_t now) const {
    if (now < p.program_ns) throw std::runtime_error("Nonmonotonic retention clock");
    return p.initial_age_days + double(now - p.program_ns) / 86400000000000.0;
}
const PageState& Reliability::State(const Address& a) const { return states_[Index(a)]; }
bool Reliability::AgeLess(size_t base, uint32_t a, uint32_t b) const {
    const auto& left = states_[base + a]; const auto& right = states_[base + b];
    // This is only an ordering key. Actual ages retain the original ns subtraction.
    const long double initial_difference = static_cast<long double>(left.initial_age_days) - right.initial_age_days;
    const long double elapsed_difference = (static_cast<long double>(left.program_ns) - right.program_ns) / 86400000000000.0L;
    return initial_difference < elapsed_difference || (initial_difference == elapsed_difference && a > b);
}
void Reliability::AddLive(size_t index) {
    const size_t block = index / pages_, base = block * pages_;
    auto& summary = summaries_[block];
    if (++summary.live_count > pages_) throw std::runtime_error("TARF live page count exceeds block capacity");
    summary.cached_mask = 0;
    const double pt = states_[index].program_temp_c;
    auto group = std::find_if(summary.groups.begin(), summary.groups.end(), [pt](const TemperatureGroup& g) { return g.program_temp_c == pt; });
    if (group == summary.groups.end()) {
        summary.groups.emplace_back(); group = summary.groups.end() - 1; group->program_temp_c = pt;
    }
    group->oldest.push_back(static_cast<uint32_t>(index - base));
    std::push_heap(group->oldest.begin(), group->oldest.end(), [this, base](uint32_t a, uint32_t b) { return AgeLess(base, a, b); });
}
void Reliability::RemoveLive(size_t index) {
    auto& summary = summaries_[index / pages_];
    if (!states_[index].live || !summary.live_count) throw std::runtime_error("TARF removing nonlive page");
    states_[index].live = false; --summary.live_count; summary.cached_mask = 0;
}
size_t Reliability::OldestLive(size_t block, TemperatureGroup& group) const {
    const size_t base = block * pages_;
    while (!group.oldest.empty() && !states_[base + group.oldest.front()].live) {
        std::pop_heap(group.oldest.begin(), group.oldest.end(), [this, base](uint32_t a, uint32_t b) { return AgeLess(base, a, b); });
        group.oldest.pop_back();
    }
    return group.oldest.empty() ? static_cast<size_t>(-1) : base + group.oldest.front();
}
double Reliability::PageRisk(const Address& a, unsigned pe, uint64_t now, const std::string& variant) const {
    const PageState& p = State(a);
    return p.live ? PredictNFBC(pe, AgeDays(p, now), p.program_temp_c, TemperatureC(now), a.ChannelID * chips_ + a.ChipID, variant) : 0;
}
double Reliability::BlockRisk(const Address& a, unsigned pe, uint64_t now, const std::string& variant) const {
    const unsigned variant_id = variant == "full" ? 0 : variant == "tempdiff" ? 1 : variant == "age" ? 2 : 3;
    if (variant_id == 3) throw std::runtime_error("Unknown NFBC variant: " + variant);
    const size_t block = Index(a) / pages_;
    auto& summary = summaries_[block];
    if (!summary.live_count) return 0;
    if (summary.cached_ns != now || summary.cached_pe != pe) {
        summary.cached_mask = 0; summary.cached_ns = now; summary.cached_pe = pe;
    }
    const unsigned mask = 1U << variant_id;
    if (summary.cached_mask & mask) return summary.cached_risk[variant_id];
    double maximum = 0; const double rt = TemperatureC(now);
    for (auto& group : summary.groups) {
        const size_t page = OldestLive(block, group);
        if (page != static_cast<size_t>(-1))
            maximum = std::max(maximum, PredictNFBC(pe, AgeDays(states_[page], now), group.program_temp_c,
                                                  rt, a.ChannelID * chips_ + a.ChipID, variant));
    }
    summary.cached_risk[variant_id] = maximum; summary.cached_mask |= mask;
    return maximum;
}
double Reliability::BlockMaxAgeDays(const Address& a, uint64_t now) const {
    const size_t block = Index(a) / pages_; double maximum = 0;
    for (auto& group : summaries_[block].groups) {
        const size_t page = OldestLive(block, group);
        if (page != static_cast<size_t>(-1)) maximum = std::max(maximum, AgeDays(states_[page], now));
    }
    return maximum;
}
double Reliability::BlockMaxTemperatureDifference(const Address& a, uint64_t now) const {
    const size_t block = Index(a) / pages_; double maximum = 0; const double rt = TemperatureC(now);
    for (auto& group : summaries_[block].groups)
        if (OldestLive(block, group) != static_cast<size_t>(-1)) maximum = std::max(maximum, std::abs(group.program_temp_c - rt));
    return maximum;
}
uint64_t Reliability::LivePageCount(const Address& a) const {
    return summaries_[Index(a) / pages_].live_count;
}
size_t Reliability::BlockCount() const { return pages_ ? states_.size() / pages_ : 0; }
Address Reliability::BlockAddress(size_t index) const {
    if (index >= BlockCount()) throw std::runtime_error("TARF block index out of bounds");
    Address a; a.BlockID = index % blocks_; index /= blocks_;
    a.PlaneID = index % planes_; index /= planes_; a.DieID = index % dies_; index /= dies_;
    a.ChipID = index % chips_; index /= chips_; a.ChannelID = index; return a;
}
void Reliability::WriteConfiguration(std::ostream& out) const {
    const Config& c = Config::Get();
    size_t heap_capacity = 0, groups = 0, logical_bytes = 0;
    for (const auto& block : summaries_) {
        groups += block.groups.size();
        for (const auto& group : block.groups) heap_capacity += group.oldest.capacity();
    }
    for (const auto& stream : logical_) logical_bytes += stream.second.capacity() * sizeof(LogicalState);
    out << std::setprecision(17) << "{\n  \"policy\": " << quote(c.policy)
        << ",\n  \"model_calibrated\": " << (c.calibrated ? "true" : "false")
        << ",\n  \"provenance\": " << quote(c.provenance)
        << ",\n  \"model_file\": " << quote(c.model_file)
        << ",\n  \"model_equations\": \"paper Eq.2-5; natural logarithms; physical chip index=channel*chips_per_channel+chip\""
        << ",\n  \"retention_unit\": \"days\",\n  \"simulation_unit\": \"nanoseconds\",\n  \"aging_time_scale\": 1"
        << ",\n  \"block_risk_aggregation\": \"maximum of live data page predictions; mapping pages excluded\""
        << ",\n  \"block_risk_cache\": \"exact oldest-live-page heap per programming temperature; monotone retention; no age binning\""
        << ",\n  \"numeric_saturation\": \"finite double range at extreme exponents; live NFBC never rounded to zero\""
        << ",\n  \"initialization\": \"deterministic consecutive valid LPA snapshot; lazy unwritten reads use same t=0 age and temperature\""
        << ",\n  \"temperature_interpolation\": \"piecewise constant, previous sample held\""
        << ",\n  \"temperature_file\": " << quote(c.temperature_file)
        << ",\n  \"horizon_ns\": " << c.horizon_ns << ",\n  \"scan_ns\": " << c.scan_ns
        << ",\n  \"measurement_start_ns\": " << c.measurement_start_ns
        << ",\n  \"initial_pe\": " << c.initial_pe << ",\n  \"initial_age_days\": " << c.initial_age_days
        << ",\n  \"initial_temp_c\": " << c.initial_temp_c << ",\n  \"threshold\": " << c.threshold
        << ",\n  \"initial_state_file\": " << quote(c.initial_state_file)
        << ",\n  \"initial_read_coverage_file\": " << quote(c.initial_read_coverage_file)
        << ",\n  \"reject_lazy_initial_reads\": " << (c.reject_lazy_initial_reads ? "true" : "false")
        << ",\n  \"initial_state_block_order\": \"channel,chip,die,plane,block; block varies fastest\""
        << ",\n  \"initial_state_block_count\": " << c.initial_blocks.size()
        << ",\n  \"initial_pe_min\": " << c.initial_pe_min << ",\n  \"initial_pe_max\": " << c.initial_pe_max
        << ",\n  \"initial_age_days_min\": " << c.initial_age_days_min << ",\n  \"initial_age_days_max\": " << c.initial_age_days_max
        << ",\n  \"initial_temp_c_min\": " << c.initial_temp_c_min << ",\n  \"initial_temp_c_max\": " << c.initial_temp_c_max
        << ",\n  \"risk_threshold\": " << c.risk_threshold << ",\n  \"period_days\": " << c.period_days
        << ",\n  \"resource_aware\": " << (c.resource_aware ? "true" : "false")
        << ",\n  \"resource_aware_baselines\": " << (c.resource_aware_baselines ? "true" : "false")
        << ",\n  \"gc_coordination\": " << (c.gc_coordination ? "true" : "false")
        << ",\n  \"gc_respect_configured_threshold\": " << (c.gc_respect_configured_threshold ? "true" : "false")
        << ",\n  \"refresh_max_pages_per_chip\": " << c.refresh_max_pages_per_chip
        << ",\n  \"refresh_max_blocks_per_chip\": " << c.refresh_max_blocks_per_chip
        << ",\n  \"refresh_max_defer_ns\": " << c.refresh_max_defer_ns
        << ",\n  \"refresh_admission_max_defer_ns\": " << c.refresh_admission_max_defer_ns
        << ",\n  \"refresh_quiet_ns\": " << c.refresh_quiet_ns
        << ",\n  \"refresh_reserve_escape_ns\": " << c.refresh_reserve_escape_ns
        << ",\n  \"selection_rank\": " << quote(c.selection_rank)
        << ",\n  \"beta0\": " << c.beta0 << ",\n  \"beta_pe\": " << c.beta_pe
        << ",\n  \"beta_diff\": " << c.beta_diff << ",\n  \"beta_common\": " << c.beta_common
        << ",\n  \"chip_bias\": " << c.retention.chip_bias << ",\n  \"t0days\": " << c.retention.t0days
        << ",\n  \"delta\": " << c.retention.delta << ",\n  \"gamma\": " << c.retention.gamma
        << ",\n  \"chip_parameters\": {";
    bool comma = false;
    for (const auto& p : c.chip_parameters) {
        if (comma) out << ',';
        comma = true;
        out << quote(std::to_string(p.first)) << ":{\"chip_bias\":" << p.second.chip_bias << ",\"t0days\":" << p.second.t0days
            << ",\"delta\":" << p.second.delta << ",\"gamma\":" << p.second.gamma << '}';
    }
    out << "},\n  \"initialized_pages\": " << initialized_pages << ",\n  \"lazy_initial_pages\": " << lazy_initial_pages
        << ",\n  \"completed_data_programs\": " << completed_programs
        << ",\n  \"stale_program_completions\": " << stale_program_completions
        << ",\n  \"metadata_page_state_bytes\": " << states_.capacity() * sizeof(PageState)
        << ",\n  \"metadata_logical_state_bytes\": " << logical_bytes
        << ",\n  \"metadata_heap_capacity_bytes\": " << heap_capacity * sizeof(uint32_t)
        << ",\n  \"metadata_temperature_groups\": " << groups
        << ",\n  \"erased_live_pages\": " << erased_live_pages << "\n}";
}
void Reliability::WriteConfigurationFile(const std::string& path) const {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write TARF model report: " + path);
    WriteConfiguration(out); out << '\n';
}
}
