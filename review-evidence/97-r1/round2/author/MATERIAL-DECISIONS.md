[A166] MATERIAL DECISIONS

R230-M1 reproduced at 1eb20dc with installed GCC 16.2.1 German and French catalogs: LC_ALL unset, LANG=en_US.UTF-8, LANGUAGE=de/fr; each gate exits 2 despite the two intended 0002 assertion diagnostics.

The correction copies the inherited environment and overrides only LC_ALL=C for compiler subprocesses. Compiler selection/arguments, the four cases, exact expected diagnostic sets, total-error count and unrelated-error refusal remain unchanged. A standard-library regression required by fixture-guards checks all four compiler calls, preservation of other environment inputs/arguments, and no mutation of the caller or Verilator environment. It needs no installed language catalogs. Only the helper, its regression, Makefile wiring and focused README text change; R230-S1/S2 remain excluded. Sequential mutant and default/5A3C verification is in progress with an explicit 8-job Verilator cap.
