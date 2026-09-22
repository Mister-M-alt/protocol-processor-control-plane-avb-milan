#!/usr/bin/env bash
# Identity, live refs and exact scope of PR100 at the reviewed head.
set -u
R=$VALIDATION_STORAGE/reviews/r230-100-r2
HEAD_SHA=5c45845ad15bd7995f20c81d7fd61501e5ca9d7e
PRIOR=1eb20dc4911880de10b745cc284e7dde306788b5
BASE=8452f564294300a82d56eed464276576f65f4d58
REPO=Mister-M-alt/protocol-processor-control-plane-avb-milan
cd "$R" || exit 1
echo "## review clone"
git rev-parse HEAD 'HEAD^{tree}'
git log -1 --format='parents: %P%nauthor: %an <%ae> %ad%ncommitter: %cn %cd%nsubject: %s' HEAD
echo "symbolic-ref: $(git symbolic-ref -q HEAD || echo detached)"
echo "status --porcelain --ignored lines: $(git status --porcelain --ignored | wc -l)"
echo "merge-base HEAD base: $(git merge-base HEAD $BASE)"
git merge-base --is-ancestor $PRIOR HEAD && echo "prior $PRIOR is ancestor of HEAD"
git merge-base --is-ancestor $BASE HEAD && echo "base $BASE is ancestor of HEAD"
echo "commits base..HEAD:"; git log --format='  %H %s' $BASE..HEAD
echo; echo "## live remote refs ($(date -Is))"
git ls-remote origin refs/heads/main refs/heads/97-assert-distinct-sr-vid-fixture \
    refs/pull/100/head refs/pull/100/merge refs/heads/97-review-evidence
echo; echo "## PR100 live state"
gh pr view 100 --repo $REPO --json state,isDraft,mergeable,headRefOid,baseRefName,baseRefOid,headRefName \
    --jq '"state=\(.state) draft=\(.isDraft) mergeable=\(.mergeable) head=\(.headRefOid) base=\(.baseRefName)@\(.baseRefOid) branch=\(.headRefName)"'
echo; echo "## merge ref"
git fetch --quiet origin +refs/pull/100/merge:refs/remotes/origin/pr100-merge
git log -1 --format='pull/100/merge %H parents %P tree %T' refs/remotes/origin/pr100-merge
echo "head tree: $(git rev-parse 'HEAD^{tree}')"
echo; echo "## diff $PRIOR..HEAD (the correction)"
git diff --raw $PRIOR HEAD
git diff --numstat $PRIOR HEAD
echo "diff --check rc: $(git diff --check $PRIOR HEAD >/dev/null 2>&1; echo $?)"
echo; echo "## diff base..HEAD (whole PR)"
git diff --raw $BASE HEAD
git diff --numstat $BASE HEAD
echo "diff --check rc: $(git diff --check $BASE HEAD >/dev/null 2>&1; echo $?)"
echo; echo "## unchanged-artifact check $PRIOR -> HEAD (mode, type, blob per path)"
git ls-tree -r --full-tree $PRIOR > /tmp/r230-100-r2-scratch/tree-prior.txt
git ls-tree -r --full-tree HEAD > /tmp/r230-100-r2-scratch/tree-head.txt
echo "entries prior=$(wc -l < /tmp/r230-100-r2-scratch/tree-prior.txt) head=$(wc -l < /tmp/r230-100-r2-scratch/tree-head.txt)"
echo "entries that differ or exist on one side only:"
diff <(sort /tmp/r230-100-r2-scratch/tree-prior.txt) <(sort /tmp/r230-100-r2-scratch/tree-head.txt) | grep -E '^[<>]' | sed 's/^/  /'
for p in hdl syn scripts .github docs tb/pp_top/sim_main.cpp tb/pp_top/pp_top_wrap.sv Makefile; do
  a=$(git rev-parse "$PRIOR:$p"); b=$(git rev-parse "HEAD:$p")
  [ "$a" = "$b" ] && echo "unchanged $p ($a)" || echo "CHANGED $p $a -> $b"
done
for p in hdl syn scripts .github docs tb/pp_top/sim_main.cpp tb/pp_top/pp_top_wrap.sv; do
  a=$(git rev-parse "$BASE:$p"); b=$(git rev-parse "HEAD:$p")
  [ "$a" = "$b" ] && echo "vs base unchanged $p" || echo "vs base CHANGED $p"
done
echo; echo "## modes of the PR files at HEAD"
git ls-tree HEAD tb/pp_top/ | grep -E 'Makefile|README.md|fixture_guards.py|test_fixture_guards.py|sim_main.cpp'
echo; echo "## non-ASCII added lines base..HEAD"
git diff $BASE HEAD | grep -nP '^\+.*[^\x00-\x7F]' || echo "none"
