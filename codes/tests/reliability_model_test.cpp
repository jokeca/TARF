#include "../src/ssd/Reliability.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    assert(argc == 2);
    setenv("TARF_TEMPERATURE_FILE", argv[1], 1);
    setenv("TARF_INITIAL_AGE_DAYS", "100", 1);
    setenv("TARF_INITIAL_TEMP_C", "30", 1);
    unsetenv("TARF_MODEL_FILE");
    unsetenv("TARF_INITIAL_STATE_FILE");
    auto& r = TARF::Reliability::Get();
    r.InitializeGeometry(1, 1, 1, 1, 4, 4);
    TARF::Address source(0, 0, 0, 0, 0, 0), older(0, 0, 0, 0, 1, 0), newer(0, 0, 0, 0, 2, 0);
    r.InitializePage(source, 0, 0);
    assert(r.TemperatureC(999999999) == 30 && r.TemperatureC(1000000000) == 70);
    const double expected = std::exp(-3 + 0.3 * std::log(301.0) + 0.8 - 0.2)
        * std::sqrt(1 + 100 / (365 * std::pow(301.0, -0.1)));
    assert(std::abs(r.PredictNFBC(300, 100, 30, 70) - expected) < 1e-12);
    assert(std::abs(r.BlockMaxAgeDays(source, 86400000000000ULL) - 101) < 1e-12);
    assert(r.PredictNFBC(300, 0, 70, 70) > r.PredictNFBC(0, 0, 70, 70));
    assert(r.PredictNFBC(300, 100, 30, 70, 0, "age") < r.PredictNFBC(300, 100, 30, 70, 0, "tempdiff"));
    r.OnProgramScheduled(older, 0, 0);
    r.OnProgramScheduled(newer, 0, 0);
    assert(r.LivePageCount(source) == 1 && r.LivePageCount(older) == 0);
    bool refused = false;
    try { r.OnErase(source); } catch (const std::runtime_error&) { refused = true; }
    assert(refused); // A migration must not make source risk disappear before programming completes.
    assert(r.OnProgramComplete(newer, 0, 0, 1000000000));
    assert(!r.State(source).live && r.State(newer).live);
    assert(r.State(newer).program_temp_c == 70 && r.State(newer).initial_age_days == 0);
    assert(r.BlockMaxAgeDays(newer, 1000000000) == 0);
    assert(!r.OnProgramComplete(older, 0, 0, 2000000000));
    assert(!r.State(older).live && r.State(newer).live && r.stale_program_completions == 1);
    r.OnErase(source);
    r.OnErase(older);
    assert(r.BlockRisk(source, 301, 2000000000) == 0);
    assert(r.BlockRisk(newer, 500, 2000000000) > r.BlockRisk(newer, 0, 2000000000));
    std::cout << "reliability regression passed: equation, units, temperature steps, program commit, preserved destination P/E, stale completions, erase guard\n";
}
