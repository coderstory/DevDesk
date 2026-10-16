#!/usr/bin/env bash
# GitHub Sync 真机验收 —— **远端夹具**脚本。
#
# 职责：用 GitHub Contents / Git refs / Repos API 准备与清理远端状态，供
# tests/live/github_sync_live.cpp 的每个场景在已知起点上开跑。
#   - 主仓（可写、非破坏性场景）：coderstory/devdesk-sync-test
#   - 一次性仓（破坏性场景：删仓 / 垃圾库）：coderstory/devdesk-sync-test-destroy
#
# 凭据红线（与 tools/quota_live_probe.cpp 同级）：
#   token 只进 Authorization 头，**绝不** echo / 进日志 / 进 argv；唯一落盘是
#   0600 的临时头文件（curl -H @文件 读它，EXIT trap 即删）——直接
#   -H "Authorization: Bearer …" 会把 token 摆进 curl 的 argv，全机 ps 可见。
#   本脚本不 `set -x`。来源：$DEVDESK_GITHUB_TOKEN，缺失则回落到 0600 的
#   /tmp/devdesk-gh-test.pat。
#
# 用法：
#   bash scripts/ghsync-live-fixture.sh reset-main
#   bash scripts/ghsync-live-fixture.sh reset-destroy
#   bash scripts/ghsync-live-fixture.sh set-note   <name> <contentFile>
#   bash scripts/ghsync-live-fixture.sh del-note   <name>
#   bash scripts/ghsync-live-fixture.sh set-db     <contentFile>
#   bash scripts/ghsync-live-fixture.sh del-db
#   bash scripts/ghsync-live-fixture.sh ensure-branch <branch> [from]
#   bash scripts/ghsync-live-fixture.sh del-branch    <branch>
#   bash scripts/ghsync-live-fixture.sh get-sha    <path>          # 打印 blob sha（无 → 空行）
#   bash scripts/ghsync-live-fixture.sh cat        <path>          # 打印远端文件内容
#   bash scripts/ghsync-live-fixture.sh repo-info                   # 打印 JSON 状态
#   bash scripts/ghsync-live-fixture.sh ensure-destroy
#   bash scripts/ghsync-live-fixture.sh delete-destroy
set -euo pipefail

API="https://api.github.com"
REPO="${DEVDESK_GH_REPO:-coderstory/devdesk-sync-test}"
REPO_DESTROY="${DEVDESK_GH_REPO_DESTROY:-coderstory/devdesk-sync-test-destroy}"
BRANCH="${DEVDESK_GH_BRANCH:-main}"

TOKEN="${DEVDESK_GITHUB_TOKEN:-}"
if [ -z "${TOKEN}" ] && [ -r /tmp/devdesk-gh-test.pat ]; then
    TOKEN="$(cat /tmp/devdesk-gh-test.pat)"
fi
if [ -z "${TOKEN}" ]; then
    echo "❌ 无 token：设 DEVDESK_GITHUB_TOKEN 或提供 /tmp/devdesk-gh-test.pat" >&2
    exit 2
fi

# ── HTTP 原语 ──
# 每次调用把响应体写 TMPBODY、状态码写 CODE；调用方读这两个全局。
# 403（次级限流）/ 429 / 5xx 重试：真机脚本在几秒内连发多次 contents 变更，
# GitHub 的 secondary rate limit 会间歇性拒绝 —— 不重试会把瞬态当夹具缺陷。
TMPBODY="$(mktemp -t ghsync-body.XXXXXX)"
# token 经 -H @文件 传给 curl（文件内容即头行）：-H "Authorization: Bearer …" 会把
# token 摆进 curl 的 argv，全机 ps 可见 —— 与脚本头「绝不进 argv」的自述矛盾。
TMPHDR="$(mktemp -t ghsync-hdr.XXXXXX)"
chmod 600 "${TMPHDR}"
trap 'rm -f "${TMPBODY}" "${TMPHDR}"' EXIT
printf 'Authorization: Bearer %s\n' "${TOKEN}" > "${TMPHDR}"
CODE=""

