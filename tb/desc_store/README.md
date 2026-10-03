<!-- SPDX-License-Identifier: CERN-OHL-W-2.0 -->
# desc_store — KL_aecp_desc_store suite

Proves the [07 §3.3](../../docs/architecture/07_memory_maps.md) entity-model
store: `make` = build + run, exit 0 = PASS, **584 checks**.

The store keeps the descriptor image in the integrator's **main memory** (DDR3
on the reference board) and serves the µCPU's `st_*` face from an on-chip line
buffer, so the harness is two independent models — never DUT logic:

1. **A latency-injecting memory BFM.** Its first-word latency is **24 clocks by
   default and never zero**: the reference SoC measures ~1424 ns on a miss to
   main memory, and a store that only ever sees zero-latency answers is
   untested against the thing that makes this hard. The BFM also plays a bridge
   that **never accepts a request**, one that answers a beat with `rsp_err`, and
   one with inter-beat gaps at a longer latency.
2. **An independent parser *and* builder of the documented image format**
   (`hdl/aecp/desc/gen_desc_image.py` header + index map). Every expectation is
   computed from the parsed image, never from anything the DUT produced. The
   suite additionally loads the generator's own worked example
   (`example_milan_8.json` → `image.bin`), so generator and RTL are proven to
   agree on the *format* rather than merely on each other.

   That worked example is a **test vector, not a compliance reference**. Its
   stream descriptors deliberately use the Milan v1.2 Annex C Table C.1 layout
   (`formats_offset` = 136), which is *not* what ships: shipping descriptors
   come from the consuming repository's `avdecc/gen_aem_store.py` in the IEEE
   1722.1-2021 Table 7-8 layout (`formats_offset` = 138), as Milan v1.2
   §5.3.3.4 requires. The disagreement is the point. The store takes every
   length from the index map and never reads a descriptor's interior, so this
   is the one image here whose `formats_offset` contradicts the length its
   index map declares. See the header of `example_milan_8.json`. Because it is
   not a Milan model, `image.bin` is packed with the semantic lint off
   (`--no-lint`); the lint's positive case is `milan_min.json`, which the
   generator gate below packs.

## What it proves

| Group | Checks |
|---|---|
| **G1–G2** | boot walk out of DRAM, `configurations_count` and the image's writable-name count (region 0xA, the D3 writer's rule for a saved name, issues #61 and #83) read back; then every descriptor of the 07 §3.1 eight-descriptor example located and served **byte-exact**, lane by lane, with its length and `name_base`; a lane past the descriptor reads 0, never the next descriptor |
| **G3** | index-map **boundaries**: the first and the last entry are both found (a scan that stops one early, or runs one past, fails exactly here); a type past the last entry and a type in a hole between entries both MISS |
| **G4** | locate misses: unknown type, `descriptor_index` past `count`, unknown configuration — each `st_err`, each counted, and the store is not wedged afterwards |
| **G5** | the 07 §3.4 **name region** via `st_name`: every entry read back against the image, semantic index 0/1 validation, then full-lane and byte-strobed writes; each write is immediately visible in the unaligned inline field served by READ_DESCRIPTOR |
| **G5b** | an 80-entry name table crosses the 511-beat request limit, loads through multiple whole-name chunks, and preserves the final entry at its absolute overlay address |
| **G6** | the image is **read-only at run time** (07 §2): a non-name write is dropped, counted, and does not reach the served bytes |
| **G7** | **back-to-back reads with `st_req` held high** — the µCPU never drops the request between two consecutive state ops, so a store that latched the request edge would deadlock here |
| **G8** | a longer memory latency with inter-beat gaps changes nothing |
| **U1–U2** | **software has not loaded the image**: a 0xA5 region and an all-zero region are both refused on the magic/version/checksum header, every locate misses, and no length, no descriptor byte, no `configurations_count` and no name count comes back — an unloaded region cannot produce a plausible-looking descriptor |
| **U3–U4** | a corrupted (stale-checksum) image and a future layout version are refused |
| **U5–U7** | more index entries than the on-chip cache, more names than the overlay, and a descriptor longer than the line buffer are each refused **up front** with a distinct fault code |
| **U8** | **self-heal**: after a garbage boot, a late software load is picked up by the re-probe a locate arms — no reset needed, and nothing wrong served in between |
| **M1** | a bridge that **never accepts** degrades to a clean TIMEOUT fault and a `NO_SUCH_DESCRIPTOR`-shaped miss; it never hangs the µCPU |
| **M2** | a `rsp_err` on the descriptor fetch becomes a locate miss, and the store recovers |
| **S1** | two descriptors of one type with a length that is **not a multiple of 8** — without the padded `elem_stride` of the index entry, index 1 starts mid-beat and the whole line buffer comes back byte-shifted |
| **S2–S3** | an index entry whose stride is shorter than the length or not 8-aligned, and one with a zero length, are refused rather than served — and one bad entry does not poison the rest of the model |

