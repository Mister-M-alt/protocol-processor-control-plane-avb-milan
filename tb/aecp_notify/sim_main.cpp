// SPDX-License-Identifier: CERN-OHL-W-2.0
// AECP registry monitor lifecycle, including TIME_LIMITED row reuse.
#include <cstdint>
#include <cstdio>
#include "VKL_aecp_notify.h"
#include "verilated.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

static constexpr uint8_t OWN_TL = 0xA0;
static constexpr uint8_t OWN_MON = 0xD0;
static constexpr uint8_t REGMON_BASE = 25;
static constexpr uint8_t N_CTRL = 2;

struct Harness {
  VKL_aecp_notify* d;
  uint32_t now = 1000;

  explicit Harness(VKL_aecp_notify* dut) : d(dut) {}

  void tick() {
    d->now_ms_i = now;
    d->clk_i = 0;
    d->eval();
    d->clk_i = 1;
    d->eval();
  }

  void idle(int n = 1) {
    for (int i = 0; i < n; ++i) tick();
  }

  void register_row(uint64_t eid, uint64_t mac, bool time_limited) {
    d->rgy_state_i = 0;
    d->rgy_op_i = 0;
    d->rgy_eid_i = eid;
    d->rgy_mac_i = mac;
    d->rgy_tl_i = time_limited;
    d->rgy_req_i = 1;
    int guard = 0;
    while (guard++ < 24) {
      d->clk_i = 0;
      d->eval();
      if (!d->rgy_wait_o) break;
      tick();
    }
    CHECK(guard < 24 && d->rgy_data_o == 0,
          "registry accepts controller tuple");
    tick();
    d->rgy_req_i = 0;
    idle(2);
  }

  void complete_draw() {
    d->prng_draw_busy_i = 0;
    int guard = 0;
    while (guard++ < 16) {
      d->clk_i = 0;
      d->eval();
      if (d->prng_draw_req_o) break;
      tick();
    }
    CHECK(guard < 16, "registration requests an independent monitor draw");
    d->prng_draw_ms_i = 30000;
    d->prng_draw_valid_i = 1;
    tick();
    d->prng_draw_valid_i = 0;
    d->prng_draw_busy_i = 1;
    idle(2);
  }

  void expire(uint8_t slot, uint8_t owner) {
    d->tmr_exp_slot_i = slot;
    d->tmr_exp_owner_i = owner;
    d->tmr_exp_valid_i = 1;
    tick();
    d->tmr_exp_valid_i = 0;
  }

  bool wait_probe(uint64_t eid, uint64_t mac) {
    for (int i = 0; i < 12; ++i) {
      d->clk_i = 0;
      d->eval();
      if (d->ca_valid_o) {
        bool exact = d->ca_owner_o == 0 && d->ca_ctlr_eid_o == eid
                     && uint64_t(d->ca_mac_o) == mac;
        tick();
        return exact;
      }
      tick();
    }
    return false;
  }
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  auto* dut = new VKL_aecp_notify;

  dut->rgy_req_i = 0;
  dut->rgy_state_i = 0;
  dut->rgy_op_i = 0;
  dut->rgy_eid_i = 0;
  dut->rgy_mac_i = 0;
  dut->rgy_tl_i = 0;
  dut->ev_stri_in_i = 0;
  dut->ev_stri_out_i = 0;
  dut->ev_avb_i = 0;
  dut->ev_asp_i = 0;
  dut->ev_amap_i = 0;
  dut->ev_amap_remove_i = 0;
  dut->ev_amap_type_i = 0;
  dut->ev_amap_index_i = 0;
  dut->ev_amap_count_i = 0;
  dut->ev_amap_excl_eid_i = 0;
  dut->ev_ctr_i = 0;
  dut->ev_ctr_type_i = 0;
  dut->ev_ctr_index_i = 0;
  dut->ev_cmd_i = 0;
  dut->ev_cmd_class_i = 0;
  dut->ev_cmd_type_i = 0;
  dut->ev_cmd_index_i = 0;
  dut->ev_cmd_arg0_i = 0;
  dut->ev_cmd_arg1_i = 0;
  dut->ev_cmd_excl_eid_i = 0;
  dut->rx_cmd_valid_i = 0;
  dut->rx_cmd_eid_i = 0;
  dut->rx_cmd_mac_i = 0;
  dut->prng_draw_busy_i = 1;
  dut->prng_draw_valid_i = 0;
  dut->prng_draw_ms_i = 0;
  dut->ca_ready_i = 1;
  dut->ca_rsp_valid_i = 0;
  dut->ca_rsp_owner_i = 0;
  dut->ca_fail_valid_i = 0;
  dut->ca_fail_owner_i = 0;
  dut->uns_done_i = 1;
  dut->tmr_exp_valid_i = 0;
  dut->tmr_exp_slot_i = 0;
  dut->tmr_exp_owner_i = 0;

  Harness h(dut);
  dut->rst_n = 0;
  h.idle(4);
  dut->rst_n = 1;
  h.idle(2);

  const uint64_t EID_A = 0x1111000000000001ull;
  const uint64_t MAC_A = 0x020000000001ull;
  const uint64_t EID_B = 0x2222000000000002ull;
  const uint64_t MAC_B = 0x020000000002ull;

  h.register_row(EID_A, MAC_A, true);
  CHECK(dut->dbg_reg_cnt_o == 1, "A TIME_LIMITED row is live");
  h.complete_draw();
  h.expire(REGMON_BASE + N_CTRL, OWN_MON);
  CHECK(h.wait_probe(EID_A, MAC_A),
        "A monitor expiry launches the exact registered tuple");

  // Expire the registry row while its availability exchange is live. The
  // drain cycle must cancel owner zero before row zero becomes reusable.
  h.expire(REGMON_BASE, OWN_TL);
  bool saw_cancel = false;
  for (int i = 0; i < 8; ++i) {
    dut->clk_i = 0;
    dut->eval();
    if (dut->ca_cancel_valid_o && dut->ca_cancel_owner_o == 0)
      saw_cancel = true;
    h.tick();
  }
  CHECK(saw_cancel,
        "TIME_LIMITED drain cancels the active availability exchange");
  CHECK(dut->dbg_reg_cnt_o == 0, "expired row is removed after cancellation");

  h.register_row(EID_B, MAC_B, false);
  CHECK(dut->dbg_reg_cnt_o == 1, "expired row can be reused by a new tuple");
  h.complete_draw();
  h.expire(REGMON_BASE + N_CTRL, OWN_MON);
  CHECK(h.wait_probe(EID_B, MAC_B),
        "reused row launches only the new controller tuple");

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  delete dut;
  return fails ? 1 : 0;
}
