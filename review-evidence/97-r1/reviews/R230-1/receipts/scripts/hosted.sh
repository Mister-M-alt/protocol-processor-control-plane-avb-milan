#!/usr/bin/env bash
# R230 read-only check of the public PR, branch refs and exact-head hosted runs.
# hosted.sh EVIDENCE_REPO LOGDIR   (LOGDIR receives the two suites job logs)
set -euo pipefail
R=Mister-M-alt/protocol-processor-control-plane-avb-milan
HEAD=1eb20dc4911880de10b745cc284e7dde306788b5
ev=$1; logdir=$2; mkdir -p "$logdir"
echo "== PR 100"
gh pr view 100 --repo $R --json number,state,isDraft,mergeable,headRefName,headRefOid,baseRefName,baseRefOid \
  --jq '"#\(.number) \(.state) draft=\(.isDraft) \(.mergeable) head=\(.headRefName)@\(.headRefOid) base=\(.baseRefName)@\(.baseRefOid)"'
echo "== refs"
git ls-remote https://github.com/$R.git refs/heads/main refs/heads/97-assert-distinct-sr-vid-fixture refs/pull/100/head refs/pull/100/merge
echo "== runs at $HEAD"
gh run list --repo $R --commit $HEAD --json databaseId,event,headSha,status,conclusion,workflowName \
  --jq '.[] | "\(.databaseId) \(.workflowName) \(.event) \(.headSha) \(.status) \(.conclusion)"'
for run in $(gh run list --repo $R --commit $HEAD --json databaseId --jq '.[].databaseId'); do
  gh run view "$run" --repo $R --json jobs \
    --jq '.jobs[] | "  job \(.databaseId) \(.name) \(.conclusion)"' | sed "s/^/  run $run/"
done
echo "== suites logs"
for job in 106644761597 106644775791; do
  gh run view --repo $R --job $job --log > "$logdir/suites-$job.log"
  echo "-- job $job"
  grep -E 'HEAD is now at|checkout --progress --force|Verilator 5\.|PASS pp_top|FAIL pp_top|suites: ' "$logdir/suites-$job.log" \
    | sed 's/^[^\t]*\t[^\t]*\t//'
done
echo "== PR merge commit used by the pull_request run"
git -C "$ev" log -1 --format='%H tree=%T parents=%P' 1a1f476b6f5dd0ebc35eb0d7e45b3ff08805735d
git -C "$ev" log -1 --format='%H tree=%T parents=%P' $HEAD
