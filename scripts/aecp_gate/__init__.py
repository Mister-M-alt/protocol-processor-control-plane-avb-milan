# SPDX-FileCopyrightText: 2026 Kebag Logic
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""The AECP command gate (issue #73), one consumer per module.
scripts/check_aecp_commands.py is the entry point, and its docstring says what
the gate holds and what it does not.

  inputs    the files the gate reads, the generator's output, the sweep digests
  table     the table's own shape, and the size rules
  rtl       the RTL consumers' declared constants
  rom       the generated µcode, walked path by path
  record    tb/aecp_cmds' decode record: written, pinned, held to the table
  doc       F06.14, generated, and 06 section 8.1
  gate      every check in order, --write and --observed
  plants    the self-test's in-memory defects
  selftest  every defect planted and required caught by name
"""
