#include "../src/ssd/Reliability.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sys/resource.h>

int main(int argc, char** argv) {
    const unsigned scans = argc > 1 ? std::stoul(argv[1]) : 100;
    const unsigned pages = argc > 2 ? std::stoul(argv[2]) : 2112;
    typedef std::chrono::steady_clock Clock;
    auto begin = Clock::now();
    auto& r = TARF::Reliability::Get();
    r.InitializeGeometry(8, 2, 2, 4, 128, pages);
    LPA_type lpa = 0;
    for (size_t block = 0; block < r.BlockCount(); ++block) {
        auto address = r.BlockAddress(block);
        for (unsigned page = 0; page < pages * 8 / 10; ++page) {
            address.PageID = page; r.InitializePage(address, 0, lpa++);
        }
    }
    std::cout << std::setprecision(10) << "{\"physical_pages\":" << r.BlockCount() * pages
              << ",\"live_pages\":" << lpa << ",\"blocks\":" << r.BlockCount()
              << ",\"initialization_seconds\":" << std::chrono::duration<double>(Clock::now() - begin).count();
    volatile double checksum = 0;
    for (const std::string policy : {"tarf", "age", "tempdiff", "periodic"}) {
        begin = Clock::now();
        for (unsigned scan = 0; scan < scans; ++scan) {
            const uint64_t now = uint64_t(scan) * 10000000000ULL;
            for (size_t block = 0; block < r.BlockCount(); ++block) {
                const auto address = r.BlockAddress(block);
                const auto pe = r.InitialState(address).pe;
                if (!r.LivePageCount(address)) continue;
                checksum += r.BlockRisk(address, pe, now);
                if (policy == "age" || policy == "tempdiff") checksum += r.BlockRisk(address, pe, now, policy);
                else if (policy == "periodic") checksum += r.BlockMaxAgeDays(address, now);
                else checksum += r.BlockRisk(address, pe, now); // Full trigger reuses the same-clock risk.
            }
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
        std::cout << ",\"" << policy << "_seconds_per_scan\":" << elapsed / scans;
    }
    struct rusage usage; getrusage(RUSAGE_SELF, &usage);
    std::cout << ",\"max_rss_kib\":" << usage.ru_maxrss << ",\"checksum\":" << checksum << "}\n";
    return std::isfinite(checksum) ? 0 : 1;
}
