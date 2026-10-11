// SPDX-License-Identifier: CERN-OHL-W-2.0
// tb/aecp_cmds - what KL_aecp_engine decodes and dispatches, observed.
//
// WHY THIS EXISTS (issue #73, REQ-VER-002). The AECP command table
// (hdl/aecp/ucode/aecp_commands.json) says, for every served command, which µPC
// programs the engine dispatches it to and the shortest command it accepts.
// Reading that off the engine's source text proves nothing about the elaborated
// design, so this bench asks the engine: it drives KL_aecp_engine alone, with
// every opcode 0x0000-0x007F, every length and every descriptor type of the
// sweep below, and records the µPC the engine hands its µCPU (aecp_cmds_wrap's
// obs_upc_o) and the response it sends. scripts/check_aecp_commands.py
// --observed turns that into tb/aecp_cmds/decode.json and holds it to the
// table; `make check` holds the table to the recorded file.
//
// THE SWEEP IS THE SAME FOR EVERY OPCODE. No stimulus is chosen per command:
// each opcode gets every payload template below at every listed length, so a
// decode keyed on the wrong opcode, an arm redirected to another program or a
// length floor moved is seen wherever it lands. The templates place the
// fields the AECP command set gives meaning to (descriptor_type @24, a second
// type or a count @28, SET_STREAM_INFO's flags @28 and latency @48, LOCK's
// target @36), each over every descriptor type 0x0000-0x0028 and 0xFFFF; the
// descriptor_type template runs at every cdl from 12 to 100.
// Then, for every program an opcode reached, the stimulus that first reached
// it is replayed at every shorter length, down to 12: the shortest length at
// which the engine still dispatches that program is its observed floor.
//
// Beside the AEM sweep: AVC and EXTENDED (message types 4 and 14, both in the
// AEM bucket), ADDRESS_ACCESS, the MVU bucket over every command_type with a
// right and four wrong protocol_ids, GET_DYNAMIC_INFO carrying one record of
// every command_type, and every unsolicited job kind.
//
// The bench writes obj_dir/observed.json (µPC addresses, not names) and exits
// 0 when every trial ended; the verdict is the gate's.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../common/verilator_harness.hpp"
#include "Vaecp_cmds_wrap.h"
#include "verilated.h"

namespace {

constexpr uint32_t DESC_BASE = 0x20000000u;
constexpr uint32_t RESP_BASE = 0x20100000u;
constexpr uint64_t ENTITY_ID = 0x000102FFFE030405ull;
constexpr int PROTO_AEM = 2;
constexpr int PROTO_MVU = 3;
constexpr int PROTO_AA = 4;
constexpr int TRIAL_BUDGET = 40000;   // clocks; a trial that runs out is a TIMEOUT

Vaecp_cmds_wrap* dut = nullptr;
std::vector<uint8_t> image;

// ---------------------------------------------------------------------------
// the environment: descriptor memory, response memory, NVM, RX slot, TX slot
// ---------------------------------------------------------------------------
struct Burst {
  bool busy = false;
  uint32_t addr = 0;
  int beats = 0;
  int idx = 0;
};

Burst dmem, rmem_rd;
std::vector<uint8_t> rmem(1024, 0);
bool rmem_done_next = false;
int nvm_state = 0;            // 0 idle, 1 grant, 2 answer
bool nvm_we = false;
std::vector<uint8_t> slot(576, 0);
uint8_t rx_data_next = 0;
bool gnt_next = false;
bool gnt_pending = false;
std::vector<uint8_t> frame(1600, 0);

uint64_t be64(const std::vector<uint8_t>& m, uint32_t off, uint8_t pad) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    uint32_t k = off + uint32_t(i);
    v = (v << 8) | (k < m.size() ? m[k] : pad);
  }
  return v;
}

struct Observed {
  std::vector<std::pair<int, bool>> disp;   // (µPC, batch)
  bool resp = false;
  int frame_len = 0;
};
Observed obs;

