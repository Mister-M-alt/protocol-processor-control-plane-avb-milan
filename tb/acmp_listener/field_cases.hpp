// SPDX-License-Identifier: CERN-OHL-W-2.0
// Issue 168 clause checks through direct transport observations.
struct FieldCases {
  const milan::tb::Model<VKL_pp_acmp_listener> dut;
  Harness h{dut.get()};
  int checks = 0;
  int fails = 0;

  FieldCases() {
    auto* d = dut.get();
    d->rst_n = 0; d->txn_valid_i = 0; d->evt_tk_valid_i = 0;
    d->pre_valid_i = 0; d->lock_held_i = 0; d->lock_ctlr_i = 0;
    d->entity_id_i = OUR_EID;
    for (int i = 0; i < 4; ++i) h.tick();
    d->rst_n = 1;
    for (int i = 0; i < 12; ++i) h.tick();
    CHECK(d->txn_ready_o == 1, "field check reset completes");
    h.col.clear();
  }
  void drive(const Stim& s) {
    h.col.clear();
    CHECK(h.drive(0, s), "field check stimulus completes");
  }
  Pdu bind(uint64_t controller) {
    Stim s; s.msg = M_BIND; s.tk_eid = TK_A; s.tk_uid = TKUID_A;
    s.ctlr = controller; s.seq = 0x4100;
    drive(s);
    Pdu probe{};
    for (const auto& frame : h.col.frames)
      if (frame.b[1] == M_PROBE_CMD) probe = frame;
    CHECK(h.col.frames.size() == 1 || h.col.frames.size() == 2, "field bind responds");
    return probe;
  }
  Stim response(const Pdu& probe) {
    auto read = [&](int off, int n) {
      uint64_t v = 0;
      for (int i = 0; i < n; ++i) v = (v << 8) | probe.b[off + i];
      return v;
    };
    Stim s; s.msg = M_PROBE_RESP; s.ctlr = read(12, 8);
    s.tk_eid = read(20, 8); s.tk_uid = uint16_t(read(36, 2));
    s.seq = uint16_t(read(48, 2)); s.sid = 0x5544332211002233ull;
    s.da = 0x91E0F0004455ull; s.vlan = 2;
    return s;
  }
  int vlan() {
    const Pdu probe = bind(CTL1);
    auto s = response(probe); s.vlan = 0xF123;
    drive(s);
    CHECK(h.col.settle, "VLAN response settles");
    printf("FIELD VLAN received=0x%04x action=0x%04x record=0x%04x\n",
           s.vlan, h.col.settle_vlan, h.shadow[0].vlan);
    // Milan v1.2 5.3.8.9: preserve the received SRP parameter.
    CHECK(h.col.settle_vlan == s.vlan, "VLAN full field at settle action");
    Stim get; get.msg = M_GETRX; get.ctlr = CTL1; get.seq = 0x4300;
    drive(get);
    CHECK(h.col.frames.size() == 1, "VLAN GET_RX_STATE response exists");
    if (h.col.frames.size() == 1) {
      const auto& p = h.col.frames[0];
      unsigned observed = (unsigned(p.b[52]) << 8) | p.b[53];
      printf("FIELD VLAN GET_RX_STATE=0x%04x expected=0x%04x\n", observed, s.vlan);
      // Milan v1.2 Table 5.38 and 5.3.8.9.
      CHECK(observed == s.vlan, "VLAN full field in GET_RX_STATE");
    }
    return fails;
  }
  int old_controller() {
    const Pdu probe = bind(CTL1);
    bind(CTL2);
    CHECK(h.shadow[0].bind_ctlr == CTL2 && h.shadow[0].sm == S_PWR,
          "same-talker rebind changes binding controller while probe remains pending");
    drive(response(probe));
    printf("FIELD saved-controller response settle=%u state=%s\n",
           unsigned(h.col.settle), SN[h.shadow[0].sm]);
    // Milan v1.2 5.5.3.5.17 step 2 and 5.5.3.5.18 step 1.
    CHECK(h.col.settle && h.shadow[0].sm == S_SNR,
          "guard accepts controller from sent probe after same-talker rebind");
    return fails;
  }
  int new_controller() {
    const Pdu probe = bind(CTL1);
    bind(CTL2);
    auto s = response(probe); s.ctlr = CTL2;
    drive(s);
    printf("FIELD current-controller response settle=%u state=%s\n",
           unsigned(h.col.settle), SN[h.shadow[0].sm]);
    // Milan v1.2 5.5.3.5.18 step 1: current binding is not the sent probe.
    CHECK(!h.col.settle && h.shadow[0].sm == S_PWR,
          "guard rejects controller absent from sent probe after same-talker rebind");
    return fails;
  }
  int duplicate() {
    const Pdu probe = bind(CTL1);
    bind(CTL2);
    Stim expiry; expiry.k = Stim::EXP;
    drive(expiry);
    CHECK(h.col.frames.size() == 1, "duplicate probe exists");
    if (h.col.frames.size() == 1) {
      const auto& retry = h.col.frames[0];
      bool same = memcmp(probe.b, retry.b, PDU_BYTES) == 0;
      printf("FIELD retry byte-identical=%u\n", unsigned(same));
      // Milan v1.2 5.5.3.5.16 step 1.
      CHECK(same, "retry preserves original probe after same-talker rebind");
    }
    return fails;
  }

