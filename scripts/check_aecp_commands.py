#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Hold the AECP command model to its generated, observed and gated consumers.

F06.14 is generated; the ROM generator consumes status codes and the MVU opcode.
The engine decode/dispatch is observed by tb/aecp_cmds, never parsed from source.
The recorded sweep covers opcodes 0x0000-0x007F, cdl 12-100 and its documented
stimuli. Its input digests bind the observations to the elaborated sources and ROM.
The ROM walk holds response sizes, lock/notify and statuses within the registry's
limits; field values/order and unswept behavior remain unproven.

BEGIN AECP FIELD REGISTRY
Generated from hdl/aecp/ucode/aecp_fields.json; edit the registry.
[] means table rows; * means named map members. Held means only the stated scope.
Rendering inputs generate F06.14; rendering equality is not behavioral proof.

- about: held to nothing. Explanatory prose; no consumer validates its assertions.
- commands: held to nothing. Container: shape is schema-checked; value claims belong to the individual
  fields below.
- commands[].clause: held to nothing. Specification citation prose; no semantic citation check.
- commands[].command: held to nothing. Command-size container; each leaf is classified below.
- commands[].command.cdl: held. Observed first-entry floor, unless floor is declared: ACQUIRE_ENTITY and
  REGISTER_UNSOLICITED_NOTIFICATION cdl are tied only by echo equality; IDENTIFY_NOTIFICATION also has an
  unsolicited body. Consumer: record._floor; table._sizes. Check: command floor or echo equality.
- commands[].command.floor: held. Explicit shorter accepted floor for echo commands. Consumer:
  record._floor. Check: declared floor.
- commands[].command.max: held to nothing. GET_DYNAMIC_INFO ceiling 524; the observed sweep stops at cdl 100
  and does not hold this ceiling.
- commands[].command.per: held. Exact gcd of accepted offsets for both mapping echo commands. Consumer:
  record._floor. Check: accepted-offset gcd.
- commands[].command.plus: held to nothing. Variable-part label used in rendering; presence selects a
  variable size rule, but its wording is not validated.
- commands[].command.unit: held to nothing. Rendering label for N records; existence is schema-checked,
  wording is not behavior.
- commands[].doc: held to nothing. F06.14 rendering data; no semantic validation of the prose.
- commands[].doc.class: held to nothing. F06.14 rendering prose; changes regenerate text without validating
  the assertion.
- commands[].doc.mandate: held to nothing. F06.14 rendering prose; changes regenerate text without
  validating the assertion.
- commands[].doc.scope: held to nothing. F06.14 rendering prose; changes regenerate text without validating
  the assertion.
- commands[].doc.size: held to nothing. F06.14 rendering prose; changes regenerate text without validating
  the assertion.
- commands[].engine_status: held to nothing. Held to nothing: engine-built status exemptions are trusted;
  their exact set and reachability are not independently held.
- commands[].entries: held. Every dispatched program and first-entry decode floor in the observed
  population. Consumer: record._dispatch; record._floor. Check: dispatch entries.
- commands[].gdi: held. Observed GET_DYNAMIC_INFO membership. Consumer: record._gdi_members. Check: GDI
  membership.
- commands[].hazard: held. Top-level classifier hazard name. Consumer: rtl.check_classifier. Check: command
  hazard.
- commands[].lock: held. ROM CHECK_LOCK reachability and scoreboard class membership. Consumer:
  rom._lock_and_notify; rtl.check_scoreboard. Check: lock protection.
- commands[].message: held. AEM/MVU dispatch identity, within the observed sweep. Consumer: record._served;
  table._identity. Check: message dispatch.
- commands[].name: held to nothing. Identity and diagnostic label; uniqueness is checked, but spelling is
  not an engine invariant (except the required MVU identity).
- commands[].notify: held. Presence of the notification contract. Consumer: rom._lock_and_notify;
  record._unsolicited. Check: notification presence.
