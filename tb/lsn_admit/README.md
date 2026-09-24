<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# lsn_admit - KL_pp_acmp_lsn_admit suite

Proves the listener admission gate (`hdl/acmp/KL_pp_acmp_lsn_admit.sv`) on its own,
against an independent C++ model of the ownership contract written from
[05 §5.1](../../docs/architecture/05_acmp_engine.md#sec-05-boot-admission). `make`
builds and runs it; exit 0 means PASS.

What it grades:

- **R** from reset the gate owns the four faces: no valid reaches the listener and no
  ready reaches a producer on the transaction and talker-event faces, the START/STOP
  valid and the expiry strobe are masked.
- **T** each release term alone: with the walk's terminal up, a preload still
  presented, the listener still busy or its last A4 strobe still up keeps the faces for
  as long as it lasts, and the gate releases the cycle after the term clears. No
  terminal, no release.
- **O** the release is one-way until the next reset, whatever the release inputs do
  afterwards; a reset takes the faces back.
- **D** the refused-expiry count: only an expiry of a listener owner (`TMR_OWNER_BASE_P`
  to `TMR_OWNER_BASE_P + N_SINKS_P - 1`) counts, only while owned; it saturates at
  0xFFFF and a reset clears it.
- **M** four hundred walks of random stimulus, every output compared with the model on
  every cycle, with the stimulus checked to have met the terminal with each release
  term raised.

Why a unit suite as well as the integration ones: in `tb/acmp_nvm` and `tb/pp_top` the
real binding manager raises its terminal one cycle after its last preload was taken,
when the listener is already idle and nothing is presented. Removing the `pre_valid`,
`busy` or `arm` term from the release condition leaves `tb/acmp_nvm` green (measured);
here each one is graded on its own.

Mutation-proven 2026-09-23 at 18 checks, one mutation of
`KL_pp_acmp_lsn_admit` at a time, each red: the gate deleted (`own_r` resets to 0,
11 FAIL); the transaction's valid admitted, or its ready passed; the talker event's
valid admitted, or its ready passed; the START/STOP valid admitted; the expiry admitted;
the release without the terminal (5 FAIL), without the preload, busy or arm term; the
refused-expiry count taken while released, or without its saturation (2 FAIL each
otherwise).

The integration evidence (the real listener, binding manager and port, issue #92) is
[`tb/acmp_nvm` group L](../acmp_nvm/README.md) and
[`tb/pp_top` section BW](../pp_top/README.md).
