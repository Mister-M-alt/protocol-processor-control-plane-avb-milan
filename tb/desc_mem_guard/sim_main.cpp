// SPDX-License-Identifier: CERN-OHL-W-2.0
// An in-order memory FIFO that accepts requests while an older burst is owed.
// Expectations are image bytes and accepted/terminal transaction counts,
// independent of the store and guard implementation.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include "Vdesc_mem_guard_wrap.h"
#include "../common/verilator_harness.hpp"

static int checks = 0, fails = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; \
    std::printf("FAIL: " __VA_ARGS__); std::puts(""); } } while (0)

constexpr uint32_t BASE = 0x20000000;
constexpr unsigned TIMEOUT = 4096; // Uncompressed store default.
constexpr unsigned BOUND = TIMEOUT + 64; // Scan + request/answer overhead.
constexpr uint64_t INPUT0 = 0x0005000011223344ull;
constexpr uint64_t INPUT1 = 0x5566778899aabbccull;
constexpr uint64_t OUTPUT0 = 0x00060000deadbeefull;
constexpr uint64_t OUTPUT1 = 0xcafef00d01234567ull;

struct Answer {
    bool valid = false, err = false;
    uint64_t data = 0;
    unsigned cycles = 0;
};

struct Harness {
    milan::tb::Model<Vdesc_mem_guard_wrap> model;
    Vdesc_mem_guard_wrap& d = *model.get();
    // Header, two index entries and two unnamed 16-byte marker descriptors.
    // This tests transport identity; descriptor interiors are opaque to the store.
    std::array<uint64_t, 12> image{{
        0x41454d4900010001ull, 0x0002000000000020ull,
        0x0000006000000060ull, 0x0010000000000000ull,
        0x0000000500010010ull, 0x00000040ffff0010ull,
        0x0000000600010010ull, 0x00000050ffff0010ull,
        INPUT0, INPUT1, OUTPUT0, OUTPUT1
    }};
    struct Burst {
        uint32_t addr;
        unsigned beats, index;
        uint64_t due;
        bool stuck, error;
    };
    std::deque<Burst> fifo;
    uint64_t cycle = 0, accepted = 0, terminal = 0;
    uint64_t delayed_accept = 0, delayed_first = 0, delayed_end = 0;
    unsigned delay_next = 24;
    bool stuck_next = false, error_next = false;
    unsigned held_cycles = 0, overlap_accepts = 0;

    Harness() {
        uint32_t sum = 0;
        for (unsigned i = 0; i < 4; ++i) {
            sum += uint32_t(image[i] >> 32);
            sum += uint32_t(image[i]);
        }
        image[3] |= uint32_t(~sum);
    }

    Answer tick() {
        d.clk_i = 0;
        d.mem_req_ready_i = 1; // CDC request FIFO, not an IDLE-only bridge.
        d.mem_rsp_valid_i = 0;
        d.mem_rsp_data_i = 0;
        d.mem_rsp_last_i = 0;
        d.mem_rsp_err_i = 0;
        if (!fifo.empty() && cycle >= fifo.front().due) {
            const auto& b = fifo.front();
            // A stuck burst emits one nonterminal beat, then goes silent.
            if (!b.stuck || b.index == 0) {
                d.mem_rsp_valid_i = 1;
                const unsigned lane = (b.addr - BASE) / 8 + b.index;
                d.mem_rsp_data_i = lane < image.size() ? image[lane] : 0xa5a5a5a5a5a5a5a5ull;
                d.mem_rsp_last_i = !b.stuck && b.index + 1 == b.beats;
                d.mem_rsp_err_i = b.error;
            }
        }
        d.eval();
        Answer a{bool(d.st_rvalid_o), bool(d.st_err_o), d.st_rdata_o, 0};
        const bool take_req = d.mem_req_valid_o && d.mem_req_ready_i && d.rst_n;
        if (d.store_req_o && !d.store_ready_o) ++held_cycles;
        if (take_req && !fifo.empty()) ++overlap_accepts;
        if (d.mem_rsp_valid_i && d.mem_rsp_ready_o) {
            auto& b = fifo.front();
            if (b.addr == BASE + 80 && delayed_accept) {
                if (!delayed_first) delayed_first = cycle;
                if (d.mem_rsp_last_i || d.mem_rsp_err_i) delayed_end = cycle;
            }
            if (d.mem_rsp_last_i || d.mem_rsp_err_i) {
                fifo.pop_front();
                ++terminal;
            } else {
                ++b.index;
                b.due = cycle + 3; // Nonzero inter-beat gaps.
            }
        }
        if (take_req) {
            fifo.push_back({d.mem_req_addr_o, unsigned(d.mem_req_beats_o), 0,
                            cycle + delay_next, stuck_next, error_next});
            if (delay_next == 6000) delayed_accept = cycle;
            delay_next = 24;
            stuck_next = error_next = false;
            ++accepted;
        }
        d.clk_i = 1;
        d.eval();
        ++cycle;
        return a;
    }

