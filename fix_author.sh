#!/bin/bash
# Re-attribute the commit to your GitHub account (private noreply email) and update the repo.
set -u
cd "$HOME/Downloads/JobAgent/qnx" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
EMAIL=$(gh api user --jq '"\(.id)+\(.login)@users.noreply.github.com"') || { echo "FAILED: gh api"; exit 1; }
gh auth setup-git || { echo "FAILED: gh auth setup-git"; exit 1; }
git config user.name "Martin Vizcaino"
git config user.email "$EMAIL"
echo "author email: $EMAIL"
git commit --amend --reset-author --no-edit -q && git push --force-with-lease -q origin main && git log -1 --format='%an <%ae>' && echo STATUS=OK
