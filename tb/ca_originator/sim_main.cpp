// SPDX-License-Identifier: CERN-OHL-W-2.0
// CONTROLLER_AVAILABLE builder: owner sequence and cancellation lifecycle.
#include <array>
#include <cstdint>
#include <cstdio>
#include "VKL_aecp_ca_originator.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

struct Harness {
  VKL_aecp_ca_originator* d;
  std::array<uint8_t, 60> frame{};
  int writes = 0;
  bool pre_commit = false;
  bool pre_issue = false;

  explicit Harness(VKL_aecp_ca_originator* dut) : d(dut) {}

  static uint16_t owner_seq(unsigned owner) {
    return uint16_t(0x1100u + 0x101u * owner);
  }

  void settle() {
    d->clk_i = 0;
    d->eval();
    d->iss_seq_i = owner_seq(d->iss_owner_o);
    d->eval();
  }

  void tick() {
    settle();
    pre_commit = d->txs_wr_commit_o;
    pre_issue = d->iss_valid_o;
    if (d->txs_wr_valid_o && d->txs_wr_addr_o < frame.size()) {
      frame[d->txs_wr_addr_o] = d->txs_wr_data_o;
      writes++;
    }
    d->clk_i = 1;
    d->eval();
  }

  void reset() {
    d->rst_n = 0;
    for (int i = 0; i < 4; ++i) tick();
    d->rst_n = 1;
    tick();
  }

  void request(unsigned owner) {
    d->req_owner_i = owner;
    d->req_ctlr_eid_i = 0xA100000000000000ull | owner;
    d->req_mac_i = 0x020000000000ull | owner;
    d->req_valid_i = 1;
    tick();
    d->req_valid_i = 0;
  }

  void grant(unsigned slot) {
    d->txs_alloc_slot_i = slot;
    d->txs_alloc_gnt_i = 1;
    tick();
    d->txs_alloc_gnt_i = 0;
  }

  void write_frame() {
    for (int i = 0; i < 60; ++i) tick();
  }

  void clear_cancel() {
    d->cancel_valid_i = 0;
    tick();
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_aecp_ca_originator;
  dut->entity_id_i = 0x0011223344556677ull;
  dut->own_mac_i = 0x020000001234ull;
  dut->req_valid_i = 0;
  dut->cancel_valid_i = 0;
  dut->txs_alloc_gnt_i = 0;
  dut->iss_ready_i = 1;
  dut->iss_gnt_i = 0;
  dut->iss_id_i = 2;
  Harness h(dut);
  h.reset();

  // A request is pulsed for one cycle, then the builder waits one cycle for
  // the registered pool result. A full pool causes another isolated pulse.
  h.request(0);
  h.settle();
  CHECK(dut->txs_alloc_req_o,
        "A allocation request is asserted for one sampling cycle");
  h.tick();
  h.settle();
  CHECK(!dut->txs_alloc_req_o,
        "A allocation request drops while the registered grant is pending");
  h.tick();
  h.settle();
  CHECK(dut->txs_alloc_req_o,
        "A full-pool miss is retried without consecutive request cycles");
  dut->txs_alloc_slot_i = 0;
  dut->txs_alloc_gnt_i = 1;
  dut->cancel_owner_i = 0;
  dut->cancel_valid_i = 1;
  h.tick();
  dut->txs_alloc_gnt_i = 0;
  CHECK(dut->txs_abort_o && dut->cancel_release_valid_o
            && dut->cancel_release_slot_o == 0,
        "A retried allocation can be canceled without leaking its slot");
  h.clear_cancel();

  // The sequence source is indexed by iss_owner_o. The builder starts with
  // owner zero, then accepts owner three, so latching before owner capture
  // writes the wrong sequence into bytes 34 and 35.
  h.request(3);
  h.grant(2);
  h.write_frame();
  CHECK(h.writes == 60, "B builder writes exactly 60 bytes");
  const uint16_t seq3 = Harness::owner_seq(3);
  CHECK(h.frame[34] == uint8_t(seq3 >> 8)
            && h.frame[35] == uint8_t(seq3),
        "B frame sequence follows the requested owner");
  h.tick();
  h.settle();
  CHECK(dut->iss_valid_o && dut->iss_owner_o == 3
            && dut->iss_tx_slot_o == 2,
        "B committed frame issues for its captured owner and slot");
  dut->iss_gnt_i = 1;
  h.tick();
  dut->iss_gnt_i = 0;
  CHECK(dut->issued_valid_o && dut->issued_owner_o == 3,
        "B issue grant completes the builder");
  h.tick();

  // Cancellation before an allocation grant has no slot to release, but it
  // must still tell the shared writer arbiter to drop ownership.
  h.request(1);
  dut->cancel_owner_i = 1;
  dut->cancel_valid_i = 1;
  h.tick();
  CHECK(dut->txs_abort_o && !dut->cancel_release_valid_o
            && dut->req_ready_o,
        "B pre-grant cancellation aborts without releasing a slot");
  h.clear_cancel();

  // A registered allocation grant is visible one cycle after allocation.
  // Cancellation on that cycle must release the granted slot, not ignore it.
  h.request(2);
  dut->txs_alloc_slot_i = 4;
  dut->txs_alloc_gnt_i = 1;
  dut->cancel_owner_i = 2;
  dut->cancel_valid_i = 1;
  h.tick();
  dut->txs_alloc_gnt_i = 0;
  CHECK(dut->txs_abort_o && dut->cancel_release_valid_o
            && dut->cancel_release_slot_o == 4,
        "C cancellation on delayed grant aborts and releases that slot");
  h.clear_cancel();

  h.request(4);
  h.grant(1);
  for (int i = 0; i < 7; ++i) h.tick();
  dut->cancel_owner_i = 4;
  dut->cancel_valid_i = 1;
  h.tick();
  CHECK(dut->txs_abort_o && dut->cancel_release_valid_o
            && dut->cancel_release_slot_o == 1 && !dut->txs_wr_commit_o,
        "D write-phase cancellation aborts and releases the allocated slot");
  h.clear_cancel();

  h.request(5);
  h.grant(2);
  h.write_frame();
  h.settle();
  CHECK(dut->txs_wr_commit_o, "E builder reaches commit after byte 59");
  dut->cancel_owner_i = 5;
  dut->cancel_valid_i = 1;
  h.tick();
  CHECK(dut->txs_abort_o && dut->cancel_release_valid_o
            && dut->cancel_release_slot_o == 2 && !h.pre_commit,
        "E commit-phase cancellation suppresses commit and releases the slot");
  h.clear_cancel();

  h.request(6);
  h.grant(3);
  h.write_frame();
  h.tick();
  h.settle();
  CHECK(dut->iss_valid_o, "F committed frame reaches issue phase");
  dut->cancel_owner_i = 6;
  dut->cancel_valid_i = 1;
  h.tick();
  CHECK(dut->txs_abort_o && dut->cancel_release_valid_o
            && dut->cancel_release_slot_o == 3 && !h.pre_issue,
        "F issue-phase cancellation suppresses issue and releases the slot");
  h.clear_cancel();
  CHECK(dut->req_ready_o && !dut->txs_abort_o
            && !dut->cancel_release_valid_o,
        "G builder returns cleanly to idle after cancellation");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
