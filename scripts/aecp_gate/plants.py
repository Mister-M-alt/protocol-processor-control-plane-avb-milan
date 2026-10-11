# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The self-test's planted defects, applied in memory: one for every finding
the AECP command gate can print and for every refusal its readers make. Each
is (what, the edit of an Inputs copy, the words every failure line must carry).
The defects the generator reads are planted in files instead (selftest.py)."""
import copy
from collections.abc import Callable

from .doc import END, doc_block, regenerated
from .inputs import (DOC, ENGINE, NOTIFY, PP_PKG, RECORD, ROM_KEY, SCOREBOARD, TABLE, TOP,
                     UCPU_PKG, GateError, Inputs, ucode_of)
from .record import split_group
from .rom import reachable


def cmd(inp: Inputs, name: str) -> dict:
    """The table row of `name` in a mutated copy."""
    return next(c for c in inp.table["commands"] if c["name"] == name)


def sub(inp: Inputs, path: str, old: str, new: str) -> None:
    """Replace the one occurrence of `old` in an input text; a fixture that no
    longer matches is itself a self-test failure."""
    text = inp.texts[path]
    if text.count(old) != 1:
        raise GateError(f"selftest fixture: {old!r} occurs {text.count(old)} times in {path}")
    inp.texts[path] = text.replace(old, new)


def sub_all(inp: Inputs, path: str, old: str, new: str) -> None:
    """Replace every occurrence of `old` in an input text, at least one."""
    if old not in inp.texts[path]:
        raise GateError(f"selftest fixture: {old!r} does not occur in {path}")
    inp.texts[path] = inp.texts[path].replace(old, new)


def rom_op(inp: Inputs, entry: str, op: str, nth: int = 0) -> int:
    """The address of the nth `op` word a path from E_entry can execute."""
    uc = ucode_of(inp)
    code = uc.ops[op]
    hits = [pc for pc in reachable(uc, uc.entry[entry]) if uc.rom[pc] >> 43 == code]
    if len(hits) <= nth:
        raise GateError(f"selftest fixture: fewer than {nth + 1} {op} from E_{entry}")
    return hits[nth]


def nop(inp: Inputs, entry: str, op: str, nth: int = 0) -> None:
    """Turn the nth `op` a path from E_entry can execute into a NOP (op 0)."""
    ucode_of(inp).rom[rom_op(inp, entry, op, nth)] = 0


def drop_all(inp: Inputs, entry: str, op: str) -> None:
    """Turn every `op` a path from E_entry can execute into a NOP (op 0)."""
    uc = ucode_of(inp)
    code = uc.ops[op]
    for pc in reachable(uc, uc.entry[entry]):
        if uc.rom[pc] >> 43 == code:
            uc.rom[pc] = 0


def into_fill(inp: Inputs, entry: str) -> None:
    """Make the word after E_entry's SEND_RESPONSE branch into the ROM fill."""
    uc = ucode_of(inp)
    free = max(set(range(len(uc.rom))) - uc.occupied)
    at = rom_op(inp, entry, "SEND_RESP") + 1
    uc.rom[at] = (uc.ops["BRANCH"] << 43) | free


def set_imm(inp: Inputs, at: int, imm: int) -> None:
    """Rewrite one ROM word's immediate."""
    uc = ucode_of(inp)
    uc.rom[at] = (uc.rom[at] & ~0xFFFFFF) | imm


def doc_cell(inp: Inputs, row: str, old: str, new: str) -> None:
    """Edit one cell of one F06.14 row (the doc-only drift)."""
    text = inp.texts[DOC]
    line = next(x for x in text.split("\n") if x.startswith(f"| {row} |"))
    if line.count(old) != 1:
        raise GateError(f"selftest fixture: {old!r} not once in F06.14 row {row}")
    inp.texts[DOC] = text.replace(line, line.replace(old, new))


def doc_rows(inp: Inputs, fn: Callable[[list], list]) -> None:
    """Rewrite F06.14's rows as a list: `fn(rows)` returns the new list."""
    have, a, b = doc_block(inp.texts[DOC])
    new = have[:2] + fn(have[2:])
    inp.texts[DOC] = inp.texts[DOC][:a] + "\n" + "\n".join(new) + "\n" + inp.texts[DOC][b:]


def row_at(rows_: list, opcode: str) -> int:
    """The index of the F06.14 row of `opcode`."""
    return next(k for k, x in enumerate(rows_) if x.startswith(f"| {opcode} |"))


def swap(rows_: list, a: str, b: str) -> list:
    """Two F06.14 rows exchanged."""
    i, j = row_at(rows_, a), row_at(rows_, b)
    rows_[i], rows_[j] = rows_[j], rows_[i]
    return rows_


def table_and_doc(inp: Inputs, fn: Callable[[dict], object]) -> None:
    """A table change with F06.14 regenerated from it, as `--write` would."""
    fn(inp.table)
    inp.texts[DOC] = regenerated(inp.table, inp.texts[DOC])


