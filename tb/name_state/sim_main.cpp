// SPDX-License-Identifier: CERN-OHL-W-2.0
// Complete fixed name populations, through real SET_NAME and GET_NAME frames.
// Milan v1.2 5.3.13; IEEE 1722.1-2021 7.4.17 and 7.4.18.
// Reuse the existing wire/device harness without changing its normal runner.
#define main(...) existing_suite_main(__VA_ARGS__)
#include "../pp_top/sim_main.cpp"
#undef main
#include <fstream>
#include <iterator>

struct CompleteNames : D3NamePhase {
  CompleteNames(H& tally, const std::vector<uint8_t>& img,
                const std::vector<ImgEnt>& ents, unsigned aaf)
      : D3NamePhase(tally, img, ents) {
    named.clear();
    const std::pair<uint16_t,unsigned> groups[] = {
      {0x0000,1}, {0x0001,1}, {0x0002,1}, {0x0005,aaf+1}, {0x0006,aaf+1},
      {0x0009,1}, {0x000A,aaf+2}, {0x0014,aaf==1 ? 25u : 72u}, {0x001A,1}, {0x0024,1}};
    for (const auto& g : groups) {
      for (unsigned idx=0;idx<g.second;idx++) {
        for (unsigned ni=0;ni<(g.first==0 ? 2u : 1u);ni++) {
          const uint16_t ordinal=uint16_t(named.size());
          std::vector<uint8_t> value(64);
          for (unsigned byte=0;byte<64;byte++) value[byte]=uint8_t(33+(ordinal*11+byte*7)%90);
          if (ordinal%7==2) std::fill(value.begin(),value.end(),0);
          named.push_back({g.first,uint16_t(idx),uint16_t(ni),ordinal,value});
        }
      }
    }
  }
  void run_inventory() {
    CHECK(named.size()==defaults.size(),"N0 independent inventory: %zu names, image %zu",named.size(),defaults.size());
    fresh();
    CHECK(x.boot_to_aecp(),"N0 erased boot reaches service");
    x.d->link_up_i=1;
    x.d->identify_index_i = 0;
    const bool identified = ok(AEM_SET_CONTROL, D3ServicePhase::pl_identify(255))
        && x.d->dbg_identify_o == 255;
    for (const auto& n : named) {
      CHECK(get_reads(n,defaults[n.ordinal]),"N1 default ordinal %u",unsigned(n.ordinal));
      CHECK(set_name(n,n.name) && get_reads(n,n.name),"N2 SET and GET ordinal %u",unsigned(n.ordinal));
    }
    x.idle(4*WINDOW);
    for (const auto& n : named) {
      const auto expected=name_record(n.ordinal,n.name);
      CHECK(std::equal(expected.begin(),expected.end(),x.nv_mem[0x80+n.ordinal].begin()),"N3 saved ordinal %u",unsigned(n.ordinal));
    }
    CHECK(!x.d->d3_unflushed_o,"N3 all names saved");
    power_cycle();
    bool seen=false;
    bool cleared=true;
    const Boot b=boot_with(6*RS_TMO,[&]{
      if (seen || !x.d->dbg_d3_proof_o) return;
      seen=true;
      for(const auto& n:named) {
        std::vector<uint8_t> got;
        for(unsigned lane=0;lane<8;lane++) {
          x.d->dbg_name_lane_i=n.ordinal*8+lane; x.d->eval();
          for(int byte=7;byte>=0;byte--) got.push_back(uint8_t(x.d->dbg_name_o>>(byte*8)));
        }
        cleared=cleared && got==defaults[n.ordinal];
      }
    });
    CHECK(seen && cleared,"N4 every name reset to image default before replay");
    CHECK(b.done>b.release && !x.d->restore_fail_o,"N4 restore complete in %ld cycles",b.done);
    x.d->link_up_i=1;
    x.d->identify_index_i = 0;
    x.d->eval();
    for(const auto& n:named) CHECK(get_reads(n,n.name),"N5 restored ordinal %u",unsigned(n.ordinal));
    const auto control = std::find_if(named.begin(), named.end(), [](const Named& n) {
      return n.type == 0x001A;
    });
    CHECK(identified && x.d->dbg_identify_o == 0 && control != named.end()
              && get_reads(*control, control->name),
          "N8 CONTROL name restored while its IDENTIFY value resets to zero");
  }
  void pending_records() {
    fresh();
    const bool ready = x.boot_to_aecp();
    const auto& first = named.front();
    const auto& last = named.back();
    const bool accepted = set_name(first, first.name) && set_name(last, last.name);
    x.idle(4 * WINDOW);
    const auto want_first = name_record(first.ordinal, first.name);
    const auto want_last = name_record(last.ordinal, last.name);
    CHECK(ready && accepted
              && std::equal(want_first.begin(), want_first.end(), x.nv_mem[0x80].begin())
              && std::equal(want_last.begin(), want_last.end(), x.nv_mem[0x80 + last.ordinal].begin()),
          "N6 pending: first and last saved name values survive separate completion");
  }

