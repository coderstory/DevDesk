import sqlite3,time,sys,statistics,random
sys.stdout.reconfigure(encoding='utf-8')
con=sqlite3.connect(':memory:')
con.executescript("""
CREATE TABLE observations(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, subtitle TEXT, narrative TEXT, facts TEXT, concepts TEXT, files TEXT);
CREATE VIRTUAL TABLE fts USING fts5(title,subtitle,narrative,facts,concepts,files,content='observations',content_rowid='id',tokenize='trigram');
CREATE TRIGGER ai AFTER INSERT ON observations BEGIN
 INSERT INTO fts(rowid,title,subtitle,narrative,facts,concepts,files) VALUES(new.id,new.title,coalesce(new.subtitle,''),coalesce(new.narrative,''),coalesce(new.facts,''),coalesce(new.concepts,''),coalesce(new.files,''));
END;
""")
t0=time.time()
con.executemany('INSERT INTO observations(title,subtitle,narrative,facts,concepts,files) VALUES(?,?,?,?,?,?)',
 [(f'记忆条目{i}','MCP 服务骨架','Windows 与 macOS 跨平台，设置端口，主会话只编排，子任务驱动，检索混合打分','事实 事实 事实','MCP,跨平台','a.cpp') for i in range(10000)])
con.commit(); print('insert 10000 real schema: %.2fs'%(time.time()-t0))
def q(s):
    t=time.perf_counter(); n=len(con.execute('SELECT rowid FROM fts WHERE fts MATCH ? LIMIT 5',(s,)).fetchall()); return n,(time.perf_counter()-t)*1000
for s in ['服务骨架','端口','MCP','Windows 与 macOS','主会话','检索混合打分','跨平台']:
    n,ms=q(s); print(f'query {s!r}: rows={n} {ms:.2f}ms')
# 2-char Chinese appears inside a longer doc; test both MATCH and LIKE
for s in ['"服务"','"端口"','"骨架"']:
    n,ms=q(s); print('quoted',s,n,'%.2fms'%ms)
