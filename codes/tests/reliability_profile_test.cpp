#include "../src/ssd/Reliability.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

using TARF::Address;
static const uint64_t DAY_NS = 86400000000000ULL;

static void compare_pages(TARF::Reliability& r, uint64_t now) {
    for (size_t block = 0; block < r.BlockCount(); ++block) {
        Address a = r.BlockAddress(block);
        const unsigned pe = r.InitialState(a).pe + 7;
        uint64_t count = 0; double age = 0, temperature_difference = 0;
        for (unsigned page = 0; page < r.PagesPerBlock(); ++page) {
            a.PageID = page; const auto& state = r.State(a);
            if (!state.live) continue;
            ++count;
            age = std::max(age, state.initial_age_days + double(now - state.program_ns) / DAY_NS);
            temperature_difference = std::max(temperature_difference, std::abs(state.program_temp_c - r.TemperatureC(now)));
        }
        assert(r.LivePageCount(a) == count);
        assert(std::abs(r.BlockMaxAgeDays(a, now) - age) < 1e-12);
        assert(r.BlockMaxTemperatureDifference(a, now) == temperature_difference);
        for (const std::string variant : {"full", "tempdiff", "age"}) {
            double expected = 0;
            for (unsigned page = 0; page < r.PagesPerBlock(); ++page) {
                a.PageID = page; expected = std::max(expected, r.PageRisk(a, pe, now, variant));
            }
            const double actual = r.BlockRisk(a, pe, now, variant);
            assert(std::abs(actual - expected) <= std::max(1.0, expected) * 1e-13);
            assert(r.BlockRisk(a, pe, now, variant) == actual); // Same-clock cache hit.
        }
    }
}

int main(int argc, char** argv) {
    assert(argc == 4);
    setenv("TARF_INITIAL_STATE_FILE", argv[1], 1);
    setenv("TARF_TEMPERATURE_FILE", argv[2], 1);
    unsetenv("TARF_MODEL_FILE");
    const bool invalid = std::string(argv[3]) == "invalid";
    try {
        auto& r = TARF::Reliability::Get();
        r.InitializeGeometry(1, 1, 1, 1, 8, 16);
        if (invalid) throw std::logic_error("Invalid profile was accepted");
        const auto& c = TARF::Config::Get();
        assert(c.initial_pe_min == 100 && c.initial_pe_max == 800);
        assert(c.initial_age_days_min == 10 && c.initial_age_days_max == 17);
        assert(c.initial_temp_c_min == 0 && c.initial_temp_c_max == 70);
        for (unsigned block = 0; block < 8; ++block) {
            const Address a = r.BlockAddress(block);
            assert(r.InitialState(a).pe == (block + 1) * 100); // Free blocks retain the profile too.
        }
        for (unsigned lpa = 0; lpa < 64; ++lpa) {
            Address a(0, 0, 0, 0, lpa / 16, lpa % 16);
            r.InitializePage(a, 0, lpa);
            assert(r.State(a).initial_age_days == 10 + lpa / 16);
            assert(r.State(a).program_temp_c == r.InitialState(a).program_temp_c);
        }
        compare_pages(r, 0);
        compare_pages(r, DAY_NS); // Exactly one day, no accelerated aging.
        std::vector<unsigned> order(64);
        for (unsigned i = 0; i < order.size(); ++i) order[i] = i;
        std::mt19937 rng(321);
        uint64_t now = 0;
        for (unsigned round = 0; round < 6; ++round) {
            std::shuffle(order.begin(), order.end(), rng);
            const unsigned destination_start = round % 2 ? 0 : 4;
            const unsigned source_start = round % 2 ? 4 : 0;
            for (unsigned i = 0; i < order.size(); ++i) {
                Address a(0, 0, 0, 0, destination_start + i / 16, i % 16);
                r.OnProgramScheduled(a, 0, order[i]);
                compare_pages(r, now); // Uncommitted source remains live and risky.
                now += 100000000ULL;
                assert(r.OnProgramComplete(a, 0, order[i], now));
                compare_pages(r, now); // Invalidates source and destination caches at identical time.
            }
            for (unsigned block = source_start; block < source_start + 4; ++block) {
                Address a = r.BlockAddress(block);
                assert(r.LivePageCount(a) == 0);
                r.OnErase(a);
            }
            compare_pages(r, now + DAY_NS);
        }
        // A lazy read materializes the global t=0 profile, not age zero at read time.
        Address lazy(0, 0, 0, 0, 4, 0);
        r.InitializePage(lazy, 0, 64);
        assert(r.BlockMaxAgeDays(lazy, DAY_NS) == 15);
        std::ostringstream report; r.WriteConfiguration(report);
        assert(report.str().find("initial_state_file") != std::string::npos);
        std::cout << "profile/cache regression passed: strict profile, free-block PE, exact grouped risks, mixed histories, cache invalidation, erase/reuse, global-time lazy age; PageState bytes=" << sizeof(TARF::PageState) << '\n';
    } catch (const std::runtime_error& error) {
        if (!invalid) throw;
        std::cout << "invalid profile rejected: " << error.what() << '\n';
    }
}
