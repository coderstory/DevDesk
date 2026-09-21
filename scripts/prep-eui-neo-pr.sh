#!/usr/bin/env bash
# Prepare an EUI-NEO PR branch and commit.
#
# Workflow:
#   1. Verify EUI-NEO has changes (fontWeight runtime fix)
#   2. Optionally check / add user's fork as 'myfork' remote
#   3. Create a feature branch in EUI-NEO
#   4. Stage the changes (text.cpp / text.h / test / docs)
#   5. Commit with EUI-NEO's conventional format
#   6. Print next-step instructions for pushing to fork + opening PR
#
# Usage:
#   ./scripts/prep-eui-neo-pr.sh [<branch-name>] [<fork-url>]
#
# Examples:
#   ./scripts/prep-eui-neo-pr.sh
#   ./scripts/prep-eui-neo-pr.sh feat/font-weight-runtime
#   ./scripts/prep-eui-neo-pr.sh feat/font-weight-runtime https://github.com/coderstory/EUI-NEO.git
#
# Notes:
#   - Does NOT push (you'll review the commit locally first)
#   - Does NOT open the PR (GitHub web UI)
#   - Idempotent: if branch already exists, asks before recreating

set -euo pipefail

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVDESK_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
EUI_NEO_DIR="$(cd "${DEVDESK_DIR}/../EUI-NEO" && pwd)"
BRANCH_NAME="${1:-feat/font-weight-runtime}"
FORK_URL="${2:-}"

echo "==> DevDesk:    ${DEVDESK_DIR}"
echo "==> EUI-NEO:    ${EUI_NEO_DIR}"
echo "==> Branch:     ${BRANCH_NAME}"
echo

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if [[ ! -d "${EUI_NEO_DIR}/.git" ]]; then
    echo "ERROR: ${EUI_NEO_DIR} is not a git repo" >&2
    echo "  Run: git clone https://github.com/sudoevolve/EUI-NEO.git ${EUI_NEO_DIR}" >&2
    exit 1
fi

if [[ ! -f "${EUI_NEO_DIR}/core/render/text.cpp" ]]; then
    echo "ERROR: ${EUI_NEO_DIR}/core/render/text.cpp not found" >&2
    echo "  Wrong EUI-NEO directory?" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Check upstream sync
# ---------------------------------------------------------------------------
echo "==> Checking EUI-NEO branch state..."
cd "${EUI_NEO_DIR}"

CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [[ "${CURRENT_BRANCH}" != "main" ]]; then
    echo "WARNING: Currently on '${CURRENT_BRANCH}', not 'main'." >&2
    echo "  Switching to main first..." >&2
    git checkout main
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "ERROR: EUI-NEO working tree is dirty:" >&2
    git status --short >&2
    echo "  Commit or stash your changes first." >&2
    exit 1
fi

git fetch origin main --quiet 2>/dev/null || echo "  (skipped fetch — no network?)"

LOCAL="$(git rev-parse HEAD)"
REMOTE="$(git rev-parse origin/main 2>/dev/null || echo "${LOCAL}")"
if [[ "${LOCAL}" != "${REMOTE}" ]]; then
    echo "WARNING: local main (${LOCAL:0:7}) != origin/main (${REMOTE:0:7})" >&2
    echo "  Rebase before opening the PR:" >&2
    echo "    cd ${EUI_NEO_DIR} && git rebase origin/main" >&2
fi
echo

# ---------------------------------------------------------------------------
# Verify the fontWeight patch is present
# ---------------------------------------------------------------------------
echo "==> Verifying fontWeight patch..."
PATCH_MARKER="resolveSystemUiFontPathForWeightImpl"
if ! grep -q "${PATCH_MARKER}" "${EUI_NEO_DIR}/core/render/text.cpp"; then
    echo "ERROR: Font-weight patch not found in ${EUI_NEO_DIR}/core/render/text.cpp" >&2
    echo "  Did you forget to apply the patch? See docs/EUI-NEO-PR-PLAN.md §1.2." >&2
    exit 1