`MEM_TIMEOUT_CYC_P` is compressed to 64 clocks here (`-G` in the Makefile) so
the "no bridge at all" arm runs in a few hundred clocks instead of tens of
thousands; the default is 4096 (41 µs at P-CLK-HZ), far inside `T-AECP-RESP`.

## Generator self-test gate

`make` runs `generator-check` before the RTL suite; the repository's
`scripts/run_suites.sh` therefore gates it too. Run just these probes with
`make -C tb/desc_store generator-check` from the repository root.

`test_gen_desc_image.py` drives only `build()` and the command-line packer. It
holds four groups: body/key agreement (below), the layout refusals, the
semantic model lint, and its waivers, ADP report and digest.

### The semantic model lint (issues #38, #39, #60, #89)

The packer lints the model by default (`hdl/aecp/desc/model_lint.py`, with the
rules L1 to L12 of docs/architecture/07 §3.1 in `model_rules.py`). The gate's evidence map is
[09 §8.5](../../docs/architecture/09_verification.md#85-the-descriptor-model-lint-issues-38-39-60-89):

- `milan_min.json` packs with every check on, and its digest equals the one
  recorded in `model_ids.json`.
- Each of the 89 named mutations in `lint_mutations.py` is refused with its
  rule and check. Where a check has several arms, the mutation's `detail`
  names the arm, and the refusal must carry it. The same mutated bytes pack
  with the lint off, so every refusal counted is the lint's. Every one of the
  56 checks has at least one mutation, and a test holds that set equal to
  `model_lint.CHECKS`.
- The caps accept their own value: 46 formats, 8 sampling rates, a 508-octet
  descriptor and 8 Annex C redundant streams pack, and so does a CRF input's
  clock source beside one at an AAF input. Each
  fixed-size Milan-subset type two octets long, and an AUDIO_MAP whose
  mappings_offset is not 8, is refused with its §7.2 extent (L12). A field
  past a descriptor's end is the finding of the rule that needs it.
- Standard-conforming models pack: CONTROLs a Unit and its Port own in the
  order IEEE 1722.1-2021 §7.2 walks them, a JACK's CONTROL and a Unit's
  SIGNAL_SELECTOR outside the top-level counts, cluster ranges in either
  order, the CONTROL of an AVB_INTERFACE, a CONTROL_BLOCK, a PTP_INSTANCE or
  a Unit's External Port outside the top-level counts, an IDENTIFY whose value
  type carries the r or the u flag, a second AVB_INTERFACE one configuration
  omits and one that first appears in configuration 1, every stream in the
  Milan v1.2 Annex C Table C.1 layout (R = 0), a redundant pair of Stream
  Outputs in that layout, each naming the other (R = 1), and a CLOCK_DOMAIN
  listing an INTERNAL source, the CRF input's INPUT_STREAM source and one
  INPUT_STREAM source per AAF input, ten in all (07 §3.1 L6).
  `example_milan_8.json` is refused with the lint on, though not for its
  Annex C streams.
- The layout refusals each have one negative case on `milan_min.json`: an
  index gap, a duplicate key, a mixed named and unnamed run, an ENTITY at
  index 1, a configuration gap.
- Waivers: a waiver applies and is reported. Without it the L1 refusal comes
  back. On a fixed model, past the descriptors, or naming a configuration the
  model lacks, it is refused as stale. It excuses no other check, index,
  descriptor type or configuration: a finding that differs from the waiver in
  any one of them is refused, and the waiver is stale. Fourteen malformed
  waivers are each refused (five of them a value of the wrong JSON type), and
  so are a waiver that shares a descriptor with an earlier one of its check,
  wholly or in part, and a `lint_waivers` that is not a list.
- The ADP values and the digest: driven values that agree pass, and a
  malformed driven value or recorded-digest map is an `ImageError`. The digest
  is tested field by field against IEEE 1722.1-2021 §6.2.2.8: object_name in
  every Table 7-1 type that has one; the first and the last octet of every
  fixed-offset field the clause names (ENTITY, AUDIO_UNIT, the streams,
  AVB_INTERFACE, CLOCK_SOURCE, MEMORY_OBJECT, VIDEO_CLUSTER, SENSOR_CLUSTER,
  SIGNAL_SELECTOR, CLOCK_DOMAIN); and the current values of every value family
  the clause names, in each type it names (linear in CONTROL, MIXER, MATRIX and
  SIGNAL_TRANSCODER; selector and array in CONTROL, MATRIX and
  SIGNAL_TRANSCODER; Bode, and the whole UTF8, SMPTE, sample-rate, gPTP and
  vendor values, in CONTROL). Each leaves the digest unchanged. The octets
  beside them (limits, defaults, options, units, strings, neighbouring fields)
  move it, and so do the values of the INT8 types, of MIXER's selector and
  array families and of MATRIX's UTF8 type, which the clause leaves out. A
  selector CONTROL's option change under a recorded digest is refused; its
  current-value change packs.
- The command line: the positive model with every check, and a refusal that
  exits 1 and writes nothing, a `--model-ids` file without `models` among them.
  Loading the packer by its path adds nothing to `sys.path` and registers no
  `model_lint` or `model_rules` module. Both load through the packer's one
  loader, and a module it cannot locate is an `ImportError` naming it.

Mutation proof: each check is suppressed in turn. Only that check's
findings are dropped, by replacing the lint's finding recorder in a child
process; no file changes. `lint_suppression.py` runs it
(`make -C tb/desc_store lint-suppression`); it exits 0 only when the control
passes and every check is killed.

| Probe | Result |
|---|---|
| Control: nothing suppressed | the gate passes |
| Each check suppressed alone, round 1 (2026-10-02, at e6cca1ff) | 53 of 53 checks make `test_mutations` fail. `WaiverTest` fails as well for `port-cluster-minimum` (4 tests), `has-parent`, `cluster-channels` and `talker-base-format` (1 each) |
| Each check suppressed alone, round 2 (2026-10-02, the committed driver) | 56 of 56 checks killed, L8's `identify-format` and L12's `descriptor-extent` and `descriptor-maximum` among them. `port-cluster-minimum` also fails 7 `WaiverTest` tests; `has-parent`, `cluster-channels` and `talker-base-format` fail one each |
| Each check suppressed alone, round 3 (2026-10-02, the committed driver) | 56 of 56 checks killed: `stream-layout` by 9 mutations (Table 7-8 and Annex C), `identify-format` by 8, one per arm |

Planted defects (round 2, 2026-10-02): the two round-1 reviews planted 22
and 32 textual defects in the lint and the packer, one per disposable copy,
47 distinct. 12 distinct ones survived the round-1 gate. Round 2 planted 46 of
them again on its own code; the 47th, L2 restricted to CONTROL, is now the
rule. With 14 more on the arms round 2 rewrote or added (its CRF, ownership,
order, per-port, digest, extent, IDENTIFY-format, waiver-typing, overlap,
digest-map and import-by-path arms), that is 60 plants, and the gate kills
every one. Each survivor now fails a named test:

| Survivor | Fails |
|---|---|
| a waiver ignores its descriptor type | `WaiverTest.test_waiver_scope_is_its_type` |
| a waiver ignores its configuration | `WaiverTest.test_waiver_scope_is_its_configuration` |
| mapping uniqueness reset per AUDIO_MAP | `test_mutations`: "AUDIO_MAP 1 repeats AUDIO_MAP 0" |
| L2's configuration-level CONTROL arm deleted | `test_mutations`: "a configuration-level CONTROL after a unit-owned one" |
| `stream-layout`'s length arm deleted | `test_mutations`: "output 8 bytes past its formats, offsets consistent" |
| a `ut` current_format covered by a wider `ut` entry (the round-1 arm was redundant and is gone; the plant clears current's `ut` bit instead) | `test_mutations`: "input current_format carries ut, under a wider ut entry" |
| the format cap 45, the rate cap 7 | `LintTest.test_boundaries_pack` |
| "exactly one" weakened to "at least one", at a CRF input and at the AAF inputs | `test_mutations`: "two INPUT_STREAM sources at the CRF input", "no CRF input, an INPUT_STREAM source at each AAF input" |
| an ENTITY outside configuration 0 accepted | `test_mutations`: "an ENTITY in configuration 1" |
| L5 compared index by index on the indices two configurations share | `ConformingModelTest.test_second_interface_optional_per_configuration` |

Planted defects (round 3, 2026-10-02): the round-2 reviews' plant scripts,
run unchanged on round 3's code, kill every plant: 37 of 37 and 37 of 37, and
8 of 8 in the internal review's re-creation of the external review's round-1
survivors. The round-1 internal script, run unchanged, kills 24 of its 29: the
expected-pass entity-id control stays green, and its four targets that round 2
rewrote are the four ported plants above, all killed. Each of the 30 round-2
survivors (22 distinct) fails a named test:

| Survivor | Fails |
|---|---|
| one `identify-format` arm deleted: the 113-octet length, `control_value_type`, `values_offset`, `number_of_values`, `minimum`, `step` or `unit` | `test_mutations`: the IDENTIFY mutation naming that arm |
| the value type compared without its r and u flags masked | `ConformingModelTest.test_identify_value_type_flags` |
| the 508-octet maximum refusing 507 or 508 octets | `LintTest.test_boundaries_pack` |
| overlap refused only for identical ranges | `WaiverTest.test_partly_overlapping_waivers_are_refused` |
| a digest exclusion dropped: CLOCK_DOMAIN `clock_source_index`, ENTITY `current_configuration` | `IdentityTest.test_fixed_exclusions_are_the_clause` |
| a digest exclusion dropped or widened: Bode current values, MIXER's linear current, SIGNAL_TRANSCODER's values, the array exclusion from `unit`, MIXER credited with the selector and array families | `IdentityTest.test_exclusions_are_the_clause` |
| the CONTROLs of an AVB_INTERFACE, a CONTROL_BLOCK, a PTP_INSTANCE or an External Port counted at the top level | `ConformingModelTest.test_other_control_owners` |
| an AVB_INTERFACE index configuration 0 lacks refused | `ConformingModelTest.test_interface_first_in_a_later_configuration` |

Each refusal statement of `model_rules.py` and `model_lint.py` (a
`ctx.bad(...)`, a `ValueError`, a refusal, stale or problem line) was then
replaced by `pass`, one per copy: the gate fails for 84 of 84.

The external review's 12 `milan_min.json` plants (one field edited, most with
the recorded digest re-recorded so only the lint can catch them) were run
again: 10 are killed, the AUDIO_CLUSTER padded by four octets now by L12. The
two that survive, `interface_flags` and `entity_capabilities` bit values, are
on 07 §3.1's "not linted" list with their reason.

Planted defects (round 4, 2026-10-02): five stricter L6 readings, one per
disposable copy, each refusing a set 07 §3.1 L6 allows. Without
`ConformingModelTest.test_a_source_per_aaf_input_beside_crf` the gate kills
one of them; with it, all five, and the new test fails for each:

| Plant | Also fails |
|---|---|
| beside a CRF input, at most one INPUT_STREAM source at the AAF inputs together | nothing |
| beside a CRF input, no INPUT_STREAM source at an AAF input | `LintTest.test_boundaries_pack` |
| `clock_sources_count` capped at 8, or at 9 | nothing |
| at most two INPUT_STREAM sources in a configuration | nothing |

### Body/key agreement

The body/key probes:

| Probe | `fields` | `bytes` |
|---|---|---|
| Legal body type/index equals the directory key | accepted, body bytes preserved | accepted, body bytes preserved |
| Only body type differs | named `ImageError`; CLI exit 1, no image or map | named `ImageError`; CLI exit 1, no image or map |
| Only body index differs | named `ImageError`; CLI exit 1, no image or map | named `ImageError`; CLI exit 1, no image or map |

Each of the six tests runs with a named type, an integer, a hexadecimal string
and a numeric type outside the name table: 24 cases. The target is configuration
1, index 1; all configurations and indices stay dense. Each mismatch changes
only the high byte of one body field. Refusals must report the configuration,
both key values and both body values. Legal controls also cover default-zero
keys, integer-string keys, unnamed fields, hex whitespace and `pad_to`.

Mutation proof (2026-09-26): remove only the body type/index comparison and its
`ImageError` from `_grouped_descriptors`, retaining every other refusal.

| Probe with check removed | Result |
|---|---|
| Legal controls, both forms | 2 tests / 8 cases pass, exit 0 |
| Type mismatch alone, both forms | 2 tests / 8 cases fail: `ImageError` not raised, exit 1 |
| Index mismatch alone, both forms | 2 tests / 8 cases fail: `ImageError` not raised, exit 1 |
| `make generator-check` | 16 refusal cases fail, both legal tests pass, make exit 2 |

The existing RTL tally above remains separate from these Python tests.

## Mutation-proven 2026-10-03 (region 0xA, issues #61 and #83)

Each break planted in a scratch copy of `hdl/`, `tb/common/` and this directory, then
`make` run there:

| Break | Went red |
|---|---|
| region 0xA answers `configurations_count` instead of the name count | **1** of 586: `G1 name count got 1 want 9` |
| region 0xA answers the header's name count while the image is invalid | **1** of 586: `U1 name count 42405 from an unloaded image` |

## Mutation-proven 2026-08-13

| Break | Went red |
|---|---|
| the whole header gate removed (`hdr_ok_w` forced true) — garbage accepted as a model | **18** of 375 |
| index-map scan stops one entry early (last-entry test `+1` → `+2`) | **21** of 375 |
| `elem_stride` replaced by `elem_len` in the locate address (index > 0 byte-shifted) | **12** of 375 |
| locate ignores `descriptor_index` vs `count` (out-of-range served) | **3** of 375 |
| checksum compare alone removed | **3** of 375 |
| magic compare alone forced true | **1** of 375 |

The last two are recorded because of what they say rather than how loud they
are: magic, version and checksum are *three* independent guards, so disabling
any one of them still leaves an unloaded region refused — only the fault CODE
changes. That is the intent (an accidental magic match must not be enough), and
it is why the honest mutation for "the store refuses what software has not
loaded" is the whole-gate one above.
