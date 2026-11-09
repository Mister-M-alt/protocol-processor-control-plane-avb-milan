#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Apply reviewed patch arms in scratch and require named simulation failures.

No behavioral oracle reads production source. Each patch is explicit reviewable
input to git apply; the driver reads only simulation logs. SRP top has a global
DUT-cycle guard; the encoder and stream-FSM suites have finite cycle-bounded
walks. A budget exit, missing tally or build failure never counts as a kill.
Every control and every arm builds and runs in a scratch copy of its own;
`--jobs N` runs up to N copies at once, and the results are read in the
declared order.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCHES = Path(__file__).resolve().parent / "mutations"

sys.path.insert(0, str(ROOT / "tb" / "common"))
from mutant_pool import add_jobs_argument, in_order  # noqa: E402

# label, suite, group, required failing assertion prefix.
MUTANTS = [
    ('bad-listener-length', 'srp_top', 'phases', 'K12:'),
    ('repeated-action', 'srp_top', 'congestion', 'N4:'),
    ('expiry-event', 'srp_top', 'phases', 'K2:'),
    ('before-slot', 'srp_top', 'edge', 'L1:'),
    ('talker-no-own', 'srp_top', 'phases', 'K2:'),
    ('listener-no-own', 'srp_top', 'phases', 'K11:'),
    ('talker-lost-txla', 'srp_stream_fsms', 'suite', 'T QA txla=2:'),
    ('listener-lost-txla', 'srp_stream_fsms', 'suite', 'L QA txla=2:'),
    ('talker-strict-lv', 'srp_top', 'phases', 'K2:'),
    ('listener-strict-lv', 'srp_top', 'phases', 'K11:'),
    ('listener-sid-ignored', 'srp_top', 'phases', 'K5:'),
    ('talker-no-renewal', 'srp_top', 'phases', 'K7:'),
    ('listener-no-renewal', 'srp_top', 'phases', 'K7:'),
    ('talker-no-expiry', 'srp_top', 'phases', 'K8:'),
    ('listener-no-expiry', 'srp_top', 'phases', 'K8:'),
    ('pending-peer-ignored', 'srp_top', 'peer', 'M1:'),
    ('encoder-peer-ignored', 'srp_top', 'peer', 'M6:'),
    ('mvrp-supersedes-msrp', 'srp_top', 'peer', 'M1:'),
    ('reset-retains-intent', 'srp_top', 'phases', 'K10:'),
    ('full-drain-missing', 'srp_top', 'congestion', 'N6:'),
    ('already-full-missed', 'srp_top', 'congestion', 'N10:'),
    ('round-completion-lost', 'srp_top', 'congestion', 'N13:'),
    ('own-flags-missing', 'srp_top', 'congestion', 'N5:'),
    ('expiry-congestion', 'srp_top', 'congestion', 'N1:'),
    ('missing-registrar-edge', 'srp_top', 'edge', 'L3:'),
    ('empty-canceled-pdu', 'srp_top', 'peer', 'M11:'),
    ('reserved-slot-not-reused', 'srp_top', 'peer', 'M12:'),
    ('renewal-congestion', 'srp_top', 'congestion', 'N7:'),
    ('leaveall-expiry-lost', 'srp_top', 'peer', 'M9:'),
    ('expiry-outranks-peer', 'srp_top', 'peer', 'M10:'),
    ('repeated-expiry-queued', 'srp_top', 'congestion', 'N12:'),
    ('receive-priority-lost', 'srp_top', 'edge', 'L2:'),
    ('action-omitted', 'srp_top', 'congestion', 'N3:'),
    ('preparation-before-slot', 'srp_top', 'peer', 'M5:'),
    ('table-one-short', 'srp_top', 'congestion', 'N9:'),
    ('my-expiry-pulse', 'srp_top', 'phases', 'K2:'),
    ('my-join-edge-peer', 'srp_top', 'guards', 'O3:'),
    ('my-cancel-eats-new-intent', 'srp_top', 'guards', 'O2:'),
    ('my-edge-cancel-lost', 'srp_top', 'peer', 'M6:'),
    ('my-la-outranks-leave', 'srp_top', 'edge', 'L2:'),
    ('my-drop-join-during-wait', 'srp_top', 'guards', 'O6:'),
    ('my-no-walk-at-sLA', 'srp_top', 'congestion', 'N6:'),
    ('my-reuse-flags-ignore-cancel', 'srp_top', 'guards', 'O4:'),
    ('r-expiry-pulse-restored', 'srp_top', 'phases', 'K2:'),
    ('r-cancel-latch-dropped', 'srp_top', 'guards', 'O1:'),
    ('r-cancel-consumes-new-intent', 'srp_top', 'guards', 'O2:'),
    ('r-join-start-guard-dropped', 'srp_top', 'guards', 'O3:'),
    ('r-sink-receive-priority-lost', 'srp_top', 'guards', 'O5:'),
    ('r-join-coalesce-dropped', 'srp_top', 'guards', 'O6:'),
    ('r-la-only-emission-lost', 'srp_top', 'peer', 'M12:'),
    ('r-collect-blocks-pushes', 'srp_top', 'congestion', 'N6:'),
    ('r-accept-edge-drain-lost', 'srp_top', 'congestion', 'N10:'),
    ('r-reuse-without-action', 'srp_top', 'peer', 'M12:'),
    ('r-mvrp-start-during-prepare', 'srp_encoder', '', 'O8:'),
    ('canceled-content-never-drains', 'srp_top', 'guards', 'O7:'),
    # issue #108: Table 10-5 rLA! restarts the leavealltimer and goes Passive
    ('expiry-only-redraw', 'srp_top', 'restart', 'P1:'),
    ('mvrp-expiry-only-redraw', 'srp_top', 'restart', 'P2:'),
    ('stale-expiry-honoured', 'srp_top', 'peer', 'M10:'),
    ('mvrp-stale-expiry-honoured', 'srp_top', 'restart', 'P6:'),
    ('mvrp-passive-lost', 'srp_top', 'restart', 'P4:'),
    ('mvrp-flag-at-expiry', 'srp_top', 'restart', 'P5:'),
    # issue #108: the stale-expiry guard holds whatever the arm-path latency
    ('rearm-at-issue', 'srp_top', 'armdelay', 'P8:'),
    ('r-rearm-no-inflight', 'srp_top', 'peer', 'M10:'),
    ('r-rearm-no-inflight', 'srp_top', 'restart', 'P6:'),
    ('r-rearm-no-deadline', 'srp_top', 'peer', 'M10:'),
    ('r-rearm-no-deadline', 'srp_top', 'armdelay', 'P8:'),
    ('r-flag-ignores-edge-peer', 'srp_top', 'restart', 'P7:'),
    # issue #64: Milan v1.2 Table 4.3 timer grading
    ('join-ms-400', 'srp_top', 'timers', 'Q1:'),
    ('periodic-ms-3000', 'srp_top', 'timers', 'Q2:'),
    ('draw-kind-0', 'srp_top', 'timers', 'Q3:'),
    # issue #65: MVRP join before the stream, both halves
    ('licence-ignores-join', 'srp_top', 'join', 'R1:'),
    ('licence-ignores-join', 'srp_stream_fsms', 'suite', 'T not ACTIVE while the VID'),
    ('join-sent-at-handover', 'srp_top', 'join', 'R1:'),
    ('count-up-unsends', 'srp_top', 'join', 'R2:'),
    ('listener-lane-cut', 'srp_top', 'join', 'R4:'),
    ('join-sent-at-handover', 'srp_encoder', '', 'W2 New VID 7 handed over'),
    ('count-up-unsends', 'srp_encoder', '', 'W4 a second user'),
    ('tx-strobe-any-app', 'srp_encoder', '', 'W1 no strobe for an MSRP'),
    # issue #230: the storage paths at both elaboration arms. The timer-arm
    # FIFOs (store_main.cpp, sources/sinks 1/1, 2/2, 3/5, 9/9) ...
    ('tf-heads-swapped', 'srp_top', 'storage', 'TF1:'),
    ('tf-head-at-write-pointer', 'srp_top', 'storage', 'TF1:'),
    ('tf-listener-push-dropped', 'srp_top', 'storage', 'TF2:'),
    ('tf-ls-written-at-tk-pointer', 'srp_top', 'storage', 'TF2:'),
    ('tf-tk-head-read-ahead', 'srp_top', 'storage', 'TF1:'),
    ('tf-ls-head-reads-tk-ram', 'srp_top', 'storage', 'TF2:'),
    ('tf-tk-write-at-rptr', 'srp_top', 'storage', 'TF1:'),
    ('tf-full-guard-31', 'srp_top', 'storage', 'TF4:'),
    ('tf-tk-write-ignores-full', 'srp_top', 'storage', 'TF4:'),
    ('tf-ls-write-ignores-full', 'srp_top', 'storage', 'TF5:'),
    # ... the walk records (walk_main.cpp, the same four shapes) ...
    ('walk-record-written-on-close', 'srp_stream_fsms', 'walk', 'WK3:'),
    ('wtsp-first-open-only', 'srp_stream_fsms', 'walk', 'WK2:'),
    ('wtsp-read-at-gate-source', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-read-at-source-0', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-latency-field-shifted', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-latency-shifted', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-rank-dropped', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wtsp-prio-rank-swapped', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-ram-first-open-only', 'srp_stream_fsms', 'walk', 'WK2:'),
    ('wid-ram-read-at-gate-source', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-ram-read-neighbour', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-flops-da-of-gate-source', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-flops-sid-of-source-0', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('wid-flops-vid-of-source-0', 'srp_stream_fsms', 'walk', 'WK1:'),
    ('talker-vid-unreset', 'srp_stream_fsms', 'walk', 'WK5:'),
    ('wsid-ram-first-settle-only', 'srp_stream_fsms', 'walk', 'WK8:'),
    ('wsid-ram-written-on-teardown', 'srp_stream_fsms', 'walk', 'WK7:'),
    ('wsid-flops-of-control-sink', 'srp_stream_fsms', 'walk', 'WK6:'),
    ('wsid-flops-read-sink-0', 'srp_stream_fsms', 'walk', 'WK6:'),
    ('wtsp-write-ignores-ready', 'srp_stream_fsms', 'walk', 'WK9:'),
    ('wsid-write-ignores-ready', 'srp_stream_fsms', 'walk', 'WK10:'),
    # ... and the admission slopes (the admission suite, 1 to 8 sources)
    ('slope-stored-at-stage-2-index', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    ('slope-stored-at-source-0', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    ('slope-store-source-0-only', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    ('slope-read-source-0', 'srp_admission', '',
     'round publishes the greedy walk over every current declaration'),
    # issue #134: a withdrawal that meets an LV Listener registrar
    ('lv-second-lv-ends', 'srp_top', 'lvleave', 'S1:'),
    ('lv-never-ends', 'srp_top', 'lvleave', 'S2:'),
    ('lv-expiry-masked', 'srp_top', 'lvcoll', 'SC2:'),
    ('lv-expiry-masked', 'srp_stream_fsms', 'suite', 'SC1:'),
    ('lv-expiry-last', 'srp_stream_fsms', 'suite', 'SC1:'),
    ('lv-expiry-dropped', 'srp_stream_fsms', 'suite', 'SC1:'),
    ('lv-sweep-misses-collision', 'srp_top', 'lvcoll', 'SC3:'),
    # issue #188: an A15 settle never abandons the owed Listener Leave, and
    # the leave copy at both elaboration arms
    ('listener-rebind-abandons-leave', 'srp_stream_fsms', 'suite', 'RB1:'),
    ('listener-declaration-not-held', 'srp_stream_fsms', 'suite', 'RB1:'),
    ('listener-park-never-retires', 'srp_stream_fsms', 'suite', 'RB1:'),
    ('listener-reprobe-abandons-leave', 'srp_stream_fsms', 'suite', 'RB2:'),
    ('listener-reprobe-parks-own-stream', 'srp_stream_fsms', 'suite', 'RB3:'),
    ('walk-ignores-leave-copy', 'srp_stream_fsms', 'walk', 'WK11:'),
    ('lsid-ram-read-sink-0', 'srp_stream_fsms', 'walk', 'WK11:'),
    ('lsid-flops-read-sink-0', 'srp_stream_fsms', 'walk', 'WK11:'),
    ('lsid-ram-written-with-new-stream', 'srp_stream_fsms', 'walk', 'WK11:'),
    # issue #188, the settle arms while a Leave is parked (10 §6.4): a third
    # stream, back to the parked one, an LO applicant, a txLA! walk, and
    # received Listener values; the held stream_id at both elaboration arms;
    # and a sink with no record, which owes nothing
    ('listener-csid-ram-ignores-park', 'srp_stream_fsms', 'suite', 'RB4:'),
    ('listener-csid-ram-ignores-park', 'srp_stream_fsms', 'walk', 'WK12:'),
    ('listener-csid-flops-ignores-park', 'srp_stream_fsms', 'walk', 'WK12:'),
    ('listener-rebind-back-keeps-park', 'srp_stream_fsms', 'suite', 'RB5:'),
    ('listener-lo-not-owed', 'srp_stream_fsms', 'suite', 'RB6:'),
    ('listener-park-retires-on-txla', 'srp_stream_fsms', 'suite', 'RB7:'),
    ('listener-parked-applies-rx', 'srp_stream_fsms', 'suite', 'RB8:'),
    ('listener-unrecorded-applicant-owes', 'srp_stream_fsms', 'suite', 'RB9:'),
    # WK12's own counts and its precondition, at every shape
    ('listener-rebind-abandons-leave', 'srp_stream_fsms', 'walk', 'WK12: the listener walk sends'),
    ('listener-park-never-retires', 'srp_stream_fsms', 'walk', 'WK12: push'),
    ('listener-aa-tx-stays-aa', 'srp_stream_fsms', 'walk', 'WK12 precondition'),
    # issue #184: the Domain re-join is sJ against the Domain Registrar
    # (802.1Q-2018 10.7.6.3, Table 10-4; Milan v1.2 4.2.7.2.2)
    ('domain-rejoin-always-joinin', 'srp_encoder', '', 'DR1 no peer'),
    ('domain-rejoin-always-joinin', 'srp_top', 'domjoin', 'DJ1:'),
    ('domain-rejoin-always-joinin', 'srp_top', 'timers', 'Q2:'),
    ('domain-rejoin-always-joinmt', 'srp_encoder', '', 'DR2 peer declaring'),
    ('domain-rejoin-always-joinmt', 'srp_encoder', '', 'DR5 LV re-joins'),
    ('domain-rejoin-always-joinmt', 'srp_top', 'domjoin', 'DJ2:'),
    ('domain-rejoin-at-request', 'srp_encoder', '', 'DR3a a received LeaveAll'),
    ('domain-rx-leaveall-dropped', 'srp_encoder', '', 'DR3 a peer LeaveAll'),
    ('domain-rx-leaveall-dropped', 'srp_top', 'domjoin', 'DJ3:'),
    ('domain-leaveall-keeps-in', 'srp_encoder', '', 'DR4 own LeaveAll'),
    ('domain-lv-ignored', 'srp_encoder', '', 'DR6 peer withdrawal'),
    ('domain-lv-ignored', 'srp_top', 'domjoin', 'DJ4:'),
    ('domain-lv-any-value', 'srp_encoder', '', 'DR7 an Lv of another'),
    ('domain-leaveall-after-receive', 'srp_encoder', '', 'DR8 a registering event'),
    ('domain-adoption-drops-registrar', 'srp_encoder', '', 'DR9 the adopted value'),
    ('domain-capture-unregistered', 'srp_encoder', '', 'DR9 the adopted value'),
    ('domain-pending-lv-ignored', 'srp_encoder', '', 'DR10 an adoption withdrawn'),
    ('domain-link-keeps-registrar', 'srp_encoder', '', 'DR11 LINK_DOWN'),
    ('top-domain-lv-unwired', 'srp_top', 'domjoin', 'DJ4:'),
    ('top-domain-tx-every-clock', 'srp_top', 'domjoin', 'DJ3:'),
    ('top-domain-tx-unwired', 'srp_top', 'domjoin', 'DJ3:'),
    # issue #193: sinks on one stream_id share one declaring applicant, the
    # owner's (802.1Q-2018 10.7.7, 35.1.2.2, 35.2.6; Milan v1.2 4.2.7.2.2):
    # one declaration on the wire, the handover with its state and its walk
    # mark, the last declarer's one Lv, the combined type
    # issue #214: the encoder keeps one entry per value, so a doubled Listener
    # declaration no longer reaches the wire (SW1, SW7); the arms are killed
    # end to end by their Leaves, and SW1 by the arm that removes both layers
    ('listener-settle-compare-dropped', 'srp_top', 'share', 'SW2:'),
    ('listener-settle-compare-dropped', 'srp_stream_fsms', 'suite', 'SH1:'),
    ('listener-rider-not-quiet', 'srp_stream_fsms', 'suite',
     'SH14: a parked Leave while no sink on the stream declares'),
    ('listener-teardown-applies-lv', 'srp_stream_fsms', 'suite',
     'SH2: the owner unbinds: the next tx! sends no Lv and no New'),
    ('listener-handover-drops-state', 'srp_stream_fsms', 'suite',
     'SH2: the owner unbinds: the rider holds the applicant in its state'),
    ('listener-withdraw-ignores-riders', 'srp_stream_fsms', 'suite',
     'SH2: the owner unbinds: the next tx! sends no Lv and no New'),
    ('listener-withdraw-ignores-riders', 'srp_top', 'share', 'SW2: the owner unbinds'),
    ('listener-teardown-keeps-declaring', 'srp_stream_fsms', 'suite', 'SH3: the owner unbinds last'),
    ('listener-teardown-keeps-declaring', 'srp_top', 'share', 'SW2: the rider unbinds last'),
    ('listener-group-order-reversed', 'srp_stream_fsms', 'suite', 'SH4: 3 sinks on one stream'),
    ('listener-rider-settle-hands-over', 'srp_stream_fsms', 'suite',
     'SH5: a rider rebinds to another stream'),
    ('listener-type-not-combined', 'srp_stream_fsms', 'suite', 'SH6: Ready with AskingFailed'),
    ('listener-type-not-combined', 'srp_top', 'share', 'SW3:'),
    ('listener-handover-forgets-walk-mark', 'srp_stream_fsms', 'suite', 'SH7:'),
    ('listener-settle-parks-shared-stream', 'srp_stream_fsms', 'suite', 'SH8:'),
    ('listener-group-op-during-settle', 'srp_stream_fsms', 'suite', 'SH9:'),
    # issue #193 with #188: a parked applicant is in the group of the value it
    # holds, so a sink settled on that value takes it over (LA + New! = VN):
    # one value per tick, no Lv beside the declaration, the peer stays IN
    ('listener-parked-outside-groups', 'srp_stream_fsms', 'suite', 'SH10:'),
    ('listener-parked-outside-groups', 'srp_top', 'share', 'SW4:'),
    ('listener-settle-skips-leave-copies', 'srp_stream_fsms', 'suite', 'SH14:'),
    ('listener-settle-skips-leave-copies', 'srp_top', 'share', 'SW4:'),
    ('listener-merge-keeps-park', 'srp_stream_fsms', 'suite', 'SH13:'),
    ('listener-retire-skips-group-mark', 'srp_stream_fsms', 'suite', 'SH15:'),
    ('listener-park-drops-matcher-pairs', 'srp_stream_fsms', 'suite', 'SH16:'),
    ('listener-repark-takes-matcher-pairs', 'srp_stream_fsms', 'suite', 'SH17:'),
    ('listener-two-parks-grouped', 'srp_stream_fsms', 'suite', 'SH23:'),
    # issue #193: each applicant has one transmit per walk, of the walk's
    # kind, whichever slot holds it (802.1Q-2018 Table 10-3, 10.7.7): the
    # walk marks every slot it passes and the group whose applicant it
    # serves, and goes back to a slot handed an applicant not yet served
    ('listener-walk-ends-owing-a-transmit', 'srp_stream_fsms', 'suite', 'SH18:'),
    ('listener-walk-marks-only-served-slots', 'srp_stream_fsms', 'suite', 'SH22:'),
    ('listener-walk-marks-group-only-on-retire', 'srp_stream_fsms', 'suite', 'SH21:'),
    # the walk's done strobe follows every push of the walk, second pass
    # included, so the cadence's MRPDU carries them (txop_done_o)
    ('listener-walk-done-before-second-pass', 'srp_stream_fsms', 'suite', 'SH24:'),
    # issue #202: a talker gate re-open never abandons the withdrawal's owed
    # Leave (802.1Q-2018 10.7.7, Table 10-3): one under another stream_id
    # while the applicant is LA or LO parks the old value until its tx! sends
    # the Lv (or s); the re-open's Join! waits; the leave copy at every shape
    ('talker-reopen-abandons-leave', 'srp_stream_fsms', 'suite', 'TP1:'),
    ('talker-reopen-abandons-leave', 'srp_stream_fsms', 'walk', 'WK13: the Leaves'),
    ('talker-park-never-retires', 'srp_stream_fsms', 'suite',
     'TP1: the tx! after it declares the new stream'),
    ('talker-reopen-parks-own-stream', 'srp_stream_fsms', 'suite', 'TP2:'),
    ('talker-rebind-back-keeps-park', 'srp_stream_fsms', 'suite', 'TP3:'),
    ('talker-lo-not-owed', 'srp_stream_fsms', 'suite', 'TP4:'),
    ('talker-unrecorded-applicant-owes', 'srp_stream_fsms', 'suite', 'TP5:'),
    ('talker-close-keeps-join', 'srp_stream_fsms', 'suite', 'TP6:'),
    ('talker-park-retires-on-txla', 'srp_stream_fsms', 'suite', 'TP7:'),
    ('talker-parked-applies-rx', 'srp_stream_fsms', 'suite', 'TP8:'),
    ('talker-park-keeps-matcher', 'srp_stream_fsms', 'suite', 'TP9:'),
    ('walk-ignores-talker-leave-copy', 'srp_stream_fsms', 'suite',
     'TP1: the next tx! sends the withdrawn stream'),
    ('walk-ignores-talker-leave-copy', 'srp_stream_fsms', 'walk', 'WK13: the Leaves'),
    ('lwid-read-source-0', 'srp_stream_fsms', 'walk', 'WK13: the Leaves'),
    ('lwtsp-read-at-gate-source', 'srp_stream_fsms', 'walk', 'WK13: the Leaves'),
    ('lwid-written-from-source-0', 'srp_stream_fsms', 'walk', 'WK13: the Leaves'),
    ('talker-leave-copy-rewritten-while-parked', 'srp_stream_fsms', 'walk', 'WK13: the Leaves'),
    # issue #212: the port Talker registrars (802.1Q-2022 10.7, 10.7.8, 35.2.7)
    # and the settle that starts its sink's registrar from them (Milan v1.2
    # 5.3.8.5, 5.5.3.5.18 step 4): a Talker registered before the settle is
    # applied at it (acceptance 1), a re-bind declares on the next tx! with
    # the failure information at once (2), a withdrawn one is not applied (3)
    ('pt-seed-dropped', 'srp_stream_fsms', 'suite', 'PS1:'),
    # a re-bind to the same stream keeps its registration without the seed
    # (PS20), so the seed's re-bind row is the one to another stream; the
    # whole path removed fails the re-bind to the same stream
    ('pt-seed-dropped', 'srp_stream_fsms', 'suite', 'PS5:'),
    ('presettle-removed', 'srp_stream_fsms', 'suite', 'PS3:'),
    ('pt-never-holds', 'srp_stream_fsms', 'suite', 'PS2:'),
    ('seed-no-registered-strobe', 'srp_stream_fsms', 'suite', 'PS1: a Talker Advertise'),
    ('seed-type-advertise', 'srp_stream_fsms', 'suite', 'PS2:'),
    ('seed-drops-failure-information', 'srp_stream_fsms', 'suite', 'PS4:'),
    ('park-retire-declares-next-walk', 'srp_stream_fsms', 'suite', 'PS4:'),
    ('park-retire-declares-next-walk', 'srp_stream_fsms', 'suite', 'RB1:'),
    ('pt-rlv-ignored', 'srp_stream_fsms', 'suite', 'PS6:'),
    ('pt-leavetimer-never-expires', 'srp_stream_fsms', 'suite', 'PS7:'),
    ('pt-leaveall-ignored', 'srp_stream_fsms', 'suite', 'PS7:'),
    ('pt-leaveall-any-lane', 'srp_stream_fsms', 'suite', 'PS7: a LeaveAll on the other lanes'),
    ('seed-lv-fresh-deadline', 'srp_stream_fsms', 'suite', 'PS8:'),
    ('seed-lv-unguarded', 'srp_stream_fsms', 'suite', 'PS15: the leave slot'),
    ('seed-from-current-state', 'srp_stream_fsms', 'suite', 'PS9:'),
    ('pt-match-ignores-da', 'srp_stream_fsms', 'suite', 'PS10:'),
    ('pt-vid-high-bits-ignored', 'srp_stream_fsms', 'suite', 'PS10:'),
    ('pt-allocation-overwrites-entry-0', 'srp_stream_fsms', 'suite', 'PS11:'),
    ('park-retire-ignores-group', 'srp_stream_fsms', 'suite', 'PS13:'),
    ('park-retire-without-hold', 'srp_stream_fsms', 'suite', 'PS14:'),
    ('seed-keeps-expiry-strobe', 'srp_stream_fsms', 'suite', 'PS15: the old registration'),
    # issue #212: none of rLv!, rIn!, rMt! takes a registrar from MT to IN
    # (802.1Q-2022 Table 10-4), rLv! in LV is -x-, the settle applies a held
    # value's latest type, FailureInformation and latency, and an A8 carries
    # no registration event (F10.5)
    ('pt-alloc-any-event', 'srp_stream_fsms', 'suite', 'PS16:'),
    ('pt-fcode-first-only', 'srp_stream_fsms', 'suite', 'PS17: a held Talker Failed'),
    ('pt-lat-on-alloc-only', 'srp_stream_fsms', 'suite',
     'PS17: a held Talker Advertise whose latency'),
    ('pt-type-on-alloc-only', 'srp_stream_fsms', 'suite',
     'PS17: a held Talker Advertise that turns Failed'),
    ('seed-on-teardown', 'srp_stream_fsms', 'suite', 'PS18:'),
    ('pt-rlv-on-lv-ends', 'srp_stream_fsms', 'suite', 'PS19:'),
    # issue #212: a re-bind to the same {stream_id, DA, VLAN} keeps the sink's
    # registration whatever the port registrars hold; while unbound the
    # registrar follows its value and raises and publishes nothing
    ('rebind-same-not-kept', 'srp_stream_fsms', 'suite',
     'PS20: every port registrar held by other values, sink 0 registered on its stream'),
    ('keep-ignores-da', 'srp_stream_fsms', 'suite',
     'PS20: a re-bind to the same stream_id on another DA'),
    ('keep-ignores-vlan', 'srp_stream_fsms', 'suite',
     'PS20: a re-bind to the same stream_id on another VLAN'),
    ('keep-declares-seed-type', 'srp_stream_fsms', 'suite',
     'PS20: every port registrar held by other values, sink 0 registered on a Talker Failed'),
    ('keep-latency-strobe-dropped', 'srp_stream_fsms', 'suite',
     'PS20: every port registrar held by other values, sink 0 registered on its stream'),
    ('unarmed-registrar-deaf', 'srp_stream_fsms', 'suite',
     'PS21: sink 0 unbound from a stream only its registrar holds, the talker withdraws'),
    ('keep-ignores-expiry', 'srp_stream_fsms', 'suite',
     'PS21: a re-bind on the clock of the kept registration'),
    ('unarmed-indicates', 'srp_stream_fsms', 'suite', 'PS22:'),
    ('unarmed-fail-chg', 'srp_stream_fsms', 'suite', 'PS22:'),
    ('unarmed-unregistered', 'srp_stream_fsms', 'suite', 'PS22:'),
    ('unarmed-latency-strobe', 'srp_stream_fsms', 'suite', 'PS22:'),
    ('unarmed-publishes-latency', 'srp_stream_fsms', 'suite', 'PS22:'),
    ('unarmed-dbg-registrar', 'srp_stream_fsms', 'suite', 'PS22:'),
    # issue #212: LINK_DOWN (Flush!) and LINK_UP (Begin!) take the port
    # registrars and every sink registrar to MT (Table 10-4)
    ('pt-link-flush-dropped', 'srp_stream_fsms', 'suite',
     'PS23: a Talker Advertise registered before the link'),
    ('link-begin-dropped', 'srp_stream_fsms', 'suite', 'PS23: LINK_UP (Begin!)'),
    ('unarmed-link-flush-dropped', 'srp_stream_fsms', 'suite', 'PS23: sink 0 unbound'),
    ('keep-on-link-clock', 'srp_stream_fsms', 'suite',
     'PS23: a re-bind to that stream on the clock the link goes down'),
    ('top-listener-link-unwired', 'srp_top', 'link', 'LK1:'),
    # issue #212: a settled sink's registrar too, its withdrawal published as at
    # its leavetimer! (Flush!: Lv, MT), so a re-bind after it keeps nothing
    ('settled-link-flush-dropped', 'srp_stream_fsms', 'suite',
     'PS24: a sink settled through a link change'),
    ('settled-link-unpublished', 'srp_stream_fsms', 'suite',
     'PS24: sink 0 settled and registered'),
    ('link-withdraw-dropped', 'srp_stream_fsms', 'suite',
     'PS24: sink 0 settled and registered'),
    ('link-clock-indicates', 'srp_stream_fsms', 'suite',
     'PS24: a Talker Advertise on the clock the link goes down'),
    ('settled-link-keeps-lv-slot', 'srp_stream_fsms', 'suite',
     'PS24: sink 0 settled on a Talker Advertise a LeaveAll ages'),
    # issue #212: the port registrars' leave deadline on $clog2(LEAVE_MS_P + 1)
    # bits, and a seeded LV registrar's absolute deadline rebuilt from it
    ('pt-expiry-sign-bit', 'srp_stream_fsms', 'suite', 'PS8:'),
    ('seed-deadline-rebuild-drops-now', 'srp_stream_fsms', 'suite', 'PS8:'),
    # issue #214: a pending table holds one entry per attribute value, so one
    # MRPDU carries each value once (802.1Q-2022 10.7.7, Table 10-3, 10.7.5.13
    # c), 10.8.2.10.1): the second queueing of a value back, then each rule of
    # the merge, the type in the value's identity and the search's restart
    ('second-queueing', 'srp_encoder', '', 'PV1 two re-joins of one Domain value'),
    ('join-displaces-new', 'srp_encoder', '', 'PV2 New then a re-join'),
    # since issue #216 each Applicant decides one message per tx!, so the wire
    # rows hold while either layer does: the intake taking every push twice
    # and the encoder appending, together, put a value in two vectors again
    ('push-twice-encoder-appends', 'srp_top', 'onevec', 'OV1:'),
    ('leave-does-not-replace', 'srp_encoder', '', 'PV4 a re-join then Lv'),
    ('s-displaces-declaration', 'srp_encoder', '', 'PV5 JoinIn then Mt'),
    ('listener-type-oldest', 'srp_encoder', '', 'PV6 Listener Ready then AskingFailed'),
    ('merge-ignores-type', 'srp_encoder', '', 'PV8 three types of one StreamID'),
    ('search-no-restart', 'srp_encoder', '', 'PV10 a value offered after another'),
    # issue #214: a search takes only its own application's read port, so the
    # other application's drain reads its entries in order, in both directions
    ('msrp-read-taken-by-any-search', 'srp_encoder', '',
     'PV13 an MVRP search beside an MSRP drain: the drain reads its own entries'),
    ('mvrp-read-taken-by-any-search', 'srp_encoder', '',
     'PV13 an MSRP search beside an MVRP drain: the drain reads its own entries'),
    ('search-survives-accept', 'srp_encoder', '', 'PV13 an absent VID offered beside an MSRP drain'),
    # the whole of a value's identity: the StreamID's top octet and the VID's
    # high bits (802.1Q-2022 35.2.2.8.1, 11.2.3.1.7)
    ('identity-drops-top-octet', 'srp_encoder', '',
     'PV14 two type 1 StreamIDs that differ only in octet 0'),
    ('identity-drops-top-octet', 'srp_encoder', '',
     'PV14 two type 3 StreamIDs that differ only in octet 0'),
    ('identity-vid-low-octet-only', 'srp_encoder', '',
     'PV15 VID 2 and the VID that differs only in bit 8'),
    # a merged entry carries the newer FirstValue (35.2.2.8)
    ('merge-keeps-older-value', 'srp_encoder', '', 'PV16 two Talker Advertise values of one StreamID'),
    ('merge-keeps-older-value', 'srp_encoder', '', 'PV16 two Talker Failed values of one StreamID'),
    # an MVRP merge reaches the entry's event (Table 10-3 Lv!, New!)
    ('mvrp-merge-never-written', 'srp_encoder', '', 'PV17 VID JoinIn then Lv'),
    ('mvrp-merge-never-written', 'srp_encoder', '', 'PV17 VID New then Lv'),
    ('mvrp-merge-never-written', 'srp_encoder', '', 'PV17 VID Lv then New'),
    ('mvrp-lv-keeps-joinin', 'srp_encoder', '', 'PV17 VID JoinIn then Lv'),
    ('mvrp-lv-keeps-new', 'srp_encoder', '', 'PV17 VID New then Lv'),
    ('mvrp-new-keeps-lv', 'srp_encoder', '', 'PV17 VID Lv then New'),
    # the remaining behaviours of the push face: the declaration type of a
    # replacing In or Mt, an unknown type beside pending entries, and a push on
    # the clock its drain starts
    ('listener-type-s-over-s-kept', 'srp_encoder', '', 'PV18 Listener In Ready then Mt AskingFailed'),
    ('unknown-type-held-while-pending', 'srp_encoder', '', 'PV19 an unknown MSRP type taken at once'),
    ('unknown-type-held-while-pending', 'srp_encoder', '', 'PV19 an unknown MVRP type taken at once'),
    ('push-on-drain-start', 'srp_encoder', '',
     "PV20 a Domain value on the MSRP drain's start clock: the offer waits"),
    ('push-on-drain-start', 'srp_encoder', '',
     "PV20 a VID on the MVRP drain's start clock: the offer rides the next MRPDU"),
    # the prepared own LeaveAll round starts from E_COLLECT: its own term of the
    # guard, removed alone, loses a push taken on that clock (802.1Q-2022 10.8.3.1 a))
    ('push-on-prepared-start', 'srp_encoder', '',
     "PV20 a Domain value on the prepared LeaveAll round's start clock: the offer waits"),
    ('push-on-prepared-start', 'srp_encoder', '',
     "PV20 a Domain value on the prepared LeaveAll round's start clock: the next frame captured"),
    # a drain owns its table from its start clock on, the allocation included
    ('push-taken-in-alloc', 'srp_encoder', '',
     "PV20 a Domain value on the MSRP drain's start clock: the next frame captured"),
    ('push-taken-in-alloc', 'srp_encoder', '',
     "PV20 a VID on the MVRP drain's start clock: the next frame captured"),
    # one Listener declaration per stream on the wire is now held by the
    # listener group (issue #193) and by the encoder (issue #214): with both
    # removed, SW1 sees the second declaration again
    ('listener-shares-nothing-encoder-appends', 'srp_top', 'share', 'SW1:'),
    # issue #216: each Applicant follows 802.1Q-2018 Table 10-3 (New on two
    # tx!, then the Join it gives), sJ follows the Registrar (10.7.6.3), and
    # the LeaveAll state machine runs with nothing declared (10.7.9, Table
    # 10-5). The acceptance arms first: one New, JoinIn with the Registrar
    # MT, LeaveAll stopped with the last declaration
    ('domain-one-new', 'srp_top', 'applicant', 'AP1:'),
    ('domain-one-new', 'srp_encoder', '', 'D1 four tx!'),
    ('vlan-one-new', 'srp_top', 'applicant', 'AP2:'),
    ('vlan-one-new', 'srp_encoder', '', 'V1 four tx!'),
    ('vlan-joinin-registrar-mt', 'srp_top', 'applicant', 'AP3: VID 2 with its Registrar MT'),
    ('vlan-joinin-registrar-mt', 'srp_encoder', '', 'V3 periodic JoinMt'),
    ('mvrp-leaveall-needs-content', 'srp_top', 'applicant', 'AP5: MVRP declaring nothing'),
    ('mvrp-leaveall-needs-content', 'srp_top', 'applicant', 'AP6: with every declaration withdrawn'),
    ('mvrp-leaveall-needs-content', 'srp_encoder', '', 'L7 the LeaveAll-only MVRPDU is captured'),
    ('top-mvrp-la-flag-dropped', 'srp_top', 'applicant', 'AP5: MVRP declaring nothing'),
    ('talker-one-new', 'srp_top', 'applicant', 'AP4: the Talker Advertise'),
    ('listener-one-new', 'srp_top', 'applicant', 'AP4: the Listener Ready'),
    # the rounds: the MVRP drain after the VID walk, the MSRP drain after the
    # Domain's tx!, and the own LeaveAll's txLA! row
    ('top-mvrp-drain-at-tick', 'srp_top', 'applicant', 'AP2:'),
    ('top-round-ignores-domain', 'srp_top', 'applicant', 'AP7:'),
    ('domain-txla-row-ignored', 'srp_top', 'applicant', 'AP5: MSRP sends a LeaveAll'),
    ('domain-txla-row-ignored', 'srp_encoder', '', 'DR4 own LeaveAll'),
    # the Domain's tx! hand-over and its received messages
    ('domain-tx-done-at-serve', 'srp_encoder', '', 'DT1'),
    ('domain-tx-owed-dropped', 'srp_encoder', '', 'DT3'),
    ('domain-applicant-deaf', 'srp_encoder', '', 'DA1'),
    ('domain-applicant-any-value', 'srp_encoder', '', 'DA5'),
    # each VID's Registrar (Table 10-4, MVRP as printed) and Applicant
    ('vlan-registrar-never-in', 'srp_top', 'applicant', 'AP3: with its Registrar IN'),
    ('vlan-registrar-never-in', 'srp_encoder', '', 'VR1'),
    ('vlan-rlv-ignored', 'srp_top', 'applicant', "AP3: after the peer's Lv"),
    ('vlan-rlv-ignored', 'srp_encoder', '', 'VR2'),
    ('vlan-rx-any-vid', 'srp_encoder', '', 'VR3'),
    ('vlan-s-registers', 'srp_encoder', '', 'VR4'),
    ('vlan-leaveall-keeps-in', 'srp_encoder', '', 'VR6'),
    ('vlan-rx-loses-to-leaveall', 'srp_encoder', '', 'VR7'),
    ('vlan-txla-keeps-in', 'srp_encoder', '', 'VR8'),
    ('vlan-applicant-deaf', 'srp_encoder', '', 'VR9'),
    ('vlan-note8-ignores-registrar', 'srp_encoder', '', 'VR10'),
    ('vlan-registrar-kept-on-allocation', 'srp_encoder', '', 'VR11'),
    # a withdrawn VID's Leave owed to its tx!, a re-join while it is owed, and
    # the s an LO entry owes
    ('vlan-leave-abandoned', 'srp_encoder', '', 'V6 last user out -> Lv VID 2'),
    ('vlan-leave-abandoned', 'srp_top', 'join', 'R4: WITHDRAW_LISTENER'),
    ('vlan-rejoin-of-leaving-allocates', 'srp_encoder', '', 'V10'),
    ('vlan-walk-frees-lo', 'srp_encoder', '', 'V11'),
]


def run(tree: Path, suite: str, group: str, log: Path) -> tuple[int, str]:
    """Build and run a cycle-bounded suite, preserving its complete receipt."""
    with log.open("w") as stream:
        result = subprocess.run(
            ["make", "-C", str(tree / "tb" / suite), "RUN_ARGS=" + group],
            stdout=stream, stderr=subprocess.STDOUT, check=False)
    return result.returncode, log.read_text()


def plant(tree: Path, label: str) -> None:
    """Apply one explicit patch to the scratch RTL, refusing drift."""
    subprocess.run(["git", "apply", "--check", str(PATCHES / (label + ".patch"))],
                   cwd=tree, check=True)
    subprocess.run(["git", "apply", str(PATCHES / (label + ".patch"))],
                   cwd=tree, check=True)


def trial(job: tuple[str | None, str, str]) -> tuple[int, str]:
    """Copy the RTL and the suites to a scratch tree of its own, plant the arm if any, and run.

    The receipt is kept inside the copy and returned: a label that runs on two
    suites has one receipt name, so the caller writes the receipts in the
    declared order, the later over the earlier, as a serial run leaves them.
    """
    label, suite, group = job
    with tempfile.TemporaryDirectory(prefix="srp-leaveall-") as tmp:
        tree = Path(tmp)
        shutil.copytree(ROOT / "hdl", tree / "hdl")
        for name in ("common", "srp_top", "srp_stream_fsms", "srp_encoder", "srp_admission"):
            shutil.copytree(ROOT / "tb" / name, tree / "tb" / name,
                            ignore=shutil.ignore_patterns("obj_*", "__pycache__"))
        if label is not None:
            plant(tree, label)
        return run(tree, suite, group, tree / "receipt.log")


def campaign(output: Path, selected: list[tuple], jobs: int) -> tuple[int, int, set[str]]:
    """Run positive controls first, then require each arm's own named assertion."""
    passed = 0
    total = 0
    covered: set[str] = set()
    pairs = sorted({(m[1], m[2]) for m in selected})
    with in_order(trial, [(None, suite, group) for suite, group in pairs], jobs) as results:
        for (suite, group), (rc, contents) in zip(pairs, results):
            (output / f"control-{suite}-{group}.log").write_text(contents)
            ok = rc == 0 and " 0 FAIL" in contents
            total += 1
            passed += ok
            print(f"control {suite} {group}: rc={rc} {'PASS' if ok else 'FAIL'}", flush=True)
            if not ok:
                print(contents[-4000:])
                return passed, total, covered
    units = [(label, suite, group) for label, suite, group, _ in selected]
    with in_order(trial, units, jobs) as results:
        for (label, suite, group, expected), (rc, contents) in zip(selected, results):
            (output / f"{label}.log").write_text(contents)
            failures = [line for line in contents.splitlines() if line.startswith("FAIL:")]
            ok = (rc != 0 and "checks:" in contents and "CYCLE_BUDGET" not in contents
                  and any(expected in line for line in failures))
            total += 1
            passed += ok
            tags = sorted({line.split(":", 2)[1].strip() for line in failures})
            if ok:
                covered.update(tags)
            verdict = "KILLED" if ok else "UNPROVEN"
            print(f"{label}: rc={rc} failures={len(failures)} {verdict} tags={','.join(tags)}",
                  flush=True)
            if not ok:
                print(contents[-2500:], flush=True)
    return passed, total, covered


def main() -> int:
    """Select patch arms, isolate all writes, and fail on any unproven result."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", default="")
    add_jobs_argument(parser)
    args = parser.parse_args()
    requested = set(args.only.split(",")) if args.only else {m[0] for m in MUTANTS}
    unknown = requested - {m[0] for m in MUTANTS}
    if unknown:
        parser.error(f"unknown mutation arms: {sorted(unknown)}")
    selected = [m for m in MUTANTS if m[0] in requested]
    args.output.mkdir(parents=True, exist_ok=True)
    passed, total, covered = campaign(args.output, selected, args.jobs)
    if not args.only:
        expected = {f"{group}{i}" for group, count in
                    [("K", 12), ("L", 4), ("M", 12), ("N", 13), ("O", 8), ("P", 8), ("Q", 4), ("R", 4),
                     ("S", 3), ("SC", 3), ("TF", 5), ("WK", 13), ("RB", 9), ("DJ", 4),
                     ("SH", 24), ("SW", 7), ("TP", 9), ("PS", 24), ("LK", 1), ("OV", 10),
                     ("AP", 7)]
                    for i in range(1, count + 1)}
        missing = expected - covered
        total += 1
        passed += not missing
        print(f"assertion coverage: {len(expected - missing)}/{len(expected)}; "
              f"missing={sorted(missing)}")
    print(f"{total} checks: {passed} PASS, {total - passed} FAIL")
    return int(passed != total)


if __name__ == "__main__":
    raise SystemExit(main())
