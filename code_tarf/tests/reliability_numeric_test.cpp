#include "../src/ssd/Reliability.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    assert(argc == 3);
    setenv("TARF_MODEL_FILE", argv[1], 1);
    unsetenv("TARF_INITIAL_STATE_FILE");
    auto& r = TARF::Reliability::Get();
    const unsigned pe = std::numeric_limits<unsigned>::max();
    const double fresh = r.PredictNFBC(pe, 0, 30, 30);
    const double aged = r.PredictNFBC(pe, 1, 30, 30);
    assert(std::isfinite(fresh) && fresh > 0);
    assert(std::isfinite(aged) && aged >= fresh);
    if (std::string(argv[2]) == "high") {
        const double expected = std::exp(-3 + 0.3 * std::log1p(double(pe)));
        assert(std::abs(fresh / expected - 1) < 1e-12); // age zero must not become 0/0.
        assert(aged == std::numeric_limits<double>::max());
    } else {
        assert(fresh == std::numeric_limits<double>::denorm_min());
        assert(aged == fresh); // An underflowing live-page risk remains strictly positive.
    }
    std::cout << "numeric retention regression passed: " << argv[2] << '\n';
}
