// SPDX-License-Identifier: CERN-OHL-W-2.0
// srp_stream_fsms suite — independent expectations, never DUT logic.
//
// Walks BOTH stream-FSM modules' applicants through an independently
// transcribed IEEE 802.1Q-2018 Table 10-3 (all rx events x all 12 states x
// both operPointToPointMAC arms, plus every tx!/txLA! row with its message),
// the Table 10-4 registrar with the Milan Δ13 deviation (rLv = immediate
// IN->MT, LeaveAll cycle = the only LV path, aged by T-MRP-LEAVE), the
// F10.4 talker declare/Ready/ACTIVE/admission-failure walk, the F10.5
// listener match / in-place swap / unregister walks, per-application
// LeaveAll isolation, the acc_latency latch and the VLAN user handshakes.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Vsrp_stream_fsms_wrap.h"
#include "verilated.h"
#include "../common/verilator_harness.hpp"
#include "../common/req_tag.hpp"

#define CHECK(cond, ...) do { \
  ++checks; \
  if (!(cond)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// ---------------------------------------------------------------------------
// Independent transcription of IEEE 802.1Q-2018 Table 10-3 (applicant).
// States (columns), events (rows). H = no transition ("--" cell).
// ---------------------------------------------------------------------------
// Applicant states, in Table 10-3 column order. Plain integers: they index
// APP_TBL, SN and the DUT's packed t_dbg_app_state_o nibbles.
constexpr int VO = 0;
constexpr int VP = 1;
constexpr int VN = 2;
constexpr int AN = 3;
constexpr int AA = 4;
constexpr int QA = 5;
constexpr int LA = 6;
constexpr int AO = 7;
constexpr int QO = 8;
constexpr int AP = 9;
constexpr int QP = 10;
constexpr int LO = 11;
constexpr int NSTATES = 12;
// Applicant events, in Table 10-3 row order. eRNew..eRLA are ordered so that
// eRNew + <srp attribute-event code> names the received event.
constexpr int eNew = 0;
constexpr int eJoin = 1;
constexpr int eLv = 2;
constexpr int eRNew = 3;
constexpr int eRJoinIn = 4;
constexpr int eRIn = 5;
constexpr int eRJoinMt = 6;
constexpr int eRMt = 7;
constexpr int eRLv = 8;
constexpr int eRLA = 9;
constexpr int ePer = 10;
constexpr int eTx = 11;
constexpr int eTxLA = 12;
constexpr int NEVENTS = 13;
constexpr int H = -1;
constexpr int APP_TBL[NEVENTS][NSTATES] = {
  //            VO  VP  VN  AN  AA  QA  LA  AO  QO  AP  QP  LO
  /*New!    */ {VN, VN,  H,  H, VN, VN, VN, VN, VN, VN, VN, VN},
  /*Join!   */ {VP,  H,  H,  H,  H,  H, AA, AP, QP,  H,  H, VP},
  /*Lv!     */ { H, VO, LA, LA, LA, LA,  H,  H,  H, AO, QO,  H},
  /*rNew!   */ { H,  H,  H,  H,  H,  H,  H,  H,  H,  H,  H,  H},
  /*rJoinIn!*/ {AO, AP,  H,  H, QA,  H,  H, QO,  H, QP,  H,  H}, // notes 4
  /*rIn!    */ { H,  H,  H,  H, QA,  H,  H,  H,  H,  H,  H,  H}, // note 5
  /*rJoinMt!|rMt!*/
               { H,  H,  H,  H,  H, AA,  H,  H, AO,  H, AP, VO},
  /*rMt! (same row)*/
               { H,  H,  H,  H,  H, AA,  H,  H, AO,  H, AP, VO},
  /*rLv!|rLA!*/{LO,  H,  H, VN, VP, VP,  H, LO, LO, VP, VP,  H},
  /*rLA! (same row)*/
               {LO,  H,  H, VN, VP, VP,  H, LO, LO, VP, VP,  H},
  /*periodic!*/{ H,  H,  H,  H,  H, AA,  H,  H,  H,  H, AP,  H},
  /*tx!     */ { H, AA, AN, AA, QA,  H, VO,  H,  H, QA,  H, VO}, // AN: note 8
  /*txLA!   */ {LO, AA, AN, QA, QA,  H, LO, LO, LO, QA, QA,  H},
};

static int app_next(int st, int ev, bool reg_in, bool p2p) {
  if (ev == eRJoinIn && p2p && (st == VO || st == VP)) return st; // note 4
  if (ev == eRIn) return (p2p && st == AA) ? QA : st;             // note 5
  if (ev == eTx && st == AN) return reg_in ? QA : AA;             // note 8
  int nx = APP_TBL[ev][st];
  return (nx == H) ? st : nx;
}

// message sent at a tx opportunity: srp attribute-event code, or -1.
// sN=New(0), sJ=JoinIn(1)/JoinMt(3) by reg_in, sL=Lv(5), s=In(2)/Mt(4).
// Optional [s]/[sJ]/[sL] encodings are never sent by the DUT (banner).
static int app_msg(int st, bool txla, bool reg_in) {
  const int sj = reg_in ? 1 : 3;
  const int se = reg_in ? 2 : 4;
  if (!txla) {
    switch (st) {
      case VP: case AA: case AP: return sj;
      case VN: case AN:          return 0;
      case LA:                   return 5;
      case LO:                   return se;
      default:                   return -1;
    }
  }
  switch (st) {
    case VP:                            return se;
    case VN: case AN:                   return 0;
    case AA: case QA: case AP: case QP: return sj;
    default:                            return -1;
  }
}

constexpr const char* SN[NSTATES] =
  {"VO","VP","VN","AN","AA","QA","LA","AO","QO","AP","QP","LO"};
constexpr const char* EN[NEVENTS] =
  {"New!","Join!","Lv!","rNew!","rJoinIn!","rIn!","rJoinMt!","rMt!","rLv!",
   "rLA!","periodic!","tx!","txLA!"};

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------
constexpr uint64_t MAC  = 0x02AABBCCDDEEull;
constexpr uint32_t NOW  = 100000;
constexpr uint32_t LEAVE_MS = 5000;

// N_STREAMS_P of the wrapper: every per-slot capture array is this wide.
constexpr int kStreams = 8;
// The MRPDU FirstValue busses are 272 bits, i.e. 34 bytes, wide.
constexpr int kFirstValueBytes = 34;
constexpr int kFirstValueMsb = 271;
// Guard on the wait for both modules to report a completed transmit
// opportunity: far more clock steps than a txop needs, never reached in a
// passing run.
constexpr int kTxopPollSteps = 300;

// received-LeaveAll lanes, one per MSRP AttributeType (lane = type - 1)
constexpr unsigned kLaTalkerAdv    = 1u << 0;
constexpr unsigned kLaTalkerFailed = 1u << 1;
constexpr unsigned kLaListener     = 1u << 2;
constexpr unsigned kLaDomain       = 1u << 3;
constexpr unsigned kAllLanes       = 0xFu;

struct Push {
  uint8_t type;
  uint8_t code;
  uint8_t fp;
  uint8_t val[kFirstValueBytes];
};
struct Arm  { bool cancel; uint8_t slot; uint8_t owner; uint32_t deadline; };
struct VOp  { bool join; uint16_t vid; };

template <std::size_t N>
static uint64_t wget(const VlWide<N>& w, int lo, int width) {
  uint64_t v = 0;
  for (int i = width - 1; i >= 0; --i) {
    int b = lo + i;
    v = (v << 1) | ((w[b >> 5] >> (b & 31)) & 1u);
  }
  return v;
}

struct Hn {
  Vsrp_stream_fsms_wrap* d;
  std::vector<Push> t_push;
  std::vector<Push> l_push;
  std::vector<Arm>  t_arm;
  std::vector<Arm>  l_arm;
  std::vector<VOp>  t_vop;
  std::vector<VOp>  l_vop;
  int t_chg[kStreams] = {0};
  int l_reg[kStreams] = {0};
  int l_unreg[kStreams] = {0};
  int l_fchg[kStreams] = {0};
  bool t_uready = true;
  bool l_uready = true;

  explicit Hn(Vsrp_stream_fsms_wrap* dd) : d(dd) {}

  void clear_logs() {
    t_push.clear(); l_push.clear(); t_arm.clear(); l_arm.clear();
    t_vop.clear(); l_vop.clear();
    memset(t_chg, 0, sizeof t_chg);
    memset(l_reg, 0, sizeof l_reg);
    memset(l_unreg, 0, sizeof l_unreg);
    memset(l_fchg, 0, sizeof l_fchg);
  }

  void step() {
    d->now_ms_i = NOW;
    d->t_ev_ready_i = 1; d->l_ev_ready_i = 1;
    d->t_user_ready_i = t_uready; d->l_user_ready_i = l_uready;
    d->clk_i = 0; d->eval();
    // harvest (all DUT outputs are registered or stable-from-registers)
    if (d->t_ev_valid_o) {
      Push p;
      p.type = d->t_ev_attr_type_o;
      p.code = d->t_ev_event_o;
      p.fp = d->t_ev_fourpack_o;
      for (int i = 0; i < kFirstValueBytes; ++i)
        p.val[i] = static_cast<uint8_t>(
            wget(d->t_ev_value_o, kFirstValueMsb - 8 * i - 7, 8));
      t_push.push_back(p);
    }
    if (d->l_ev_valid_o) {
      Push p;
      p.type = d->l_ev_attr_type_o;
      p.code = d->l_ev_event_o;
      p.fp = d->l_ev_fourpack_o;
      for (int i = 0; i < kFirstValueBytes; ++i)
        p.val[i] = static_cast<uint8_t>(
            wget(d->l_ev_value_o, kFirstValueMsb - 8 * i - 7, 8));
      l_push.push_back(p);
    }
    if (d->t_arm_valid_o)
      t_arm.push_back({static_cast<bool>(d->t_arm_cancel_o),
                       static_cast<uint8_t>(d->t_arm_slot_o),
                       static_cast<uint8_t>(d->t_arm_owner_o),
                       static_cast<uint32_t>(d->t_arm_deadline_ms_o)});
    if (d->l_arm_valid_o)
      l_arm.push_back({static_cast<bool>(d->l_arm_cancel_o),
                       static_cast<uint8_t>(d->l_arm_slot_o),
                       static_cast<uint8_t>(d->l_arm_owner_o),
                       static_cast<uint32_t>(d->l_arm_deadline_ms_o)});
    if (d->t_user_valid_o && t_uready)
      t_vop.push_back({static_cast<bool>(d->t_user_join_o),
                       static_cast<uint16_t>(d->t_user_vid_o)});
    if (d->l_user_valid_o && l_uready)
      l_vop.push_back({static_cast<bool>(d->l_user_join_o),
                       static_cast<uint16_t>(d->l_user_vid_o)});
    for (int s = 0; s < kStreams; ++s) {
      if ((d->t_lstn_reg_change_o >> s) & 1) ++t_chg[s];
      if ((d->l_evt_tk_registered_o >> s) & 1) ++l_reg[s];
      if ((d->l_evt_tk_unregistered_o >> s) & 1) ++l_unreg[s];
      if ((d->l_evt_tk_fail_chg_o >> s) & 1) ++l_fchg[s];
    }
    d->clk_i = 1; d->eval();
    // one-shot inputs auto-clear
    d->evt_valid_i = 0; d->join_tick_i = 0; d->periodic_tick_i = 0;
    d->leaveall_rx_i = 0; d->leaveall_own_i = 0; d->exp_valid_i = 0;
    d->gate_valid_i = 0; d->ctl_valid_i = 0;
  }

  void idle(int n) { for (int i = 0; i < n; ++i) step(); }

  void reset() {
    d->rst_n = 0; d->p2p_i = 1; d->own_mac_i = MAC;
    d->sr_admitted_i = 0xFF;
    // the VLAN view: entry 0 holds VID 5, entry 1 VID0, both transmitted
    d->t_vid_sent_i = 0x3; d->t_vid_val_i = (2ull << 12) | 5ull;   // 2 = VID0
    d->evt_valid_i = 0; d->join_tick_i = 0; d->periodic_tick_i = 0;
    d->leaveall_rx_i = 0; d->leaveall_own_i = 0; d->exp_valid_i = 0;
    d->gate_valid_i = 0; d->ctl_valid_i = 0; d->exp_slot_i = 0;
    t_uready = true; l_uready = true;
    idle(3);
    d->rst_n = 1;
    idle(2);
    clear_logs();
  }

  void gate(bool open, int src, uint64_t sid, uint64_t da, uint16_t vid,
            uint32_t lat = 500) {
    d->gate_valid_i = 1; d->gate_open_i = open; d->gate_src_i = src;
    d->gate_stream_id_i = sid; d->gate_da_i = da; d->gate_vid_i = vid;
    d->gate_max_frame_i = 217; d->gate_max_interval_i = 1;
    d->gate_prio_i = 3; d->gate_rank_i = 1; d->gate_acc_lat_i = lat;
    step(); idle(2);
  }

  void ctl(bool settle, int sink, uint64_t sid, uint64_t da, uint16_t vid) {
    d->ctl_valid_i = 1; d->ctl_settle_i = settle; d->ctl_sink_i = sink;
    d->ctl_stream_id_i = sid; d->ctl_da_i = da; d->ctl_vid_i = vid;
    step(); idle(2);
  }

  void inject(bool msrp, uint8_t type, uint64_t sid, uint64_t da,
              uint16_t vid, uint8_t ev, uint8_t fp = 0, uint32_t lat = 0,
              uint64_t sysid = 0, uint8_t fcode = 0) {
    d->evt_valid_i = 1; d->evt_msrp_i = msrp; d->evt_attr_type_i = type;
    d->evt_stream_id_i = sid; d->evt_da_i = da; d->evt_vid_i = vid;
    d->evt_mrp_event_i = ev; d->evt_fourpacked_i = fp;
    d->evt_acc_latency_i = lat; d->evt_failure_system_id_i = sysid;
    d->evt_failure_code_i = fcode;
    step(); idle(3);
  }

  // a received LeaveAll on the given MSRP AttributeType lanes (bit t-1 =
  // type t); la_rx() is one on every type, the shape of a conformant
  // peer's LeaveAll MRPDU (802.1Q-2014 §10.7.5.20 NOTE)
  void la_rx_lanes(unsigned lanes) { d->leaveall_rx_i = lanes; step(); idle(3); }
  void la_rx()  { la_rx_lanes(kAllLanes); }
  void la_own() { d->leaveall_own_i = 1; step(); idle(2); }
  void expire(int slot) { d->exp_valid_i = 1; d->exp_slot_i = slot; step(); idle(2); }

  bool tick() {
    d->join_tick_i = 1; step();
    bool td = false;
    bool ld = false;
    for (int i = 0; i < kTxopPollSteps && !(td && ld); ++i) {
      td |= (d->t_txop_done_o != 0); ld |= (d->l_txop_done_o != 0);
      if (td && ld) break;
      step();
      td |= (d->t_txop_done_o != 0); ld |= (d->l_txop_done_o != 0);
    }
    idle(2);
    return td && ld;
  }

  void ptick() { d->periodic_tick_i = 1; step(); idle(2); }

  int t_app(int s) const { return (d->t_dbg_app_state_o >> (4 * s)) & 15; }
  int l_app(int s) const { return (d->l_dbg_app_state_o >> (4 * s)) & 15; }
  int t_reg(int s) const { return (d->t_dbg_reg_state_o >> (2 * s)) & 3; }
  int l_regst(int s) const { return (d->l_dbg_reg_state_o >> (2 * s)) & 3; }
  int t_lstn(int s) const { return (d->t_lstn_reg_state_o >> (2 * s)) & 3; }
  int t_decl(int s) const { return (d->t_tk_decl_state_o >> (2 * s)) & 3; }
  int l_tkreg(int s) const { return (d->l_tk_reg_state_o >> (2 * s)) & 3; }
  int l_decl(int s) const { return (d->l_lstn_decl_state_o >> (2 * s)) & 3; }
  uint32_t l_lat(int s) const {
    return static_cast<uint32_t>(wget(d->l_acc_latency_o, 32 * s, 32));
  }
  uint64_t l_fbridge(int s) const { return wget(d->l_msrp_fail_bridge_o, 64 * s, 64); }
  uint8_t  l_fcode(int s) const { return (d->l_msrp_fail_code_o >> (8 * s)) & 0xFF; }
  uint64_t t_fbridge(int s) const { return wget(d->t_msrp_fail_bridge_o, 64 * s, 64); }
  uint8_t  t_fcode(int s) const { return (d->t_msrp_fail_code_o >> (8 * s)) & 0xFF; }
};

// stream identities used by the recipes
constexpr uint64_t SID0 = 0x02AABBCCDDEE0001ull;
constexpr uint64_t DA0  = 0x91E0F0001234ull;
constexpr uint16_t VID0 = 2;

// applicant-recipe drivers: land source/sink 0 in state S, tracking the
// model alongside; each returns the model state for a final CHECK
static int t_goto(Hn& h, int S) {
  int m = VO;
  h.gate(true, 0, SID0, DA0, VID0);                 // New!
  m = app_next(m, eNew, false, true);
  auto closev = [&]() { h.gate(false, 0, SID0, DA0, VID0); m = app_next(m, eLv, false, true); };
  auto tickv  = [&]() { h.tick(); m = app_next(m, eTx, false, true); };
  auto rx     = [&](uint8_t code, bool p2p) {
    h.d->p2p_i = p2p;
    h.inject(true, 1, SID0, DA0, VID0, code);
    m = app_next(m, eRNew + code, false, p2p);
    h.d->p2p_i = 1;
  };
  switch (S) {
    case VN: break;
    case AN: tickv(); break;
    case AA: tickv(); tickv(); break;
    case QA: tickv(); tickv(); tickv(); break;
    case LA: tickv(); tickv(); closev(); break;
    case VO: closev(); tickv(); break;                       // LA --tx/sL--> VO
    case VP: closev(); tickv(); h.gate(true, 0, SID0, DA0, VID0);   // Join!
             m = app_next(m, eJoin, false, true); break;
    case AO: closev(); tickv(); rx(1, false); break;         // VO+rJoinIn(!p2p)
    case QO: closev(); tickv(); rx(1, false); rx(1, true); break;
    case AP: closev(); tickv(); h.gate(true, 0, SID0, DA0, VID0);
             m = app_next(m, eJoin, false, true);
             rx(1, false); break;                            // VP+rJoinIn(!p2p)
    case QP: closev(); tickv(); h.gate(true, 0, SID0, DA0, VID0);
             m = app_next(m, eJoin, false, true);
             rx(1, false); rx(1, true); break;
    case LO: closev(); tickv(); rx(5, true); break;          // VO+rLv
    default: break;
  }
  return m;
}

static int l_goto(Hn& h, int S) {
  int m = VO;
  h.ctl(true, 0, SID0, DA0, VID0);                  // A15: armed, app VO
  auto adv = [&]() {                                // registrar -> New!
    h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
    m = app_next(m, eNew, false, true);
  };
  auto tickv = [&]() { h.tick(); m = app_next(m, eTx, false, true); };
  auto rxl   = [&](uint8_t code, bool p2p) {        // Listener-type rx
    h.d->p2p_i = p2p;
    h.inject(true, 3, SID0, 0, 0, code, 2);
    m = app_next(m, eRNew + code, false, p2p);
    h.d->p2p_i = 1;
  };
  switch (S) {
    case VO: break;
    case VN: adv(); break;
    case AN: adv(); tickv(); break;
    case AA: adv(); tickv(); tickv(); break;
    case QA: adv(); tickv(); tickv(); tickv(); break;
    case LA: adv(); tickv(); tickv();
             h.ctl(false, 0, SID0, DA0, VID0);      // A8: Lv!
             m = app_next(m, eLv, false, true); break;
    case VP: adv(); tickv(); tickv(); h.la_rx();    // AA+rLA -> VP
             m = app_next(m, eRLA, false, true); break;
    case AO: rxl(1, false); break;
    case QO: rxl(1, false); rxl(1, true); break;
    case AP: adv(); tickv(); tickv(); h.la_rx();
             m = app_next(m, eRLA, false, true);
             rxl(1, false); break;                  // VP+rJoinIn(!p2p)
    case QP: adv(); tickv(); tickv(); h.la_rx();
             m = app_next(m, eRLA, false, true);
             rxl(1, false); rxl(1, true); break;
    case LO: rxl(5, true); break;
    default: break;
  }
  return m;
}

namespace {

//! The whole suite: one DUT, one BFM, one tally, one recipe walk per section.
class SrpStreamFsmsSuite {
 public:
  int run();

 private:
  void expiry_and_received_event_follow_table_10_4();
  void rx_events_match_table_10_3();
  void talker_tx_rows_send_the_table_10_3_message();
  void listener_tx_rows_send_the_table_10_3_message();
  void talker_registrar_follows_table_10_4_with_delta13();
  void talker_declaration_tracks_ready_and_admission();
  void listener_matcher_registers_swaps_and_unregisters();
  void vlan_user_handshake_holds_and_collapses();
  void talker_tracker_isolates_applications();
  void received_leave_all_reaches_only_its_attribute_type();
  void run_b_switch_leave_all_keeps_the_listener_registration_in();
  void domain_only_leave_all_never_ages_the_listener_registrar();
  void listener_only_leave_all_ages_the_listener_registrar_to_mt();
  void listener_failure_change_notifies_without_redeclaring();
  void listener_failure_change_compares_each_sink_with_its_own_latch();
  void listener_failure_change_notifies_registered_sinks_on_a_shared_stream();
  void settle_keeps_the_owed_listener_leave();
  void a_settle_while_parked_keeps_the_held_stream();
  void a_parked_leave_survives_txla_and_received_values();
  void a_sink_with_no_record_owes_nothing();
  int shared_stream_goes_quiet(int n, const char* tag);
  void tick_sends(const std::string& want, const char* what);
  int ticks_follow_the_model(int m, int fp, int n, const char* what);
  void owner_unbinds_first_then_the_rider();
  void rider_unbinds_first_then_the_owner();
  void three_sinks_hand_the_applicant_down_the_line();
  void a_rider_rebinds_to_another_stream();
  void a_handover_during_a_walk_keeps_one_tx();
  void an_owner_rebind_on_the_next_clock_hands_over();
  void a_rider_rebind_on_the_clock_after_the_owner_unbinds();
  int park_the_stream_alone(int parker, bool via_a8);
  void a_new_sink_takes_over_a_parked_leave(int parker, int joiner, bool via_a8,
                                            const char* tag);
  void a_parked_leave_is_still_sent_while_no_sink_declares();
  void a_declaration_after_the_parked_lv_waits_for_the_next_tx();
  void a_sink_whose_own_leave_goes_first_takes_over_a_parked_leave();
  void a_sink_parked_again_keeps_the_pairs_of_the_value_it_holds();
  bool tick_with_input_at(int at, int sink, uint64_t sid, uint64_t da, uint16_t vid);
  void a_handover_after_the_walk_passed_the_taker_has_the_leaveall_walks_txla();
  void a_handover_after_the_walk_passed_the_taker_has_the_walks_tx();
  void a_rebind_handover_after_the_walk_passed_the_taker_has_the_txla();
  void a_declaration_after_the_last_declarers_lv_waits_for_the_next_tx();
  void a_rider_settling_after_its_streams_transmit_lets_the_walk_end();
  void two_parked_leaves_each_keep_their_own_group();
  void ready_with_asking_failed_declares_ready_failed();

  const milan::tb::Model<Vsrp_stream_fsms_wrap> model;
  Vsrp_stream_fsms_wrap* const d = model.get();
  int checks = 0;
  int fails = 0;
  Hn h{d};
};

// ==== A. Table 10-3 walk: all rx events x all 12 states x both p2p arms,
//         through BOTH modules (one attribute each) =======================
void SrpStreamFsmsSuite::rx_events_match_table_10_3() {
  constexpr int rxevs[7] = {
      eRNew, eRJoinIn, eRIn, eRJoinMt, eRMt, eRLv, eRLA};
  for (int p2p = 1; p2p >= 0; --p2p) {
    for (int S = 0; S < NSTATES; ++S) {
      for (int k = 0; k < 7; ++k) {
        const int ev = rxevs[k];
        // talker applicant (Talker Advertise attribute)
        h.reset();
        int m = t_goto(h, S);
        CHECK(h.t_app(0) == m && m == S, "T recipe %s: dut %s model %s",
              SN[S], SN[h.t_app(0)], SN[m]);
        d->p2p_i = p2p;
        if (ev == eRLA) h.la_rx();
        else h.inject(true, 1, SID0, DA0, VID0,
                      static_cast<uint8_t>(ev - eRNew));
        int exp = app_next(S, ev, false, p2p);
        CHECK(h.t_app(0) == exp, "T %s x %s p2p=%d: dut %s want %s",
              SN[S], EN[ev], p2p, SN[h.t_app(0)], SN[exp]);
        // listener applicant (Listener attribute)
        h.reset();
        m = l_goto(h, S);
        CHECK(h.l_app(0) == m && m == S, "L recipe %s: dut %s model %s",
              SN[S], SN[h.l_app(0)], SN[m]);
        d->p2p_i = p2p;
        if (ev == eRLA) h.la_rx();
        else h.inject(true, 3, SID0, 0, 0,
                      static_cast<uint8_t>(ev - eRNew), 2);
        exp = app_next(S, ev, false, p2p);
        CHECK(h.l_app(0) == exp, "L %s x %s p2p=%d: dut %s want %s",
              SN[S], EN[ev], p2p, SN[h.l_app(0)], SN[exp]);
      }
    }
  }
}

// ==== B. tx! / txLA! rows: transition + exact message, talker ===========
void SrpStreamFsmsSuite::talker_tx_rows_send_the_table_10_3_message() {
  for (int txla = 0; txla <= 2; ++txla) {
    for (int S = 0; S < NSTATES; ++S) {
      h.reset();
      (void)t_goto(h, S);
      if (txla == 1) h.la_own();
      if (txla == 2) d->leaveall_own_i = 1;
      h.clear_logs();
      CHECK(h.tick(), "T tx walk %s txla=%d completes", SN[S], txla);
      int exp = app_next(S, txla ? eTxLA : eTx, false, true);
      CHECK(h.t_app(0) == exp, "T %s x %s: dut %s want %s",
            SN[S], txla ? "txLA!" : "tx!", SN[h.t_app(0)], SN[exp]);
      int msg = app_msg(S, txla, false);
      if (msg < 0) {
        CHECK(h.t_push.empty(), "T %s txla=%d: no message, got %zu",
              SN[S], txla, h.t_push.size());
      } else {
        CHECK(h.t_push.size() == 1 && h.t_push[0].code == msg,
              "T %s txla=%d: one msg code %d", SN[S], txla, msg);
        if (h.t_push.size() == 1) {
          const Push& p = h.t_push[0];
          CHECK(p.type == 1, "T %s: Talker Advertise type, got %u", SN[S], p.type);
          uint64_t sid = 0;
          uint64_t da = 0;
          for (int i = 0; i < 8; ++i) sid = (sid << 8) | p.val[i];
          for (int i = 8; i < 14; ++i) da = (da << 8) | p.val[i];
          CHECK(sid == SID0 && da == DA0, "T %s FirstValue stream_id/DA", SN[S]);
          CHECK(p.val[14] == 0 && p.val[15] == VID0, "T %s FirstValue VLAN", SN[S]);
          CHECK(p.val[16] == 0 && p.val[17] == 217, "T %s MaxFrameSize", SN[S]);
          CHECK(p.val[18] == 0 && p.val[19] == 1, "T %s MaxIntervalFrames", SN[S]);
          CHECK(p.val[20] == ((3 << 5) | (1 << 4)), "T %s prio/rank byte", SN[S]);
        }
      }
    }
  }
}

// ==== B2. tx! / txLA! rows on the listener (Listener attribute) =========
void SrpStreamFsmsSuite::listener_tx_rows_send_the_table_10_3_message() {
  for (int txla = 0; txla <= 2; ++txla) {
    for (int S = 0; S < NSTATES; ++S) {
      h.reset();
      (void)l_goto(h, S);
      // fourpack declared by the recipe: READY iff the recipe declared
      const bool declared = !(S == VO || S == AO || S == QO || S == LO);
      if (txla == 1) h.la_own();
      if (txla == 2) d->leaveall_own_i = 1;
      h.clear_logs();
      CHECK(h.tick(), "L tx walk %s txla=%d completes", SN[S], txla);
      int exp = app_next(S, txla ? eTxLA : eTx, false, true);
      CHECK(h.l_app(0) == exp, "L %s x %s: dut %s want %s",
            SN[S], txla ? "txLA!" : "tx!", SN[h.l_app(0)], SN[exp]);
      int msg = app_msg(S, txla, false);
      if (msg < 0) {
        CHECK(h.l_push.empty(), "L %s txla=%d: no message, got %zu",
              SN[S], txla, h.l_push.size());
      } else {
        CHECK(h.l_push.size() == 1 && h.l_push[0].code == msg,
              "L %s txla=%d: one msg code %d", SN[S], txla, msg);
        if (h.l_push.size() == 1) {
          const Push& p = h.l_push[0];
          uint64_t sid = 0;
          for (int i = 0; i < 8; ++i) sid = (sid << 8) | p.val[i];
          CHECK(p.type == 3 && sid == SID0, "L %s Listener FirstValue", SN[S]);
          CHECK(p.fp == (declared ? 2 : 0), "L %s fourpack %u want %u",
                SN[S], p.fp, declared ? 2 : 0);
        }
      }
    }
  }
}

// ==== C. Δ13 + registrar walk, talker side (Listener tracker) ===========
void SrpStreamFsmsSuite::talker_registrar_follows_table_10_4_with_delta13() {
  h.reset();
  h.gate(true, 1, SID0 + 1, DA0 + 1, VID0);
  h.clear_logs();
  // JoinMt registers Ready -> IN, LISTENER_REG_CHANGE
  h.inject(true, 3, SID0 + 1, 0, 0, 3 /*JoinMt*/, 2 /*Ready*/);
  CHECK(h.t_reg(1) == 1 && h.t_lstn(1) == 2, "T reg: Ready registered");
  CHECK(h.t_chg[1] == 1, "T reg: one LISTENER_REG_CHANGE, got %d", h.t_chg[1]);
  CHECK(h.t_arm.empty(), "T reg: no timer op on registration");
  // Δ13: rLv -> immediate MT + change, never a leavetimer
  h.inject(true, 3, SID0 + 1, 0, 0, 5 /*Lv*/, 2);
  REQ_TAG("REQ-SRP-005", "MTXW", "immediate IN->MT");
  CHECK(h.t_reg(1) == 0 && h.t_lstn(1) == 0, "T reg: Δ13 immediate IN->MT");
  CHECK(h.t_chg[1] == 2, "T reg: change strobe on rLv, got %d", h.t_chg[1]);
  CHECK(h.t_arm.empty(), "T reg: Δ13 arms NO leavetimer");
  // LeaveAll-cycle exception: IN -> LV with T-MRP-LEAVE armed
  h.inject(true, 3, SID0 + 1, 0, 0, 1 /*JoinIn*/, 3 /*ReadyFailed*/);
  CHECK(h.t_lstn(1) == 3 && h.t_chg[1] == 3, "T reg: ReadyFailed registered");
  h.la_rx();
  CHECK(h.t_reg(1) == 2, "T reg: LeaveAll -> LV");
  CHECK(h.t_lstn(1) == 3, "T reg: LV keeps the registration published");
  CHECK(h.t_arm.size() == 1 && !h.t_arm[0].cancel && h.t_arm[0].slot == 17
        && h.t_arm[0].owner == 0x41 && h.t_arm[0].deadline == NOW + LEAVE_MS,
        "T reg: ARM slot 17 owner 0x41 now+5000 (got %zu ops)", h.t_arm.size());
  CHECK(h.t_chg[1] == 3, "T reg: no change strobe entering LV");
  // re-confirm within the cycle: -> IN, CANCEL, no strobes
  h.inject(true, 3, SID0 + 1, 0, 0, 1, 3);
  CHECK(h.t_reg(1) == 1 && h.t_chg[1] == 3, "T reg: re-join -> IN, silent");
  CHECK(h.t_arm.size() == 2 && h.t_arm[1].cancel && h.t_arm[1].slot == 17,
        "T reg: CANCEL on re-join");
  // age out: LV again, expire -> MT + change
  h.la_rx();
  CHECK(h.t_reg(1) == 2 && h.t_arm.size() == 3, "T reg: LV again + re-arm");
  h.expire(17);
  CHECK(h.t_reg(1) == 0 && h.t_lstn(1) == 0, "T reg: leavetimer! -> MT");
  CHECK(h.t_chg[1] == 4, "T reg: change strobe on expiry, got %d", h.t_chg[1]);
  // foreign expiry slots never touch this block
  h.inject(true, 3, SID0 + 1, 0, 0, 1, 2);
  h.expire(3);
  h.expire(24);
  CHECK(h.t_reg(1) == 1, "T reg: foreign slots ignored");
}

// ==== D. talker declare -> Ready -> ACTIVE -> admission-loss walk =======
void SrpStreamFsmsSuite::talker_declaration_tracks_ready_and_admission() {
  h.reset();
  h.clear_logs();
  h.gate(true, 2, SID0 + 2, DA0 + 2, 5);
  CHECK(h.t_decl(2) == 1, "T decl ADVERTISE after gate open");
  CHECK(h.t_vop.size() == 1 && h.t_vop[0].join && h.t_vop[0].vid == 5,
        "T VLAN user++ on open");
  CHECK(!((d->t_active_o >> 2) & 1), "T not ACTIVE before Ready");
  // Milan 4.3.2 (issue #65): the licence also waits until the entry holding
  // the source's VID reports its MVRP join transmitted
  d->t_vid_sent_i = 0x0; h.idle(1);
  h.inject(true, 3, SID0 + 2, 0, 0, 3, 2 /*Ready*/);
  CHECK(!((d->t_active_o >> 2) & 1), "T not ACTIVE while the VID's join is untransmitted");
  d->t_vid_val_i = (7ull << 12) | 5ull; d->t_vid_sent_i = 0x2; h.idle(1);
  CHECK(!((d->t_active_o >> 2) & 1), "T not ACTIVE on another VID's transmitted join");
  d->t_vid_val_i = (5ull << 12) | 7ull; h.idle(1);
  CHECK(((d->t_active_o >> 2) & 1) == 1, "T ACTIVE once any entry holding the VID is transmitted");
  d->t_vid_sent_i = 0x3; d->t_vid_val_i = (static_cast<uint64_t>(VID0) << 12) | 5ull; h.idle(1);
  h.inject(true, 3, SID0 + 2, 0, 0, 3, 2 /*Ready*/);
  CHECK(((d->t_active_o >> 2) & 1) == 1, "T ACTIVE = declaring x Ready x admitted x VID joined");
  // ReadyFailed also satisfies ACTIVE
  h.inject(true, 3, SID0 + 2, 0, 0, 1, 3 /*ReadyFailed*/);
  CHECK(((d->t_active_o >> 2) & 1) == 1, "T ACTIVE holds on ReadyFailed");
  // AskingFailed does not
  h.inject(true, 3, SID0 + 2, 0, 0, 1, 1 /*AskingFailed*/);
  CHECK(((d->t_active_o >> 2) & 1) == 0, "T not ACTIVE on AskingFailed");
  h.inject(true, 3, SID0 + 2, 0, 0, 1, 2);
  // admission loss: in-place swap to Talker Failed code 1, own MAC
  d->sr_admitted_i = 0xFF & ~(1u << 2);
  h.idle(3);
  CHECK(h.t_decl(2) == 2, "T decl FAILED on admission loss");
  CHECK(((d->t_active_o >> 2) & 1) == 0, "T ACTIVE drops on admission loss");
  CHECK(h.t_fcode(2) == 1, "T fail code 1 (insufficient bandwidth)");
  CHECK(h.t_fbridge(2) == MAC, "T fail bridge id = own MAC");
  CHECK(h.t_fcode(3) == 0 && h.t_fbridge(3) == 0, "T fail publication scoped");
  h.clear_logs();
  CHECK(h.tick(), "T failed-swap tick completes");
  CHECK(h.t_push.size() == 1 && h.t_push[0].type == 2 && h.t_push[0].code == 0,
        "T swap re-declares: New of Talker Failed");
  if (h.t_push.size() == 1) {
    const Push& p = h.t_push[0];
    uint64_t sys = 0;
    for (int i = 25; i < 33; ++i) sys = (sys << 8) | p.val[i];
    CHECK(sys == MAC && p.val[33] == 1,
          "T Failed FirstValue: system id own MAC + code 1");
  }
  // admission return: swap back to Advertise
  d->sr_admitted_i = 0xFF;
  h.idle(3);
  CHECK(h.t_decl(2) == 1 && h.t_fcode(2) == 0, "T back to ADVERTISE");
  CHECK(((d->t_active_o >> 2) & 1) == 1, "T ACTIVE returns with admission");
  d->t_vid_sent_i = 0x0; h.idle(1);
  CHECK(((d->t_active_o >> 2) & 1) == 0, "T ACTIVE drops when the VID membership is gone");
  d->t_vid_sent_i = 0x3; h.idle(1);
  h.clear_logs();
  h.tick();
  CHECK(h.t_push.size() == 1 && h.t_push[0].type == 1 && h.t_push[0].code == 0,
        "T swap-back re-declares: New of Talker Advertise");
  // gate close: Lv + VLAN user--
  h.clear_logs();
  h.gate(false, 2, 0, 0, 0);
  CHECK(h.t_decl(2) == 0, "T decl NONE after close");
  CHECK(h.t_vop.size() == 1 && !h.t_vop[0].join && h.t_vop[0].vid == 5,
        "T VLAN user-- on close");
  h.clear_logs();
  h.tick();
  CHECK(h.t_push.size() == 1 && h.t_push[0].code == 5 && h.t_push[0].type == 1,
        "T close withdraws with Lv");
}

// ==== E. listener matcher: match, near-miss, swap, unregister ===========
void SrpStreamFsmsSuite::listener_matcher_registers_swaps_and_unregisters() {
  h.reset();
  h.clear_logs();
  h.ctl(true, 0, SID0, DA0, VID0);
  CHECK(h.l_vop.size() == 1 && h.l_vop[0].join && h.l_vop[0].vid == VID0,
        "L VLAN user++ on settle");
  // near-misses arm nothing (exact triple match, Milan §5.3.8.9)
  h.inject(true, 1, SID0 + 1, DA0, VID0, 3, 0, 111);
  h.inject(true, 1, SID0, DA0 + 1, VID0, 3, 0, 111);
  h.inject(true, 1, SID0, DA0, VID0 + 1, 3, 0, 111);
  CHECK(h.l_regst(0) == 0 && h.l_reg[0] == 0, "L near-misses never match");
  // MVRP application isolation: same bytes, wrong application
  h.inject(false, 1, SID0, DA0, VID0, 3, 0, 111);
  h.la_own(); // own MSRP LeaveAll must not fabricate registrations either
  CHECK(h.l_regst(0) == 0 && h.l_reg[0] == 0, "L MVRP/app isolation holds");
  // exact match registers ADVERTISE, latches acc_latency, declares Ready
  h.inject(true, 1, SID0, DA0, VID0, 0 /*New*/, 0, 777);
  CHECK(h.l_regst(0) == 1 && h.l_tkreg(0) == 1, "L ADVERTISE registered");
  CHECK(h.l_reg[0] == 1, "L EVT_TK_REGISTERED once, got %d", h.l_reg[0]);
  CHECK(h.l_lat(0) == 777, "L acc_latency latched, got %u", h.l_lat(0));
  CHECK(h.l_decl(0) == 2, "L declares READY");
  h.clear_logs();
  h.tick();
  CHECK(h.l_push.size() == 1 && h.l_push[0].code == 0 && h.l_push[0].fp == 2,
        "L Ready rides New on the next tick");
  // refresh with a new latency: silent re-latch (counters were cleared
  // before the tick above, so the running count restarts at zero)
  h.inject(true, 1, SID0, DA0, VID0, 1 /*JoinIn*/, 0, 888);
  CHECK(h.l_lat(0) == 888 && h.l_reg[0] == 0, "L refresh re-latches silently");
  // in-place swap Advertise -> Failed: strobe, no unregistration
  h.inject(true, 2, SID0, DA0, VID0, 1, 0, 999, 0xBBBB0000CCCCull, 7);
  CHECK(h.l_tkreg(0) == 2, "L in-place swap to FAILED");
  CHECK(h.l_reg[0] == 1, "L swap fires EVT_TK_REGISTERED, got %d", h.l_reg[0]);
  CHECK(h.l_unreg[0] == 0, "L swap fires NO unregistration");
  CHECK(h.l_fcode(0) == 7 && h.l_fbridge(0) == 0xBBBB0000CCCCull,
        "L failure info latched + published");
  CHECK(h.l_lat(0) == 999, "L Failed also carries acc_latency");
  CHECK(h.l_decl(0) == 1, "L declaration swaps to ASKING_FAILED");
  h.clear_logs();
  h.tick();
  CHECK(h.l_push.size() == 1 && h.l_push[0].code == 0 && h.l_push[0].fp == 1,
        "L AskingFailed rides New after the swap");
  // swap back: Failed -> Advertise
  const uint64_t failed_bridge = h.l_fbridge(0);
  h.inject(true, 1, SID0, DA0, VID0, 3, 0, 555);
  CHECK(h.l_tkreg(0) == 1 && h.l_reg[0] == 1, "L swap back to ADVERTISE");
  // the code is gated here; the bridge is the raw latch, valid only with
  // tk_reg_state FAILED (its one consumer gates it after its index mux)
  CHECK(h.l_fcode(0) == 0 && h.l_tkreg(0) != 2 && failed_bridge != 0
        && h.l_fbridge(0) == failed_bridge,
        "L failure gated off: code 0, bridge left ungated under ADVERTISE");
  CHECK(h.l_decl(0) == 2, "L declaration back to READY");
  // Δ13 unregister: on the withdrawing frame
  h.inject(true, 1, SID0, DA0, VID0, 5 /*Lv*/);
  REQ_TAG("REQ-SRP-005", "MTXW", "rLv -> unregistered");
  CHECK(h.l_tkreg(0) == 0 && h.l_unreg[0] == 1, "L Δ13 rLv -> unregistered");
  CHECK(h.l_arm.empty(), "L Δ13 arms no leavetimer");
  CHECK(h.l_decl(0) == 0, "L declared state drops once withdrawal applies");
  h.clear_logs();
  h.tick();
  CHECK(h.l_push.size() == 1 && h.l_push[0].code == 5,
        "L withdraw rides Lv on the next tick");
  CHECK(h.l_decl(0) == 0, "L declared state NONE after withdraw");
  // LeaveAll cycle on the listener registrar
  h.inject(true, 1, SID0, DA0, VID0, 0, 0, 444);
  h.clear_logs();
  h.la_rx();
  CHECK(h.l_regst(0) == 2 && h.l_tkreg(0) == 1, "L LeaveAll -> LV, still ADV");
  CHECK(h.l_arm.size() == 1 && !h.l_arm[0].cancel && h.l_arm[0].slot == 24
        && h.l_arm[0].owner == 0x60 && h.l_arm[0].deadline == NOW + LEAVE_MS,
        "L ARM slot 24 owner 0x60 now+5000");
  h.expire(24);
  CHECK(h.l_regst(0) == 0 && h.l_unreg[0] == 1, "L expiry -> MT + unregistered");
  // teardown: no event, VLAN user--, declaration withdrawn
  h.inject(true, 1, SID0, DA0, VID0, 0, 0, 333);
  CHECK(h.l_tkreg(0) == 1, "L re-registered before teardown");
  h.clear_logs();
  h.ctl(false, 0, 0, 0, 0);
  CHECK(h.l_tkreg(0) == 0, "L teardown -> NONE");
  CHECK(h.l_unreg[0] == 0 && h.l_reg[0] == 0, "L teardown fires NO events");
  CHECK(h.l_vop.size() == 1 && !h.l_vop[0].join && h.l_vop[0].vid == VID0,
        "L VLAN user-- on teardown");
  h.clear_logs();
  h.tick();
  CHECK(h.l_push.size() == 1 && h.l_push[0].code == 5,
        "L teardown withdraws the declaration");
  // torn-down matcher is deaf
  h.clear_logs();
  h.inject(true, 1, SID0, DA0, VID0, 0, 0, 222);
  CHECK(h.l_regst(0) == 0 && h.l_reg[0] == 0, "L A8 disarms the matcher");
}

// ==== F. VLAN handshake back-pressure + collapse =========================
void SrpStreamFsmsSuite::vlan_user_handshake_holds_and_collapses() {
  h.reset();
  h.clear_logs();
  h.t_uready = false;
  h.gate(true, 3, SID0 + 3, DA0 + 3, 7);
  CHECK(d->t_user_valid_o && d->t_user_join_o && d->t_user_vid_o == 7,
        "T VLAN op held under back-pressure");
  h.idle(3);
  CHECK(d->t_user_valid_o, "T VLAN op still held");
  CHECK(h.t_vop.empty(), "T no op consumed while not ready");
  h.t_uready = true;
  h.idle(2);
  CHECK(h.t_vop.size() == 1 && h.t_vop[0].join && h.t_vop[0].vid == 7,
        "T held op lands once ready");
  // close-before-join collapses to nothing
  h.t_uready = false;
  h.gate(true, 4, SID0 + 4, DA0 + 4, 9);
  h.gate(false, 4, 0, 0, 0);
  h.t_uready = true;
  h.idle(4);
  CHECK(h.t_vop.size() == 1, "T open+close before issue = net zero, got %zu",
        h.t_vop.size());
}

// ==== G. per-application isolation on the talker tracker ================
void SrpStreamFsmsSuite::talker_tracker_isolates_applications() {
  h.reset();
  h.gate(true, 5, SID0 + 5, DA0 + 5, VID0);
  h.clear_logs();
  h.inject(false, 3, SID0 + 5, 0, 0, 3, 2);   // MVRP application: ignored
  CHECK(h.t_reg(5) == 0 && h.t_chg[5] == 0, "T MVRP-app Listener ignored");
  h.inject(false, 1, SID0 + 5, DA0 + 5, VID0, 5);  // MVRP VID type-1 collision
  CHECK(h.t_app(5) == VN, "T MVRP type-1 collision never reaches the applicant");
}

// registrar states as published on dbg_reg_state, and the Ready code
constexpr int kRegMt = 0;
constexpr int kRegIn = 1;
constexpr int kRegLv = 2;
constexpr int kDeclReady = 2;
constexpr const char* kLaneName[4] = {"TalkerAdvertise", "TalkerFailed",
                                      "Listener", "Domain"};

// ==== H. rLA! reaches only the state machines of its Attribute Type =====
// 802.1Q-2014 §10.7.5.20 b)2): rLA! occurs for an Applicant or Registrar
// only when "the PDU contains a Message in which the Attribute Type is the
// type associated with the state machine". Each lane alone, against every
// plane: the talker applicant (Talker Advertise, or Talker Failed while
// swapped), the talker registrar (Listener), the listener applicant
// (Listener) and the listener registrar (the talker type it holds).
void SrpStreamFsmsSuite::received_leave_all_reaches_only_its_attribute_type() {
  for (unsigned lane = 0; lane < 4; ++lane) {
    const unsigned bit = 1u << lane;
    const char* ln = kLaneName[lane];
    const int app_hit = app_next(QA, eRLA, false, true);

    // talker source 0: Advertise applicant quiet (QA), Listener Ready IN
    h.reset();
    (void)t_goto(h, QA);
    h.inject(true, 3, SID0, 0, 0, 3 /*JoinMt*/, kDeclReady);
    CHECK(h.t_app(0) == QA && h.t_reg(0) == kRegIn, "H %s: talker set-up", ln);
    h.clear_logs();
    h.la_rx_lanes(bit);
    CHECK(h.t_app(0) == (bit == kLaTalkerAdv ? app_hit : QA),
          "H %s: talker Advertise applicant %s", ln, SN[h.t_app(0)]);
    CHECK(h.t_reg(0) == (bit == kLaListener ? kRegLv : kRegIn),
          "H %s: talker Listener registrar %d", ln, h.t_reg(0));
    CHECK(h.t_arm.size() == (bit == kLaListener ? 1u : 0u),
          "H %s: talker leave-timer ops %zu", ln, h.t_arm.size());

    // talker source 0 swapped to Talker Failed (admission lost), quiet
    h.reset();
    h.gate(true, 0, SID0, DA0, VID0);
    d->sr_admitted_i = 0xFE;
    h.idle(3);
    h.tick(); h.tick(); h.tick();
    CHECK(h.t_decl(0) == 2 && h.t_app(0) == QA, "H %s: talker Failed set-up", ln);
    h.la_rx_lanes(bit);
    CHECK(h.t_app(0) == (bit == kLaTalkerFailed ? app_hit : QA),
          "H %s: talker Failed applicant %s", ln, SN[h.t_app(0)]);

    // listener sink 0: Listener applicant quiet, Advertise registered IN
    h.reset();
    (void)l_goto(h, QA);
    CHECK(h.l_app(0) == QA && h.l_regst(0) == kRegIn && h.l_tkreg(0) == 1,
          "H %s: listener set-up", ln);
    h.clear_logs();
    h.la_rx_lanes(bit);
    CHECK(h.l_app(0) == (bit == kLaListener ? app_hit : QA),
          "H %s: listener Listener applicant %s", ln, SN[h.l_app(0)]);
    CHECK(h.l_regst(0) == (bit == kLaTalkerAdv ? kRegLv : kRegIn),
          "H %s: listener Advertise registrar %d", ln, h.l_regst(0));

    // listener sink 0 holding a Talker Failed registration
    h.reset();
    h.ctl(true, 0, SID0, DA0, VID0);
    h.inject(true, 2, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500, 0xBB, 1);
    CHECK(h.l_tkreg(0) == 2 && h.l_regst(0) == kRegIn, "H %s: Failed set-up", ln);
    h.la_rx_lanes(bit);
    CHECK(h.l_regst(0) == (bit == kLaTalkerFailed ? kRegLv : kRegIn),
          "H %s: listener Failed registrar %d", ln, h.l_regst(0));
  }
}

// ==== I. (a) the Run B switch LeaveAll MRPDU, as the decoder presents it =
// [Listener LA JoinMt/Ready][Domain LA n=2][TalkerAdvertise LA n=0]
// [TalkerFailed LA n=0]: the decoder strobes each type's lane once, ahead
// of that type's events (srp_decoder P), so the talker registrar sees the
// Listener lane, then the re-declaration, then only other types' lanes.
// The Listener registration must end IN, with no aging left armed.
void SrpStreamFsmsSuite::run_b_switch_leave_all_keeps_the_listener_registration_in() {
  h.reset();
  h.gate(true, 1, SID0 + 1, DA0 + 1, VID0);
  h.inject(true, 3, SID0 + 1, 0, 0, 3 /*JoinMt*/, kDeclReady);
  CHECK(h.t_reg(1) == kRegIn && ((d->t_active_o >> 1) & 1),
        "I set-up: Ready IN, source 1 ACTIVE");
  h.clear_logs();
  h.la_rx_lanes(kLaListener);
  h.inject(true, 3, SID0 + 1, 0, 0, 3 /*JoinMt*/, kDeclReady);
  h.la_rx_lanes(kLaDomain);
  h.inject(true, 4, 0, 0, VID0, 3);           // the two Domain values
  h.inject(true, 4, 0, 0, VID0, 3);
  h.la_rx_lanes(kLaTalkerAdv);
  h.la_rx_lanes(kLaTalkerFailed);
  CHECK(h.t_reg(1) == kRegIn, "I Listener registration stays IN, got %d", h.t_reg(1));
  CHECK(h.t_lstn(1) == kDeclReady, "I Ready still published");
  CHECK(((d->t_active_o >> 1) & 1) == 1, "I source 1 stays ACTIVE");
  CHECK(h.t_chg[1] == 0, "I no LISTENER_REG_CHANGE, got %d", h.t_chg[1]);
  CHECK(h.t_arm.size() == 2 && !h.t_arm[0].cancel && h.t_arm[1].cancel
        && h.t_arm[1].slot == 17,
        "I the Listener lane's ARM is CANCELled by the re-declaration (%zu ops)",
        h.t_arm.size());
  // a T-MRP-LEAVE expiry for the slot finds nothing to age
  h.expire(17);
  CHECK(h.t_reg(1) == kRegIn && h.t_lstn(1) == kDeclReady,
        "I a leave expiry after the MRPDU leaves the registration IN");
}

// ==== J. (b) LeaveAll on the Domain type only ============================
// The processor's old own LeaveAll shape. No Listener re-declaration
// follows, and none is needed: the Listener registrar never ages.
void SrpStreamFsmsSuite::domain_only_leave_all_never_ages_the_listener_registrar() {
  h.reset();
  h.gate(true, 2, SID0 + 2, DA0 + 2, VID0);
  h.inject(true, 3, SID0 + 2, 0, 0, 1 /*JoinIn*/, kDeclReady);
  CHECK(h.t_reg(2) == kRegIn, "J set-up: Ready IN");
  h.clear_logs();
  h.la_rx_lanes(kLaDomain);
  h.inject(true, 4, 0, 0, VID0, 1);
  CHECK(h.t_reg(2) == kRegIn, "J Domain-only LeaveAll: registrar stays IN, got %d",
        h.t_reg(2));
  CHECK(h.t_arm.empty(), "J no leave timer armed, got %zu ops", h.t_arm.size());
  h.expire(18);
  CHECK(h.t_reg(2) == kRegIn && h.t_lstn(2) == kDeclReady && h.t_chg[2] == 0,
        "J nothing ages: Ready published, no change strobe");
  CHECK(((d->t_active_o >> 2) & 1) == 1, "J source 2 stays ACTIVE");
}

// ==== K. (c) LeaveAll on the Listener type, nothing re-declared ==========
// The registrar enters LV with T-MRP-LEAVE armed, keeps the registration
// published through LV, and without a re-declaration ages to MT.
void SrpStreamFsmsSuite::listener_only_leave_all_ages_the_listener_registrar_to_mt() {
  h.reset();
  h.gate(true, 3, SID0 + 3, DA0 + 3, VID0);
  h.inject(true, 3, SID0 + 3, 0, 0, 3 /*JoinMt*/, kDeclReady);
  CHECK(h.t_reg(3) == kRegIn && ((d->t_active_o >> 3) & 1), "K set-up: Ready IN, ACTIVE");
  h.clear_logs();
  h.la_rx_lanes(kLaListener);
  CHECK(h.t_reg(3) == kRegLv, "K Listener LeaveAll -> LV, got %d", h.t_reg(3));
  CHECK(h.t_lstn(3) == kDeclReady && h.t_chg[3] == 0, "K LV keeps Ready published");
  CHECK(h.t_arm.size() == 1 && !h.t_arm[0].cancel && h.t_arm[0].slot == 19
        && h.t_arm[0].owner == 0x43 && h.t_arm[0].deadline == NOW + LEAVE_MS,
        "K ARM slot 19 owner 0x43 now+5000 (got %zu ops)", h.t_arm.size());
  h.expire(19);                                // no re-declaration arrived
  CHECK(h.t_reg(3) == kRegMt && h.t_lstn(3) == 0, "K leavetimer! -> MT");
  CHECK(h.t_chg[3] == 1, "K one LISTENER_REG_CHANGE, got %d", h.t_chg[3]);
  CHECK(((d->t_active_o >> 3) & 1) == 0, "K source 3 no longer ACTIVE");
}

// ==== L. FailureInformation change: notification strobe only ============
// A changed failure code or bridge ID while Talker Failed stays registered
// is NOT a Listener declaration change: same attribute (stream_id), same
// AskingFailed parameter. It raises evt_tk_fail_chg_o for the
// GET_STREAM_INFO notification (Milan Table 5.22) and nothing else: no
// EVT_TK_REGISTERED, no declaration request, so no New on the next tick.
void SrpStreamFsmsSuite::listener_failure_change_notifies_without_redeclaring() {
  auto no_new = [this]() {
    for (const Push& p : h.l_push) if (p.code == 0) return false;
    return true;
  };
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID0);
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 0 /*New*/, 0, 999, 0xBBBB0000CCCCull, 7);
  CHECK(h.l_tkreg(0) == 2 && h.l_reg[0] == 1 && h.l_fchg[0] == 0,
        "L1: a fresh Failed registration strobes REGISTERED, not the change "
        "(reg %d, fchg %d)", h.l_reg[0], h.l_fchg[0]);
  h.clear_logs();
  h.tick();
  CHECK(h.l_push.size() == 1 && h.l_push[0].code == 0 && h.l_push[0].fp == 1,
        "L1: the fresh registration's AskingFailed rides New");
  for (int i = 0; i < 6; ++i) { h.clear_logs(); h.tick(); }
  CHECK(h.l_push.empty(), "L2 precondition: the applicant is quiet, got %zu",
        h.l_push.size());

  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 1 /*JoinIn*/, 0, 999,
           0xBBBB0000DDDDull, 9);
  CHECK(h.l_fchg[0] == 1 && h.l_reg[0] == 0 && h.l_unreg[0] == 0,
        "L2: a changed FailureInformation strobes the change once and "
        "nothing else (fchg %d, reg %d, unreg %d)",
        h.l_fchg[0], h.l_reg[0], h.l_unreg[0]);
  CHECK(h.l_fcode(0) == 9 && h.l_fbridge(0) == 0xBBBB0000DDDDull
        && h.l_tkreg(0) == 2 && h.l_decl(0) == 1,
        "L2: new code and bridge latched, still FAILED and AskingFailed");
  h.clear_logs();
  h.tick();
  CHECK(h.l_push.empty(),
        "L2: no Listener New, no message at all, on the next tick (got %zu)",
        h.l_push.size());

  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 1, 0, 999, 0xBBBB0000DDDEull, 9);
  CHECK(h.l_fchg[0] == 1 && h.l_reg[0] == 0,
        "L3: a bridge-only change strobes the change");
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 999,
           0xBBBB0000DDDEull, 10);
  CHECK(h.l_fchg[0] == 1 && h.l_reg[0] == 0,
        "L3: a code-only change strobes the change");
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 1, 0, 999, 0xBBBB0000DDDEull, 10);
  CHECK(h.l_fchg[0] == 0 && h.l_reg[0] == 0,
        "L4: an unchanged refresh strobes nothing");
  h.tick();
  CHECK(h.l_push.empty(), "L4: ...and declares nothing");
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 2 /*In*/, 0, 999, 0x1111ull, 3);
  CHECK(h.l_fchg[0] == 0 && h.l_fcode(0) == 10
        && h.l_fbridge(0) == 0xBBBB0000DDDEull,
        "L5: a non-registering In carrying other values strobes and "
        "latches nothing");

  // LV is still registered: a changed re-join notifies, never re-declares
  h.clear_logs();
  h.la_rx();
  CHECK(h.l_regst(0) == 2 && h.l_tkreg(0) == 2,
        "L6 precondition: LeaveAll -> LV, still FAILED");
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 3, 0, 999, 0xCCCC0000EEEEull, 4);
  CHECK(h.l_regst(0) == 1 && h.l_fchg[0] == 1 && h.l_reg[0] == 0,
        "L6: a changed re-join in LV strobes the change, back IN, no "
        "REGISTERED (fchg %d, reg %d)", h.l_fchg[0], h.l_reg[0]);
  h.tick();
  CHECK(no_new(), "L6: the LeaveAll re-declaration carries no New");

  // the swap is a type change, not a FailureInformation change
  h.clear_logs();
  h.inject(true, 1, SID0, DA0, VID0, 1, 0, 999);
  h.inject(true, 2, SID0, DA0, VID0, 1, 0, 999, 0xCCCC0000EEEEull, 4);
  CHECK(h.l_reg[0] == 2 && h.l_fchg[0] == 0,
        "L7: Failed->Advertise->Failed swaps strobe REGISTERED, never the "
        "change (reg %d, fchg %d)", h.l_reg[0], h.l_fchg[0]);
}

