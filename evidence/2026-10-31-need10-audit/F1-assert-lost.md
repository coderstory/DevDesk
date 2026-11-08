# F-1 断言脚本重建说明

原始 /tmp/sa-test/assert_f1.py 因 /tmp 清理丢失。断言内容已由 assert_audit_fix.py
继承并扩展(P1/P2/P3 三项为独立审计修复的回归断言);F-1 原始五断言(中文命中/
混合词/空结果/幂等/注入安全)在 2026-10-31 03:2x 首轮跑绿,输出见
docs/plans/2026-10-31-session-assets-tests.md。
重跑法:SESSION_ASSETS_DB=<临时库> python3 tools/session_archive.py ingest < payload 后 search 断言。