req() {  # req <method> <path> [jsonFile]
    local method="$1" path="$2" data="${3:-}"
    local attempt try_code
    for attempt in 1 2 3 4; do
        local args=(-s -o "${TMPBODY}" -w '%{http_code}' -X "${method}"
                    -H "@${TMPHDR}"
                    -H "Accept: application/vnd.github+json"
                    -H "X-GitHub-Api-Version: 2022-11-28")
        if [ -n "${data}" ]; then
            args+=(-H "Content-Type: application/json" --data-binary "@${data}")
        fi
        try_code="$(curl "${args[@]}" "${API}${path}")"
        case "${try_code}" in
            403|429|5*) sleep $((attempt * 3)) ;;   # 次级限流退避：3/6/9s，到点再试
            *) CODE="${try_code}"; return 0 ;;
        esac
    done
    CODE="${try_code}"
}

json_get() {  # json_get <pythonExprOnD> ; 读 TMPBODY
    python3 -c "import json,sys
try:
    d=json.load(open('${TMPBODY}'))
except Exception:
    print(''); sys.exit(0)
print($1)" 2>/dev/null || true
}

b64() { base64 < "$1" | tr -d '\n\r'; }

# 写一个文件到远端分支（存在则带 sha 覆盖）。
put_path() {  # put_path <repo> <path> <contentFile> <message> [branch]
    local repo="$1" path="$2" file="$3" msg="$4" br="${5:-${BRANCH}}"
    req GET "/repos/${repo}/contents/${path}?ref=${br}"
    local sha=""
    if [ "${CODE}" = "200" ]; then
        sha="$(json_get "d.get('sha','')")"
    fi
    local payload="${TMPBODY}.payload"
    {
        printf '{"message":%s,"branch":"%s","content":"%s"' \
            "$(python3 -c "import json,sys;print(json.dumps(sys.argv[1]))" "${msg}")" \
            "${br}" "$(b64 "${file}")"
        if [ -n "${sha}" ]; then printf ',"sha":"%s"' "${sha}"; fi
        printf '}'
    } > "${payload}"
    req PUT "/repos/${repo}/contents/${path}" "${payload}"
    rm -f "${payload}"
    if [ "${CODE}" != "200" ] && [ "${CODE}" != "201" ]; then
        echo "❌ PUT ${repo}/${path} 失败 HTTP ${CODE}: $(head -c 300 "${TMPBODY}")" >&2
        return 1
    fi
}

delete_path() {  # delete_path <repo> <path> <branch>
    local repo="$1" path="$2" br="${3:-${BRANCH}}"
    req GET "/repos/${repo}/contents/${path}?ref=${br}"
    [ "${CODE}" = "200" ] || return 0   # 不存在即无事
    local sha; sha="$(json_get "d.get('sha','')")"
    local payload="${TMPBODY}.payload"
    printf '{"message":"fixture: delete %s","branch":"%s","sha":"%s"}' \
        "${path}" "${br}" "${sha}" > "${payload}"
    req DELETE "/repos/${repo}/contents/${path}" "${payload}"
    rm -f "${payload}"
    if [ "${CODE}" != "200" ]; then
        echo "❌ DELETE ${repo}/${path} 失败 HTTP ${CODE}" >&2
        return 1
    fi
}

# 清空一个分支上的 notes/ 与 data/（保留 README.md，保证分支有提交）。
wipe_tree() {  # wipe_tree <repo> <branch>
    local repo="$1" br="$2"
    req GET "/repos/${repo}/git/trees/${br}?recursive=1"
    if [ "${CODE}" != "200" ]; then
        echo "❌ 读树失败 ${repo}@${br} HTTP ${CODE}" >&2
        return 1
    fi
    local paths
    paths="$(json_get "chr(10).join(e['path'] for e in d.get('tree',[]) if e.get('type')=='blob' and (e['path'].startswith('notes/') or e['path'].startswith('data/')))")"
    while IFS= read -r p; do
        [ -n "${p}" ] || continue
        delete_path "${repo}" "${p}" "${br}"
    done <<< "${paths}"
    # README.md 兜底：仓库若被清空到零提交，分支会消失 → 后续 404 与夹具缺陷混淆。
    req GET "/repos/${repo}/contents/README.md?ref=${br}"
    if [ "${CODE}" = "404" ]; then
        local tmp; tmp="$(mktemp -t ghsync-readme.XXXXXX)"
        printf '# devdesk-sync-test\n\nGitHub Sync 真机验收夹具仓（自动生成，勿手工改）。\n' > "${tmp}"
        put_path "${repo}" "README.md" "${tmp}" "fixture: init README" "${br}"
        rm -f "${tmp}"
    fi
}