- commands[].notify.body: held. Observed unsolicited dispatch and ROM body length. Consumer:
  record._unsolicited; rom._lock_and_notify. Check: unsolicited body.
- commands[].notify.class: held. Enqueued notification class; null means an engine event. Consumer:
  rom._lock_and_notify; rtl.check_notify. Check: notification class.
- commands[].notify.doc: held to nothing. F06.14 notification prose; no semantic validation.
- commands[].notify.kind: held. Notification kind mapping and observed unsolicited job. Consumer:
  rtl.check_notify; record._unsolicited. Check: notification kind.
- commands[].opcode: held. Engine constant and observed command_type, within the sweep. Consumer:
  rtl.check_engine; record._served. Check: opcode.
- commands[].oversize: held to nothing. Held to nothing: F06.14 rendering input; no gate derives the
  oversize flag from response size or the Milan list.
- commands[].response: held to nothing. Response-size container; each leaf is classified below.
- commands[].response.cdl: held. ROM fixed part, failure/unsolicited bodies and observed answers; echo
  responses equal command cdl (see command.cdl limits). Consumer: rom._entries; record._answers;
  table._sizes. Check: response size.
- commands[].response.echo: held. Observed command-length responses and ROM echo rule. Consumer:
  record._answers; rom._entries. Check: echo response.
- commands[].response.engine: held. Engine-built aggregate versus dispatched program. Consumer:
  record._dispatch. Check: engine-built answer.
- commands[].response.max: held to nothing. GET_DYNAMIC_INFO ceiling 524; the observed sweep stops at cdl
  100 and does not hold this ceiling.
- commands[].response.per: held. ROM variable step; echo step equals the command step, whose accepted-offset
  gcd is exact. Consumer: rom._entries; table._sizes; record._floor. Check: response record step.
- commands[].response.plus: held to nothing. Variable-part label used in rendering; presence selects a
  variable size rule, but its wording is not validated.
- commands[].response.stub: held. Fixed failure response length. Consumer: rom._entries; record._answers.
  Check: failure stub.
- commands[].response.unit: held to nothing. Rendering label for N records; existence is schema-checked,
  wording is not behavior.
- commands[].rtl: held. Declared engine constant identity. Consumer: rtl.check_engine. Check: opcode
  constant.
- commands[].status: held. ROM path statuses and observed answers, conditional on the trusted common_status
  and engine_status exemptions. Consumer: rom._statuses; record._answers. Check: command status set.
- common_status: held to nothing. Status exemptions trusted by the ROM and decode checks; reachability from
  a shared path only is not checked.
- common_status.AEM: held to nothing. Held to nothing: these shared-status exemptions are trusted, not
  proven reachable for every command; they can hide command-owned statuses.
- common_status.MVU: held to nothing. Held to nothing: these shared-status exemptions are trusted, not
  proven reachable for every command; they can hide command-owned statuses.
- documented: held to nothing. Includes six semantic unimplemented GDI members; individual fields below
  distinguish semantics from identity and rendering.
- documented[].cells: held to nothing. F06.14 rendering cells; cell count is checked, their prose is not
  machine semantics.
- documented[].gdi: held. Optional semantic membership flag for the six unimplemented GDI members. Consumer:
  record._gdi_members. Check: unimplemented GDI membership.
- documented[].name: held to nothing. Identity and diagnostic label, also rendered; required by schema,
  spelling has no behavioral consumer.
- documented[].opcode: held. Semantic input for the six unimplemented GDI members; other documented rows use
  opcode as rendering text only. Consumer: rtl._gdi_consts; record._gdi_members. Check: unimplemented GDI
  opcode.
- documented[].rtl: held. Optional semantic constant name for the six unimplemented GDI members. Consumer:
  rtl._gdi_consts. Check: unimplemented GDI constant.
- documented[].section: held to nothing. F06.14 rendering group and sort order; schema checks the allowed
  groups, not semantic placement.
- echo_entries: held to nothing. Trusted shared echo exceptions in size checks; membership is not
  independently held.
