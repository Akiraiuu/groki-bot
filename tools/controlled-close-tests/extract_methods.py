"""Extract named production C methods for host boundary tests; no network."""
from pathlib import Path
import argparse,re,hashlib,json
parser=argparse.ArgumentParser();parser.add_argument('source',type=Path);parser.add_argument('output',type=Path);parser.add_argument('names',nargs='+');a=parser.parse_args()
s=a.source.read_text(encoding='utf-8');methods=[]
for name in a.names:
    # Require the definition brace immediately after the complete signature;
    # a forward declaration must never consume later globals or methods.
    m=re.search(r'^(?:static\s+)?(?:void|int|bool|esp_err_t)\s+'+re.escape(name)+r'\([^;{}]*\)\s*\{',s,re.M)
    if not m:raise ValueError('missing method '+name)
    i=s.index('{',m.start());depth=0;mode='code';start=m.start()
    while i<len(s):
        c=s[i];pair=s[i:i+2]
        if mode=='line':
            if c=='\n':mode='code'
        elif mode=='block':
            if pair=='*/':mode='code';i+=1
        elif mode in ('str','char'):
            if c=='\\':i+=1
            elif c==('"' if mode=='str' else "'"):mode='code'
        elif pair=='//':mode='line';i+=1
        elif pair=='/*':mode='block';i+=1
        elif c=='"':mode='str'
        elif c=="'":mode='char'
        elif c=='{':depth+=1
        elif c=='}':
            depth-=1
            if depth==0:methods.append(s[start:i+1]);break
        i+=1
    else:raise ValueError('unterminated '+name)
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text('\n\n'.join(methods)+'\n',encoding='utf-8',newline='\n')
a.output.with_suffix('.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),extracted_sha256=hashlib.sha256(a.output.read_bytes()).hexdigest(),methods=a.names,actual_method_bodies=True,full_translation_unit=False),indent=2)+'\n',encoding='utf-8')
