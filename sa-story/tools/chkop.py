# Проверка команд сценария в коде: номер опкода и число параметров по Sanny Builder sa_mobile.json.
# Запуск: python3 chkop.py sa_mobile.json sa-story/src/*.cpp third_party/swat/*.cpp sa-living/src/squad/*.cpp
import json, re, sys, glob
d=json.load(open(sys.argv[1]))
byid={}
for e in d['extensions']:
    for c in e['commands']:
        byid[c['id'].upper()]=c
pat=re.compile(r'(?:Do|Is|OutI|Out3|Check|OutInt|Run|Cmd|CmdOut|Q|D)\(\s*0x([0-9A-Fa-f]{4})\s*,\s*')
bad=0; total=0
for f in sys.argv[2:]:
    src=open(f,encoding='utf-8').read()
    for m in pat.finditer(src):
        op=m.group(1).upper(); i=m.end()
        if src[i]!='{': continue   # vector variable
        depth=0; j=i; args=0; nonempty=False
        while True:
            ch=src[j]
            if ch in '{([': depth+=1
            elif ch in '})]':
                depth-=1
                if depth==0: break
            elif ch==',' and depth==1: args+=1
            elif depth>=1 and not ch.isspace(): nonempty=True
            j+=1
        n=args+1 if nonempty else 0
        c=byid.get(op); total+=1
        if not c: print(f'{f}: {op} нет в справочнике'); bad+=1; continue
        need=c.get('num_params',0)
        if n!=need:
            line=src[:m.start()].count('\n')+1
            print(f'{f}:{line}: {op} {c["name"]}: аргументов {n}, нужно {need}'); bad+=1
print('проверено',total,'ошибок',bad)
