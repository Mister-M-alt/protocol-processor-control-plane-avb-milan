# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""What the AECP command gate reads: the table, the decode record, the RTL and
doc texts, the swept files, and the ROM the generator builds from the table.

Every path is relative to the repository root, so the self-test can run the
whole gate on a copy of these files.
"""
import hashlib
import importlib.util
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = "hdl/aecp/ucode/aecp_commands.json"
UCODE = "hdl/aecp/ucode/gen_ucode.py"
RECORD = "tb/aecp_cmds/decode.json"
ENGINE = "hdl/aecp/KL_aecp_engine.sv"
NOTIFY = "hdl/aecp/KL_aecp_notify.sv"
UCPU_PKG = "hdl/aecp/ucpu_pkg.sv"
PP_PKG = "hdl/common/pp_pkg.sv"
SCOREBOARD = "hdl/packet_engine/KL_pp_scoreboard.sv"
TOP = "hdl/top/protocol_processor_top.sv"
DOC = "docs/architecture/06_aecp_engine.md"
TEXTS = (ENGINE, NOTIFY, UCPU_PKG, PP_PKG, SCOREBOARD, TOP, DOC)
#! what tb/aecp_cmds' sweep is made of: the SystemVerilog it elaborates (held
#! by its code, comments and spacing ignored), the bench and the descriptor
#! image's inputs (by their bytes), and the generated ROM. decode.json records
#! each digest, so a record swept from another engine is refused by name.
SWEPT_SV = (PP_PKG, UCPU_PKG, "hdl/aecp/KL_aecp_ucpu.sv", "hdl/aecp/KL_aecp_desc_store.sv",
            "hdl/aecp/KL_aecp_dyn_state.sv", "hdl/aecp/KL_aecp_nvm_writer.sv",
            "hdl/aecp/KL_aecp_resp_buf.sv", ENGINE, "tb/aecp_cmds/aecp_cmds_wrap.sv")
SWEPT_RAW = ("tb/aecp_cmds/sim_main.cpp", "hdl/aecp/desc/milan_min.json",
             "hdl/aecp/desc/gen_desc_image.py")
ROM_KEY = "ucode.hex (generated)"

SCHEMA = "aecp-command-table/1"
RECORD_SCHEMA = "aecp-decode-record/1"
COLUMNS = ("Opcode", "Command", "Mandate", "Scope rule", "Class", "Lock-prot.",
           "GDI", "Oversize", "Notif", "Cmd cdl", "Resp. size", "Status set")
SECTIONS = ("aem", "mvu", "gdi", "tail")
#! the µCPU's response record: BUILD_HEADER owns bytes 0..11 and the cursor
#! starts at 12, so a program's length at SEND_RESPONSE IS the cdl (06 section 8)
HDR = 12
#! outcomes of a trial that are not a program: an answer the engine built
#! without one, a command it dropped, a trial that never ended
OUTCOMES = ("engine", "drop", "timeout")


class GateError(Exception):
    """An input that cannot be read: exit 2, never a pass."""


@dataclass
class Ucode:
    """What the generator built: the ROM words, which words a program placed,
    the place() targets, and the E_* entry points."""

    rom: list
    occupied: set
    placed: set
    entry: dict
    ops: dict


@dataclass
class Inputs:
    """Everything the gate reads, so the self-test can mutate a copy. The
    generator runs only once the table has passed every check that does not
    need it (`ucode_of`); a copy made without the decode record reads it from
    `root` when it is needed."""

    table: dict
    texts: dict
    ucode: Ucode | None = None
    record: dict | None = None
    root: Path = ROOT
    swept: dict | None = None


def load_ucode(path: Path) -> Ucode:
    """Run gen_ucode.py as a module (it reads aecp_commands.json beside it) and
    keep what it built, writing no bytecode next to it."""
    dont = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location("gen_ucode_for_gate", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except (Exception, SystemExit) as exc:  # any failure to generate is a refusal
        why = str(exc) if isinstance(exc, SystemExit) else repr(exc)
        raise GateError(f"{UCODE} did not generate: {why}") from exc
    finally:
        sys.dont_write_bytecode = dont
    for name in ("rom", "occupied", "placed", "OPS"):
        if not hasattr(mod, name):
            raise GateError(f"{UCODE} defines no {name}: the generator's shape has "
                            "changed under this gate")
    entry = {k[2:]: v for k, v in vars(mod).items()
             if k.startswith("E_") and isinstance(v, int)}
    return Ucode(list(mod.rom), set(mod.occupied), set(mod.placed), entry, dict(mod.OPS))


def read_json(root: Path, rel: str) -> dict:
    """One JSON input; unreadable or not JSON is a refusal."""
    try:
        raw = (root / rel).read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"cannot read an input: {exc}") from exc
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise GateError(f"{rel} is not JSON: {exc}") from exc


def load(root: Path, record: bool = True) -> Inputs:
    """Read the table, the decode record (unless it is about to be written)
    and the RTL and doc texts under `root`."""
    table = read_json(root, TABLE)
    kept = read_json(root, RECORD) if record else {}
    try:
        texts = {p: (root / p).read_text(encoding="utf-8") for p in TEXTS}
        swept = {p: (root / p).read_bytes() for p in SWEPT_SV + SWEPT_RAW}
    except OSError as exc:
        raise GateError(f"cannot read an input: {exc}") from exc
    return Inputs(table, texts, None, kept, root, swept)


def ucode_of(inp: Inputs) -> Ucode:
    """The generator's output, run on first use."""
    if inp.ucode is None:
        inp.ucode = load_ucode(inp.root / UCODE)
    return inp.ucode


def strip_sv(text: str) -> str:
    """SystemVerilog without its // and /* */ comments."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def digests(swept: dict, uc: Ucode) -> dict:
    """The sweep's inputs, digested: path -> sha256. SystemVerilog by its
    code alone, so a comment does not ask for a new sweep; the ROM as the
    ucode.hex the suites load."""
    out = {}
    for path in SWEPT_SV + SWEPT_RAW:
        data = swept[path]
        if path in SWEPT_SV:
            data = " ".join(strip_sv(data.decode("utf-8")).split()).encode("utf-8")
        out[path] = hashlib.sha256(data).hexdigest()
    out[ROM_KEY] = hashlib.sha256("".join(f"{w:012x}\n" for w in uc.rom)
                                  .encode("ascii")).hexdigest()
    return out
