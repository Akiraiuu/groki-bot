"""Prepare one pinned local SDK component; never modify managed_components."""
from pathlib import Path
import argparse,hashlib,json,urllib.request
EXPECTED={'CMakeLists.txt','Kconfig','LICENSE','idf_component.yml','esp_websocket_client.c','include/esp_websocket_client.h'}
PREFIX='https://raw.githubusercontent.com/espressif/esp-protocols/websocket-v1.8.0/components/esp_websocket_client/'
def prepare(root,source_dir=None,manifest=None):
    root=root.resolve(strict=True);dest=root/'build-inputs/esp_websocket_client'
    if dest.exists():raise ValueError('SDK input already exists; this preparation is single use')
    if not dest.resolve().is_relative_to(root) or dest.parent.is_symlink():raise ValueError('SDK input outside project root')
    if manifest is None:manifest=json.loads(Path(__file__).with_name('sdk-source-manifest.json').read_text(encoding='utf-8'))
    if manifest.get('version')!='1.8.0' or {e['path'] for e in manifest['files']}!=EXPECTED or len(manifest['files'])!=6:raise ValueError('unexpected SDK inventory')
    collected=[]
    for entry in manifest['files']:
        rel=Path(entry['path'])
        if rel.is_absolute() or '..' in rel.parts or entry['url']!=PREFIX+entry['path']:raise ValueError('unexpected SDK source path')
        if source_dir is not None:raw=(source_dir/rel).read_bytes()
        else:
            request=urllib.request.Request(entry['url'],headers={'User-Agent':'StopWatch-pinned-SDK-build'})
            with urllib.request.urlopen(request,timeout=15) as response:raw=response.read(200001)
        if len(raw)>200000 or hashlib.sha256(raw).hexdigest()!=entry['sha256']:raise ValueError('SDK source hash mismatch: '+entry['path'])
        collected.append((rel,raw))
    # No writes until every file is verified. Partial write evidence is retained.
    dest.mkdir(parents=True,exist_ok=False)
    for rel,raw in collected:
        target=dest/rel;target.parent.mkdir(parents=True,exist_ok=True)
        with target.open('xb') as out:out.write(raw)
    return dest
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2]);parser.add_argument('--source-dir',type=Path)
    a=parser.parse_args();dest=prepare(a.root,a.source_dir)
    print(json.dumps(dict(status='prepared',files=6,component=str(dest),managed_components_modified=False,build_started=False)))
if __name__=='__main__':main()
