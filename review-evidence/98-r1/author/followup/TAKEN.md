[A165] TAKEN (follow-up)

I continue as the sole author of the bounded follow-up authorized in [A10's correction decision](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/issues/98#issuecomment-5773480374). The lane is clean on `98-clarify-srp-domain-events` at `88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf`, which is not pushed.

Scope: only the note at `tb/srp_encoder/README.md:75-77`.
- It will stop saying that the F10.2 revert edge reads the old label.
- It will describe the edge without copying the new label.
- Its interpretation stays: defaults restored at LINK_DOWN, declared at LINK_UP.

F10.2, every other document, RTL, tests, parameters and timers stay byte-identical to `88a4eb4`.

Plan:
- one local commit with a one-line message;
- `make -j1 check` once on the final bytes;
- `git diff --check`, and scope and hash receipts;
- keep the F10.2 render proof attributed to `88a4eb4`, backed by the unchanged fence hash, with no re-render.

No push, PR, project change or merge. R233/R234 stay with the manager.
