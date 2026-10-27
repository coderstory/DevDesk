import json, os, sys, time, glob, urllib.request
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
root=r'C:/Users/e-Yunfei.Qian/.claude/projects/D--project-gui'
files=[p for p in glob.glob(root+'/*.jsonl') if 'b265becd' not in p]  # 排除当前会话
files=sorted(files,key=os.path.getmtime,reverse=True)[:2]
s=json.load(open(os.path.expanduser('~/.claude/settings.json'),encoding='utf-8')); e=s['env']
base,tok=e['ANTHROPIC_BASE_URL'],e['ANTHROPIC_AUTH_TOKEN']
P="""为开发者项目记忆库抽取记忆，输出严格 JSON：
{"observations":[{"kind":"decision|pitfall|convention|fact|procedure","title":"≤20字","narrative":"≤100字","facts":["..."],"concepts":["..."],"files":["..."],"importance":1-5,"evidence":"逐字原文≤60字"}]}
只写片段明确出现的内容，禁止推测；没有则 {"observations":[]}；仅输出 JSON，不要解释。"""
def segs_of(p, cap=15):
    ev=[]
    for line in open(p,encoding='utf-8',errors='replace'):
        try: d=json.loads(line)
        except: continue
        t=d.get('type'); c=(d.get('message') or {}).get('content')
        if t=='user' and isinstance(c,str) and len(c.strip())>10: ev.append(('user',c.strip()))
        elif isinstance(c,list):
            for b in c:
                if not isinstance(b,dict): continue
                if b.get('type')=='text' and len(b.get('text','').strip())>10: ev.append((t,b['text'].strip()))
                elif t=='assistant' and b.get('type')=='tool_use': ev.append(('tool_use',f"{b.get('name')} {json.dumps(b.get('input'),ensure_ascii=False)[:300]}"))
                elif t=='user' and b.get('type')=='tool_result':
                    x=b.get('content'); x=x if isinstance(x,str) else json.dumps(x,ensure_ascii=False)
                    if x and len(x)>100: ev.append(('tool_result',x[:300]))
    out,cur,n=[],[],0
    for r,x in ev:
        pc=f"[{r}] {x}"
        if n+len(pc)>4000 and cur: out.append('\n'.join(cur)); cur,n=[],0
        if len(out)>=cap: return out
        cur.append(pc); n+=len(pc)
    if cur: out.append('\n'.join(cur))
    return out
def ask(seg):
    body={"model":"qianfan/deepseek-v4.1-flash","max_tokens":4000,"messages":[{"role":"user","content":P+"\n\n---片段---\n"+seg}]}
    req=urllib.request.Request(base+'/v1/messages',data=json.dumps(body).encode(),headers={'content-type':'application/json','authorization':'Bearer '+tok,'anthropic-version':'2023-06-01'})
    t0=time.time()
    with urllib.request.urlopen(req,timeout=180) as r: d=json.loads(r.read().decode())
    text=''.join(b.get('text','') for b in d.get('content',[]) if b.get('type')=='text')
    return time.time()-t0,d.get('usage',{}),text
allobs=[]; stats=[]
for p in files:
    ss=segs_of(p); ok=0; fail=0; t0=time.time()
    for i,seg in enumerate(ss):
        for attempt in range(2):
            try:
                dt,u,text=ask(seg)
                j=json.loads(text.strip().removeprefix('```json').removesuffix('```').strip())
                for o in j.get('observations',[]):
                    o['_session']=os.path.basename(p)[:8]; o['_seg']=i
                    allobs.append(o)
                ok+=1; break
            except Exception as ex:
                if attempt==1: fail+=1
    stats.append((os.path.basename(p)[:8],len(ss),ok,fail,round(time.time()-t0,1)))
    print(dict(session=os.path.basename(p)[:8],segments=len(ss),ok=ok,fail=fail,secs=round(time.time()-t0,1)))
json.dump(allobs,open('C:/Users/e-Yunfei.Qian/AppData/Local/Temp/mem_obs_batch.json','w',encoding='utf-8'),ensure_ascii=False,indent=1)
from collections import Counter
print('TOTAL obs',len(allobs),'kinds',dict(Counter(o.get('kind') for o in allobs)))
for o in allobs[:12]: print(' -',o.get('kind'),'|',o.get('title'),'| imp',o.get('importance'))
