import json, subprocess
A = '/Users/coderstory/dev/DevDesk/tools/session_archive.py'
env = {'SESSION_ASSETS_DB': '/tmp/sa-test/sa.db', 'PATH': '/usr/bin:/bin'}

# P1:非 dict 行不再炸穿 exit-0 契约
payload = '{"session_id":"audit-fix-001","transcript_path":"/tmp/sa-test/sample.jsonl","reason":"other"}'
r = subprocess.run(['python3', A, 'ingest'], input=payload, capture_output=True, text=True, env=env)
assert r.returncode == 0 and 'Traceback' not in r.stderr, f'P1 修复失败: {r.stderr[-300:]}'
d = json.loads(subprocess.run(['python3', A, 'search', '软删'], capture_output=True, text=True, env=env).stdout)
assert d['hits'] == 1, f'正常行仍应入库: {d}'
print('P1 PASS: 非 dict 行跳过, 正常行入库, exit 0 无 traceback')

# P2:DB 不可写时 ingest 仍 exit 0(整体兜底)
env2 = {'SESSION_ASSETS_DB': '/proc/no-such-dir/x.db', 'PATH': '/usr/bin:/bin'}
r = subprocess.run(['python3', A, 'ingest'], input=payload, capture_output=True, text=True, env=env2)
assert r.returncode == 0 and 'Traceback' not in r.stderr, f'P2 兜底失败: {r.stderr[-300:]}'
print('P2 PASS: DB 不可写 → 静默 exit 0')

# P3:缺值/非法 --limit 不再 traceback
for args in (['search', '词', '--project'], ['search', '词', '--limit', 'abc']):
    r = subprocess.run(['python3', A] + args, capture_output=True, text=True, env=env)
    assert r.returncode == 2 and 'Traceback' not in r.stderr, f'P3 失败: {args} {r.stderr[-200:]}'
print('P3 PASS: 参数缺值/非法值 → 用法错误 exit 2, 无 traceback')

# sync 幂等未破坏
r = subprocess.run(['python3', A, 'sync'], capture_output=True, text=True, env=env)
assert json.loads(r.stdout)['added'] == 0
print('sync 幂等保持 PASS')