    void idle(unsigned n) { while (n--) tick(); }

    void boot() {
        d.unit_i = 0;
        d.rst_n = 0;
        d.store_rst_n = 1;
        d.st_req_i = 0;
        idle(4);
        d.rst_n = 1;
        for (unsigned i = 0; i < BOUND && !d.img_valid_o; ++i) tick();
        CHECK(d.img_valid_o, "boot image did not validate");
    }

    Answer read(uint32_t addr, uint64_t data = 0) {
        d.st_req_i = 1;
        d.st_addr_i = addr;
        d.st_wdata_i = data;
        Answer a;
        for (unsigned n = 1; n <= BOUND; ++n) {
            a = tick();
            a.cycles = n;
            if (a.valid) break;
        }
        d.st_req_i = 0;
        tick();
        CHECK(a.valid, "state read %05x never answered within %u cycles", addr, BOUND);
        return a;
    }
    Answer locate(unsigned type) { return read(0xf0000, uint64_t(type) << 16); }

    void check_input(const char* tag) {
        const auto first = read(0);
        const auto second = read(8);
        std::printf("%s bytes: %016llx %016llx; want %016llx %016llx\n", tag,
                    (unsigned long long)first.data, (unsigned long long)second.data,
                    (unsigned long long)INPUT0, (unsigned long long)INPUT1);
        CHECK(!first.err && !second.err && first.data == INPUT0 && second.data == INPUT1,
              "%s late_beats_never_served: STREAM_INPUT received another burst's bytes", tag);
    }
};

static void late_case() {
    Harness h;
    h.boot();
    h.delay_next = 6000;
    const auto late = h.locate(6);
    CHECK(late.err && late.cycles >= TIMEOUT, "late fetch did not hit the store watchdog");
    CHECK(!h.fifo.empty() && !h.delayed_first, "late burst was not still owed at timeout");
    const auto next = h.locate(5);
    CHECK(next.err, "documented post-timeout locate should report an error");
    const uint64_t presented = h.cycle;
    const auto third = h.locate(5);
    CHECK(presented < h.delayed_first, "third locate was not presented while old burst was owed");
    CHECK(!third.err, "finite late burst should drain in time to serve the third locate");
    h.check_input("third locate");
    h.idle(80); // Drain queued responses even in the failing baseline/mutant.
    CHECK(h.fifo.empty(), "finite burst did not finish");
    const auto after = h.locate(5);
    CHECK(!after.err, "locate after late burst did not recover");
    h.check_input("after drain");
    std::printf("late case completed: accept=%llu first=%llu end=%llu third-presented=%llu "
                "held=%u overlap-accepts=%u timeout-answer=%u third-answer=%u\n",
                (unsigned long long)h.delayed_accept, (unsigned long long)h.delayed_first,
                (unsigned long long)h.delayed_end, (unsigned long long)presented,
                h.held_cycles, h.overlap_accepts, late.cycles, third.cycles);
}

static void stuck_case() {
    Harness h;
    h.boot();
    h.stuck_next = true;
    const auto first = h.locate(6);
    CHECK(first.err, "unterminated fetch did not answer an error");
    CHECK(h.d.debt_o && h.fifo.size() == 1 && h.fifo.front().index == 1,
          "stuck burst premise: a consumed nonterminal beat must leave debt");
    const auto accepted = h.accepted;
    unsigned longest = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const auto a = h.locate(5);
        if (a.cycles > longest) longest = a.cycles;
        CHECK(a.err && a.cycles <= BOUND, "stuck locate %u was not a bounded error", i);
        CHECK(h.d.debt_o && h.accepted == accepted && !h.d.mem_req_valid_o,
              "stuck locate %u issued a request or forgot debt", i);
    }
    CHECK(h.held_cycles >= TIMEOUT && h.overlap_accepts == 0,
          "stuck memory did not hold the store requests");
    std::printf("stuck case completed: 5 later locates answered errors; max=%u bound=%u "
                "held=%u accepted=%llu terminal=%llu debt=%u\n", longest, BOUND,
                h.held_cycles, (unsigned long long)h.accepted,
                (unsigned long long)h.terminal, unsigned(h.d.debt_o));

    // This proves the independent reset seam only, not a D3 rollback writer.
    h.d.store_rst_n = 0;
    h.idle(8);
    CHECK(h.d.debt_o && h.accepted == accepted, "store-only reset cleared memory debt");
    h.fifo.front().stuck = false;
    h.idle(8);
    CHECK(!h.d.debt_o && h.fifo.empty(), "terminal beat did not drain during store reset");
    h.d.store_rst_n = 1;
    h.idle(200);
    CHECK(h.d.img_valid_o, "store did not re-walk after debt drained");
    CHECK(!h.locate(5).err, "locate after independent store reset failed");
    h.check_input("after store reset");

    h.stuck_next = true;
    CHECK(h.locate(6).err && h.d.debt_o, "hard-reset premise lacks debt");
    h.fifo.clear(); // Hard reset must also flush the integrator's memory path.
    h.d.rst_n = 0;
    h.idle(4);
    CHECK(!h.d.debt_o, "hard reset did not clear debt");
    h.d.rst_n = 1;
    h.idle(200);
    CHECK(!h.locate(5).err, "hard reset did not recover service");
    h.check_input("after hard reset");
}