def group(inp: Inputs, bucket: str, op: int) -> dict:
    """The programs of one opcode in the decode record, split out of its group
    so a plant touches that opcode alone."""
    return split_group(inp.record, bucket, op)


def aem(inp: Inputs, op: int) -> dict:
    """The programs of one AEM opcode in the decode record (`group`)."""
    return group(inp, "AEM", op)


def rename(d: dict, old: str, new: str) -> None:
    """One key of a dict renamed."""
    d[new] = d.pop(old)


#: (what, mutation, words each failure line must carry) per planted defect
MUTANTS = (
    ("an opcode renumbered in the table only",
     lambda i: cmd(i, "GET_SAMPLING_RATE").update(opcode="0x001E"),
     ("engine: GET_SAMPLING_RATE", "0x001E")),
    ("a size changed in the doc table only",
     lambda i: doc_cell(i, "0x0015", "32 B (cdl 20)", "36 B (cdl 24)"),
     ("F06.14: row 0x0015 GET_SAMPLING_RATE, column Resp. size",)),
    ("an OP_*_C changed in the RTL only",
     lambda i: sub(i, ENGINE, "OP_GET_SAMP_RATE_C   = 16'h0015;",
                     "OP_GET_SAMP_RATE_C   = 16'h001E;"),
     ("engine: GET_SAMPLING_RATE", "OP_GET_SAMP_RATE_C = 0x001E")),
    ("a command missing from the table",
     lambda i: i.table["commands"].remove(cmd(i, "GET_AS_PATH")),
     ("OP_GET_AS_PATH_C = 0x0028 and the table has no command",)),
    ("a command missing from the table, as the engine decodes it",
     lambda i: i.table["commands"].remove(cmd(i, "GET_AS_PATH")),
     ("decode: AEM 0x0028 is no table command", "GASP")),
    ("an OP_*_C written in another form",
     lambda i: sub(i, ENGINE, "localparam logic [15:0] OP_GET_AS_PATH_C     = 16'h0028;",
                     "localparam int unsigned OP_GET_AS_PATH_C = 40;"),
     ("OP_GET_AS_PATH_C is not written",)),
    ("a GDI_*_C the table does not document",
     lambda i: i.table["documented"].remove(
         next(d for d in i.table["documented"] if d.get("rtl") == "GDI_GET_MEM_LEN_C")),
     ("GDI_GET_MEM_LEN_C = 0x0048 and the table documents no such",)),
    # --- the observed decode and dispatch (tb/aecp_cmds/decode.json) ---
    ("a success arm redirected to another program (R608-1 q1, as recorded)",
     lambda i: rename(aem(i, 0x0016), "SCLKS", "GCLKS"),
     ("decode: SET_CLOCK_SOURCE: the table's entries name SCLKS",)),
    ("a success arm redirected to another program, the other line",
     lambda i: rename(aem(i, 0x0016), "SCLKS", "GCLKS"),
     ("decode: SET_CLOCK_SOURCE: the engine dispatches AEM 0x0016 to GCLKS",)),
    ("a decode keyed on another opcode (R608-1 q2, as recorded)",
     lambda i: (aem(i, 0x0017).clear(), aem(i, 0x0017).update(
         copy.deepcopy(aem(i, 0x0016))), aem(i, 0x0016).clear(),
         aem(i, 0x0016).update({"NOTIMPL": {"floor": 12, "cdls": "12-100",
                                            "answers": {"1": "echo"}}})),
     ("decode: GET_CLOCK_SOURCE: the engine dispatches AEM 0x0017 to SCLKS",)),
    ("an entry the engine cannot dispatch to",
     lambda i: cmd(i, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN2"),
     ("GET_CLOCK_SOURCE: entry SCFGRUN2 has no UPC_SCFGRUN2_C",)),
    ("an entry the engine never dispatches the command to",
     lambda i: cmd(i, "GET_CLOCK_SOURCE")["entries"].append("SCFGRUN"),
     ("decode: GET_CLOCK_SOURCE: the table's entries name SCFGRUN",)),
    ("an engine entry no command claims",
     lambda i: (cmd(i, "GET_CONTROL")["entries"].remove("NSUPP1"),
                cmd(i, "SET_CONTROL")["entries"].remove("NSUPP1")),
     ("UPC_NSUPP1_C is claimed by no command",)),
    ("a command cdl off the engine's decode floor",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "GET_SAMPLING_RATE")["command"]
         .update(cdl=20)),
     ("decode: GET_SAMPLING_RATE: the shortest AEM 0x0015 the engine dispatches to "
      "GSRATE is cdl 16, the table's command cdl is 20",)),
    ("a decode floor moved in the engine (as recorded)",
     lambda i: aem(i, 0x0016)["SCLKS"].update(floor=16),
     ("decode: SET_CLOCK_SOURCE: the shortest AEM 0x0016", "is cdl 16")),
    ("a declared floor dropped from the table",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "REGISTER_UNSOLICITED_NOTIFICATION")
         ["command"].pop("floor")),
     ("decode: REGISTER_UNSOLICITED_NOTIFICATION: the shortest AEM 0x0024",
      "the table's command cdl is 16")),
    ("a per-record command accepted between records (as recorded)",
     lambda i: aem(i, 0x002C)["AMADD"].update(cdls="20, 24, 28, 36"),
     ("decode: ADD_AUDIO_MAPPINGS: the engine dispatches AEM 0x002C to AMADD at cdl 24",)),
    ("a response the bench saw at another size (as recorded)",
     lambda i: aem(i, 0x0016)["SCLKS"]["answers"].update({"0": "24"}),
     ("decode: SET_CLOCK_SOURCE: SCLKS answered SUCCESS at cdl 24",)),
    ("a response the bench saw at the command's length on a fixed rule (as recorded)",
     lambda i: aem(i, 0x0029)["GCTRS"]["answers"].update({"0": "echo"}),
     ("decode: GET_COUNTERS: GCTRS answered SUCCESS at the command's own length",)),
    ("a status the bench saw outside the set (as recorded)",
     lambda i: aem(i, 0x0007)["GCFG"]["answers"].update({"8": "16"}),
     ("decode: GET_CONFIGURATION: GCFG answered NO_RESOURCES",)),
    ("a program that sent no frame (as recorded)",
     lambda i: aem(i, 0x0007)["GCFG"]["answers"].update({"none": "no frame"}),
     ("decode: GET_CONFIGURATION: AEM 0x0007 dispatched to GCFG sent no frame",)),
    ("a command answered without a program (as recorded)",
     lambda i: aem(i, 0x0002).update(engine={"floor": 12, "cdls": "12",
                                             "answers": {"0": "32"}}),
     ("decode: ENTITY_AVAILABLE: the engine answers AEM 0x0002 without a program",)),
    ("a command the bench saw dropped (as recorded)",
     lambda i: aem(i, 0x0002).update(drop={"floor": 12, "cdls": "12",
                                           "answers": {"none": "no frame"}}),
     ("decode: ENTITY_AVAILABLE: the bench saw AEM 0x0002 dropped",)),
    ("a served command the sweep never sent (as recorded)",
     lambda i: i.record["decode"].pop("MVU"),
     ("decode: GET_MILAN_INFO: tb/aecp_cmds/decode.json has no MVU 0x0000",)),
    ("an unserved opcode decoded (as recorded)",
     lambda i: aem(i, 0x0003).update(GCFG={"floor": 12, "cdls": "12-100",
                                           "answers": {"0": "16"}}),
     ("decode: AEM 0x0003 is no table command", "GCFG")),
    ("another message type decoded as AEM (as recorded)",
     lambda i: group(i, "AVC", 0x0016).update(SCLKS={"floor": 24, "cdls": "24, 100",
                                                     "answers": {"0": "20"}}),
     ("decode: AVC 0x0016 is no table command", "SCLKS")),
    ("an engine source changed after the sweep",
     lambda i: i.swept.update({ENGINE: i.swept[ENGINE].replace(
         b"upc_r <= UPC_SCLKS_C;", b"upc_r <= UPC_GCLKS_C;")}),
     (f"decode: {RECORD} was swept from another {ENGINE}",
      "sweep this engine with make -C tb/aecp_cmds record")),
    ("the bench changed after the sweep",
     lambda i: i.swept.update({"tb/aecp_cmds/sim_main.cpp":
                               i.swept["tb/aecp_cmds/sim_main.cpp"] + b"\n"}),
     (f"decode: {RECORD} was swept from another tb/aecp_cmds/sim_main.cpp",)),
    ("the ROM changed after the sweep",
     lambda i: ucode_of(i).rom.__setitem__(
         max(set(range(len(ucode_of(i).rom))) - ucode_of(i).occupied), 1),
     (f"decode: {RECORD} was swept from another {ROM_KEY}",)),
    ("a trial that never ended (as recorded)",
     lambda i: i.record["sweep"].update(timeouts=1),
     ("decode: tb/aecp_cmds/decode.json: 1 trials never ended",)),
    ("a record of another schema",
     lambda i: i.record.update(schema="aecp-decode-record/0"),
     ("decode: tb/aecp_cmds/decode.json declares 'aecp-decode-record/0'",)),
    ("a record without its parts",
     lambda i: i.record.pop("gdi"),
     ("decode: tb/aecp_cmds/decode.json is not a decode record",)),
    ("a GET_DYNAMIC_INFO member the engine refuses (as recorded)",
     lambda i: i.record["gdi"]["members"].pop("0x0029"),
     ("decode: GET_COUNTERS: the table makes it a GET_DYNAMIC_INFO member",)),
    ("a GET_DYNAMIC_INFO flag changed in the table",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "GET_NAME").update(gdi=False)),
     ("decode: GET_NAME: the engine runs a record of it inside GET_DYNAMIC_INFO",)),
    ("a GET_DYNAMIC_INFO record length changed (as recorded)",
     lambda i: i.record["gdi"]["members"]["0x0029"].update(GCTRS="0:132"),
     ("decode: GET_COUNTERS: inside GET_DYNAMIC_INFO its GCTRS record is 132 bytes",)),
    ("a GET_DYNAMIC_INFO member that never runs its program (as recorded)",
     lambda i: i.record["gdi"]["members"]["0x0007"].pop("GCFG"),
     ("decode: GET_CONFIGURATION: inside GET_DYNAMIC_INFO the engine never dispatches it to GCFG",)),
    ("a GET_DYNAMIC_INFO record dispatched outside the entries (as recorded)",
     lambda i: i.record["gdi"]["members"]["0x0015"].update(GCLKS="0:8"),
     ("decode: GET_SAMPLING_RATE: inside GET_DYNAMIC_INFO the engine dispatches it to GCLKS",)),
    ("GET_DYNAMIC_INFO's empty aggregate at another size (as recorded)",
     lambda i: i.record["gdi"]["empty"].update(engine="0:cdl 16"),
     ("decode: GET_DYNAMIC_INFO: with no record the engine answers 0:cdl 16",)),
    ("a GET_DYNAMIC_INFO record that voids the aggregate (as recorded)",
     lambda i: i.record["gdi"]["members"]["0x0029"].update(GCTRS="10:aggregate cdl 12"),
     ("decode: GET_COUNTERS: inside GET_DYNAMIC_INFO its GCTRS record leaves the "
      "aggregate ENTITY_MISBEHAVING at aggregate cdl 12",)),
    ("GET_DYNAMIC_INFO's response cdl changed in the table",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "GET_DYNAMIC_INFO")["response"]
         .update(cdl=16)),
     ("decode: GET_DYNAMIC_INFO: with no record the engine answers 0:cdl 12",)),
    ("an unsolicited job carrying another command_type (as recorded)",
     lambda i: i.record["unsolicited"]["PP_UNS_CFG_C"].update(GCFG=["0x0007:16"]),
     ("decode: SET_CONFIGURATION: the PP_UNS_CFG_C job answers",)),
    ("an unsolicited job no table command claims (as recorded)",
     lambda i: i.record["unsolicited"]["PP_UNS_CFG_C"].update(GCFG=["0x0007:16"]),
     ("decode: the engine's PP_UNS_CFG_C job answers 0x0007 with GCFG",)),
    ("an unsolicited body changed in the table",
     lambda i: cmd(i, "SET_CLOCK_SOURCE")["notify"].update(body="GSRATE"),
     ("decode: SET_CLOCK_SOURCE: the PP_UNS_CLKS_C job answers",)),
    ("an unsolicited kind the engine does not run",
     lambda i: cmd(i, "SET_CLOCK_SOURCE")["notify"].update(kind="PP_UNS_CLOCK_C"),
     ("decode: SET_CLOCK_SOURCE: the engine runs no PP_UNS_CLOCK_C job",)),
    ("an unsolicited job at another size (as recorded)",
     lambda i: i.record["unsolicited"]["PP_UNS_CLKS_C"].update(GCLKS=["0x0016:24"]),
     ("decode: SET_CLOCK_SOURCE: its unsolicited GCLKS is cdl 24",)),
    ("an unsolicited arm no command claims",
     lambda i: cmd(i, "GET_AVB_INFO").update(notify=None),
     ("decode: the engine's PP_UNS_AVB_C job answers 0x0027 with GAVB",)),
    # --- the other consumers ---
    ("a hazard class changed in the top",
     lambda i: sub(i, TOP, "16'h0010: begin                        // SET_NAME\n"
                     "          hz_class_w = 4'(PP_HZ_NAME_WR);",
                     "16'h0010: begin                        // SET_NAME\n"
                     "          hz_class_w = 4'(PP_HZ_STREAM_CFG);"),
     ("classifier: SET_NAME: the table says NAME_WR", "0x0010 STREAM_CFG")),
    ("a hazard class changed in the table",
     lambda i: cmd(i, "GET_COUNTERS").update(hazard="MAP_CFG"),
     ("classifier: GET_COUNTERS: the table says MAP_CFG",)),
    ("a classified opcode no command carries",
     lambda i: i.table["commands"].remove(cmd(i, "GET_DYNAMIC_INFO")),
     ("hz_classify names 0x004B",)),
    ("a hazard code changed in pp_pkg",
     lambda i: sub(i, PP_PKG, "PP_HZ_IDENTIFY    = 4'd8", "PP_HZ_IDENTIFY    = 4'd9"),
     ("classifier: hazard class IDENTIFY is 8 in the table and 9",)),
    ("a lock flag changed in the table",
     lambda i: cmd(i, "SET_CONTROL").update(lock=False),
     ("rom: SET_CONTROL: the table says lock false",)),
    ("CHECK_LOCK dropped from a program",
     lambda i: drop_all(i, "SCLKS", "CHECK_LOCK"),
     ("rom: SET_CLOCK_SOURCE: the table says lock true", "never reach CHECK_LOCK")),
    ("the scoreboard's lock-protected set changed",
     lambda i: sub(i, SCOREBOARD, "|| (c == HZ_NAME_WR_C)     ", ""),
     ("scoreboard:", "hz_is_lockprot")),
    ("a notification class changed in the table",
     lambda i: cmd(i, "SET_SAMPLING_RATE")["notify"].update(**{"class": 8}),
     ("rom: SET_SAMPLING_RATE: its programs enqueue notification class [5]",)),
    ("a NOTIFY_ENQ class changed in a program",
     lambda i: set_imm(i, rom_op(i, "SCTRL", "NOTIFY_ENQ"), 2),
     ("rom: SET_CONTROL: its programs enqueue notification class [2]",)),
    ("the notify block's class map changed",
     lambda i: sub(i, NOTIFY, "4'd8: pick_kind_w = PP_UNS_CLKS_C;",
                     "4'd8: pick_kind_w = PP_UNS_SRATE_C;"),
     ("notify: SET_CLOCK_SOURCE: class 8 is PP_UNS_SRATE_C",)),
    ("a status dropped from the table",
     lambda i: cmd(i, "SET_CLOCK_SOURCE")["status"].remove("ENTITY_LOCKED"),
     ("rom: SET_CLOCK_SOURCE: status ENTITY_LOCKED is answered",)),
    ("a status the programs cannot answer",
     lambda i: cmd(i, "ENTITY_AVAILABLE")["status"].append("NO_RESOURCES"),
     ("rom: ENTITY_AVAILABLE: status NO_RESOURCES is in the table",)),
    ("a refusal status changed in a program",
     lambda i: set_imm(i, rom_op(i, "SCFGRUN", "SET_STATUS"), 8),
     ("rom: SET_CONFIGURATION: status NO_RESOURCES is answered",)),
    ("a status code changed in ucpu_pkg",
     lambda i: sub(i, UCPU_PKG, "ST_NO_RESOURCES_C   = 5'd8;", "ST_NO_RESOURCES_C   = 5'd9;"),
     ("status: NO_RESOURCES is 8 in the table and ST_NO_RESOURCES_C = 9",)),
    ("a response size changed in the table",
     lambda i: cmd(i, "GET_CLOCK_SOURCE")["response"].update(cdl=24),
     ("rom: GET_CLOCK_SOURCE: E_GCLKS answers SUCCESS at cdl 20",)),
    ("a field dropped from a program",
     lambda i: nop(i, "GSTRI", "BUILD_FLD", 3),
     ("rom: GET_STREAM_INFO: E_GSTRI answers SUCCESS at cdl",)),
    # --- every size held: the variable programs (R609-1) ---
    ("GET_AUDIO_MAP's response cdl 20, F06.14 regenerated",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "GET_AUDIO_MAP")["response"]
         .update(cdl=20)),
     ("rom: GET_AUDIO_MAP: E_GAMAP answers SUCCESS at cdl 24 + 8·N",
      "the table's response is 20 + 8·N")),
    ("GET_AUDIO_MAP's response cdl 28, F06.14 regenerated",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "GET_AUDIO_MAP")["response"]
         .update(cdl=28)),
     ("rom: GET_AUDIO_MAP: E_GAMAP answers SUCCESS at cdl 24 + 8·N",
      "the table's response is 28 + 8·N")),
    ("a header field NOPed in E_GAMAP",
     lambda i: nop(i, "GAMAP", "BUILD_FLD"),
     ("rom: GET_AUDIO_MAP: E_GAMAP answers SUCCESS at cdl 22",)),
    ("a header field NOPed in E_GAVB",
     lambda i: nop(i, "GAVB", "BUILD_FLD"),
     ("rom: GET_AVB_INFO: E_GAVB answers SUCCESS at cdl 28",)),
    ("a header field NOPed in E_GASP",
     lambda i: nop(i, "GASP", "BUILD_FLD"),
     ("rom: GET_AS_PATH: E_GASP answers SUCCESS at cdl 14",)),
    ("a header field NOPed in E_RDESC",
     lambda i: nop(i, "RDESC", "BUILD_FLD"),
     ("rom: READ_DESCRIPTOR: E_RDESC answers SUCCESS at cdl 14 + a copy",)),
    ("the record APPEND NOPed in E_GAMAP",
     lambda i: nop(i, "GAMAP", "APPEND"),
     ("rom: GET_AUDIO_MAP: its programs answer SUCCESS only at cdl 24",
      "no path builds the variable part")),
    ("the record APPEND NOPed in E_GAVB",
     lambda i: nop(i, "GAVB", "APPEND"),
     ("rom: GET_AVB_INFO: its programs answer SUCCESS only at cdl 32",)),
    ("the record APPEND NOPed in E_GASP",
     lambda i: nop(i, "GASP", "APPEND"),
     ("rom: GET_AS_PATH: its programs answer SUCCESS only at cdl 16",)),
    ("a per-record response read as a fixed one",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "GET_AS_PATH")["response"]
         .pop("per")),
     ("rom: GET_AS_PATH: E_GASP answers SUCCESS at cdl 16 + 8·N, the table's response is 16",)),
    ("an echo row's response off its command size",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "ACQUIRE_ENTITY")["response"]
         .update(cdl=32)),
     ("schema: ACQUIRE_ENTITY: an echo response is the command's own size",)),
    ("an echo row's command off its response size",
     lambda i: table_and_doc(i, lambda t: next(
         c for c in t["commands"] if c["name"] == "ADD_AUDIO_MAPPINGS")["command"]
         .update(cdl=24)),
     ("schema: ADD_AUDIO_MAPPINGS: an echo response is the command's own size",)),
    ("an unsolicited body of another size",
     lambda i: nop(i, "STRMUNS", "BUILD_FLD"),
     ("unsolicited body E_STRMUNS is cdl 12",)),
    ("a path into the ROM fill",
     lambda i: into_fill(i, "EAVL"),
     ("rom: ENTITY_AVAILABLE: a path from E_EAVL runs into ROM word",)),
    ("section 8.1 missing a served command",
     lambda i: sub(i, DOC, "| 0x0002 ENTITY_AVAILABLE |", "| ENTITY_AVAILABLE |"),
     ("8.1: ENTITY_AVAILABLE (AEM 0x0002) is served",)),
    ("a row missing from F06.14",
     lambda i: setattr(i, "texts", {**i.texts, DOC: "\n".join(
         x for x in i.texts[DOC].split("\n") if not x.startswith("| 0x0029 |"))}),
     ("F06.14: row 0x0029 GET_COUNTERS is missing",)),
    ("two commands on one opcode",
     lambda i: cmd(i, "STOP_STREAMING").update(opcode="0x0022"),
     ("schema: STOP_STREAMING: opcode 0x0022 is also START_STREAMING's",)),
    ("a status the table does not define",
     lambda i: cmd(i, "GET_NAME")["status"].append("ENTITY_ACQUIRED"),
     ("schema: GET_NAME: status ENTITY_ACQUIRED is not in status_codes",)),
    ("a status code off IEEE Table 7-141 (no RTL constant holds it)",
     lambda i: i.table["status_codes"]["STREAM_IS_RUNNING"].update(code=13),
     ("schema: status STREAM_IS_RUNNING is 13 in the table, IEEE 1722.1-2021 "
      "Table 7-141 gives 12",)),
    ("a status code that is not an integer",
     lambda i: i.table["status_codes"]["NO_RESOURCES"].update(code="8"),
     ("schema: status NO_RESOURCES: code must be an integer",)),
    ("a common status the table does not define",
     lambda i: i.table["common_status"]["AEM"].append("BUSY"),
     ("schema: common_status AEM: status BUSY is not in status_codes",)),
    ("an unserved program that is no shared entry",
     lambda i: i.table.update(unserved="NOTIMPL2"),
     ("schema: unserved 'NOTIMPL2' is no shared entry",)),
    ("a table of another schema",
     lambda i: i.table.update(schema="aecp-command-table/2"),
     (f"schema: {TABLE} declares 'aecp-command-table/2'",)),
    ("an empty part of the table",
     lambda i: i.table.update(hazard_classes={}),
     ("schema: status_codes, hazard_classes or commands is empty",)),
    ("a command without a field",
     lambda i: cmd(i, "GET_NAME").pop("hazard"),
     ("schema: GET_NAME: missing hazard",)),
    ("a command listed twice",
     lambda i: i.table["commands"].append(copy.deepcopy(cmd(i, "GET_NAME"))),
     ("schema: GET_NAME is listed twice",)),
    ("a message type that is neither AEM nor MVU",
     lambda i: cmd(i, "GET_MILAN_INFO").update(message="AAU"),
     ("schema: GET_MILAN_INFO: message 'AAU' is neither AEM nor MVU",)),
    ("the MVU row the generator reads, gone",
     lambda i: cmd(i, "GET_MILAN_INFO").update(message="AAU"),
     ("schema: GET_MILAN_INFO: no command with message MVU",)),
    ("a message type without its common statuses",
     lambda i: i.table["common_status"].pop("MVU"),
     ("schema: GET_MILAN_INFO: message 'MVU' has no common_status row",)),
    ("an opcode not written 0xHHHH",
     lambda i: cmd(i, "GET_NAME").update(opcode="0x11"),
     ("schema: GET_NAME: opcode '0x11' is not 0xHHHH",)),
    ("a hazard class the table does not define",
     lambda i: cmd(i, "GET_NAME").update(hazard="NAME_RD"),
     ("schema: GET_NAME: hazard NAME_RD is not in hazard_classes",)),
    ("a flag that is not true or false",
     lambda i: cmd(i, "GET_NAME").update(lock="no"),
     ("schema: GET_NAME: lock, gdi and oversize must be true or false",)),
    ("a size without an integer cdl",
     lambda i: cmd(i, "GET_NAME")["response"].update(cdl="84"),
     ("schema: GET_NAME: response has no integer cdl",)),
    ("a per-record size without its unit",
     lambda i: cmd(i, "GET_AS_PATH")["response"].pop("unit"),
     ("schema: GET_AS_PATH: response per must be an integer with its unit",)),
    ("a floor at or above the command cdl",
     lambda i: cmd(i, "ACQUIRE_ENTITY")["command"].update(floor=28),
     ("schema: ACQUIRE_ENTITY: command floor must be",)),
    ("an entry listed twice",
     lambda i: cmd(i, "GET_NAME")["entries"].append("GNAME"),
     ("schema: GET_NAME: entries must be a non-empty list of names without repeats",)),
    ("a notification without its body",
     lambda i: cmd(i, "SET_NAME")["notify"].pop("body"),
     ("schema: SET_NAME: notify must be null or carry class, kind and body",)),
    ("an unserved F06.14 row short of a cell",
     lambda i: i.table["documented"][0]["cells"].pop(),
     ("schema: documented row '0x0003': needs",)),
    ("an unserved F06.14 row without its name (R609-1 S2)",
     lambda i: i.table["documented"][0].pop("name"),
     ("schema: documented row '0x0003': needs",)),
    ("a constant the engine does not declare",
     lambda i: cmd(i, "GET_NAME").update(rtl="OP_GET_NAMES_C"),
     ("engine: GET_NAME: the table names OP_GET_NAMES_C", "declares no such constant")),
    ("a GDI_*_C renumbered in the table",
     lambda i: next(d for d in i.table["documented"]
                    if d.get("rtl") == "GDI_GET_VIDEO_FMT_C").update(opcode="0x000C"),
     ("engine: GET_VIDEO_FORMAT: the table says 0x000C", "GDI_GET_VIDEO_FMT_C = 0x000B")),
    ("a GDI_*_C the engine does not declare",
     lambda i: next(d for d in i.table["documented"]
                    if d.get("rtl") == "GDI_GET_VIDEO_FMT_C")
               .update(rtl="GDI_GET_VIDEO_FORMAT_C"),
     ("engine: GET_VIDEO_FORMAT: the table names GDI_GET_VIDEO_FORMAT_C",)),
    ("a hazard code pp_pkg adds",
     lambda i: sub(i, PP_PKG, "PP_HZ_IDENTIFY    = 4'd8",
                     "PP_HZ_IDENTIFY    = 4'd8,\n    PP_HZ_SPARE       = 4'd9"),
     (f"classifier: {PP_PKG} declares PP_HZ_SPARE and the table does not",)),
    ("a non-AEM command off the classifier's default",
     lambda i: cmd(i, "GET_MILAN_INFO").update(hazard="MAP_CFG"),
     ("classifier: GET_MILAN_INFO: a non-AEM command takes the classifier's default",)),
    ("a class the notify block maps and does not queue",
     lambda i: sub(i, NOTIFY, "4'd5, 4'd7, 4'd8, 4'd9};", "4'd5, 4'd7, 4'd8};"),
     ("notify:", "class 9 (START_STREAMING) is in one only")),
    ("a class off the notify block's map whose kind another class carries",
     lambda i: cmd(i, "SET_NAME")["notify"].update(**{"class": 6}),
     ("notify: SET_NAME: class 6 is not in", "its kind PP_UNS_NAME_C is another class's")),
    ("a class the notify block maps and no command enqueues",
     lambda i: sub(i, NOTIFY, "        4'd9: pick_kind_w = PP_UNS_STRM_C;\n",
                     "        4'd9: pick_kind_w = PP_UNS_STRM_C;\n"
                     "        4'd10: pick_kind_w = PP_UNS_STRM_C;\n"),
     ("notify:", "maps class 10 to PP_UNS_STRM_C and no table command enqueues it")),
    ("a ucpu_pkg status the table does not name",
     lambda i: i.table["status_codes"]["NO_RESOURCES"].update(rtl=None),
     (f"status: {UCPU_PKG} declares ST_NO_RESOURCES_C and the table has no status for it",)),
    ("an engine-built status on a program-built response",
     lambda i: cmd(i, "GET_NAME").update(engine_status=["SUCCESS"]),
     ("rom: GET_NAME: engine_status is only for an engine-built response",)),
    ("an entry the generator never placed",
     lambda i: ucode_of(i).entry.pop("EAVL"),
     ("rom: ENTITY_AVAILABLE: entry EAVL is no placed program",)),
    ("a status code the table does not know set in a program",
     lambda i: set_imm(i, rom_op(i, "SCFGRUN", "SET_STATUS"), 13),
     ("rom: SET_CONFIGURATION: a path sets status 13, which the table does not know",)),
    ("a notification body the generator never placed",
     lambda i: ucode_of(i).entry.pop("STRMUNS"),
     ("rom: START_STREAMING: notification body STRMUNS is no placed program",)),
    ("F06.14's header changed",
     lambda i: sub(i, DOC, "| Opcode | Command | Mandate |", "| Op | Command | Mandate |"),
     ("F06.14: the header is not",)),
    ("a row repeated in F06.14",
     lambda i: doc_rows(i, lambda r: r + [r[row_at(r, "0x0029")]]),
     ("F06.14: 42 rows where the table generates 41",)),
    ("a row in F06.14 the table does not generate",
     lambda i: doc_cell(i, "0x0029", "| GET_COUNTERS |", "| GET_COUNTER |"),
     ("F06.14: row 0x0029 GET_COUNTER is not in the table",)),
    ("an F06.14 row with an extra cell",
     lambda i: doc_rows(i, lambda r: [x + " extra |" if x.startswith("| 0x0029 |") else x
                                      for x in r]),
     ("F06.14: row 0x0029 GET_COUNTERS has 13 cells, not 12",)),
    ("F06.14 rows out of order",
     lambda i: doc_rows(i, lambda r: swap(r, "0x0029", "0x002B")),
     ("F06.14: the rows are out of the generated order",)),
    ("section 8.1 listing a command the engine does not serve",
     lambda i: sub(i, DOC, "| 0x0002 ENTITY_AVAILABLE |",
                     "| 0x0002 ENTITY_AVAILABLE, 0x0003 CONTROLLER_AVAILABLE |"),
     ("8.1: section 8.1 lists AEM 0x0003 and the table serves no such command",)),
    ("a parsed block renamed (a stale pattern)",
     lambda i: sub(i, TOP, "always_comb begin : hz_classify",
                     "always_comb begin : hz_classifier"),
     ("gate refused", "hz_classify", "the pattern has gone stale")),
    ("a parsed block with no end",
     lambda i: sub_all(i, NOTIFY, "endcase", "endcas"),
     ("gate refused", f"{NOTIFY} emit_pick: no ")),
    ("the engine's entries written in another form",
     lambda i: sub_all(i, ENGINE, "localparam logic [10:0] UPC_",
                         "localparam logic [11:0] UPC_"),
     ("gate refused", f"{ENGINE}: a pattern parsed nothing")),
    ("hz_classify's opcode case written in another form",
     lambda i: sub(i, TOP, "unique case (hz_opcode_w)", "case (hz_opcode_w)"),
     ("gate refused", "hz_classify's default or its opcode case is not parseable")),
    ("hz_classify's opcode labels written in another form",
     lambda i: sub_all(i, TOP, "16'h", "16'H"),
     ("gate refused", "hz_classify's opcode case parsed nothing")),
    ("pp_pkg's hazard codes written in another form",
     lambda i: sub_all(i, PP_PKG, "= 4'd", "= 4'h"),
     ("gate refused", "no PP_HZ_* enum parsed")),
    ("hz_is_lockprot naming a class without its alias",
     lambda i: sub(i, SCOREBOARD,
                   "localparam logic [3:0] HZ_NAME_WR_C     = pp_pkg::PP_HZ_NAME_WR;", ""),
     ("gate refused", "hz_is_lockprot is not parseable")),
    ("the notify block's queued classes written in another form",
     lambda i: sub(i, NOTIFY, "assign cmd_class_ok_w = ev_cmd_class_i inside {",
                     "assign cmd_class_ok_w = (ev_cmd_class_i) inside {"),
     ("gate refused", "the command-class map is not parseable")),
    ("ucpu_pkg's status codes written in another form",
     lambda i: sub_all(i, UCPU_PKG, "localparam logic [4:0] ST_", "localparam logic [5:0] ST_"),
     ("gate refused", "no ST_*_C parsed")),
    ("F06.14's end marker gone",
     lambda i: sub(i, DOC, END, ""),
     ("gate refused", "the F06.14 markers are missing")),
    ("section 8.1's heading renamed",
     lambda i: sub(i, DOC, "### 8.1 Realization status", "### 8.1 Realisation status"),
     ("gate refused", "section 8.1's table is not parseable")),
)