void SrpStreamFsmsSuite::listener_failure_change_compares_each_sink_with_its_own_latch() {
  // One comparator on the HIT sink: two sinks on different streams hold
  // different latches, and each refresh is compared with its own sink's.
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID0);
  h.ctl(true, 1, SIDB, DAB, VID0);
  h.inject(true, 2, SID0, DA0, VID0, 0, 0, 1, 0xAAAA0000AAAAull, 7);
  h.inject(true, 2, SIDB, DAB, VID0, 0, 0, 1, 0xBBBB0000BBBBull, 8);
  h.clear_logs();
  h.inject(true, 2, SIDB, DAB, VID0, 1, 0, 1, 0xBBBB0000BBBBull, 8);
  CHECK(h.l_fchg[0] == 0 && h.l_fchg[1] == 0,
        "L8: sink 1's unchanged refresh is compared with sink 1's latch, "
        "not sink 0's (fchg %d/%d)", h.l_fchg[0], h.l_fchg[1]);
  h.inject(true, 2, SIDB, DAB, VID0, 1, 0, 1, 0xAAAA0000AAAAull, 7);
  CHECK(h.l_fchg[0] == 0 && h.l_fchg[1] == 1,
        "L8: sink 1 taking sink 0's values still strobes sink 1 only "
        "(fchg %d/%d)", h.l_fchg[0], h.l_fchg[1]);
}