fi

if [[ ! -f "${EUI_NEO_DIR}/tests/unit/font_weight_resolution.cpp" ]]; then
    echo "ERROR: Unit test not found at ${EUI_NEO_DIR}/tests/unit/font_weight_resolution.cpp" >&2
    exit 1
fi
echo "  text.cpp:        patched"
echo "  text.h:          patched"
echo "  tests/unit:      font_weight_resolution.cpp present"
echo "  docs/DSL.md:     updated"
echo

# ---------------------------------------------------------------------------
# Handle fork remote
# ---------------------------------------------------------------------------
if [[ -n "${FORK_URL}" ]]; then
    if ! git remote get-url myfork >/dev/null 2>&1; then
        echo "==> Adding 'myfork' remote: ${FORK_URL}"
        git remote add myfork "${FORK_URL}"
        git fetch myfork --quiet 2>/dev/null || true
    else
        echo "==> 'myfork' remote already set: $(git remote get-url myfork)"
        if [[ "$(git remote get-url myfork)" != "${FORK_URL}" ]]; then
            echo "  Current URL differs from passed URL." >&2
            echo "  Update with: git remote set-url myfork ${FORK_URL}" >&2
        fi
    fi
else
    if ! git remote get-url myfork >/dev/null 2>&1; then
        echo "==> No 'myfork' remote configured."
        echo "  To push your branch, fork EUI-NEO first:"
        echo "    1. Open https://github.com/sudoevolve/EUI-NEO"
        echo "    2. Click 'Fork'"
        echo "    3. Re-run this script with your fork URL:"
        echo "       $0 ${BRANCH_NAME} https://github.com/<your-username>/EUI-NEO.git"
        echo
    else
        echo "==> 'myfork' remote: $(git remote get-url myfork)"
    fi
fi

# ---------------------------------------------------------------------------
# Create feature branch
# ---------------------------------------------------------------------------
echo "==> Creating branch '${BRANCH_NAME}'..."
if git show-ref --verify --quiet "refs/heads/${BRANCH_NAME}"; then
    echo "  Branch '${BRANCH_NAME}' already exists locally."
    read -r -p "  Recreate from main? [y/N] " RECREATE
    if [[ "${RECREATE}" =~ ^[Yy]$ ]]; then
        git branch -D "${BRANCH_NAME}"
        git checkout -b "${BRANCH_NAME}"
    else
        git checkout "${BRANCH_NAME}"
    fi
else
    git checkout -b "${BRANCH_NAME}"
fi

# ---------------------------------------------------------------------------
# Stage changes
# ---------------------------------------------------------------------------
echo "==> Staging changes..."
git add core/render/text.cpp
git add core/render/text.h
git add tests/unit/font_weight_resolution.cpp
git add docs/DSL.md

echo "  Staged files:"
git diff --cached --stat
echo

# ---------------------------------------------------------------------------
# Build verification (best effort)
# ---------------------------------------------------------------------------
echo "==> Verifying build (best effort)..."
if [[ -d "${EUI_NEO_DIR}/build" ]]; then
    if (cd "${EUI_NEO_DIR}/build" && cmake -DEUI_BUILD_TEST_FIXTURES=ON . >/dev/null 2>&1); then
        if (cd "${EUI_NEO_DIR}/build" && cmake --build . --target eui_neo font_weight_resolution -j 4 >/dev/null 2>&1); then
            if [[ -x "${EUI_NEO_DIR}/build/font_weight_resolution" ]]; then
                if "${EUI_NEO_DIR}/build/font_weight_resolution" >/dev/null 2>&1; then
                    echo "  Build + test OK"
                else
                    echo "  Build OK, but test failed. Inspect manually:" >&2
                    echo "    ${EUI_NEO_DIR}/build/font_weight_resolution" >&2
                fi
            else
                echo "  Build OK, test binary not produced (CONFIGURE_DEPENDS may need re-glob)" >&2
            fi
        else
            echo "  Build failed. Inspect manually:" >&2
            echo "    cd ${EUI_NEO_DIR}/build && cmake --build . --target eui_neo -j 4" >&2
        fi
    else
        echo "  cmake configure failed. Inspect manually." >&2
    fi