ensure_destroy_repo() {
    req GET "/repos/${REPO_DESTROY}"
    if [ "${CODE}" = "200" ]; then return 0; fi
    local payload="${TMPBODY}.payload"
    printf '{"name":"%s","private":true,"auto_init":true,"description":"DevDesk GitHub Sync 破坏性场景一次性夹具仓（可随时删除）"}' \
        "$(basename "${REPO_DESTROY}")" > "${payload}"
    req POST "/user/repos" "${payload}"
    rm -f "${payload}"
    if [ "${CODE}" != "201" ]; then
        echo "❌ 创建 ${REPO_DESTROY} 失败 HTTP ${CODE}: $(head -c 300 "${TMPBODY}")" >&2
        return 1
    fi
}

cmd="${1:-}"; shift || true
case "${cmd}" in
    reset-main)
        wipe_tree "${REPO}" "${BRANCH}"
        echo "OK reset-main ${REPO}@${BRANCH}"
        ;;
    reset-destroy)
        ensure_destroy_repo
        wipe_tree "${REPO_DESTROY}" "${BRANCH}"
        echo "OK reset-destroy ${REPO_DESTROY}@${BRANCH}"
        ;;
    ensure-destroy)
        ensure_destroy_repo
        echo "OK ensure-destroy ${REPO_DESTROY}"
        ;;
    delete-destroy)
        req DELETE "/repos/${REPO_DESTROY}"
        if [ "${CODE}" != "204" ] && [ "${CODE}" != "404" ]; then
            echo "❌ 删除 ${REPO_DESTROY} 失败 HTTP ${CODE}" >&2
            exit 1
        fi
        echo "OK delete-destroy ${REPO_DESTROY}"
        ;;
    set-note)
        put_path "${REPO}" "notes/$1" "$2" "fixture: set note $1"
        echo "OK set-note $1"
        ;;
    del-note)
        delete_path "${REPO}" "notes/$1"
        echo "OK del-note $1"
        ;;
    set-db)
        put_path "${REPO}" "data/database.sql" "$1" "fixture: set database.sql"
        echo "OK set-db"
        ;;
    del-db)
        delete_path "${REPO}" "data/database.sql"
        echo "OK del-db"
        ;;
    ensure-branch)
        br="$1"; from="${2:-${BRANCH}}"
        req GET "/repos/${REPO}/git/ref/heads/${from}"
        if [ "${CODE}" != "200" ]; then
            echo "❌ 基点分支 ${from} 不存在 HTTP ${CODE}" >&2; exit 1
        fi
        sha="$(json_get "d['object']['sha']")"
        payload="${TMPBODY}.payload"
        printf '{"ref":"refs/heads/%s","sha":"%s"}' "${br}" "${sha}" > "${payload}"
        req POST "/repos/${REPO}/git/refs" "${payload}"
        rm -f "${payload}"
        if [ "${CODE}" = "422" ]; then echo "OK ensure-branch ${br}（已存在）"; exit 0; fi
        if [ "${CODE}" != "201" ]; then
            echo "❌ 建分支 ${br} 失败 HTTP ${CODE}" >&2; exit 1
        fi
        echo "OK ensure-branch ${br}"
        ;;
    del-branch)
        br="$1"
        req DELETE "/repos/${REPO}/git/refs/heads/${br}"
        if [ "${CODE}" != "204" ] && [ "${CODE}" != "404" ] && [ "${CODE}" != "422" ]; then
            echo "❌ 删分支 ${br} 失败 HTTP ${CODE}" >&2; exit 1
        fi
        echo "OK del-branch ${br}"
        ;;
    get-sha)
        req GET "/repos/${REPO}/contents/$1?ref=${BRANCH}"
        if [ "${CODE}" = "200" ]; then json_get "d.get('sha','')"; else echo ""; fi
        ;;
    cat)
        req GET "/repos/${REPO}/contents/$1?ref=${BRANCH}"
        if [ "${CODE}" = "200" ]; then
            python3 -c "import json,base64,sys
d=json.load(open('${TMPBODY}'))
sys.stdout.write(base64.b64decode(d['content']).decode('utf-8','replace'))"
        else
            echo "❌ cat $1 HTTP ${CODE}" >&2; exit 1
        fi
        ;;
    repo-info)
        req GET "/repos/${REPO}"
        if [ "${CODE}" = "200" ]; then cat "${TMPBODY}"; else echo "HTTP ${CODE}"; fi
        ;;
    *)
        echo "用法见脚本头注释" >&2; exit 2
        ;;
esac