void SrpStreamFsmsSuite::listener_failure_change_notifies_registered_sinks_on_a_shared_stream() {
  // Sinks on the SAME stream share the latch. A sink settled later (MT,
  // zeroed) is not a candidate even at the lowest index: its fresh
  // registration strobes REGISTERED while the registered pair stays quiet.
  h.reset();
  h.ctl(true, 1, SID0, DA0, VID0);
  h.ctl(true, 2, SID0, DA0, VID0);
  h.inject(true, 2, SID0, DA0, VID0, 0, 0, 1, 0xAAAA0000AAAAull, 7);
  h.ctl(true, 0, SID0, DA0, VID0);
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 1, 0, 1, 0xAAAA0000AAAAull, 7);
  CHECK(h.l_reg[0] == 1 && h.l_fchg[0] == 0 && h.l_fchg[1] == 0
        && h.l_fchg[2] == 0 && h.l_reg[1] == 0 && h.l_reg[2] == 0,
        "L9: unchanged on a shared stream: the late sink registers, the "
        "registered pair stays quiet (reg %d/%d/%d, fchg %d/%d/%d)",
        h.l_reg[0], h.l_reg[1], h.l_reg[2],
        h.l_fchg[0], h.l_fchg[1], h.l_fchg[2]);
  h.clear_logs();
  h.inject(true, 2, SID0, DA0, VID0, 1, 0, 1, 0xAAAA0000AAABull, 7);
  CHECK(h.l_fchg[0] == 1 && h.l_fchg[1] == 1 && h.l_fchg[2] == 1
        && h.l_reg[0] + h.l_reg[1] + h.l_reg[2] == 0,
        "L9: a change on a shared stream strobes every registered sink "
        "once (fchg %d/%d/%d)", h.l_fchg[0], h.l_fchg[1], h.l_fchg[2]);
}