  int unbind_fields() {
    bind(CTL1);
    Stim s; s.msg = M_UNBIND; s.ctlr = CTL1;
    s.tk_eid = TK_A; s.tk_uid = TKUID_A; s.seq = 0x6101;
    drive(s);
    CHECK(h.col.frames.size() == 1, "LD1 response exists");
    if (h.col.frames.size() == 1) {
      const auto& frame = h.col.frames[0];
      bool zero = true;
      for (int i = 20; i < 28; ++i) zero = zero && frame.b[i] == 0;
      zero = zero && frame.b[36] == 0 && frame.b[37] == 0;
      // Milan v1.2 Table 5.36.
      CHECK(zero, "LD1 successful unbind has zero talker fields");
    }
    return fails;
  }
  int retry_status() {
    const Pdu probe = bind(CTL1);
    auto s = response(probe); s.status = 5;
    drive(s);
    Stim discovered; discovered.k = Stim::TK; discovered.tk_kind = 0;
    drive(discovered);
    Stim expiry; expiry.k = Stim::EXP;
    drive(expiry);
    // Milan v1.2 5.5.3.5.30 step 2.
    CHECK(h.shadow[0].sm == S_PWD && h.shadow[0].acmpsta == 5,
          "LD2 discovered retry preserves received status");
    drive(expiry);
    // Milan 5.5.3.5.10: this transition also leaves status unchanged.
    CHECK(h.shadow[0].sm == S_PWR && h.shadow[0].acmpsta == 5,
          "LD2 retry probe preserves received status");
    return fails;
  }
  int lock_status() {
    bind(CTL1);
    const auto before = h.shadow[0];
    dut.get()->lock_held_i = 1; dut.get()->lock_ctlr_i = CTL1;
    for (int msg : {M_BIND, M_UNBIND}) {
      Stim s; s.msg = uint8_t(msg); s.ctlr = CTL2;
      s.tk_eid = TK_A; s.tk_uid = TKUID_A;
      drive(s);
      // IEEE 1722.1-2021 Table 8-3, Milan Tables 5.31 and 5.35.
      CHECK(h.col.frames.size() == 1 && (h.col.frames[0].b[2] >> 3) == 16,
            "LD3 lock refusal status 16 for message %d", msg);
      CHECK(h.shadow[0] == before && !h.col.nvm && !h.col.disc_arm
            && !h.col.disc_disarm && !h.col.teardown,
            "LD3 lock refusal preserves binding for message %d", msg);
    }
    return fails;
  }
};