void drive() {
  dut->mem_req_ready_i = !dmem.busy;
  dut->mem_rsp_valid_i = dmem.busy;
  dut->mem_rsp_data_i = dmem.busy ? be64(image, dmem.addr - DESC_BASE + 8u * dmem.idx, 0xA5) : 0;
  dut->mem_rsp_last_i = dmem.busy && dmem.idx == dmem.beats - 1;
  dut->mem_rsp_err_i = 0;
  dut->rmem_req_ready_i = !rmem_rd.busy;
  dut->rmem_rsp_valid_i = rmem_rd.busy;
  dut->rmem_rsp_data_i = rmem_rd.busy ? be64(rmem, rmem_rd.addr - RESP_BASE + 8u * rmem_rd.idx, 0) : 0;
  dut->rmem_rsp_last_i = rmem_rd.busy && rmem_rd.idx == rmem_rd.beats - 1;
  dut->rmem_rsp_err_i = 0;
  dut->rmem_wr_ready_i = !rmem_done_next;
  dut->rmem_wr_done_i = rmem_done_next;
  dut->d3_m_gnt_i = nvm_state == 1;
  // a restore READ answers "erased" (UNFRAMED, nothing forwarded): a blank
  // device, so the D3 restore ends on defaults; a commit is refused DEVICE
  dut->d3_m_err_i = nvm_state == 2;
  dut->d3_m_err_cause_i = nvm_state == 2 ? (nvm_we ? 1 : 2) : 0;
  dut->d3_m_done_i = 0;
  dut->rxs_rd_data_i = rx_data_next;
  dut->txs_alloc_gnt_i = gnt_next;
}

void sample() {
  if (!dmem.busy) {
    if (dut->mem_req_valid_o) {
      dmem.busy = true; dmem.addr = dut->mem_req_addr_o;
      dmem.beats = dut->mem_req_beats_o; dmem.idx = 0;
    }
  } else if (dut->mem_rsp_ready_o) {
    if (++dmem.idx >= dmem.beats) dmem.busy = false;
  }
  if (!rmem_rd.busy) {
    if (dut->rmem_req_valid_o) {
      rmem_rd.busy = true; rmem_rd.addr = dut->rmem_req_addr_o;
      rmem_rd.beats = dut->rmem_req_beats_o; rmem_rd.idx = 0;
    }
  } else if (dut->rmem_rsp_ready_o) {
    if (++rmem_rd.idx >= rmem_rd.beats) rmem_rd.busy = false;
  }
  if (rmem_done_next) {
    rmem_done_next = false;
  } else if (dut->rmem_wr_valid_o) {
    uint32_t a = dut->rmem_wr_addr_o - RESP_BASE;
    for (int n = 0; n < 8; ++n)
      if ((dut->rmem_wr_strb_o >> n) & 1)
        if (a + n < rmem.size()) rmem[a + n] = uint8_t(dut->rmem_wr_data_o >> (56 - 8 * n));
    rmem_done_next = true;
  }
  if (nvm_state == 0 && dut->d3_m_req_o) { nvm_state = 1; nvm_we = dut->d3_m_we_o; }
  else if (nvm_state == 1) nvm_state = 2;
  else if (nvm_state == 2) nvm_state = 0;
  if (dut->rxs_rd_en_o) rx_data_next = slot[dut->rxs_rd_addr_o % slot.size()];
  gnt_next = false;
  if (dut->txs_alloc_req_o && !gnt_pending) { gnt_next = true; gnt_pending = true; }
  if (!dut->txs_alloc_req_o) gnt_pending = false;
  if (dut->txs_wr_valid_o && dut->txs_wr_addr_o < frame.size())
    frame[dut->txs_wr_addr_o] = dut->txs_wr_data_o;
  if (dut->txs_wr_commit_o) { obs.resp = true; obs.frame_len = dut->txs_wr_len_o; }
  if (dut->obs_disp_o) obs.disp.emplace_back(int(dut->obs_upc_o), bool(dut->obs_batch_o));
}

void tick() {
  drive();
  dut->clk_i = 0; dut->eval();
  sample();
  dut->clk_i = 1; dut->eval();
}

// ---------------------------------------------------------------------------
// one trial
// ---------------------------------------------------------------------------
struct Stim {
  int proto = PROTO_AEM;
  int msg = 0;
  int cdl = 12;
  uint16_t ct = 0;                  // AECPDU @22..@23 (the record's opcode)
  std::map<int, uint8_t> bytes;     // AECPDU offset -> byte, from @24
  bool running = false;
};