// ==== an A15 settle never abandons the owed Listener Leave ==============
// (issue #188; 802.1Q-2018 §10.7.7: one applicant per attribute value, and
// Table 10-3: Lv! takes AA/QA to LA, whose tx! sends the Lv.) Sink 0 is
// declared Ready on SID0 (QA), withdrawn by A8 or by the talker's rLv, and
// settled again before the next tx!. Every expectation is the model's.
//! the one Listener message a tick pushed, as {code, fourpack, stream_id}
bool one_listener_push(const std::vector<Push>& pushes, int code, int fp,
                       uint64_t sid) {
  if (pushes.size() != 1 || pushes[0].type != 3) return false;
  uint64_t got = 0;
  for (int i = 0; i < 8; ++i) got = (got << 8) | pushes[0].val[i];
  return pushes[0].code == code && pushes[0].fp == fp && got == sid;
}

void SrpStreamFsmsSuite::settle_keeps_the_owed_listener_leave() {
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  const int sL = app_msg(LA, false, false);     // LA tx!: sL
  const int sN = app_msg(VN, false, false);     // VN tx!: sN

  // The rebind: A8 withdraws SID0, the sink settles on SIDB and its
  // talker registers, all before tx!. The tx! sends SID0's Lv, Ready as
  // declared, and nothing for SIDB; SIDB's New goes on the tx! after.
  h.reset();
  int m = l_goto(h, QA);
  h.ctl(false, 0, SID0, DA0, VID0);              // A8: Lv!
  m = app_next(m, eLv, false, true);
  h.ctl(true, 0, SIDB, DAB, VID0);               // A15 on another stream
  h.inject(true, 1, SIDB, DAB, VID0, 3 /*JoinMt*/, 0, 500);
  CHECK(h.l_tkreg(0) == 1 && h.l_decl(0) == 0,
        "RB1: the new stream registers at once, its Listener not yet "
        "declared (reg %d, decl %d)", h.l_tkreg(0), h.l_decl(0));
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, app_msg(m, false, false), 2, SID0)
        && m == LA,
        "RB1: the next tx! sends the old stream's Lv, Ready, and nothing "
        "else (%zu pushes, code %d)", h.l_push.size(),
        h.l_push.empty() ? -1 : h.l_push[0].code);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sN, 2, SIDB) && h.l_decl(0) == 2,
        "RB1: the tx! after it declares the new stream, New Ready (%zu "
        "pushes, decl %d)", h.l_push.size(), h.l_decl(0));

  // The re-probe: the talker withdraws (Δ13 rLv, the declaration is
  // withdrawn), A8, and the sink settles on the same SID0 before tx!:
  // that is the applicant already in LA, and its tx! sends the Lv.
  h.reset();
  m = l_goto(h, QA);
  h.inject(true, 1, SID0, DA0, VID0, 5 /*Lv*/);   // Δ13: Lv!
  m = app_next(m, eLv, false, true);
  h.ctl(false, 0, SID0, DA0, VID0);              // A8
  h.ctl(true, 0, SID0, DA0, VID0);               // A15, the same stream
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sL, 2, SID0) && m == LA,
        "RB2: after a re-probe onto the same stream the next tx! sends "
        "its Lv, Ready (%zu pushes, code %d)", h.l_push.size(),
        h.l_push.empty() ? -1 : h.l_push[0].code);

  // The same re-probe, but the talker registers again before tx!.
  // LA + New! is VN (Table 10-3): the tx! sends New, never the Lv.
  h.reset();
  m = l_goto(h, QA);
  h.inject(true, 1, SID0, DA0, VID0, 5 /*Lv*/);
  m = app_next(m, eLv, false, true);
  h.ctl(false, 0, SID0, DA0, VID0);
  h.ctl(true, 0, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  m = app_next(m, eNew, false, true);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, app_msg(m, false, false), 2, SID0)
        && m == VN,
        "RB3: a re-probe whose talker registers before tx! sends New Ready "
        "on the same stream, no Lv (%zu pushes, code %d)", h.l_push.size(),
        h.l_push.empty() ? -1 : h.l_push[0].code);
}

