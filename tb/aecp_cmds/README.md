<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# tb/aecp_cmds — what the AECP engine decodes and dispatches, observed

The AECP command table (`hdl/aecp/ucode/aecp_commands.json`, issue #73) says,
for each served command, which µPC programs the engine dispatches it to and
the shortest command it accepts. This suite asks the elaborated engine rather
than its source text. It drives `KL_aecp_engine` alone (`aecp_cmds_wrap.sv`,
`sim_main.cpp`) and records the µPC that the engine hands its µCPU (the
`u_ucpu` dispatch handshake) and the response the engine sends.
`scripts/check_aecp_commands.py --observed` then holds that record to the
table and to `decode.json`, the recorded copy that `make check` holds the
table to.

```
make            # build, sweep (about 45 s), hold the sweep to the table and to decode.json
make record     # rewrite decode.json from a fresh sweep (after an intended engine change)
make mutants    # the standing controls below, each in a scratch copy
```

## The sweep

The same stimulus goes to every opcode. No stimulus is chosen per command,
so a decode keyed on the wrong opcode, or an arm redirected to another
program, shows wherever it lands.

- **AEM** (message_type 0), every opcode 0x0000–0x007F, each of these
  templates over every descriptor type 0x0000–0x0028 and 0xFFFF:
  - descriptor_type @24 at every cdl from 12 to 100;
  - the same at cdl 16, 24 and 96 with every stream bound and streaming;
  - configuration 0 @24 with descriptor_type @28 (READ_DESCRIPTOR's form), at
    cdl 20 and 100;
  - descriptor_type @24 with count 1 @28 at cdl 24 and 28, and count 2 at cdl 28 and 36;
  - descriptor_type @24 with SET_STREAM_INFO's MSRP_ACC_LAT_VALID flags @28,
    with and without bit 31 of the latency @48, at cdl 60 and 96 (the
    IEEE 1722.1-2013 and 2021 bodies) and at cdl 97 and 100 (longer bodies,
    which neither standard defines);
  - and a LOCK target @36 that is not ENTITY, at cdl 28.
- **Floors.** Every program an opcode reached is replayed, with the stimulus
  that first reached it, at every shorter cdl down to 12. The shortest cdl at
  which the engine still dispatches that program is its observed floor.
- **Other message types.** AVC and EXTENDED (message types 4 and 14, both in
  the AEM bucket) and ADDRESS_ACCESS, every opcode with the descriptor-type
  template at cdl 24 and 100.
- **MVU.** Every command_type 0x0000–0x007F at cdl 12, 19, 20, 24, 32 and 100,
  under the Milan protocol_id and under four ids with one wrong byte.
- **GET_DYNAMIC_INFO** carrying one record of every command_type, with record
  lengths 0, 2, 4 and 8 and every descriptor type, a non-SUCCESS info_status,
  and the aggregate with no record.
- **Unsolicited jobs.** Every kind, with both directions where a kind has two.

The count-one trial at cdl 24 is a half-record control for the eight-byte mapping
step; cdl 28 accepts one and cdl 36 accepts two. A four-byte engine step adds offset
4. A sixteen-byte step reaches no positive accepted offset in these count templates,
so its gcd is 0 and the gate refuses it rather than treating an empty sample as proof.

The build sets `EN_IDENTIFY_NOTIF_P = 1`, so that the IDENTIFY_NOTIFICATION job
runs its program. No command decode reads that parameter. The environment
answers at once: a descriptor image from `milan_min.json`, a blank NVM, so the
D3 restore ends on defaults before the first command, and every gather face
answering zero.

## What the gate holds (`scripts/check_aecp_commands.py`)

For each served command:

- the programs its opcode is dispatched to on every outcome of the sweep are
  exactly the table's `entries`;
- the shortest cdl dispatched to its first entry is its command cdl, or its
  declared `floor`;
- the gcd of accepted offsets from the floor equals the mapping record step exactly;
- every response the bench saw fits its response rule and carries a status of
  its set, subject to the registry's trusted common_status and engine_status exemptions.

Beyond the served commands:

- every opcode the table does not serve reaches only the `unserved` program,
  on every message type swept;
- the GET_DYNAMIC_INFO members, the program each record runs and its record
  length, and the empty aggregate's size are the table's;
- every unsolicited job's command_type, body and size are the table's.

Three commands declare a floor of 12, because the engine judges no length for
them: ACQUIRE_ENTITY and IDENTIFY_NOTIFICATION (refused at every length) and
REGISTER_UNSOLICITED_NOTIFICATION (the 2013 form without flags is accepted).
SET_STREAM_INFO declares a floor of 60: the engine accepts the IEEE 1722.1-2013
body (cdl 60) beside the 2021 one (cdl 96) and refuses every other length,
shorter, between or longer, which the recorded SIBAD lengths hold (issue #176,
REQ-AEM-009).

## Standing controls (`make mutants`)

Each control is one exact edit, built and swept in its own copy. The arm is
detected only when the suite fails with a `FAIL:` line carrying its words.

| Arm | Edit | Named failure |
|---|---|---|
| q1-set-clock-source-to-gclks | SET_CLOCK_SOURCE's success arm to `UPC_GCLKS_C` (R608-1 q1) | `decode: SET_CLOCK_SOURCE: the table's entries name SCLKS, which the engine never dispatches AEM 0x0016 to` |
| q2-set-clock-source-decoded-on-get | `sclks_w` keyed on `OP_GET_CLOCK_SRC_C` (R608-1 q2) | `decode: GET_CLOCK_SOURCE: the engine dispatches AEM 0x0017 to SCLKS` |
| floor-set-clock-source-16 | SET_CLOCK_SOURCE's length floor 20 to 16 | `decode: SET_CLOCK_SOURCE: the shortest AEM 0x0016 the engine dispatches to SCLKS is cdl 16` |
| mvu-sub-decode-other-command-type | the MVU sub-decode keyed on command_type 0x0001 | `decode: GET_MILAN_INFO: the table's entries name MVUINFO, which the engine never dispatches MVU 0x0000 to` |
| mvu-protocol-id-tail-unchecked | the protocol_id tail compare dropped | `decode: MVU/wrong-protocol-id 0x0000 is no table command and the engine dispatches it to MVUINFO` |
| message-type-guard-dropped | `sclks_w` without its message_type 0 guard | `decode: AVC 0x0016 is no table command and the engine dispatches it to SCLKS` |
| unserved-opcode-decoded | `gcfg_w` also on 0x0005 | `decode: AEM 0x0005 is no table command and the engine dispatches it to GCFG` |
| mappings-accepted-between-records | ADD/REMOVE_AUDIO_MAPPINGS accepted at any cdl from 20 + 8·count | `decode: ADD_AUDIO_MAPPINGS: the engine dispatches AEM 0x002C to AMADD at cdl` |
| mappings-four-byte-record | Mapping length step 8 to 4 | `decode: ADD_AUDIO_MAPPINGS: command per is 8, the gcd of accepted offsets from floor 20 is 4` |
| mappings-sixteen-byte-record | Mapping length step 8 to 16 | `decode: ADD_AUDIO_MAPPINGS: command per is 8, the gcd of accepted offsets from floor 20 is 0` |
| gdi-member-refused | GET_COUNTERS out of `gdi_allowed` | `decode: GET_COUNTERS: the table makes it a GET_DYNAMIC_INFO member and the engine refuses a record of it` |
| gdi-record-length | GET_COUNTERS' record reservation 136 to 132 bytes (its program still writes 136, so the engine voids the aggregate) | `decode: GET_COUNTERS: inside GET_DYNAMIC_INFO its GCTRS record leaves the aggregate ENTITY_MISBEHAVING` |
| gdi-member-dispatch | GET_SAMPLING_RATE's record to `UPC_GCLKS_C` | `decode: GET_SAMPLING_RATE: inside GET_DYNAMIC_INFO the engine dispatches it to GCLKS` |
| unsolicited-command-type | the CFG job carrying `OP_GET_CONFIG_C` | `decode: SET_CONFIGURATION: the PP_UNS_CFG_C job answers` |
| unsolicited-body | the CLKS job's body to `UPC_GSRATE_C` | `decode: SET_CLOCK_SOURCE: the PP_UNS_CLKS_C job answers` |
| response-field-dropped | E_GCLKS's reserved field NOPed in the generator | `decode: GET_CLOCK_SOURCE: GCLKS answered SUCCESS at cdl 18` |

`make check` reads the recorded `decode.json`, not the engine, so on its own it
names none of these commands. It refuses each of these edits all the same, by
file name, because `decode.json` carries the digest of every input of its sweep
(the engine's SystemVerilog by its code, comments and spacing ignored, the
bench, the image's inputs and the generated ROM), and an edited input no longer
matches. The generator edit also fails the ROM walk. The command names come
from this suite and these controls, which run in CI's suites job, where
Verilator is present. After an intended engine change, `make record` and
commit `decode.json`.

The suite invokes its existing observed-command gate through `check_observed.py`.
`ObservedCommands.test_sweep` carries REQ-VER-002 as DIR for the executed
traceability gate; the command gate's checks, output and exit status are unchanged.
REQ-VER-002 remains a lint row held by `make check`, with the `no check` state
required by the traceability rules (09 §8.10). The command model is in 09 §8.11.
