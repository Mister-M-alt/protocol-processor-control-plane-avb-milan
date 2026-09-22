#!/usr/bin/env python3
"""R224 independent, sequential fixture mutations and boundary/reset probes.

Run from the isolated pinned checkout. Product files are read with git archive;
all mutations/builds run under a new /tmp/r224-pr96-* directory. Each build uses
the recorded verilator8 wrapper, and each command/exit is retained as a receipt.
"""
import difflib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import tempfile
import time

OUT = Path(__file__).resolve().parent
HEAD = 'ea93023fbdd31cbf718da0d8f1aab4d750bfd21a'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD']).decode().strip() == HEAD
archive = subprocess.check_output(['git', 'archive', HEAD])
scratch = Path(tempfile.mkdtemp(prefix='r224-pr96-binding-'))
results = []
binding = '      .DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),\n'
top = 'hdl/top/protocol_processor_top.sv'
cases = [
    ('missing_binding', '5A3C', top, binding, '', 13),
    ('truncated_binding', '5A3C', top, binding,
     "      .DOM_DEF_VID_P    (16'(SRP_DOM_DEF_VID_P[11:0])),\n", 4),
    ('child_default_control', '5A3C', 'hdl/srp/KL_srp_top.sv',
     "parameter logic [15:0] DOM_DEF_VID_P  = 16'd2,",
     "parameter logic [15:0] DOM_DEF_VID_P  = 16'd7,", 0),
    ('boundary_zero', '0000', None, None, None, 0),
    ('boundary_all_ones', 'FFFF', None, None, None, 0),
    ('warm_reset_and_isolation', '5A3C', None, None, None, 0),
]

for name, fixture, file, old, new, expected_failures in cases:
    root = scratch / name
    root.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tf:
        tf.extractall(root, filter='data')
    receipt = OUT / 'probes' / name
    receipt.mkdir(parents=True, exist_ok=True)
    changes = []

    def edit(path, old_text, new_text):
        p = root / path
        before = p.read_text()
        assert before.count(old_text) == 1, (name, path)
        after = before.replace(old_text, new_text)
        p.write_text(after)
        changes.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                           fromfile='a/' + path, tofile='b/' + path))

    if file:
        edit(file, old, new)
    if name == 'warm_reset_and_isolation':
        edit('tb/pp_top/sim_main.cpp',
             '    dv6_link_up_declares_the_default_again();\n  }',
             '''    dv6_link_up_declares_the_default_again();
    dv4_a_bridge_domain_is_still_adopted();
    h2.reset();
    h2.flush_all();
    dv1_reset_holds_the_default();
    dv2_link_up_declares_the_default();
    CHECK(h.t == 0 && h.domain_changes == 0,
          "R224: fixture parent model remains unclocked and unaffected");
  }''')
        edit('tb/pp_top/sim_main.cpp',
             '  DomainDefaultPhase{h}.run();\n  const char* const build = "fixture";',
             '  DomainDefaultPhase{h}.run();\n  DomainDefaultPhase{h}.run();\n  const char* const build = "fixture";')
    (receipt / 'probe.diff').write_text(''.join(changes))
    cwd = root / 'tb/pp_top'
    commands = []

    def run(cmd, log, timeout=300):
        started = time.monotonic()
        with (receipt / log).open('w') as f:
            rc = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                                timeout=timeout).returncode
        commands.append(dict(argv=cmd, cwd=str(cwd), exit_code=rc,
                             seconds=round(time.monotonic()-started, 3), log=log))
        return rc

    make_args = ['make', '-j1', 'VERILATOR=' + str(OUT / 'verilator8'),
                 'SRP_VID_FIXTURE=' + fixture]
    assert run(make_args + ['ltn_rom.hex', 'ucode.hex'], 'roms.log') == 0
    dry = subprocess.check_output(make_args + ['-n', 'run'], cwd=cwd).decode()
    (receipt / 'make-dry-run.txt').write_text(dry)
    lines = dry.replace('\\\n', '').splitlines()
    cmd = next(shlex.split(line) for line in lines
               if line.startswith(str(OUT / 'verilator8')) and '--Mdir obj_vid' in line)
    assert run(cmd, 'build.log') == 0
    (cwd / 'obj_dir').mkdir(exist_ok=True)
    rc = run(['./obj_vid/Vpp_top_vid'], 'run.log')
    text = (receipt / 'run.log').read_text()
    match = re.search(r'\[build fixture, SRP_DOM_DEF_VID_P 0x([0-9a-f]+)\] (\d+) checks, (\d+) failures', text)
    assert match, name
    checks, failures = int(match[2]), int(match[3])
    assert failures == expected_failures, (name, failures)
    assert rc == (1 if failures else 0), (name, rc)
    assert checks == (64 if name == 'warm_reset_and_isolation' else 20), (name, checks)
    result = dict(name=name, head=HEAD, fixture=fixture, root=str(root),
                  checks=checks, failures=failures, expected_failures=expected_failures,
                  exit_code=rc, commands=commands)
    (receipt / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    results.append(result)
    (OUT / 'binding-probe-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(name, checks, 'checks;', failures, 'failures; expected', expected_failures, flush=True)
