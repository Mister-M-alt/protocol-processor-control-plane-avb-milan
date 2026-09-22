#!/usr/bin/env bash
# Published round-2 evidence (author A166, manager A10) and live hosted state.
set -u
R=$VALIDATION_STORAGE/reviews/r230-100-r2
RC=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r230/receipts
S=/tmp/r230-100-r2-scratch
HEAD_SHA=5c45845ad15bd7995f20c81d7fd61501e5ca9d7e
PRIOR=1eb20dc4911880de10b745cc284e7dde306788b5
BASE=8452f564294300a82d56eed464276576f65f4d58
REPO=Mister-M-alt/protocol-processor-control-plane-avb-milan
cd "$R" || exit 1
git fetch --quiet origin +refs/heads/97-review-evidence:refs/remotes/origin/97-review-evidence
echo "## evidence branch 97-review-evidence tip: $(git rev-parse refs/remotes/origin/97-review-evidence)"
for c in 4d005092a0c5bec91b7768d5ab46beef0e6464c1 290d5a2ceb23a5cb4eac41257ac331d63babeaf3; do
  echo "commit $c parent $(git rev-parse $c^) subject: $(git log -1 --format=%s $c)"
  echo "  touched top-level prefixes:"; git diff-tree --no-commit-id -r --name-only $c^ $c | awk -F/ '{print "    " $1"/"$2"/"$3"/"$4}' | sort | uniq -c
  git merge-base --is-ancestor $c $HEAD_SHA && echo "  IS ancestor of head" || echo "  not an ancestor of head (never merged)"
done
rm -rf "$S/evidence" && mkdir -p "$S/evidence"
git archive 290d5a2ceb23a5cb4eac41257ac331d63babeaf3 review-evidence/97-r1/round2 review-evidence/97-r1/MANIFEST.json | tar -x -C "$S/evidence"
cd "$S/evidence/review-evidence/97-r1" || exit 1
echo; echo "## MANIFEST.json round2 entries vs published bytes"
python3 - <<'EOF'
import json, hashlib, os
m = json.load(open('MANIFEST.json'))
r2 = [e for e in m if e['file'].startswith('round2/')]
bad = [e['file'] for e in r2 if not os.path.exists(e['file']) or
       hashlib.sha256(open(e['file'], 'rb').read()).hexdigest() != e['published_sha256']]
flag = [e['file'] for e in r2 if e['path_redacted'] != (e['original_sha256'] != e['published_sha256'])]
disk = {os.path.join(r, f) for r, _, fs in os.walk('round2') for f in fs}
print(f"entries={len(r2)} on_disk={len(disk)} unlisted={sorted(disk - {e['file'] for e in r2})} "
      f"hash_mismatch={bad} redaction_flag_inconsistent={flag} path_redacted={sum(e['path_redacted'] for e in r2)}")
EOF
A=round2/author; M=round2/manager
echo; echo "## author patches vs actual diffs"
(cd "$R" && git diff $PRIOR $HEAD_SHA) | cmp - $A/source.patch && echo "author/source.patch == git diff prior..head"
(cd "$R" && git diff $BASE $HEAD_SHA) | cmp - $A/source-vs-main.patch && echo "author/source-vs-main.patch == git diff base..head"
echo; echo "## author IDENTITY.json"; cat $A/IDENTITY.json; echo
echo "## author RESULTS.json (return codes)"
python3 -c "
import json; r=json.load(open('$A/RESULTS.json'))
for k,v in r.items():
    if isinstance(v, dict) and 'returncode' in v: print(' ', k, 'rc', v['returncode'], v.get('canonical_tally',''), v.get('locale_regression',''))
print('  job_cap', r.get('job_cap'))"
echo; echo "## manager candidate / native / integrity"
cat $M/candidate.json; echo
python3 -c "
import json; r=json.load(open('$M/full-native/results.json')); print('head', r['head'])
for x in r['results']: print('  rc', x['exit_code'], ' '.join(x['command']))"
cat $M/full-native/complete.json; echo
grep -E 'PASS pp_top|suites:' $M/full-native/04.log
cat $M/full-native/01.log
python3 -c "
import json; f=json.load(open('$M/final-integrity.json')); print('final-integrity live_main', f['live_main'], 'candidate_equals_head_tree', f['candidate_equals_head_tree'])
for c in f['checkouts']: print('  ', c['checkout'], c['head'], c['tree'], 'porcelain=%r' % c['porcelain'])"
echo; echo "## manager hosted.json snapshot"
python3 -c "
import json; h=json.load(open('$M/hosted.json')); print('headRefOid', h['headRefOid'], 'isDraft', h['isDraft'])
for c in h['statusCheckRollup']: print('  ', c['name'], c['status'], c['conclusion'], c['detailsUrl'].split('/runs/')[1])"
echo; echo "## live hosted runs ($(date -Is))"
for r in 35708824838 35708829801; do
  gh api repos/$REPO/actions/runs/$r --jq '"run \(.id) \(.event) \(.status)/\(.conclusion) head_sha=\(.head_sha) attempt=\(.run_attempt)"'
  gh api repos/$REPO/actions/runs/$r/jobs --jq '.jobs[] | "  job \(.id) \(.name) \(.status)/\(.conclusion) head_sha=\(.head_sha)"'
done
echo "all hdl runs for head:"
gh api "repos/$REPO/actions/runs?head_sha=$HEAD_SHA" --jq '.workflow_runs[] | "  \(.id) \(.name) \(.event) \(.status)/\(.conclusion)"'
gh run view 35708824838 --repo $REPO --job 106684179182 --log > "$RC/01-hosted-suites-push.log" 2>&1
gh run view 35708829801 --repo $REPO --job 106684193618 --log > "$RC/01-hosted-suites-pr.log" 2>&1
for f in "$RC/01-hosted-suites-push.log" "$RC/01-hosted-suites-pr.log"; do
  echo "== $(basename $f)"
  grep -A1 -E 'git log -1 --format=%H' "$f" | tail -1 | awk '{print "  checked out: " $NF}'
  grep -E 'HEAD is now at|Verilator [0-9]\.[0-9]+ |PASS pp_top|FAIL pp_top|suites: ' "$f" | sed -E 's/^[^\t]*\t[^\t]*\t[^ ]+ /  /'
done
