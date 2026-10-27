import json, os, sys, math, re, urllib.request, time
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
obs=json.load(open('C:/Users/e-Yunfei.Qian/AppData/Local/Temp/mem_obs_batch.json',encoding='utf-8'))
# 去重标题
seen=set(); items=[]
for o in obs:
    t=(o.get('title') or '').strip()
    if not t or t in seen: continue
    seen.add(t); items.append(o)
print('items',len(items))

s=json.load(open(os.path.expanduser('~/.claude/settings.json'),encoding='utf-8')); e=s['env']
base,tok=e['ANTHROPIC_BASE_URL'],e['ANTHROPIC_AUTH_TOKEN']
# 1) LLM 生成查询（不照抄标题）
sample=items[:15]
lst=[{'i':i,'title':o['title'],'narrative':o.get('narrative','')} for i,o in enumerate(sample)]
Q="""下面每条是从开发者历史会话中抽取的记忆。请为每条写 1 个真实的开发者提问（中文，15-30 字），
要求：不知道这条记忆的人也会这样问，但该记忆正是答案；禁止照抄标题里的词；输出 JSON 数组 [{"i":0,"q":"..."}]"""
body={"model":"qianfan/deepseek-v4.1-flash","max_tokens":4000,"thinking":{"type":"disabled"},
      "messages":[{"role":"user","content":Q+"\n\n"+json.dumps(lst,ensure_ascii=False)}]}
req=urllib.request.Request(base+'/v1/messages',data=json.dumps(body).encode(),
    headers={'content-type':'application/json','authorization':'Bearer '+tok,'anthropic-version':'2023-06-01'})
with urllib.request.urlopen(req,timeout=120) as r: d=json.loads(r.read().decode())
text=''.join(b.get('text','') for b in d.get('content',[]) if b.get('type')=='text')
qs=json.loads(text.strip().removeprefix('```json').removesuffix('```').strip())
print('queries',len(qs))

# 2) 本地嵌入
from fastembed import TextEmbedding
m=TextEmbedding('BAAI/bge-small-zh-v1.5')
docs=[(o['title'] or '')+'。'+(o.get('narrative') or '') for o in items]
dv=[list(x) for x in m.embed(docs)]
qv=[list(x) for x in m.embed([x['q'] for x in qs])]
def cos(a,b): return sum(x*y for x,y in zip(a,b))/(math.sqrt(sum(x*x for x in a))*math.sqrt(sum(x*x for x in b))+1e-9)

# 3) BM25（字符 bigram 近似中文分词）
def toks(t): 
    t=re.sub(r'\s+',' ',t.lower())
    return [t[i:i+2] for i in range(len(t)-1)] + re.findall(r'[a-z0-9_.\-]+',t)
dt=[toks(x) for x in docs]; N=len(dt); avg=sum(len(x) for x in dt)/max(1,N)
df={}
for t in dt:
    for w in set(t): df[w]=df.get(w,0)+1
def bm25(q,k1=1.2,b=0.75):
    qt=toks(q); sc=[]
    for i,t in enumerate(dt):
        s=0.0
        for w in qt:
            if w not in df: continue
            f=t.count(w); idf=math.log(1+(N-df[w]+0.5)/(df[w]+0.5))
            s+=idf*f*(k1+1)/(f+k1*(1-b+b*len(t)/avg))
        sc.append(s)
    return sc
def ranks(scores):
    return sorted(range(len(scores)), key=lambda i:-scores[i])
rec={'bm25':[], 'vec':[], 'hybrid':[]}; rr={'bm25':[], 'vec':[], 'hybrid':[]}
for qi,item in enumerate(qs):
    ti=item['i']
    b=bm25(item['q']); v=[cos(qv[qi],x) for x in dv]
    rb=ranks(b); rv=ranks(v)
    rr_hy={}
    for name,r in (('bm',rb),('ve',rv)):
        for pos,idx in enumerate(r[:60]): rr_hy[idx]=rr_hy.get(idx,0)+1/(60+pos)
    rh=ranks([rr_hy.get(i,0) for i in range(N)])
    for mode,r in (('bm25',rb),('vec',rv),('hybrid',rh)):
        pos=r.index(ti)
        rec[mode].append(1 if pos<5 else 0); rr[mode].append(1/(pos+1))
print('\n== Recall@5 / MRR (n=%d queries, %d items) =='%(len(qs),N))
for k in rec: print(f'{k:7s} R@5={sum(rec[k])/len(rec[k]):.2f}  MRR={sum(rr[k])/len(rr[k]):.2f}')
print('\n样例查询:')
for item in qs[:6]: print(' -',item['q'],'=>',items[item['i']]['title'])
