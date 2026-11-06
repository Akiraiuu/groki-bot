"""Check-only by default. No builds, network, devices, or retries."""
from pathlib import Path
import argparse,subprocess,json,hashlib,os
ROOT=Path(__file__).resolve().parents[1]
IDF='3ea97e7591b6a83f5d6f8e0cc7f1b16f27b2b855'
LWIP='c6f2f878e7b0f86033214b85547d579be43351e3'
def sha(data):return hashlib.sha256(data).hexdigest()
def contained(root,relative):
    rel=Path(relative)
    if rel.is_absolute() or '..' in rel.parts:raise ValueError('invalid relative path')
    dest=root/rel
    if not dest.resolve().is_relative_to(root.resolve()):raise ValueError('outside intended root')
    for item in [dest,*dest.parents]:
        if item==root.parent:break
        if item.is_symlink():raise ValueError('symlink refused')
    return dest
def validate(idf,lwip,sdk,idf_ref,lwip_ref):
    roots={k:v.resolve(strict=True) for k,v in [('idf',idf),('lwip',lwip),('sdk',sdk)]}
    if roots['lwip']!=(roots['idf']/'components/lwip/lwip').resolve():raise ValueError('unexpected lwIP submodule root')
    if roots['sdk'].is_relative_to(roots['idf']) or roots['idf'].is_relative_to(roots['sdk']):raise ValueError('SDK and IDF roots overlap')
    if idf_ref!=IDF or lwip_ref!=LWIP:raise ValueError('IDF or lwIP commit mismatch')
    manifest=json.loads((ROOT/'overlay/overlay-manifest-v03.json').read_text(encoding='utf-8'))
    if manifest['idf_commit']!=IDF or manifest['lwip_commit']!=LWIP or len(manifest['files'])!=19:raise ValueError('unexpected pinned manifest')
    plan=[]
    for entry in manifest['files']:
        if entry['kind'] not in roots:raise ValueError('unknown source kind')
        dest=contained(roots[entry['kind']],entry['path']);source=contained(ROOT,entry['candidate']);raw=source.read_bytes()
        if sha(raw)!=entry['candidate_sha256']:raise ValueError('candidate content mismatch')
        expected=entry['original_sha256']
        if expected is None:
            if dest.exists():raise ValueError('new file already exists')
        elif not dest.is_file() or sha(dest.read_bytes())!=expected:raise ValueError('upstream content mismatch: '+entry['path'])
        if dest.with_name(dest.name+'.stopwatch-original-v03').exists() or dest.with_name(dest.name+'.stopwatch-new-v03').exists():raise ValueError('overlay attempt already recorded')
        plan.append((dest,raw,expected))
    if len({str(x[0]).casefold() for x in plan})!=19:raise ValueError('duplicate destination')
    return plan
def apply(plan):
    for dest,raw,expected in plan:
        if (expected is None and dest.exists()) or (expected is not None and sha(dest.read_bytes())!=expected):raise ValueError('input changed after preflight')
        dest.parent.mkdir(parents=True,exist_ok=True)
        if expected is not None:
            with dest.with_name(dest.name+'.stopwatch-original-v03').open('xb') as f:f.write(dest.read_bytes())
        temp=dest.with_name(dest.name+'.stopwatch-new-v03')
        with temp.open('xb') as f:f.write(raw)
        os.replace(temp,dest)
        if sha(dest.read_bytes())!=sha(raw):raise ValueError('write verification failed')
def commit(root):return subprocess.run(['git','-C',str(root),'rev-parse','--verify','HEAD^{commit}'],check=True,capture_output=True,text=True,timeout=5).stdout.strip()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--idf',type=Path,required=True);parser.add_argument('--lwip',type=Path,required=True);parser.add_argument('--sdk',type=Path,required=True)
    group=parser.add_mutually_exclusive_group();group.add_argument('--apply',action='store_true');group.add_argument('--check-only',action='store_true');a=parser.parse_args()
    plan=validate(a.idf,a.lwip,a.sdk,commit(a.idf),commit(a.lwip))
    if a.apply:apply(plan)
    print(json.dumps(dict(status='passed',mode='applied' if a.apply else 'check_only',files=len(plan),build_started=False,network_access=False)))
if __name__=='__main__':main()