struct Result {
  std::vector<std::pair<int, bool>> disp;
  bool resp = false;
  bool timeout = false;
  int status = -1;
  int cdl = -1;
  int ct = -1;
};

void put16(std::map<int, uint8_t>& b, int at, uint16_t v) {
  b[at] = uint8_t(v >> 8); b[at + 1] = uint8_t(v);
}
void put32(std::map<int, uint8_t>& b, int at, uint32_t v) {
  put16(b, at, uint16_t(v >> 16)); put16(b, at + 2, uint16_t(v));
}

bool idle_for(int n, int budget) {
  int quiet = 0;
  for (int i = 0; i < budget; ++i) {
    tick();
    quiet = dut->dbg_busy_o ? 0 : quiet + 1;
    if (quiet >= n) return true;
  }
  return false;
}

Result finish(int budget) {
  Result r;
  bool ok = idle_for(8, budget);
  r.timeout = !ok;
  r.disp = obs.disp;
  r.resp = obs.resp;
  if (obs.resp) {
    const uint8_t* a = &frame[14];        // the AECPDU after the Ethernet header
    r.status = a[2] >> 3;
    r.cdl = ((a[2] & 7) << 8) | a[3];
    r.ct = (a[22] << 8) | a[23];
  }
  return r;
}

Result run(const Stim& s) {
  std::fill(slot.begin(), slot.end(), 0);
  int len = 12 + s.cdl;
  slot[0] = 0xFB; slot[1] = uint8_t(s.msg);
  slot[2] = uint8_t((s.cdl >> 8) & 7); slot[3] = uint8_t(s.cdl);
  for (int i = 0; i < 8; ++i) slot[4 + i] = uint8_t(ENTITY_ID >> (56 - 8 * i));
  slot[22] = uint8_t(s.ct >> 8); slot[23] = uint8_t(s.ct);
  for (auto& [at, v] : s.bytes) if (at < int(slot.size())) slot[at] = v;
  obs = Observed{};
  dut->strm_bound_i = s.running ? 0xFF : 0;
  dut->strm_streaming_i = s.running ? 0xFF : 0;
  dut->t_protocol_i = s.proto; dut->t_msg_type_i = s.msg; dut->t_cdl_i = s.cdl;
  dut->t_opcode_i = s.ct;
  dut->t_ctlr_eid_i = 0x0011223344556677ull; dut->t_src_mac_i = 0x021122334455ull;
  dut->t_target_eid_i = ENTITY_ID; dut->t_seq_i = 0x0202; dut->t_slot_i = 0;
  dut->rxs_slot_len_i = len > 576 ? 576 : len;
  dut->t_valid_i = 1;
  int i = 0;
  for (; i < TRIAL_BUDGET; ++i) {
    drive();
    dut->clk_i = 0; dut->eval();
    bool took = dut->txn_ready_o;
    sample();
    dut->clk_i = 1; dut->eval();
    if (took) break;
  }
  dut->t_valid_i = 0;
  if (i == TRIAL_BUDGET) { Result r; r.timeout = true; return r; }
  return finish(TRIAL_BUDGET);
}

Result run_uns(int kind, uint16_t arg0, bool remove) {
  obs = Observed{};
  dut->strm_bound_i = 0; dut->strm_streaming_i = 0;
  dut->uns_kind_i = kind; dut->uns_desc_type_i = 0; dut->uns_desc_index_i = 0;
  dut->uns_amap_remove_i = remove; dut->uns_amap_count_i = 0;
  dut->uns_arg0_i = arg0; dut->uns_arg1_i = 0;
  dut->uns_valid_i = 1;
  int i = 0;
  for (; i < TRIAL_BUDGET; ++i) {
    drive();
    dut->clk_i = 0; dut->eval();
    bool done = dut->uns_done_o;
    sample();
    dut->clk_i = 1; dut->eval();
    if (done) break;
  }
  dut->uns_valid_i = 0;
  if (i == TRIAL_BUDGET) { Result r; r.timeout = true; return r; }
  return finish(TRIAL_BUDGET);
}

