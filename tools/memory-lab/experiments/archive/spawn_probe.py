import subprocess,time,sys,os
sys.stdout.reconfigure(encoding='utf-8')
py=r'D:/software/venvs/memory-lab/Scripts/python.exe'
exe=r'C:/Users/e-Yunfei.Qian/AppData/Local/Temp/spawn.exe'
def run(cmd):
    t=time.perf_counter(); p=subprocess.run(cmd,capture_output=True,timeout=60); dt=(time.perf_counter()-t)*1000
    return dt, p.returncode, (p.stdout or b'').decode('utf-8','replace')[:80].strip(), (p.stderr or b'').decode('utf-8','replace')[:80].strip()
print('spawn.exe exists', os.path.exists(exe))
for i in range(3):
    print('python -c pass', ['%.1fms'%run([py,'-I','-c','pass'])[0]][0])
for i in range(3):
    dt,rc,out,err=run([exe])
    print('spawn.exe %.1fms rc=%s out=%r err=%r'%(dt,rc,out,err))
