#!/usr/bin/env bash
# Trigger the mac release workflow.
# Version is read from VERSION (single source of truth) and must be based on
# the 7-Zip release in C/7zVersion.h.
# Creates the mac-v<version> tag and pushes it.
# Usage: .github/scripts/release-mac.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERSION="$(tr -d '[:space:]' < "${REPO_ROOT}/VERSION" 2>/dev/null || true)"
if [[ -z "$VERSION" ]]; then
    echo "ERROR: could not read VERSION" >&2
    exit 1
fi

UPSTREAM="$(sed -n 's/^#define MY_VERSION_NUMBERS "\(.*\)"$/\1/p' "${REPO_ROOT}/C/7zVersion.h")"
if [[ "${VERSION%%+*}" != "$UPSTREAM" ]]; then
    echo "ERROR: VERSION is '${VERSION}', 7-Zip in this tree is '${UPSTREAM}' — run bump.sh first" >&2
    exit 1
fi

TAG="mac-v${VERSION}"

if git -C "${REPO_ROOT}" rev-parse "${TAG}" &>/dev/null 2>&1; then
    echo "Tag ${TAG} already exists locally."
    printf "Delete and recreate? [y/N]: "
    read -r confirm
    if [[ "$confirm" != "y" ]]; then
        echo "Aborted."
        exit 1
    fi
    git -C "${REPO_ROOT}" tag -d "${TAG}"
    git -C "${REPO_ROOT}" push origin ":refs/tags/${TAG}" 2>/dev/null || true
fi

git -C "${REPO_ROOT}" tag "${TAG}"
git -C "${REPO_ROOT}" push origin "${TAG}"

echo ""
echo "Release triggered: ${TAG}"
echo "Watch: gh run list --workflow release-mac.yaml"
