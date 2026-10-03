#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Run a mutation campaign's units on a pool of workers; read the results in declared order.

The mutation drivers share the meaning of `--jobs N` that `tb/pp_top/d3_mutants.py`
gave it: up to N units (a positive control or an arm) build and run at once, each in
its own private copy of the tree, so no two share a tree or an `obj_dir`. A driver
hands `in_order` its units in their declared order and reads each result back in that
order, whatever order they finish in, so its summary is the same at any N.
"""

import argparse
import concurrent.futures
import contextlib
from collections.abc import Callable, Iterator, Sequence
from typing import TypeVar

Unit = TypeVar("Unit")
Result = TypeVar("Result")

#: `tb/pp_top/d3_mutants.py`'s default
DEFAULT_JOBS = 4


def add_jobs_argument(parser: argparse.ArgumentParser) -> None:
    """Add `--jobs N`, how many units run at once, with d3_mutants.py's default."""
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                        help=f"units built and run at once, each in its own copy "
                             f"(default {DEFAULT_JOBS})")


@contextlib.contextmanager
def in_order(work: Callable[[Unit], Result], units: Sequence[Unit],
             jobs: int) -> Iterator[Iterator[Result]]:
    """Run `work` on every unit, at most `jobs` at once; give the results in the units' order.

    A unit's exception is raised when its turn comes, as a serial loop would raise it.
    Leaving the block (its end, a return, an exception) cancels every unit not yet
    started and waits for the running ones, so no build outlives its campaign.
    """
    pool = concurrent.futures.ThreadPoolExecutor(max(1, jobs))
    try:
        futures = [pool.submit(work, unit) for unit in units]
        yield (future.result() for future in futures)
    finally:
        pool.shutdown(wait=True, cancel_futures=True)
