import json,os,sys,math,re,urllib.request
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
obs=json.load(open('C:/Users/e-Yunfei.Qian/AppData/Local/Temp/mem_obs_batch.json',encoding='utf-8'))
seen=set(); items=[]
for o in obs:
    t=(o.get('title') or '').strip()
    if t and t not in seen: seen.add(t); items.append(o)
from fastembed import TextEmbedding
m=TextEmbedding('BAAI/bge-small-zh-v1.5')
docs=[(o['title'] or '')+'。'+(o.get('narrative') or '') for o in items]
dv=[list(x) for x in m.embed(docs)]
def cos(a,b): return sum(x*y for x,y in zip(a,b))/(math.sqrt(sum(x*x for x in a))*math.sqrt(sum(x*x for x in b))+1e-9)
s=json.load(open(os.path.expanduser('~/.claude/settings.json'),encoding='utf-8')); e=s['env']
base,tok=e['ANTHROPIC_BASE_URL'],e['ANTHROPIC_AUTH_TOKEN']
def ask(prompt):
    body={"model":"qianfan/deepseek-v4.1-flash","max_tokens":600,"thinking":{"type":"disabled"},"messages":[{"role":"user","content":prompt}]}
    req=urllib.request.Request(base+'/v1/messages',data=json.dumps(body).encode(),headers={'content-type':'application/json','authorization':'Bearer '+tok,'anthropic-version':'2023-06-01'})
    with urllib.request.urlopen(req,timeout=90) as r: d=json.loads(r.read().decode())
    return ''.join(b.get('text','') for b in d.get('content',[]) if b.get('type')=='text').strip()
qs=["这个P1阶段的任务怎么用子任务驱动的方式接手推进？",
    "这个功能默认关着，用户想用时去哪里打开和改端口？",
    "这个服务骨架要同时跑在Windows和Mac上，怎么设计？",
    "主会话能不能自己写代码？",
    "MCP 这块打算怎么开发，放哪里？"]
for q in qs:
    qv=list(m.embed([q]))[0]; sc=[(cos(qv,x),i) for i,x in enumerate(dv)]; sc.sort(reverse=True)
    top=[items[i] for _,i in sc[:3]]
    mem="\n".join(f"- {o['title']}：{o.get('narrative','')}" for o in top)
    base_p=f"你是这个项目的工程师。用户问：{q}\n只回答你第一步要做什么，30字以内，不要解释。"
    a0=ask(base_p)
    a1=ask(base_p+"\n\n【已知项目记忆】\n"+mem)
    print("Q:",q)
    print("  无记忆:",a0)
    print("  有记忆:",a1)
    for o in top: print("    ·",o['title'])
    print()