// ---------------------------------------------------------------------------
// the sweep
// ---------------------------------------------------------------------------
std::vector<uint16_t> desc_types() {
  std::vector<uint16_t> t;
  for (uint16_t v = 0; v <= 0x0028; ++v) t.push_back(v);
  t.push_back(0xFFFF);
  return t;
}

//! one payload template, applied to every opcode
struct Tpl {
  std::string name;
  std::map<int, uint8_t> bytes;
  std::vector<int> cdls;
  bool running = false;
};

std::vector<Tpl> templates() {
  std::vector<Tpl> out;
  char nm[64];
  for (uint16_t t : desc_types()) {
    Tpl a; snprintf(nm, sizeof nm, "type@24=%04X", t); a.name = nm;
    put16(a.bytes, 24, t);
    for (int c = 12; c <= 100; ++c) a.cdls.push_back(c);
    out.push_back(a);
    Tpl r = a; r.name += ",running"; r.running = true; r.cdls = {16, 24, 96};
    out.push_back(r);
    Tpl b; snprintf(nm, sizeof nm, "type@28=%04X", t); b.name = nm;
    put16(b.bytes, 24, 0); put16(b.bytes, 28, t);
    b.cdls = {20, 100};
    out.push_back(b);
    for (uint16_t k : {1, 2}) {
      Tpl c; snprintf(nm, sizeof nm, "type@24=%04X,count@28=%u", t, k); c.name = nm;
      put16(c.bytes, 24, t); put16(c.bytes, 28, k);
      c.cdls = {28, 36};
      out.push_back(c);
    }
    Tpl f; snprintf(nm, sizeof nm, "type@24=%04X,flags@28=20000000", t); f.name = nm;
    put16(f.bytes, 24, t); put32(f.bytes, 28, 0x20000000u);
    f.cdls = {96};
    out.push_back(f);
    Tpl g = f; g.name += ",latency@48=80000000"; put32(g.bytes, 48, 0x80000000u);
    out.push_back(g);
  }
  Tpl l; l.name = "target@36=0001"; put16(l.bytes, 36, 1); l.cdls = {28};
  out.push_back(l);
  return out;
}

//! (µPC or outcome) -> {cdl: {response "status:cdl" ...}}
using Seen = std::map<std::string, std::map<int, std::set<std::string>>>;

std::string outcome(const Result& r, bool& batch) {
  batch = false;
  if (r.timeout) return "timeout";
  if (r.disp.empty()) return r.resp ? "engine" : "drop";
  std::string s;
  for (auto& [u, b] : r.disp) {
    if (!s.empty()) s += "+";
    s += std::to_string(u);
    batch = batch || b;
  }
  return s;
}

//! "status:response cdl", or "none" when no frame was sent
std::string answer(const Result& r) {
  return r.resp ? std::to_string(r.status) + ":" + std::to_string(r.cdl) : "none";
}

struct Key {
  std::string bucket;
  int op;
  bool operator<(const Key& o) const {
    return bucket != o.bucket ? bucket < o.bucket : op < o.op;
  }
};

std::map<Key, Seen> seen;
std::map<std::pair<Key, std::string>, std::pair<int, Stim>> first;   // min cdl, stimulus
long trials = 0;
long timeouts = 0;

void note(const Key& k, const Stim& s, const Result& r) {
  ++trials;
  if (r.timeout) ++timeouts;
  bool batch;
  std::string o = outcome(r, batch);
  seen[k][o][s.cdl].insert(answer(r));
  auto fk = std::make_pair(k, o);
  auto it = first.find(fk);
  if (it == first.end() || s.cdl < it->second.first) first[fk] = {s.cdl, s};
}

//! every program an opcode reached, replayed at every shorter length
void floors(const std::string& bucket) {
  std::vector<std::pair<Key, std::string>> todo;
  for (auto& [fk, v] : first)
    if (fk.first.bucket == bucket && v.first > 12) todo.push_back(fk);
  for (auto& fk : todo) {
    Stim s = first[fk].second;
    for (int c = first[fk].first - 1; c >= 12; --c) {
      s.cdl = c;
      note(fk.first, s, run(s));
    }
  }
}

