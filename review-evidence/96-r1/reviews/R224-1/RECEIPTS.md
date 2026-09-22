R224-1 receipts for exact head ea93023fbdd31cbf718da0d8f1aab4d750bfd21a

All paths below are relative to this directory. REPORT.md is the review verdict.

- Public requirements/decisions: issue95*.json, parent400*.json, pr96*.json.
- Diff and history: reviewed.diff, reviewed-history.txt.
- Immutable public receipts: published/; public Git blob and published SHA256 verification: published-fetch-verification.json, evidence-identity.json.
- Hosted job identities: 35689949228*.json, 35689952257*.json; downloaded push logs: hosted-push-*.log.
- Reviewer focused executions: focused-results.json, pp_top.log, srp_top.log, srp_encoder.log, verilator-version.log.
- Effective compiler argv and cap: verilator8, verilator-invocations.jsonl, concurrency-verification.json.
- Independent binding, boundary and reset probes: probe_binding.py, binding-probe-results.json, probes/*/{probe.diff,make-dry-run.txt,build.log,run.log,result.json}.
- Synthetic Makefile controls (not RTL runs): probe_tally.py, tally-probe-results.json, tally-probes/*.log.
- Real-log aggregation and complete donor inventory assessment: tally-verification.json. The full sweep belongs to A10.
- Local documentation receipts: links.log, requirements-matrix.log, module-matrix.log, diagram-lint.log, stale.log, diffcheck.log.
- Source integrity: source-before.json, source-after.json, final-integrity.json. Includes types, modes, index entries and SHA256 for every tracked file; generated-ROM comparisons recorded separately in final-integrity.json.
- Reviewer probe setup limitations/errors: reviewer-probe-setup.txt.

Reproduction from the isolated checkout at the exact head, with this directory available:

```sh
rtk proxy make -C tb/pp_top -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/96-r1-r224/verilator8
rtk proxy make -C tb/srp_top -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/96-r1-r224/verilator8
rtk proxy make -C tb/srp_encoder -j1 VERILATOR=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/96-r1-r224/verilator8
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/96-r1-r224/probe_binding.py
rtk proxy python3 $WORKSPACE_HOME/milan-fpga-management/2026-09-22/96-r1-r224/probe_tally.py
rtk proxy python3 scripts/check-links.py
rtk proxy python3 scripts/check-matrix.py
rtk proxy python3 scripts/gen_matrix.py --check
rtk proxy bash scripts/lint-diagrams.sh
rtk proxy make -j1 stale
rtk proxy git diff --check 424c688fa2205b934a7689a58f2aa766420f2326 ea93023fbdd31cbf718da0d8f1aab4d750bfd21a
```

Run compilation commands sequentially. The wrapper replaces the existing Verilator -j 0 argument with -j 8; MAKEFLAGS alone is not used to enforce the cap. Probe scripts make independent Git-archive scratch copies and do not edit tracked product files. Their expected mutant failures are successful sensitivity results, not candidate failures.
