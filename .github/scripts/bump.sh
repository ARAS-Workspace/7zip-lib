#!/usr/bin/env bash
# Bump the fork version in VERSION.
# The scheme is <upstream-version>+<Consumer>.Patch.<n>: the upstream base is
# the 7-Zip release this tree carries (C/7zVersion.h), the consumer is the app
# the round serves, and the counter records how many patch rounds it took on
# top of that base. A bump keeps the consumer VERSION names; a new base or a
# new consumer is given explicitly.
# Rewrites VERSION and commits it as `Bump v<new>`, leaving only the push to
# the operator.
# Usage:
#   .github/scripts/bump.sh                                  …Patch.1 → …Patch.2
#   .github/scripts/bump.sh 26.06+PhantomArchiver.Patch.1    set explicitly (new
#                                                            base or consumer)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VERSION_FILE="${REPO_ROOT}/VERSION"
UPSTREAM_HEADER="${REPO_ROOT}/C/7zVersion.h"

# The commit below stages one explicit path, but a dirty tree still means
# the operator has work in flight that a version commit would sit on top of
# unreviewed.
if ! git -C "${REPO_ROOT}" diff --quiet || ! git -C "${REPO_ROOT}" diff --cached --quiet; then
    echo "ERROR: working tree has uncommitted changes — commit or stash them first" >&2
    git -C "${REPO_ROOT}" status --short >&2
    exit 1
fi

UPSTREAM="$(sed -n 's/^#define MY_VERSION_NUMBERS "\(.*\)"$/\1/p' "${UPSTREAM_HEADER}")"
if [[ -z "$UPSTREAM" ]]; then
    echo "ERROR: could not read MY_VERSION_NUMBERS from ${UPSTREAM_HEADER}" >&2
    exit 1
fi

if [[ ! -f "$VERSION_FILE" ]]; then
    echo "ERROR: ${VERSION_FILE} is missing" >&2
    exit 1
fi
CURRENT="$(tr -d '[:space:]' < "$VERSION_FILE")"
if [[ -z "$CURRENT" ]]; then
    echo "ERROR: ${VERSION_FILE} is empty" >&2
    exit 1
fi

if [[ $# -ge 1 ]]; then
    NEW="$1"
elif [[ "$CURRENT" =~ ^([^+]+)\+([A-Za-z]+)\.Patch\.([0-9]+)$ ]]; then
    NEW="${BASH_REMATCH[1]}+${BASH_REMATCH[2]}.Patch.$(( BASH_REMATCH[3] + 1 ))"
else
    echo "ERROR: VERSION (${CURRENT}) names no consumer; give the version explicitly" >&2
    exit 1
fi
if [[ ! "$NEW" =~ ^[^+]+\+([A-Za-z]+)\.Patch\.[0-9]+$ ]]; then
    echo "ERROR: '${NEW}' is not <7-Zip release>+<Consumer>.Patch.<n>" >&2
    exit 1
fi
CONSUMER="${BASH_REMATCH[1]}"

# A library built from one 7-Zip release must not carry another's number:
# after taking an upstream release the base moves, and only an explicit
# version may move it.
if [[ "${NEW%%+*}" != "$UPSTREAM" ]]; then
    echo "ERROR: '${NEW}' is not based on the 7-Zip release in this tree (${UPSTREAM})" >&2
    echo "       run: .github/scripts/bump.sh ${UPSTREAM}+${CONSUMER}.Patch.1" >&2
    exit 1
fi

if [[ "$NEW" == "$CURRENT" ]]; then
    echo "ERROR: new version equals current version (${CURRENT})" >&2
    exit 1
fi

printf '%s\n' "$NEW" > "$VERSION_FILE"
git -C "${REPO_ROOT}" commit --quiet --only -m "Bump v${NEW}" -- "$VERSION_FILE"

echo "${CURRENT} → ${NEW}  (7-Zip ${UPSTREAM})"
echo
git -C "${REPO_ROOT}" log -1 --format="committed %h %s"
