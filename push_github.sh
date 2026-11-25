#!/bin/bash
# Create github.com/veldig/qnx-vibguard (public) and push this folder.
# Run:  bash ~/Downloads/JobAgent/qnx/push_github.sh
set -u
cd "$HOME/Downloads/JobAgent/qnx" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
LOG="logs/push-$(date +%Y%m%d-%H%M%S).log"; mkdir -p logs
exec > >(tee "$LOG") 2>&1
fail() { echo "FAILED: $*"; echo "STATUS=FAILED"; exit 1; }
REPO=qnx-vibguard

command -v git >/dev/null || fail "git missing (run: xcode-select --install)"
command -v gh  >/dev/null || brew install gh || fail "brew install gh"
if ! gh auth status >/dev/null 2>&1; then
  echo ">>> Sign in to GitHub: a code will appear; press Enter and finish in the browser."
  gh auth login --hostname github.com --git-protocol https --web || fail "gh auth login"
fi
gh auth status 2>&1 | head -3
USER_GH=$(gh api user --jq .login) || fail "gh api user"
echo "GitHub user: $USER_GH"

[ -d .git ] || git init -q -b main
git config user.name >/dev/null || git config user.name "Martin Vizcaino"
git add -A
echo "== Files to be committed (keys/, vm/, build/, logs/ are excluded):"
git status --short | head -80
if git status --short | grep -E "keys/|qnx_vm_key|vm/|license" ; then fail "sensitive file staged, stopping"; fi
git diff --cached --quiet || git commit -q -m "vibguard: real-time vibration guard for a SCARA robot on QNX 8.0

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Dbu7KapLUrRXVMpB9aFL7x" || fail commit

if gh repo view "$USER_GH/$REPO" >/dev/null 2>&1; then
  git remote get-url origin >/dev/null 2>&1 || git remote add origin "https://github.com/$USER_GH/$REPO.git"
  git push -u origin main || fail push
else
  gh repo create "$USER_GH/$REPO" --public --source . --remote origin --push \
    --description "Real-time vibration guard for a SCARA robot on QNX 8.0 (QNX Global Hackathon 2026)" || fail "gh repo create"
fi
echo "Repo: https://github.com/$USER_GH/$REPO"
echo "STATUS=OK"
