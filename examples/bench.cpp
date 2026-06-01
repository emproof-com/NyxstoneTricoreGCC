// NyxstoneTricoreGCC throughput benchmark.
//
// Measures assemble() and disassemble() throughput for Package-1 (single
// instruction) and Package-10 (ten instructions) inputs over a 1-second
// window (default).  Single-process, hot-cache.
//
// Usage: ./bench [seconds]   (default 1.0)

#include "nyxstone/nyxstone.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using clock_type = std::chrono::steady_clock;

namespace {

const std::vector<const char*> INSTRUCTIONS {
    "mov %d4, %d5", "add %d4, %d5", "sub %d4, %d5", "nop", "ret",
    "mov.aa %a4, %a5", "ld.w %d4, [%a4]", "st.w [%a4], %d4",
    "and %d4, %d5", "or %d4, %d5",
};
constexpr std::array<size_t, 2> PKG { 1, 10 };

std::string make_pkg(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) { if (i) s += "; "; s += INSTRUCTIONS[i % INSTRUCTIONS.size()]; }
    return s;
}

template <typename Fn> double run_for(double secs, Fn&& fn) {
    for (int i = 0; i < 3; ++i) fn();
    size_t count = 0;
    auto t0 = clock_type::now();
    while (true) {
        for (int i = 0; i < 10; ++i) { fn(); ++count; }
        double e = std::chrono::duration<double>(clock_type::now() - t0).count();
        if (e >= secs) return static_cast<double>(count) / e;
    }
}

std::string fmt(double v) {
    std::ostringstream s; s << std::fixed << std::setprecision(2);
    if (v >= 1e6) s << (v/1e6) << " M";
    else if (v >= 1e3) s << (v/1e3) << " k";
    else s << v << "  ";
    auto r = s.str();
    while (r.size() < 10) r.insert(r.begin(), ' ');
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    double secs = (argc > 1) ? std::strtod(argv[1], nullptr) : 1.0;
    auto created = nyxstone::NyxstoneTricoreGCC::create();
    if (!created) { std::cerr << "NyxstoneTricoreGCC::create failed: " << created.error() << "\n"; return 1; }
    auto& a = **created;

    std::cout << "NyxstoneTricoreGCC bench (target " << secs << "s/measurement)\n";
    for (size_t pkg : PKG) {
        std::string text = make_pkg(pkg);
        auto b = a.assemble(text, 0, {});
        if (!b) { std::cerr << "  [skip pkg=" << pkg << "] assemble failed: " << b.error() << "\n"; continue; }

        double r_asm = run_for(secs, [&]{ (void) a.assemble(text, 0, {}); });
        double r_dis = run_for(secs, [&]{ (void) a.disassemble_to_instructions(*b, 0, 0); });

        std::cout << "  Package " << pkg << " (" << pkg << " insns, "
                  << b->size() << " bytes)\n"
                  << "    assemble   : " << fmt(r_asm) << " ops/s   "
                  << fmt(r_asm * pkg) << " insns/s\n"
                  << "    disassemble: " << fmt(r_dis) << " ops/s   "
                  << fmt(r_dis * pkg) << " insns/s\n";
    }
    return 0;
}
