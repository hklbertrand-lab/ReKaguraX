#!/usr/bin/env bash
# Pousse Ikaruga/ sur ReKaguraX avec une copie du runtime commun dans Xbox360Runtime/ (le dépôt reste compilable seul).
# Usage (depuis Ikaruga/) : sh tools/push_rekagurax.sh [--dry-run]
set -e
URL=https://github.com/hklbertrand-lab/ReKaguraX.git
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
W="$(mktemp -d)"
cleanup() { cd "$ROOT"; git worktree remove --force "$W" 2>/dev/null || true; git branch -D rkx-split >/dev/null 2>&1 || true; git update-ref -d refs/remotes/rkx/main 2>/dev/null || true; }
trap cleanup EXIT
git fetch -q "$URL" main:refs/remotes/rkx/main
git subtree split -q --prefix=Ikaruga -b rkx-split >/dev/null
git worktree add -q "$W" rkx-split
cd "$W"
git merge -q --no-edit rkx/main -m "Fusion de ReKaguraX main" >/dev/null
# Copie du runtime tel qu'il est commité dans le monodépôt
rm -rf Xbox360Runtime
(cd "$ROOT" && git archive HEAD Xbox360Runtime) | tar -x
git add -A Xbox360Runtime
if ! git diff --cached --quiet; then
    git commit -q -m "Xbox360Runtime : copie du runtime commun ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi
git log --oneline -3
if [ "$1" = "--dry-run" ]; then echo "(--dry-run : rien poussé)"; ls Xbox360Runtime; exit 0; fi
git push "$URL" rkx-split:main