void aem_bucket(const std::string& label, int proto, int msg, const std::vector<Tpl>& tpls,
                const std::vector<int>* only_cdls) {
  for (int op = 0; op <= 0x7F; ++op)
    for (const Tpl& t : tpls)
      for (int c : (only_cdls ? *only_cdls : t.cdls)) {
        Stim s; s.proto = proto; s.msg = msg; s.cdl = c; s.ct = uint16_t(op);
        s.bytes = t.bytes; s.running = t.running;
        note({label, op}, s, run(s));
      }
}

void mvu_bucket() {
  // the Milan protocol_id 00-1B-C5-0A-C1-00 at @22..@27, then one byte of it
  // wrong at a time; the command_type is @28..@29
  const uint8_t pid[6] = {
      0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00};
  for (int wrong = -1; wrong < 6; ++wrong) {
    if (wrong == 0 || wrong == 2) continue;   // one wrong byte per captured field
    for (int ct = 0; ct <= 0x7F; ++ct)
      for (int c : {12, 19, 20, 24, 32, 100}) {
        Stim s; s.proto = PROTO_MVU; s.msg = 6; s.cdl = c;
        uint8_t p[6]; memcpy(p, pid, 6);
        if (wrong >= 0) p[wrong] ^= 0x01;
        s.ct = uint16_t((p[0] << 8) | p[1]);
        for (int i = 2; i < 6; ++i) s.bytes[22 + i] = p[i];
        put16(s.bytes, 28, uint16_t(ct));
        note({wrong < 0 ? "MVU" : "MVU/wrong-protocol-id", ct}, s, run(s));
      }
  }
}

//! GET_DYNAMIC_INFO carrying one record: {length, -, -, status, -, command_type}
//! then `length` data bytes, descriptor_type first
std::map<int, std::map<std::string, std::set<std::string>>> gdi;   // record ct -> outcome -> answers

void gdi_records() {
  for (int ct = 0; ct <= 0x7F; ++ct) {
    std::vector<std::pair<int, uint16_t>> forms = {{0, 0}, {2, 0}};
    for (uint16_t t : desc_types()) { forms.push_back({4, t}); forms.push_back({8, t}); }
    for (int status : {0, 1})
      for (auto& [len, t] : forms) {
        if (status && (len != 4 || t != 0)) continue;
        Stim s; s.proto = PROTO_AEM; s.msg = 0; s.ct = 0x004B;
        put16(s.bytes, 24, uint16_t(len));
        s.bytes[28] = uint8_t(status);
        put16(s.bytes, 30, uint16_t(ct));
        if (len >= 2) put16(s.bytes, 32, t);
        s.cdl = 12 + 8 + len;
        Result r = run(s);
        ++trials;
        if (r.timeout) ++timeouts;
        bool batch;
        std::string o = outcome(r, batch);
        if (!r.disp.empty() && batch) o = "batch:" + o;
        gdi[ct][o].insert(answer(r));
      }
  }
  // and the aggregate with no record at all
  Stim s; s.proto = PROTO_AEM; s.msg = 0; s.ct = 0x004B; s.cdl = 12;
  Result r = run(s);
  ++trials;
  if (r.timeout) ++timeouts;
  bool batch;
  gdi[-1][outcome(r, batch)].insert(answer(r));
}

std::map<int, std::map<std::string, std::set<std::string>>> uns;   // kind -> outcome -> "ct:cdl"