// The settle arms while a Leave is parked (10 §6.4): the applicant holds
// SID0 until its tx!, whatever settles come before it.
void SrpStreamFsmsSuite::a_settle_while_parked_keeps_the_held_stream() {
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  constexpr uint64_t SIDC = 0x02AABBCCDDEE0003ull;
  constexpr uint64_t DAC  = 0x91E0F0001236ull;
  const int sL = app_msg(LA, false, false);     // LA tx!: sL
  const int sN = app_msg(VN, false, false);     // VN tx!: sN
  const int sE = app_msg(LO, false, false);     // LO tx!: s (Mt, reg_in 0)

  // A settle while parked onto a third stream: rebound to SIDB, then
  // to SIDC, all before tx!. The applicant still holds SID0, so the tx!
  // sends SID0's Lv, and the tx! after it declares SIDC.
  h.reset();
  int m = l_goto(h, QA);
  h.ctl(false, 0, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  h.ctl(true, 0, SIDB, DAB, VID0);
  h.ctl(false, 0, SIDB, DAB, VID0);
  h.ctl(true, 0, SIDC, DAC, VID0);
  h.inject(true, 1, SIDC, DAC, VID0, 3 /*JoinMt*/, 0, 500);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sL, 2, SID0) && m == LA,
        "RB4: a second rebind while parked: the next tx! sends the held "
        "stream's Lv, Ready (%zu pushes, code %d)", h.l_push.size(),
        h.l_push.empty() ? -1 : h.l_push[0].code);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sN, 2, SIDC),
        "RB4: the tx! after it declares the third stream, New Ready (%zu "
        "pushes)", h.l_push.size());

  // A rebind back to the parked stream: rebound to SIDB, then back to
  // SID0, whose talker registers again, before tx!. That is the applicant
  // SID0 already has, the park is cleared, and LA + New! is VN.
  h.reset();
  m = l_goto(h, QA);
  h.ctl(false, 0, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  h.ctl(true, 0, SIDB, DAB, VID0);
  h.ctl(false, 0, SIDB, DAB, VID0);
  h.ctl(true, 0, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  m = app_next(m, eNew, false, true);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sN, 2, SID0) && m == VN,
        "RB5: a rebind back to the parked stream keeps its applicant: the "
        "next tx! sends New Ready on it, no Lv (%zu pushes, code %d)",
        h.l_push.size(), h.l_push.empty() ? -1 : h.l_push[0].code);

  // An LO applicant settled onto another stream: Lv! leaves LO (an
  // Empty owed), so SID0 is parked; its tx! sends the Empty, the tx! after
  // it declares SIDB.
  h.reset();
  m = l_goto(h, LO);
  h.ctl(true, 0, SIDB, DAB, VID0);
  m = app_next(m, eLv, false, true);
  h.inject(true, 1, SIDB, DAB, VID0, 3 /*JoinMt*/, 0, 500);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sE, 0, SID0) && m == LO,
        "RB6: an LO applicant settled on another stream: the next tx! sends "
        "the held stream's Empty (%zu pushes, code %d)", h.l_push.size(),
        h.l_push.empty() ? -1 : h.l_push[0].code);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sN, 2, SIDB),
        "RB6: the tx! after it declares the new stream, New Ready (%zu "
        "pushes)", h.l_push.size());
}

// A parked applicant against the events that are not a settle: its own
// LeaveAll's txLA! walk and a received Listener value (10 §6.4).
void SrpStreamFsmsSuite::a_parked_leave_survives_txla_and_received_values() {
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  const int sN = app_msg(VN, false, false);     // VN tx!: sN
  const int sE = app_msg(LO, false, false);     // LO tx!: s (Mt, reg_in 0)

  // A txLA! walk while parked: LA + txLA! is LO and sends nothing,
  // and the park stays, so the tx! after it sends SID0's Empty and the
  // tx! after that declares SIDB.
  h.reset();
  int m = l_goto(h, QA);
  h.ctl(false, 0, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  h.ctl(true, 0, SIDB, DAB, VID0);
  h.inject(true, 1, SIDB, DAB, VID0, 3 /*JoinMt*/, 0, 500);
  h.la_own();
  h.clear_logs();
  h.tick();
  const bool quiet = h.l_push.empty();
  m = app_next(m, eTxLA, false, true);
  h.clear_logs();
  h.tick();
  CHECK(quiet && m == LO && one_listener_push(h.l_push, sE, 2, SID0),
        "RB7: a txLA! walk while parked sends nothing and keeps the park: the "
        "tx! after it sends the held stream's Empty (quiet %d, %zu pushes, "
        "code %d)", quiet, h.l_push.size(),
        h.l_push.empty() ? -1 : h.l_push[0].code);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sN, 2, SIDB),
        "RB7: the tx! after the Empty declares the new stream, New Ready (%zu "
        "pushes)", h.l_push.size());

  // Received Listener values while parked: an LO applicant parked on
  // SID0 receives a Listener Mt on SIDB. Applied, LO + rMt! would be VO
  // and the owed Empty lost; while parked it is not applied.
  h.reset();
  m = l_goto(h, LO);
  h.ctl(true, 0, SIDB, DAB, VID0);
  h.inject(true, 3, SIDB, 0, 0, 4 /*Mt*/, 2);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sE, 0, SID0),
        "RB8: a Listener value received while parked is not applied: the "
        "next tx! sends the held stream's Empty (%zu pushes, code %d)",
        h.l_push.size(), h.l_push.empty() ? -1 : h.l_push[0].code);
}