else
    echo "  No build directory; skipping."
    echo "  Tip: cmake -S ${EUI_NEO_DIR} -B ${EUI_NEO_DIR}/build -DEUI_BUILD_TEST_FIXTURES=ON"
fi

# ---------------------------------------------------------------------------
# Commit
# ---------------------------------------------------------------------------
COMMIT_MSG_TEMPLATE="feat(text): fontWeight() picks weight-specific platform UI font

Resolves the long-standing issue where ui.text(...).fontWeight(N) accepted
the value 100..900 but always rendered the same default face (Bold-only).

- macOS: SFNS-Bold / SFNS-Heavy / SFNS-Light → SFNS.ttf fallback
- Windows: segoeuib / segoeuiz / segoeuil → segoeui.ttf fallback
- Linux: NotoSans-Bold / NotoSans-Light / NotoSans-Regular / variable font

When the platform does not ship a weight-specific file, gracefully falls
back to the regular UI font (previous behavior for weight 400). Default
weight 400 is unchanged so existing apps are visually identical.

Added:
- core::TextPrimitive::resolveFontPath(family, weight) public static
- core::TextPrimitive::resolveSystemUiFontPathForWeight(weight) public static
- tests/unit/font_weight_resolution.cpp covering passthrough / aliases /
  system weight resolution / measureTextMetrics

Documented in docs/DSL.md.

Driven by DevDesk (https://github.com/coderstory/DevDesk) needing actual
font weight layers for heading/body/button hierarchy."

echo "==> Suggested commit message:"
echo "-----"
echo "${COMMIT_MSG_TEMPLATE}"
echo "-----"
echo

read -r -p "Commit with the above message? [Y/n] " DO_COMMIT
DO_COMMIT="${DO_COMMIT:-Y}"
if [[ "${DO_COMMIT}" =~ ^[Yy]$ ]]; then
    git commit -m "${COMMIT_MSG_TEMPLATE}"
    echo
    echo "==> Committed. Recent commits on ${BRANCH_NAME}:"
    git log --oneline -3
else
    echo "  Skipped commit. Run manually when ready:"
    echo "    cd ${EUI_NEO_DIR}"
    echo "    git commit -m 'feat(text): ...'"
fi

# ---------------------------------------------------------------------------
# Push instructions
# ---------------------------------------------------------------------------
echo
echo "============================================================"
echo "  NEXT STEPS"
echo "============================================================"
echo
echo "1. Review the commit:"
echo "     cd ${EUI_NEO_DIR}"
echo "     git show ${BRANCH_NAME}"
echo
echo "2. Push to your fork:"
if git remote get-url myfork >/dev/null 2>&1; then
    echo "     git push -u myfork ${BRANCH_NAME}"
else
    echo "     # First, fork EUI-NEO on GitHub:"
    echo "     #   https://github.com/sudoevolve/EUI-NEO  →  Fork"
    echo "     # Then add it as a remote:"
    echo "     git remote add myfork https://github.com/<your-username>/EUI-NEO.git"
    echo "     git push -u myfork ${BRANCH_NAME}"
fi
echo
echo "3. Open the PR:"
echo "     https://github.com/sudoevolve/EUI-NEO/compare/main...${BRANCH_NAME//\//%2F}?expand=1"
echo
echo "4. Paste the description from docs/PR-DESCRIPTION.md in the DevDesk repo."
echo
echo "5. After merge:"
echo "     - Update DevDesk docs/EUI-NEO-CAPABILITIES.md (remove the 'fontWeight 不切 face' row)"
echo "     - Update DevDesk CLAUDE.md EUI-NEO real capabilities table"
echo
