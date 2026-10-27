import json,os,sys,time,urllib.request,urllib.error
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
seg=open('C:/Users/e-Yunfei.Qian/AppData/Local/Temp/seg0.txt',encoding='utf-8').read() if os.path.exists('C:/Users/e-Yunfei.Qian/AppData/Local/Temp/seg0.txt') else ''
if not seg:
    p='C:/Users/e-Yunfei.Qian/.claude/projects/D--project-gui/d6edc432-2baf-46df-8f9f-d209107c0f39.jsonl'
    ev=[]
    for line in open(p,encoding='utf-8',errors='replace'):
        try: d=json.loads(line)
        except: continue
        t=d.get('type'); c=(d.get('message') or {}).get('content')
        if t=='user' and isinstance(c,str) and len(c.strip())>10: ev.append(('user',c.strip()))
        elif isinstance(c,list):
            for b in c:
                if isinstance(b,dict) and b.get('type')=='text' and len(b.get('text','').strip())>10: ev.append((t,b['text'].strip()))
    cur,n=[],0
    for r,x in ev:
        pc=f"[{r}] {x}"
        if n+len(pc)>4000 and cur: break
        cur.append(pc); n+=len(pc)
    seg='\n'.join(cur)
    open('C:/Users/e-Yunfei.Qian/AppData/Local/Temp/seg0.txt','w',encoding='utf-8').write(seg)
s=json.load(open(os.path.expanduser('~/.claude/settings.json'),encoding='utf-8')); e=s['env']
base,tok=e['ANTHROPIC_BASE_URL'],e['ANTHROPIC_AUTH_TOKEN']
P="为开发者项目记忆库抽取记忆，输出严格 JSON：{\"observations\":[{\"kind\":\"decision|pitfall|convention|fact|procedure\",\"title\":\"≤20字\",\"narrative\":\"≤100字\",\"importance\":1-5,\"evidence\":\"逐字原文≤60字\"}]}。只写片段明确出现的内容；没有则 {\"observations\":[]}；仅输出 JSON。"
variants=[
 ('no_think_param',{}, ),
 ('thinking_disabled',{'thinking':{'type':'disabled'}}),
 ('no_think_prompt',{}),
]
for name,extra in variants:
    pr=P if name!='no_think_prompt' else '不要使用思考过程，直接输出 JSON。'+P
    body={"model":"qianfan/deepseek-v4.1-flash","max_tokens":4000,"messages":[{"role":"user","content":pr+"\n\n---片段---\n"+seg}]}
    body.update(extra)
    req=urllib.request.Request(base+'/v1/messages',data=json.dumps(body).encode(),headers={'content-type':'application/json','authorization':'Bearer '+tok,'anthropic-version':'2023-06-01'})
    t0=time.time()
    try:
        with urllib.request.urlopen(req,timeout=90) as r: d=json.loads(r.read().decode())
        text=''.join(b.get('text','') for b in d.get('content',[]) if b.get('type')=='text')
        u=d.get('usage',{})
        print(name, f'{time.time()-t0:.1f}s stop={d.get("stop_reason")} out={u.get("output_tokens")} thinking={(u.get("output_tokens_details") or {}).get("thinking_tokens")} text={len(text)}')
    except Exception as ex:
        print(name, f'{time.time()-t0:.1f}s ERR {type(ex).__name__}: {ex}')