void unsolicited() {
  for (int kind = 0; kind < 16; ++kind)
    for (uint16_t arg0 : {0, 1})
      for (bool remove : {false, true}) {
        Result r = run_uns(kind, arg0, remove);
        ++trials;
        if (r.timeout) ++timeouts;
        bool batch;
        char ct[32];
        snprintf(ct, sizeof ct, "0x%04X:%d", r.ct & 0x7FFF, r.cdl);
        uns[kind][outcome(r, batch)].insert(r.resp ? ct : "none");
      }
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------
std::string jstr(const std::string& s) { return "\"" + s + "\""; }

template <class S>
std::string jset(const S& v) {
  std::string o = "[";
  for (auto& x : v) o += (o.size() > 1 ? ", " : "") + jstr(x);
  return o + "]";
}

void write(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) { perror(path); return; }
  fprintf(f, "{\n  \"trials\": %ld, \"timeouts\": %ld,\n  \"decode\": {\n", trials, timeouts);
  bool firstk = true;
  for (auto& [k, by] : seen) {
    fprintf(f, "%s    \"%s %04X\": {", firstk ? "" : ",\n", k.bucket.c_str(), k.op);
    firstk = false;
    bool fo = true;
    for (auto& [o, cdls] : by) {
      fprintf(f, "%s\"%s\": {", fo ? "" : ", ", o.c_str());
      fo = false;
      bool fc = true;
      for (auto& [c, ans] : cdls) {
        fprintf(f, "%s\"%d\": %s", fc ? "" : ", ", c, jset(ans).c_str());
        fc = false;
      }
      fprintf(f, "}");
    }
    fprintf(f, "}");
  }
  fprintf(f, "\n  },\n  \"gdi\": {\n");
  firstk = true;
  for (auto& [ct, by] : gdi) {
    fprintf(f, "%s    \"%d\": {", firstk ? "" : ",\n", ct);
    firstk = false;
    bool fo = true;
    for (auto& [o, ans] : by) {
      fprintf(f, "%s\"%s\": %s", fo ? "" : ", ", o.c_str(), jset(ans).c_str());
      fo = false;
    }
    fprintf(f, "}");
  }
  fprintf(f, "\n  },\n  \"unsolicited\": {\n");
  firstk = true;
  for (auto& [kind, by] : uns) {
    fprintf(f, "%s    \"%d\": {", firstk ? "" : ",\n", kind);
    firstk = false;
    bool fo = true;
    for (auto& [o, cts] : by) {
      fprintf(f, "%s\"%s\": %s", fo ? "" : ", ", o.c_str(), jset(cts).c_str());
      fo = false;
    }
    fprintf(f, "}");
  }
  fprintf(f, "\n  }\n}\n");
  fclose(f);
}

bool load_image(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); return false; }
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) image.insert(image.end(), buf, buf + n);
  fclose(f);
  return !image.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  const char* out = argc > 1 ? argv[1] : "obj_dir/observed.json";
  if (!load_image("image.bin")) return 1;
  const milan::tb::Model<Vaecp_cmds_wrap> model;
  dut = model.get();
  dut->rst_n = 0;
  dut->t_valid_i = 0; dut->uns_valid_i = 0;
  for (int i = 0; i < 8; ++i) tick();
  dut->rst_n = 1;
  // the D3 restore owns dispatch from reset: let it reach its terminal
  int i = 0;
  for (; i < 200000 && !dut->d3_done_o; ++i) tick();
  if (!dut->d3_done_o || dut->d3_closed_o || !dut->dbg_img_valid_o) {
    printf("aecp_cmds: the engine never came out of its D3 restore "
           "(done %d, closed %d, image valid %d)\n",
           int(dut->d3_done_o), int(dut->d3_closed_o), int(dut->dbg_img_valid_o));
    return 1;
  }
  idle_for(8, 1000);

  const std::vector<Tpl> tpls = templates();
  aem_bucket("AEM", PROTO_AEM, 0, tpls, nullptr);
  floors("AEM");
  // the other AEM-bucket message types and ADDRESS_ACCESS: the type
  // templates only, at two lengths
  std::vector<Tpl> types;
  for (const Tpl& t : tpls)
    if (t.name.rfind("type@24=", 0) == 0 && t.name.find(',') == std::string::npos) types.push_back(t);
  const std::vector<int> two = {24, 100};
  aem_bucket("AVC", PROTO_AEM, 4, types, &two);
  aem_bucket("EXTENDED", PROTO_AEM, 14, types, &two);
  aem_bucket("ADDRESS_ACCESS", PROTO_AA, 2, types, &two);
  mvu_bucket();
  floors("MVU");
  gdi_records();
  unsolicited();
  write(out);
  printf("aecp_cmds sweep: %ld trials, %ld timeouts, written to %s\n", trials, timeouts, out);
  return timeouts ? 1 : 0;
}