static void terminal_error_case() {
    Harness h;
    h.boot();
    h.error_next = true; // err on beat zero of two, without last.
    CHECK(h.locate(6).err, "terminal memory error did not reach the store");
    CHECK(!h.d.debt_o && h.fifo.empty(), "err without last did not clear debt");
    CHECK(!h.locate(5).err, "request after terminal error remained held");
    h.check_input("after terminal error");
}

static void handshake_case() {
    milan::tb::Model<Vdesc_mem_guard_wrap> model;
    auto& d = *model.get();
    auto eval = [&] { d.clk_i = 0; d.eval(); };
    auto tick = [&] { eval(); d.clk_i = 1; d.eval(); };
    d.unit_i = 1;
    d.store_rst_n = 0;
    d.rst_n = 0;
    tick();
    CHECK(!d.debt_o, "unit reset debt");
    d.rst_n = 1;
    d.unit_req_valid_i = 1;
    d.unit_req_addr_i = 0x23456780;
    d.unit_req_beats_i = 511;
    d.mem_req_ready_i = 0;
    tick();
    CHECK(!d.debt_o && !d.store_ready_o && d.mem_req_valid_o,
          "request without ready must not create debt");
    CHECK(d.mem_req_addr_o == 0x23456780 && d.mem_req_beats_o == 511,
          "request payload was not passed through");
    d.mem_req_ready_i = 1;
    tick();
    CHECK(d.debt_o && !d.store_ready_o && !d.mem_req_valid_o,
          "accepted request must hold both request faces");
    d.unit_rsp_ready_i = 1;
    d.mem_rsp_data_i = OUTPUT0;
    d.mem_rsp_last_i = 1;
    d.mem_rsp_valid_i = 0;
    tick();
    CHECK(d.debt_o, "last without valid cleared debt");
    d.mem_rsp_valid_i = 1;
    d.mem_rsp_last_i = 0;
    tick();
    CHECK(d.debt_o && d.unit_rsp_valid_o && d.unit_rsp_data_o == OUTPUT0,
          "nonterminal beat must pass without clearing debt");
    d.unit_rsp_ready_i = 0;
    d.mem_rsp_last_i = 1;
    tick();
    CHECK(d.debt_o && !d.mem_rsp_ready_o && d.unit_rsp_last_o,
          "backpressured last must pass but not clear debt");
    d.unit_rsp_ready_i = 1;
    eval();
    CHECK(!d.mem_req_valid_o && !d.store_ready_o,
          "request must remain held through the terminal cycle");
    tick();
    CHECK(!d.debt_o && d.mem_req_valid_o && d.store_ready_o,
          "consumed last must release request on the next cycle");
    d.mem_rsp_valid_i = 0;
    tick(); // Next request accepted; no terminal beat this cycle.
    CHECK(d.debt_o, "following request did not establish debt");
    d.unit_req_valid_i = 0;
    d.mem_rsp_last_i = 0;
    d.mem_rsp_err_i = 1;
    d.mem_rsp_valid_i = 0;
    tick();
    CHECK(d.debt_o, "err without valid cleared debt");
    d.mem_rsp_valid_i = 1;
    d.unit_rsp_ready_i = 0;
    tick();
    CHECK(d.debt_o && d.unit_rsp_err_o && d.unit_rsp_valid_o
          && d.unit_rsp_data_o == OUTPUT0 && !d.mem_rsp_ready_o,
          "backpressured error must pass but not clear debt");
    d.unit_rsp_ready_i = 1;
    tick();
    CHECK(!d.debt_o, "consumed err without last did not clear debt");
    // The response path is transparent even with no debt.
    d.mem_rsp_data_i = INPUT1;
    eval();
    CHECK(d.unit_rsp_valid_o && d.unit_rsp_err_o && !d.unit_rsp_last_o
          && d.unit_rsp_data_o == INPUT1 && d.mem_rsp_ready_o,
          "response path filtered a beat while no debt was owed");
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    late_case();
    if (argc == 1 || std::strcmp(argv[1], "--late-only") != 0) {
        stuck_case();
        terminal_error_case();
        handshake_case();
    }
    std::printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
    return fails ? 1 : 0;
}