- fields: held. Sibling registry filename. Consumer: fields.check_fields. Check: export registry.
- hazard_classes: held to nothing. Container: shape is schema-checked; value claims belong to the individual
  fields below.
- hazard_classes.*: held. Hazard code by name. Consumer: rtl.check_classifier. Check: hazard class code.
- schema: held. Export schema tag. Consumer: table.check_schema. Check: schema tag.
- shared_entries: held to nothing. Shared-entry inventory keys are checked for missing claims only; added
  claims are not independently held.
- shared_entries.*: held to nothing. Diagnostic explanations of shared program names; wording is not checked
  and adding a key only widens the inventory.
- status_codes: held to nothing. Container: shape is schema-checked; value claims belong to the individual
  fields below.
- status_codes.*: held to nothing. Container: shape is schema-checked; value claims belong to the individual
  fields below.
- status_codes.*.code: held. IEEE 1722.1-2021 Table 7-141 code; also consumed by the ROM generator.
  Consumer: table._status_rows; rtl.check_status_codes. Check: IEEE status code.
- status_codes.*.rtl: held. Optional status constant, where non-null; null for STREAM_IS_RUNNING has no RTL
  constant. Consumer: rtl.check_status_codes. Check: status constant.
- unserved: held. Default program for every unserved opcode in the observed population. Consumer:
  record._unserved. Check: unserved program.
END AECP FIELD REGISTRY

--write regenerates F06.14 and the tracked registry references. --pr-body FILE
checks an external review draft's marked field reference (writes with --write).
--selftest plants every held registry field, every finding and every refusal.
--observed FILE compares a fresh sweep with the table and recorded decode.json;
--write-record rewrites that record. Exit 0 agrees, 1 disagrees, 2 refuses input.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from aecp_gate.field_docs import pr_reference
from aecp_gate.doc import rows  # noqa: E402
from aecp_gate.gate import check, observed, write  # noqa: E402
from aecp_gate.inputs import ROOT, TABLE, GateError, Inputs, load  # noqa: E402
from aecp_gate.plants import rom_op  # noqa: E402
from aecp_gate.selftest import selftest  # noqa: E402

#: the gate's API for a tool that loads this file as a module (the reviews'
#: ROM size probe does): read the inputs, plant a defect in an Inputs copy
#: (rom_op finds a ROM word to plant it in), and check the copy
__all__ = ["GateError", "Inputs", "check", "load", "main", "rom_op"]


def main() -> int:
    """The gate; `--selftest` first proves it can fail, `--write` regenerates,
    `--observed` is tb/aecp_cmds's verdict."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="plant each defect and require it caught")
    ap.add_argument("--write", action="store_true", help="regenerate F06.14 from the table")
    ap.add_argument("--pr-body", type=Path, help="check or regenerate the PR field reference")
    ap.add_argument("--observed", type=Path, help="tb/aecp_cmds's sweep output to hold")
    ap.add_argument("--write-record", action="store_true",
                    help="with --observed: rewrite tb/aecp_cmds/decode.json from it")
    ap.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = ap.parse_args()
    try:
        if args.selftest:
            return selftest(args.root)
        if args.write:
            result = write(args.root)
            if not result and args.pr_body:
                pr_reference(load(args.root), args.pr_body, True)
            return result
        if args.observed:
            return observed(args.root, args.observed, args.write_record)
        inp = load(args.root)
        problems = check(inp)
        if args.pr_body:
            problems += pr_reference(inp, args.pr_body, False)
    except GateError as exc:
        print(f"AECP COMMAND GATE: REFUSED - {exc}", file=sys.stderr)
        return 2
    if problems:
        print("AECP COMMAND GATE: FAIL - a consumer disagrees with "
              f"{TABLE}\n")
        print("\n".join(problems))
        return 1
    t = inp.table
    print(f"aecp commands: {len(t['commands'])} commands, "
          f"{sum(len(c['entries']) for c in t['commands'])} entries walked and observed, "
          f"F06.14 {len(rows(t))} rows, OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