  void changes_during_capture() {
    // Sweep a SET through the expiry of the first change's fixed debounce.
    // At any overlap the saved frame must be one whole old or new name.
    // Checking the first WRITE matters: a later tainted retry can hide a tear.
    const auto& n = named[1];
    const std::vector<uint8_t> initial_name(64, 'A');
    const std::vector<uint8_t> old_name(64, 'B');
    const std::vector<uint8_t> new_name(64, 'Z');
    const auto old_frame = name_record(n.ordinal, old_name);
    const auto new_frame = name_record(n.ordinal, new_name);
    long overlaps = 0;
    long mixed = 0;
    bool coherent = true;
    bool final_values = true;
    for (long offset = -120; offset <= 120; offset += 12) {
      fresh();
      const bool ready = x.boot_to_aecp();
      x.d3_own_max = 0;
      const size_t before = x.nvm_ops.size();
      const bool first = set_name(n, initial_name);
      const long start = long(x.t);
      const bool calibrated = set_name(n, old_name);
      // The repeated descriptor is now cached. Its actual fourth lane
      // gives the next SET's phase, independently of instruction addresses.
      const bool eight_lanes = x.name_wr_cycles.size() == 16;
      const long middle = eight_lanes ? long(x.name_wr_cycles[11]) - start : 0;
      const long expiry = x.name_wr_cycles.empty() ? long(x.t) :
          long(x.name_wr_cycles.front()) + WINDOW;
      const long delay = expiry - middle + offset - long(x.t);
      if (delay > 0) x.idle(delay);
      const bool second = set_name(n, new_name);
      x.idle(3 * WINDOW);
      overlaps += x.d3_own_max > 100 ? 1 : 0;
      bool wrote = false;
      for (size_t i = before; i < x.nvm_ops.size(); ++i) {
        const auto& op = x.nvm_ops[i];
        if (op.op != 1 || op.region != 0x80 + n.ordinal) continue;
        wrote = true;
        mixed += (op.wr == old_frame || op.wr == new_frame) ? 0 : 1;
        coherent = coherent && (op.wr == old_frame || op.wr == new_frame);
      }
      final_values = final_values && ready && first && calibrated && eight_lanes && second && wrote
          && std::equal(new_frame.begin(), new_frame.end(), x.nv_mem[0x80 + n.ordinal].begin())
          && get_reads(n, new_name);
    }
    CHECK(coherent && final_values && overlaps > 0,
          "N7 capture: every saved name contains eight coherent lanes, latest value retained (%ld overlaps, %ld mixed)",
          overlaps, mixed);
  }

  void run() {
    run_inventory();
    pending_records();
    changes_during_capture();
    n5_a_roll_back_restores_the_image_names();
    n6_a_change_during_the_write_taints_it();
    n7_a_late_image_is_walked_before_the_names();
  }

  void measure_names() {
    printf("DR3a names=%zu CLK_HZ=%ld RS_TMO=%ld AGG=%ld; byte-per-cycle NVM\n",
           named.size(), CLK_HZ, RS_TMO, AGG);
    for (int latency : {31, 143}) {
      x.dram_lat = latency;
      fresh();
      for (const auto& n : named) seed(uint8_t(0x80 + n.ordinal), name_record(n.ordinal, n.name));
      char description[96];
      std::snprintf(description, sizeof description, "all %zu names, DRAM %d cyc", named.size(), latency);
      report(description, measured(8 * RS_TMO));
      x.dram.clear();
      power_cycle();
      x.idle(2000);
      x.dram = image;
      std::snprintf(description, sizeof description, "late image, %zu names, DRAM %d cyc", named.size(), latency);
      report(description, measured(8 * RS_TMO));
      fresh();
      for (const auto& n : named) seed(uint8_t(0x80 + n.ordinal), name_record(n.ordinal, n.name));
      x.nv_rd_region = 0x80 + named.back().ordinal;
      x.nv_rd_nth = 3;  // pass 1 header, after the earlier names were replayed
      x.nv_rd_after = 0;
      x.nv_rd_silent = false;
      x.nv_rd_seen = 0;
      std::snprintf(description, sizeof description, "last name DEVICE, DRAM %d cyc", latency);
      report(description, measured(8 * RS_TMO));
    }
  }

};

int main(int argc,char** argv) {
  if(argc!=3 && argc!=4) return 2;
  std::ifstream input(argv[1],std::ios::binary);
  std::vector<uint8_t> img((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
  if(img.size()<32) return 2;
  const milan::tb::Model<Vpp_top_wrap> model;
  H h(model.get());
  std::vector<ImgEnt> ents;
  CompleteNames names{h,img,ents,unsigned(std::stoi(argv[2]))};
  const std::string selection = argc == 4 ? argv[3] : "all";
  if (selection == "--measure") names.measure_names();
  else if (selection == "inventory") names.run_inventory();
  else if (selection == "pending") names.pending_records();
  else if (selection == "capture") names.changes_during_capture();
  else if (selection == "taint") names.n6_a_change_during_the_write_taints_it();
  else if (selection == "healing") names.n7_a_late_image_is_walked_before_the_names();
  else if (selection == "rollback") names.n5_a_roll_back_restores_the_image_names();
  else if (selection == "all") names.run();
  else return 2;
  printf("%d checks: %d PASS, %d FAIL\n",h.checks,h.checks-h.fails,h.fails);
  return h.fails ? 1 : 0;
}