// A sink with no record since reset holds no value: a received LeaveAll
// takes its applicant from VO to LO, but its first settle owes nothing,
// so the next tx! declares the new stream and sends no Empty.
void SrpStreamFsmsSuite::a_sink_with_no_record_owes_nothing() {
  const int sN = app_msg(VN, false, false);     // VN tx!: sN
  h.reset();
  h.la_rx();
  h.ctl(true, 0, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  h.clear_logs();
  h.tick();
  CHECK(one_listener_push(h.l_push, sN, 2, SID0),
        "RB9: a first settle after a received LeaveAll owes nothing: the next "
        "tx! sends New Ready and no Empty (%zu pushes, code %d)",
        h.l_push.size(), h.l_push.empty() ? -1 : h.l_push[0].code);
}

// ==== sinks on one stream share one declaring applicant ==================
// (issue #193; 802.1Q-2018 §10.7.7: one applicant per attribute value, the
// §35.2.6 NOTE: at most one Listener declaration per StreamID per port,
// §35.1.2.2: Lv only for a stream no longer wanted.) The owner, the
// lowest-index sink declaring the stream, holds the applicant; a rider is
// VO. Every expectation is the suite's Table 10-3 model of ONE applicant.
//! the Listener messages one tick pushed for sid, as "code/fp" words
std::string listener_words(const std::vector<Push>& pushes, uint64_t sid) {
  std::string w;
  for (const Push& p : pushes) {
    uint64_t got = 0;
    for (int i = 0; i < 8; ++i) got = (got << 8) | p.val[i];
    if (p.type != 3 || got != sid) continue;
    w += (w.empty() ? "" : " ") + std::to_string(p.code) + "/"
         + std::to_string(p.fp);
  }
  return w;
}
//! the words one applicant's message gives: "" when it sends nothing
std::string model_words(int msg, int fp) {
  return msg < 0 ? "" : std::to_string(msg) + "/" + std::to_string(fp);
}

//! Sinks 0..n-1 settled on SID0, every talker registered Ready, ticked
//! until the applicant is quiet (QA); each tick sends what ONE applicant
//! sends. Returns the model state.
int SrpStreamFsmsSuite::shared_stream_goes_quiet(int n, const char* tag) {
  h.reset();
  for (int s = 0; s < n; ++s) h.ctl(true, s, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  bool one = true;
  std::string got;
  for (int k = 0; k < 4; ++k) {
    h.clear_logs();
    h.tick();
    const std::string want = model_words(app_msg(m, false, false), 2);
    m = app_next(m, eTx, false, true);
    if (listener_words(h.l_push, SID0) != want) {
      one = false;
      got = listener_words(h.l_push, SID0);
    }
  }
  bool riders_vo = true;
  bool decl = true;
  for (int s = 0; s < n; ++s) {
    if (s > 0 && h.l_app(s) != VO) riders_vo = false;
    if (h.l_decl(s) != 2) decl = false;
  }
  CHECK(one && h.l_app(0) == m && m == QA && riders_vo && decl,
        "%s: %d sinks on one stream: every tx! sends what ONE applicant sends, "
        "the owner (sink 0) holds it, the riders are VO, each sink reports "
        "Ready (one %d got '%s', app0 %s, riders VO %d, decl %d)", tag, n,
        one, got.c_str(), SN[h.l_app(0)], riders_vo, decl);
  return m;
}

//! one tick; CHECKs that it pushed exactly want for SID0 and nothing else
void SrpStreamFsmsSuite::tick_sends(const std::string& want, const char* what) {
  h.clear_logs();
  h.tick();
  const std::string got = listener_words(h.l_push, SID0);
  CHECK(got == want && h.l_push.size() == (want.empty() ? 0u : 1u),
        "%s (want '%s', got '%s', %zu pushes)", what, want.c_str(),
        got.c_str(), h.l_push.size());
}

void SrpStreamFsmsSuite::owner_unbinds_first_then_the_rider() {
  int m = shared_stream_goes_quiet(2, "SH1");
  // a received LeaveAll: a rider's own applicant would answer it with an
  // Empty (VO + rLA! is LO); quiet, only the owner's re-join goes out
  h.la_rx();
  m = app_next(m, eRLA, false, true);
  m = ticks_follow_the_model(m, 2, 3,
                             "SH1: a received LeaveAll: each tx! sends what ONE "
                             "applicant sends");
  // the owner's A8: the rider takes the applicant in QA, nothing is sent
  h.ctl(false, 0, SID0, DA0, VID0);
  CHECK(h.l_app(1) == m && h.l_app(0) == VO && h.l_decl(1) == 2
        && h.l_decl(0) == 0,
        "SH2: the owner unbinds: the rider holds the applicant in its state, "
        "still Ready (app %s/%s, decl %d/%d)", SN[h.l_app(0)], SN[h.l_app(1)],
        h.l_decl(0), h.l_decl(1));
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH2: the owner unbinds: the next tx! sends no Lv and no New");
  m = app_next(m, eTx, false, true);
  // the declaration goes on: periodic! re-joins it from the rider's slot
  h.ptick();
  m = app_next(m, ePer, false, true);
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH2: the declaration goes on: periodic! re-joins it, JoinMt Ready");
  m = app_next(m, eTx, false, true);
  // the last declarer's A8: one Lv
  h.ctl(false, 1, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH2: the rider unbinds last: the next tx! sends one Lv, Ready");
  m = app_next(m, eTx, false, true);
  tick_sends("", "SH2: after the Lv nothing more is sent");
}

void SrpStreamFsmsSuite::rider_unbinds_first_then_the_owner() {
  int m = shared_stream_goes_quiet(2, "SH3");
  h.ctl(false, 1, SID0, DA0, VID0);
  CHECK(h.l_app(0) == m && h.l_app(1) == VO && h.l_decl(0) == 2
        && h.l_decl(1) == 0,
        "SH3: the rider unbinds: the owner keeps the applicant (app %s/%s, "
        "decl %d/%d)", SN[h.l_app(0)], SN[h.l_app(1)], h.l_decl(0),
        h.l_decl(1));
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH3: the rider unbinds: the next tx! sends no Lv and no New");
  m = app_next(m, eTx, false, true);
  h.ptick();
  m = app_next(m, ePer, false, true);
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH3: the declaration goes on: periodic! re-joins it, JoinMt Ready");
  m = app_next(m, eTx, false, true);
  h.ctl(false, 0, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH3: the owner unbinds last: the next tx! sends one Lv, Ready");
  m = app_next(m, eTx, false, true);
  tick_sends("", "SH3: after the Lv nothing more is sent");
}

void SrpStreamFsmsSuite::three_sinks_hand_the_applicant_down_the_line() {
  int m = shared_stream_goes_quiet(3, "SH4");
  for (int s = 0; s < 2; ++s) {
    h.ctl(false, s, SID0, DA0, VID0);
    const int next = s + 1;
    bool others_vo = true;
    for (int t = 0; t < 3; ++t) if (t != next && h.l_app(t) != VO) others_vo = false;
    CHECK(h.l_app(next) == m && others_vo && h.l_decl(next) == 2,
          "SH4: sink %d unbinds: sink %d, the lowest still declaring, holds the "
          "applicant in its state, the others are VO (app %s %s %s)", s, next,
          SN[h.l_app(0)], SN[h.l_app(1)], SN[h.l_app(2)]);
    tick_sends(model_words(app_msg(m, false, false), 2),
               "SH4: a handover sends no Lv and no New");
    m = app_next(m, eTx, false, true);
  }
  h.ctl(false, 2, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  tick_sends(model_words(app_msg(m, false, false), 2),
             "SH4: the third sink unbinds last: the next tx! sends one Lv, Ready");
}

void SrpStreamFsmsSuite::a_rider_rebinds_to_another_stream() {
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  int m = shared_stream_goes_quiet(2, "SH5");
  h.ctl(false, 1, SID0, DA0, VID0);
  h.ctl(true, 1, SIDB, DAB, VID0);
  h.inject(true, 1, SIDB, DAB, VID0, 3 /*JoinMt*/, 0, 500);
  int mb = app_next(VO, eNew, false, true);
  h.clear_logs();
  h.tick();
  std::string a = listener_words(h.l_push, SID0);
  std::string b = listener_words(h.l_push, SIDB);
  CHECK(a == model_words(app_msg(m, false, false), 2)
        && b == model_words(app_msg(mb, false, false), 2)
        && h.l_decl(0) == 2 && h.l_decl(1) == 2,
        "SH5: a rider rebinds to another stream: the owner's declaration goes "
        "on with no Lv and no New, the rider declares the new stream, New Ready "
        "(first stream '%s', new stream '%s', decl %d/%d)", a.c_str(), b.c_str(),
        h.l_decl(0), h.l_decl(1));
  m = app_next(m, eTx, false, true);
  mb = app_next(mb, eTx, false, true);
  // the owner, now alone on the first stream, unbinds: one Lv for it
  h.ctl(false, 0, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  h.clear_logs();
  h.tick();
  a = listener_words(h.l_push, SID0);
  b = listener_words(h.l_push, SIDB);
  CHECK(a == model_words(app_msg(m, false, false), 2)
        && b == model_words(app_msg(mb, false, false), 2),
        "SH5: the owner then unbinds: one Lv, Ready, for the first stream, and "
        "the new stream goes on (first stream '%s', new stream '%s')", a.c_str(),
        b.c_str());
}

// The owner unbinds after the walk has sent its New and before the walk
// reaches the rider (sink 5): the rider takes the applicant already walked,
// so this walk sends ONE New, as one applicant's one tx! does.
void SrpStreamFsmsSuite::a_handover_during_a_walk_keeps_one_tx() {
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID0);
  h.ctl(true, 5, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  const int m = app_next(VO, eNew, false, true);
  h.clear_logs();
  d->join_tick_i = 1;
  bool unbound = false;
  bool done = false;
  for (int i = 0; i < kTxopPollSteps && !done; ++i) {
    if (!unbound && !h.l_push.empty()) {     // the A8 lands on the next clock
      d->ctl_valid_i = 1; d->ctl_settle_i = 0; d->ctl_sink_i = 0;
      d->ctl_stream_id_i = SID0; d->ctl_da_i = DA0; d->ctl_vid_i = VID0;
      unbound = true;
    }
    h.step();
    done = d->l_txop_done_o != 0;
  }
  h.idle(2);
  const std::string got = listener_words(h.l_push, SID0);
  const int walked = app_next(m, eTx, false, true);
  CHECK(unbound && done && got == model_words(app_msg(m, false, false), 2)
        && h.l_app(5) == walked && h.l_app(0) == VO,
        "SH7: the owner unbinds while the walk is between it and the rider: the "
        "rider takes the walked applicant and the walk sends ONE New (got '%s', "
        "app %s/%s)", got.c_str(), SN[h.l_app(0)], SN[h.l_app(5)]);
}

// The owner's A8 and its A15 onto another stream on consecutive clocks,
// before the rider has taken the applicant: the settle hands it over
// instead of parking it (banner), so the first stream gets no Lv and no
// New, and the new stream is declared.
void SrpStreamFsmsSuite::an_owner_rebind_on_the_next_clock_hands_over() {
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  int m = shared_stream_goes_quiet(2, "SH8");
  d->ctl_valid_i = 1; d->ctl_settle_i = 0; d->ctl_sink_i = 0;
  d->ctl_stream_id_i = SID0; d->ctl_da_i = DA0; d->ctl_vid_i = VID0;
  h.step();
  d->ctl_valid_i = 1; d->ctl_settle_i = 1; d->ctl_sink_i = 0;
  d->ctl_stream_id_i = SIDB; d->ctl_da_i = DAB; d->ctl_vid_i = VID0;
  h.step();
  h.idle(2);
  const bool held = h.l_app(1) == m && h.l_app(0) == VO;
  h.inject(true, 1, SIDB, DAB, VID0, 3 /*JoinMt*/, 0, 500);
  const int mb = app_next(VO, eNew, false, true);
  h.clear_logs();
  h.tick();
  const std::string a = listener_words(h.l_push, SID0);
  const std::string b = listener_words(h.l_push, SIDB);
  CHECK(held && a == model_words(app_msg(m, false, false), 2)
        && b == model_words(app_msg(mb, false, false), 2) && h.l_decl(1) == 2,
        "SH8: the owner rebinds on the clock after its A8: the rider holds the "
        "applicant in its state, the first stream gets no Lv and no New, the new "
        "stream New Ready (held %d, first stream '%s', new stream '%s')", held,
        a.c_str(), b.c_str());
}

// The owner's A8, then on the next clock the rider's A15 onto another
// stream: the rider leaves before it takes the applicant, so the first
// stream, no longer wanted, gets one Lv, and the new stream is declared
// only as its own talker registers.
void SrpStreamFsmsSuite::a_rider_rebind_on_the_clock_after_the_owner_unbinds() {
  constexpr uint64_t SIDB = 0x02AABBCCDDEE0002ull;
  constexpr uint64_t DAB  = 0x91E0F0001235ull;
  int m = shared_stream_goes_quiet(2, "SH9");
  d->ctl_valid_i = 1; d->ctl_settle_i = 0; d->ctl_sink_i = 0;
  d->ctl_stream_id_i = SID0; d->ctl_da_i = DA0; d->ctl_vid_i = VID0;
  h.step();
  d->ctl_valid_i = 1; d->ctl_settle_i = 1; d->ctl_sink_i = 1;
  d->ctl_stream_id_i = SIDB; d->ctl_da_i = DAB; d->ctl_vid_i = VID0;
  h.step();
  h.idle(2);
  m = app_next(m, eLv, false, true);
  h.inject(true, 1, SIDB, DAB, VID0, 3 /*JoinMt*/, 0, 500);
  const int mb = app_next(VO, eNew, false, true);
  h.clear_logs();
  h.tick();
  const std::string a = listener_words(h.l_push, SID0);
  const std::string b = listener_words(h.l_push, SIDB);
  CHECK(a == model_words(app_msg(m, false, false), 2)
        && b == model_words(app_msg(mb, false, false), 2),
        "SH9: the rider rebinds on the clock after the owner's A8: the first "
        "stream gets one Lv, Ready, the new stream New Ready (first stream '%s', "
        "new stream '%s')", a.c_str(), b.c_str());
}

// ==== a parked Leave meets a sink that settles on its stream ==============
// (issue #193 with #188) A sink alone on SID0 re-binds and parks the owed
// Leave. A sink that then settles on SID0 joins the parked applicant's
// group as a rider, and its first declaration takes the applicant over: LA
// + New! is VN, as one sink's re-probe. So each tx! sends ONE value
// for SID0 and no Lv while a sink declares it (802.1Q-2018 §10.7.7, the
// §35.2.6 NOTE, §35.1.2.2), and a Milan v1.2 §4.2.7.2.2 peer Registrar,
// which deregisters at once on Lv, stays IN. While no sink on SID0
// declares, the owed Lv goes out as before (#188).
constexpr uint64_t SIDP = 0x02AABBCCDDEE0002ull;   // the parker's new stream
constexpr uint64_t DAP  = 0x91E0F0001235ull;

//! a Milan v1.2 §4.2.7.2.2 peer Registrar for SID0, fed the listener's
//! pushes in order: New/JoinIn/JoinMt register, Lv deregisters at once
struct PeerReg {
  bool in = false;
  int deregs = 0;
  int lvs = 0;
  void see(const std::vector<Push>& pushes) {
    for (const Push& p : pushes) {
      uint64_t got = 0;
      for (int i = 0; i < 8; ++i) got = (got << 8) | p.val[i];
      if (p.type != 3 || got != SID0) continue;
      if (p.code == 0 || p.code == 1 || p.code == 3) in = true;
      if (p.code == 5) {
        ++lvs;
        if (in) ++deregs;
        in = false;
      }
    }
  }
};

//! The parker alone on SID0, registered Ready and ticked to QA, re-binds to
//! SIDP (an A8 first when via_a8), whose talker then registers: the
//! applicant owes SID0's Lv and is parked. Returns the model state, LA.
int SrpStreamFsmsSuite::park_the_stream_alone(int parker, bool via_a8) {
  h.reset();
  h.ctl(true, parker, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  for (int k = 0; k < 3; ++k) {
    h.tick();
    m = app_next(m, eTx, false, true);
  }
  if (via_a8) h.ctl(false, parker, SID0, DA0, VID0);
  h.ctl(true, parker, SIDP, DAP, VID0);
  h.inject(true, 1, SIDP, DAP, VID0, 3 /*JoinMt*/, 0, 500);
  return app_next(m, eLv, false, true);
}

void SrpStreamFsmsSuite::a_new_sink_takes_over_a_parked_leave(int parker, int joiner,
                                                               bool via_a8,
                                                               const char* tag) {
  int m = park_the_stream_alone(parker, via_a8);
  const bool parked = h.l_app(parker) == m && m == LA;
  PeerReg peer;
  peer.in = true;                // declared by the parker until it re-bound
  h.ctl(true, joiner, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  m = app_next(m, eNew, false, true);
  const bool took = h.l_app(joiner) == m && h.l_decl(joiner) == 2;
  int mp = app_next(VO, eNew, false, true);   // the parker's request, no longer held
  bool one = true;
  std::string got;
  for (int k = 0; k < 3; ++k) {
    h.clear_logs();
    h.tick();
    peer.see(h.l_push);
    const std::string a = listener_words(h.l_push, SID0);
    const std::string b = listener_words(h.l_push, SIDP);
    if (one && (a != model_words(app_msg(m, false, false), 2)
                || b != model_words(app_msg(mp, false, false), 2))) {
      one = false;
      got = "tx! " + std::to_string(k + 1) + ": '" + a + "' and '" + b + "'";
    }
    m = app_next(m, eTx, false, true);
    mp = app_next(mp, eTx, false, true);
  }
  CHECK(parked && took && one && peer.in && peer.deregs == 0 && peer.lvs == 0
        && h.l_decl(joiner) == 2 && h.l_decl(parker) == 2,
        "%s: sink %d parks its Leave of the stream (%s), sink %d settles on it and "
        "declares: it takes the parked applicant over, LA + New! = VN, each tx! "
        "sends ONE New or JoinMt for it and no Lv, the parker declares its new "
        "stream, a peer Registrar never deregisters (parked %d, took %d, one %d "
        "got %s, peer IN %d deregs %d Lv %d, decl %d/%d)", tag, parker,
        via_a8 ? "A8, then A15" : "A15", joiner, parked, took, one, got.c_str(),
        peer.in, peer.deregs, peer.lvs, h.l_decl(joiner), h.l_decl(parker));
}

// No sink on SID0 declares (the joiner's talker has not registered yet),
// and a LeaveAll is received: the joiner, a rider, does not answer it, and
// the parked applicant's owed Lv goes out once (#188). The joiner's later
// declaration is the same applicant's next New.
void SrpStreamFsmsSuite::a_parked_leave_is_still_sent_while_no_sink_declares() {
  int m = park_the_stream_alone(1, false);
  h.ctl(true, 0, SID0, DA0, VID0);
  h.la_rx();
  m = app_next(m, eRLA, false, true);
  h.clear_logs();
  h.tick();
  const std::string a = listener_words(h.l_push, SID0);
  const bool rider_vo = h.l_app(0) == VO;
  m = app_next(m, eTx, false, true);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  m = app_next(m, eNew, false, true);
  h.clear_logs();
  h.tick();
  const std::string b = listener_words(h.l_push, SID0);
  CHECK(a == model_words(app_msg(LA, false, false), 2) && rider_vo
        && b == model_words(app_msg(m, false, false), 2),
        "SH14: a parked Leave while no sink on the stream declares: the next tx! "
        "sends its one Lv, Ready, and nothing from the rider after a received "
        "LeaveAll; the joiner's later declaration is one New (first '%s', rider "
        "%s, then '%s')", a.c_str(), SN[h.l_app(0)], b.c_str());
}

// The walk sends the parked Lv (sink 0), then, before it reaches sink 7 on
// the stream, sink 7's talker registers: the applicant has had its tx! in
// this walk, so the New waits for the next tx! (one value per tick).
void SrpStreamFsmsSuite::a_declaration_after_the_parked_lv_waits_for_the_next_tx() {
  int m = park_the_stream_alone(0, false);
  h.ctl(true, 7, SID0, DA0, VID0);
  h.clear_logs();
  d->join_tick_i = 1;
  bool registered = false;
  bool done = false;
  for (int i = 0; i < kTxopPollSteps && !done; ++i) {
    if (!registered && !h.l_push.empty()) {   // the Advertise lands next clock
      d->evt_valid_i = 1; d->evt_msrp_i = 1; d->evt_attr_type_i = 1;
      d->evt_stream_id_i = SID0; d->evt_da_i = DA0; d->evt_vid_i = VID0;
      d->evt_mrp_event_i = 3; d->evt_fourpacked_i = 0;
      d->evt_acc_latency_i = 500; d->evt_failure_system_id_i = 0;
      d->evt_failure_code_i = 0;
      registered = true;
    }
    h.step();
    done = d->l_txop_done_o != 0;
  }
  h.idle(2);
  const std::string a = listener_words(h.l_push, SID0);
  m = app_next(app_next(m, eTx, false, true), eNew, false, true);
  h.clear_logs();
  h.tick();
  const std::string b = listener_words(h.l_push, SID0);
  CHECK(registered && done && a == model_words(app_msg(LA, false, false), 2)
        && b == model_words(app_msg(m, false, false), 2) && h.l_decl(7) == 2,
        "SH15: sink 7 declares the stream after the walk sent its parked Lv: the "
        "New waits for the next tx!, one value per tick (this walk '%s', the next "
        "'%s', decl %d)", a.c_str(), b.c_str(), h.l_decl(7));
}

// Sink 0 re-binds from SIDP onto SID0, which sink 1 declares: sink 0 parks
// SIDP's Leave and its SID0 request waits. Sink 1 then re-binds away and
// parks SID0's Leave, beside sink 0 on SID0. The walk sends sink 0's owed
// Lv; sink 0 then declares SID0 and takes the parked applicant over before
// the walk reaches sink 1, so no Lv goes out for SID0. The applicant has
// not had this walk's tx!: the walk goes back to sink 0 and sends its New
// (LA + New! is VN, VN on tx! is sN), and the next tx! the next New.
void SrpStreamFsmsSuite::a_sink_whose_own_leave_goes_first_takes_over_a_parked_leave() {
  constexpr uint64_t SIDQ = 0x02AABBCCDDEE0003ull;
  constexpr uint64_t DAQ  = 0x91E0F0001236ull;
  h.reset();
  h.ctl(true, 0, SIDP, DAP, VID0);
  h.ctl(true, 1, SID0, DA0, VID0);
  h.inject(true, 1, SIDP, DAP, VID0, 3 /*JoinMt*/, 0, 500);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  for (int k = 0; k < 3; ++k) {
    h.tick();
    m = app_next(m, eTx, false, true);
  }
  PeerReg peer;
  peer.in = true;
  h.ctl(true, 0, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  h.ctl(true, 1, SIDQ, DAQ, VID0);
  m = app_next(app_next(m, eLv, false, true), eNew, false, true);
  h.clear_logs();
  h.tick();
  peer.see(h.l_push);
  const std::string a = listener_words(h.l_push, SID0);
  const std::string p = listener_words(h.l_push, SIDP);
  const std::string wa = model_words(app_msg(m, false, false), 2);
  m = app_next(m, eTx, false, true);
  h.clear_logs();
  h.tick();
  peer.see(h.l_push);
  const std::string b = listener_words(h.l_push, SID0);
  CHECK(a == wa && p == model_words(app_msg(LA, false, false), 2)
        && b == model_words(app_msg(m, false, false), 2) && peer.in
        && peer.deregs == 0 && h.l_decl(0) == 2,
        "SH16: a sink whose own Leave goes first, then declares the stream parked "
        "by another sink: it takes the parked applicant over, no Lv for the stream, "
        "and the walk goes back to it for its New, then the next New (stream '%s' "
        "then '%s', own Leave '%s', peer IN %d deregs %d, decl %d)", a.c_str(),
        b.c_str(), p.c_str(), peer.in, peer.deregs, h.l_decl(0));
}

// Sink 1 declares SIDP. Sink 0, alone on SID0 in QA, re-binds onto SIDP:
// its owed Lv of SID0 is parked, and its matcher now pairs it with sink 1.
// Before any tx! it re-binds again, onto SIDQ: parked again, it keeps the
// pairs of the value it holds (SID0, no other sink's), not SIDP's. The
// next tx! sends SID0's Lv once and leaves sink 1's SIDP applicant as the
// model has it (802.1Q-2018 §10.7.7; the owed Leave of #188).
void SrpStreamFsmsSuite::a_sink_parked_again_keeps_the_pairs_of_the_value_it_holds() {
  constexpr uint64_t SIDQ = 0x02AABBCCDDEE0003ull;
  constexpr uint64_t DAQ  = 0x91E0F0001236ull;
  h.reset();
  h.ctl(true, 1, SIDP, DAP, VID0);
  h.inject(true, 1, SIDP, DAP, VID0, 3 /*JoinMt*/, 0, 500);
  h.ctl(true, 0, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m0 = app_next(VO, eNew, false, true);
  int m1 = app_next(VO, eNew, false, true);
  for (int k = 0; k < 3; ++k) {
    h.tick();
    m0 = app_next(m0, eTx, false, true);
    m1 = app_next(m1, eTx, false, true);
  }
  h.ctl(true, 0, SIDP, DAP, VID0);
  h.ctl(true, 0, SIDQ, DAQ, VID0);
  m0 = app_next(m0, eLv, false, true);
  std::string a[2];
  std::string p[2];
  bool model = true;
  for (int k = 0; k < 2; ++k) {
    h.clear_logs();
    h.tick();
    a[k] = listener_words(h.l_push, SID0);
    p[k] = listener_words(h.l_push, SIDP);
    model = model && a[k] == model_words(app_msg(m0, false, false), 2)
            && p[k] == model_words(app_msg(m1, false, false), 2);
    m0 = app_next(m0, eTx, false, true);
    m1 = app_next(m1, eTx, false, true);
  }
  CHECK(model && h.l_app(1) == m1 && h.l_decl(1) == 2,
        "SH17: a parked sink re-binds again while another sink declares the stream "
        "it re-bound to first: it keeps the pairs of the value it holds, so the next "
        "tx! sends that value's one Lv, Ready, and the other stream goes on as one "
        "applicant (held '%s' then '%s', other '%s' then '%s', app1 %s, decl1 %d)",
        a[0].c_str(), a[1].c_str(), p[0].c_str(), p[1].c_str(), SN[h.l_app(1)],
        h.l_decl(1));
}

// ==== each applicant has one transmit per walk, whichever slot holds it ===
// (issue #193; 802.1Q-2018 Table 10-3, §10.7.7) The walk marks every slot it
// passes and every member of a group whose applicant it serves, and a
// handover carries the mark. An applicant handed over, after the walk
// passed the taker, from a slot the walk had not reached has the walk's tx!
// or txLA! before the walk ends: the walk goes back to the taker. Sink 0
// rides on VLAN 3 (VID1), so its talker registers alone. Every expectation
// is the suite's Table 10-3 model of ONE applicant.
constexpr uint16_t VID1 = 3;

//! One tick. At clock `at` of its walk (at < 0: the clock after its first
//! push) `sink`'s A15 onto {sid, da, vid}, or, when sink < 0, a Talker
//! Advertise of them registers (JoinMt). True when the walk ended; the
//! pushes stay in h.l_push.
bool SrpStreamFsmsSuite::tick_with_input_at(int at, int sink, uint64_t sid,
                                            uint64_t da, uint16_t vid) {
  h.clear_logs();
  d->join_tick_i = 1;
  bool fired = false;
  bool done = false;
  for (int i = 0; i < kTxopPollSteps && !done; ++i) {
    if (!fired && (at < 0 ? !h.l_push.empty() : i == at)) {
      if (sink >= 0) {
        d->ctl_valid_i = 1; d->ctl_settle_i = 1; d->ctl_sink_i = sink;
        d->ctl_stream_id_i = sid; d->ctl_da_i = da; d->ctl_vid_i = vid;
      } else {
        d->evt_valid_i = 1; d->evt_msrp_i = 1; d->evt_attr_type_i = 1;
        d->evt_stream_id_i = sid; d->evt_da_i = da; d->evt_vid_i = vid;
        d->evt_mrp_event_i = 3; d->evt_fourpacked_i = 0;
        d->evt_acc_latency_i = 500; d->evt_failure_system_id_i = 0;
        d->evt_failure_code_i = 0;
      }
      fired = true;
    }
    h.step();
    done = d->l_txop_done_o != 0;
  }
  h.idle(2);
  return fired && done;
}

// Sink 0 and a higher sink on SID0; the higher one declares alone and is
// ticked to QA. An own LeaveAll makes the next walk txLA!. At clock 1, 2 or
// 3 of that walk, after it passed sink 0 and before it reaches the holder,
// sink 0's talker registers: sink 0, now the owner, takes the QA applicant
// over. The applicant has the LeaveAll walk's txLA! in sink 0's slot: one
// JoinMt, Ready (QA on txLA! is sJ), and nothing at the next tx!.
void SrpStreamFsmsSuite::a_handover_after_the_walk_passed_the_taker_has_the_leaveall_walks_txla() {
  for (int holder : {5, 7}) {
    std::string bad;
    for (int at = 1; at <= 3 && bad.empty(); ++at) {
      h.reset();
      h.ctl(true, 0, SID0, DA0, VID1);
      h.ctl(true, holder, SID0, DA0, VID0);
      h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
      int m = app_next(VO, eNew, false, true);
      for (int k = 0; k < 3; ++k) {
        h.tick();
        m = app_next(m, eTx, false, true);
      }
      h.la_own();
      const bool done = tick_with_input_at(at, -1, SID0, DA0, VID1);
      const std::string w = listener_words(h.l_push, SID0);
      const std::string want = model_words(app_msg(m, true, false), 2);
      m = app_next(m, eTxLA, false, true);
      const bool took = h.l_app(0) == m && h.l_app(holder) == VO && h.l_decl(0) == 2;
      h.clear_logs();
      h.tick();
      const std::string nx = listener_words(h.l_push, SID0);
      if (!done || !took || w != want || nx != model_words(app_msg(m, false, false), 2)) {
        bad = "clock " + std::to_string(at) + ": walk '" + w + "', next '" + nx + "', app "
              + SN[h.l_app(0)] + "/" + SN[h.l_app(holder)] + ", ended "
              + std::to_string(done);
      }
    }
    CHECK(bad.empty(),
          "SH18: an own LeaveAll's walk passes sink 0, then sink 0's talker registers and "
          "it takes the QA applicant over from sink %d: the walk goes back to sink 0 and "
          "sends ONE JoinMt, Ready, and the next tx! nothing (%s)", holder,
          bad.empty() ? "clocks 1-3" : bad.c_str());
  }
}

// The same handover in a plain tx! walk, the applicant AN (its New sent
// once): the walk goes back to sink 0 and sends the New (AN on tx! is sN),
// and the next tx! the JoinMt.
void SrpStreamFsmsSuite::a_handover_after_the_walk_passed_the_taker_has_the_walks_tx() {
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID1);
  h.ctl(true, 5, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  h.tick();
  m = app_next(m, eTx, false, true);
  const bool done = tick_with_input_at(2, -1, SID0, DA0, VID1);
  const std::string w = listener_words(h.l_push, SID0);
  const std::string want = model_words(app_msg(m, false, false), 2);
  m = app_next(m, eTx, false, true);
  const bool took = h.l_app(0) == m && h.l_app(5) == VO;
  h.clear_logs();
  h.tick();
  const std::string nx = listener_words(h.l_push, SID0);
  CHECK(done && took && w == want && nx == model_words(app_msg(m, false, false), 2),
        "SH19: a tx! walk passes sink 0, then sink 0 takes the AN applicant over from "
        "sink 5: the walk goes back to sink 0 and sends its New once, the next tx! the "
        "JoinMt (walk '%s', next '%s', app %s/%s, ended %d)", w.c_str(), nx.c_str(),
        SN[h.l_app(0)], SN[h.l_app(5)], done);
}

// Sink 0 rides SID0 (its talker silent), which sink 5 declares alone in QA.
// During an own LeaveAll's walk, after it passed sink 0, sink 5 re-binds
// onto SIDP: it hands the applicant to sink 0, and as no sink declares SID0
// the applicant withdraws (Lv!, LA). It has the LeaveAll walk's txLA! in
// sink 0's slot: LA on txLA! is LO, no message, so the next tx! sends Empty
// (LO on tx! is s), not Lv.
void SrpStreamFsmsSuite::a_rebind_handover_after_the_walk_passed_the_taker_has_the_txla() {
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID1);
  h.ctl(true, 5, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  for (int k = 0; k < 3; ++k) {
    h.tick();
    m = app_next(m, eTx, false, true);
  }
  h.la_own();
  const bool done = tick_with_input_at(2, 5, SIDP, DAP, VID0);
  const std::string w = listener_words(h.l_push, SID0);
  m = app_next(m, eLv, false, true);
  const std::string want = model_words(app_msg(m, true, false), 2);
  m = app_next(m, eTxLA, false, true);
  const bool took = h.l_app(0) == m;
  h.clear_logs();
  h.tick();
  const std::string nx = listener_words(h.l_push, SID0);
  CHECK(done && took && w == want && nx == model_words(app_msg(m, false, false), 2),
        "SH20: an own LeaveAll's walk passes sink 0, then sink 5 re-binds and hands "
        "the applicant to sink 0, which withdraws it: the walk goes back to sink 0, LA "
        "on txLA! is LO, so the walk sends nothing and the next tx! one Empty, not Lv "
        "(walk '%s', next '%s', app %s, ended %d)", w.c_str(), nx.c_str(),
        SN[h.l_app(0)], done);
}

// Sink 1 declares SID0 alone; sink 6 (its talker silent) rides it. Sink 1
// unbinds, the last declarer: Lv!, LA. The walk sends the Lv; on the next
// clock, before the walk reaches sink 6, sink 6's talker registers and sink
// 6 declares: New! on the applicant that has had this walk's tx!, so the
// New waits for the next tx! (the walk marks the group it serves).
void SrpStreamFsmsSuite::a_declaration_after_the_last_declarers_lv_waits_for_the_next_tx() {
  h.reset();
  h.ctl(true, 1, SID0, DA0, VID0);
  h.ctl(true, 6, SID0, DA0, VID1);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  for (int k = 0; k < 3; ++k) {
    h.tick();
    m = app_next(m, eTx, false, true);
  }
  h.ctl(false, 1, SID0, DA0, VID0);
  m = app_next(m, eLv, false, true);
  const bool done = tick_with_input_at(-1, -1, SID0, DA0, VID1);
  const std::string w = listener_words(h.l_push, SID0);
  const std::string want = model_words(app_msg(m, false, false), 2);
  m = app_next(app_next(m, eTx, false, true), eNew, false, true);
  h.clear_logs();
  h.tick();
  const std::string nx = listener_words(h.l_push, SID0);
  CHECK(done && w == want && nx == model_words(app_msg(m, false, false), 2)
        && h.l_decl(6) == 2,
        "SH21: a rider ahead of the walk declares after the walk sent the last "
        "declarer's Lv: the New waits for the next tx!, one value per walk (this walk "
        "'%s', the next '%s', decl %d)", w.c_str(), nx.c_str(), h.l_decl(6));
}

// Sink 0 declares SID0 alone, in AA. On the clock after the walk pushes its
// JoinMt, sink 6 settles on SID0: a rider that joins the group after its
// applicant's transmit. The walk passes sink 6 (marked as it passes) and
// ends; the next tx! follows the model.
void SrpStreamFsmsSuite::a_rider_settling_after_its_streams_transmit_lets_the_walk_end() {
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID0);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  for (int k = 0; k < 2; ++k) {
    h.tick();
    m = app_next(m, eTx, false, true);
  }
  const bool done = tick_with_input_at(-1, 6, SID0, DA0, VID1);
  const std::string w = listener_words(h.l_push, SID0);
  const std::string want = model_words(app_msg(m, false, false), 2);
  m = app_next(m, eTx, false, true);
  h.clear_logs();
  const bool next = h.tick();
  const std::string nx = listener_words(h.l_push, SID0);
  CHECK(done && next && w == want && nx == model_words(app_msg(m, false, false), 2)
        && h.l_app(6) == VO,
        "SH22: a sink settles on the stream after the walk sent its JoinMt, ahead of "
        "the walk: the walk passes it and ends, one value, and the next tx! follows the "
        "model (walk '%s' ended %d, next '%s' ended %d, rider %s)", w.c_str(), done,
        nx.c_str(), next, SN[h.l_app(6)]);
}

// Sink 0 declares SIDP and sink 1 SID0, each alone, in QA. Sink 0 re-binds
// onto SID0 (its talker silent there): it parks SIDP's Lv. Sink 1 re-binds
// onto SIDQ: it parks SID0's Lv, the value sink 0's matcher is on. Two
// parked applicants are never one group: an own LeaveAll's walk gives each
// its txLA! (LA to LO, no message, still parked), and the next tx! sends
// each one's Empty (LO on tx! is s), no Lv.
void SrpStreamFsmsSuite::two_parked_leaves_each_keep_their_own_group() {
  constexpr uint64_t SIDQ = 0x02AABBCCDDEE0003ull;
  constexpr uint64_t DAQ  = 0x91E0F0001236ull;
  h.reset();
  h.ctl(true, 0, SIDP, DAP, VID0);
  h.ctl(true, 1, SID0, DA0, VID0);
  h.inject(true, 1, SIDP, DAP, VID0, 3 /*JoinMt*/, 0, 500);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  for (int k = 0; k < 3; ++k) {
    h.tick();
    m = app_next(m, eTx, false, true);
  }
  h.ctl(true, 0, SID0, DA0, VID1);
  h.ctl(true, 1, SIDQ, DAQ, VID0);
  m = app_next(m, eLv, false, true);
  const std::string wla = model_words(app_msg(m, true, false), 2);
  m = app_next(m, eTxLA, false, true);
  h.la_own();
  h.clear_logs();
  h.tick();
  const std::string a0 = listener_words(h.l_push, SID0);
  const std::string p0 = listener_words(h.l_push, SIDP);
  const bool kept = h.l_app(0) == m && h.l_app(1) == m;
  const std::string want = model_words(app_msg(m, false, false), 2);
  h.clear_logs();
  h.tick();
  const std::string a1 = listener_words(h.l_push, SID0);
  const std::string p1 = listener_words(h.l_push, SIDP);
  CHECK(kept && a0 == wla && p0 == wla && a1 == want && p1 == want,
        "SH23: two parked Leaves, one on the value the other's matcher is on: each "
        "has an own LeaveAll's txLA! (LO, parked), and the next tx! sends each one's "
        "Empty, no Lv (LeaveAll walk '%s'/'%s', apps %s/%s, then '%s'/'%s')",
        a0.c_str(), p0.c_str(), SN[h.l_app(0)], SN[h.l_app(1)], a1.c_str(), p1.c_str());
}

//! n ticks; each must send what the model applicant m sends with fp
int SrpStreamFsmsSuite::ticks_follow_the_model(int m, int fp, int n,
                                               const char* what) {
  for (int k = 0; k < n; ++k) {
    tick_sends(model_words(app_msg(m, false, false), fp), what);
    m = app_next(m, eTx, false, true);
  }
  return m;
}

// Ready (sink 0, VLAN VID0) with AskingFailed (sink 1, another VLAN, its
// talker Failed) on one stream_id: the one declaration is ReadyFailed
// (802.1Q-2018 §35.1.2.2; §35.2.4.4.3, Table 35-14; Milan v1.2 §4.2.7);
// each sink still reports its own type. A change of the combination is
// New!, a change in place (§35.2.6).
void SrpStreamFsmsSuite::ready_with_asking_failed_declares_ready_failed() {
  constexpr int kAskFail = 1;
  constexpr int kReadyFail = 3;
  h.reset();
  h.ctl(true, 0, SID0, DA0, VID0);
  h.ctl(true, 1, SID0, DA0, VID1);
  h.inject(true, 1, SID0, DA0, VID0, 3 /*JoinMt*/, 0, 500);
  int m = app_next(VO, eNew, false, true);
  m = ticks_follow_the_model(m, kDeclReady, 4, "SH6 precondition: Ready alone");
  h.inject(true, 2, SID0, DA0, VID1, 3 /*JoinMt*/, 0, 500, 0xBB, 1);
  CHECK(h.l_decl(0) == kDeclReady && h.l_decl(1) == kAskFail,
        "SH6: each sink reports its own type, Ready and AskingFailed (decl %d/%d)",
        h.l_decl(0), h.l_decl(1));
  m = app_next(m, eNew, false, true);
  m = ticks_follow_the_model(m, kReadyFail, 3,
                             "SH6: Ready with AskingFailed: New, ReadyFailed");
  // the Ready owner unbinds: the rider takes the applicant and, the
  // combination now AskingFailed alone, New! declares it
  h.ctl(false, 0, SID0, DA0, VID0);
  m = app_next(m, eNew, false, true);
  m = ticks_follow_the_model(m, kAskFail, 3,
                             "SH6: the Ready owner unbinds: New, AskingFailed");
  h.ctl(false, 1, SID0, DA0, VID1);
  m = app_next(m, eLv, false, true);
  (void)ticks_follow_the_model(m, kAskFail, 2,
                               "SH6: the last sink unbinds: one Lv, AskingFailed");
}

// SC1: independently compose Table 10-4's LV/leavetimer row with each
// received event at MT. New/JoinIn/JoinMt end IN; Lv/LA/In/Mt end MT.
// Exercise the real input strobes together, after the leave ARM has issued.
void SrpStreamFsmsSuite::expiry_and_received_event_follow_table_10_4() {
  for (bool listener : {false, true}) for (bool own : {false, true})
    for (int variant : {0, 1}) for (int slot : {0, 7}) for (int ev = 0; ev < 8; ++ev) {
      h.reset();
      const int type = listener ? 1 + variant : 3;
      const int fp = 2 + variant;
      if (listener) h.ctl(true, slot, SID0, DA0, VID0);
      else h.gate(true, slot, SID0, DA0, VID0);
      h.inject(true, type, SID0, DA0, VID0, 0, fp, 500, 0x1234, 1);
      if (own) h.la_own(); else h.la_rx();
      const bool aged = (listener ? h.l_regst(slot) : h.t_reg(slot)) == 2;
      h.clear_logs();
      d->exp_valid_i = 1;
      d->exp_slot_i = (listener ? 24 : 16) + slot;
      if (ev < 6) h.inject(true, type, SID0, DA0, VID0, ev, fp, 501, 0x5678, 2);
      else if (ev == 6) h.la_rx(); else h.la_own();
      const bool registering = ev == 0 || ev == 1 || ev == 3;
      const int want = registering ? 1 : 0;
      const int reg = listener ? h.l_regst(slot) : h.t_reg(slot);
      const auto& arms = listener ? h.l_arm : h.t_arm;
      const bool timer = registering ? arms.size() == 1 && arms[0].cancel : arms.empty();
      const bool indications = listener
        ? h.l_unreg[slot] == int(!registering) && h.l_reg[slot] == 0
          && h.l_fchg[slot] == int(registering && variant == 1)
        : h.t_chg[slot] == int(!registering);
      const bool published = listener ? h.l_tkreg(slot) == (registering ? type : 0)
        : h.t_lstn(slot) == (registering ? fp : 0)
          && bool((d->t_active_o >> slot) & 1) == registering;
      CHECK(aged && reg == want && timer && indications && published,
            "SC1: plane=%s cause=%s variant=%d slot=%d event=%d expiry then event: "
            "reg=%d want=%d timer=%d indications=%d published=%d",
            listener ? "listener" : "talker", own ? "own" : "peer", variant, slot,
            ev, reg, want, timer, indications, published);
    }
}

int SrpStreamFsmsSuite::run() {
  expiry_and_received_event_follow_table_10_4();
  rx_events_match_table_10_3();
  talker_tx_rows_send_the_table_10_3_message();
  listener_tx_rows_send_the_table_10_3_message();
  talker_registrar_follows_table_10_4_with_delta13();
  talker_declaration_tracks_ready_and_admission();
  listener_matcher_registers_swaps_and_unregisters();
  vlan_user_handshake_holds_and_collapses();
  talker_tracker_isolates_applications();
  received_leave_all_reaches_only_its_attribute_type();
  run_b_switch_leave_all_keeps_the_listener_registration_in();
  domain_only_leave_all_never_ages_the_listener_registrar();
  listener_only_leave_all_ages_the_listener_registrar_to_mt();
  listener_failure_change_notifies_without_redeclaring();
  listener_failure_change_compares_each_sink_with_its_own_latch();
  listener_failure_change_notifies_registered_sinks_on_a_shared_stream();
  settle_keeps_the_owed_listener_leave();
  a_settle_while_parked_keeps_the_held_stream();
  a_parked_leave_survives_txla_and_received_values();
  a_sink_with_no_record_owes_nothing();
  owner_unbinds_first_then_the_rider();
  rider_unbinds_first_then_the_owner();
  three_sinks_hand_the_applicant_down_the_line();
  a_rider_rebinds_to_another_stream();
  a_handover_during_a_walk_keeps_one_tx();
  an_owner_rebind_on_the_next_clock_hands_over();
  a_rider_rebind_on_the_clock_after_the_owner_unbinds();
  a_new_sink_takes_over_a_parked_leave(1, 0, false, "SH10");
  a_new_sink_takes_over_a_parked_leave(0, 1, false, "SH11");
  a_new_sink_takes_over_a_parked_leave(1, 0, true, "SH12");
  a_new_sink_takes_over_a_parked_leave(0, 1, true, "SH13");
  a_parked_leave_is_still_sent_while_no_sink_declares();
  a_declaration_after_the_parked_lv_waits_for_the_next_tx();
  a_sink_whose_own_leave_goes_first_takes_over_a_parked_leave();
  a_sink_parked_again_keeps_the_pairs_of_the_value_it_holds();
  a_handover_after_the_walk_passed_the_taker_has_the_leaveall_walks_txla();
  a_handover_after_the_walk_passed_the_taker_has_the_walks_tx();
  a_rebind_handover_after_the_walk_passed_the_taker_has_the_txla();
  a_declaration_after_the_last_declarers_lv_waits_for_the_next_tx();
  a_rider_settling_after_its_streams_transmit_lets_the_walk_end();
  two_parked_leaves_each_keep_their_own_group();
  ready_with_asking_failed_declares_ready_failed();

  printf("%d checks: %d PASS, %d FAIL\n", checks, checks - fails, fails);
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  SrpStreamFsmsSuite suite;
  return suite.run();
}
