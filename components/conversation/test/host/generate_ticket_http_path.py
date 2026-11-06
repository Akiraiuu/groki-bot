# SPDX-License-Identifier: BSL-1.0
from pathlib import Path
import json,re,hashlib,sys
sdk=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
manifest=json.loads((sdk/'overlay/overlay-manifest-v03.json').read_text())
sha=lambda b:hashlib.sha256(b).hexdigest()
def extract(text,name):
    m=re.search(r'^(?:static\s+)?(?:inline\s+)?(?:void|int|bool|esp_err_t)\s+'+re.escape(name)+r'\([^;{}]*\)\s*\{',text,re.M)
    assert m,name
    i=text.index('{',m.start());depth=0;mode='code'
    while i<len(text):
        c=text[i];pair=text[i:i+2]
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
            if depth==0:return text[m.start():i+1]
        i+=1
    raise AssertionError('unterminated '+name)
parts=[('components/esp-tls/esp_tls.c',['esp_tls_attach_scope','scoped_io_begin','scoped_permitted','esp_tls_conn_new_sync'],'tls.inc'),
       ('components/tcp_transport/transport_ssl.c',['esp_transport_ssl_set_scope','ssl_connect'],'ssl.inc'),
       ('components/esp_http_client/esp_http_client.c',['http_scope_config_valid','http_scope_begin','esp_http_client_connect'],'http.inc')]
records=[]
for name,names,target in parts:
    raw=(sdk/'idf/candidate'/name).read_bytes();entry=next(e for e in manifest['files'] if e['kind']=='idf' and e['path']==name)
    assert sha(raw)==entry['candidate_sha256'];text=raw.decode()
    content='\n\n'.join(extract(text,n) for n in names)+'\n';(out/target).write_text(content,encoding='utf-8')
    records.append(dict(path=name,sha256=sha(raw),methods=names,extracted_sha256=sha(content.encode()),actual_bodies=True))
for name in ('esp_tls_scope.c','esp_tls_scope.h'):
    raw=(sdk/'idf/candidate/components/esp-tls'/name).read_bytes();(out/name).write_bytes(raw)
    entry=next(e for e in manifest['files'] if e['kind']=='idf' and e['path']=='components/esp-tls/'+name)
    assert sha(raw)==entry['candidate_sha256'];records.append(dict(path='components/esp-tls/'+name,sha256=sha(raw),full_file=True))

# URL setup can call close before any connection exists. Keep these exact
# methods and the transport-default fragment in the initialization regression.
http_text=(sdk/'idf/candidate/components/esp_http_client/esp_http_client.c').read_text()
start=http_text.index('    if (config->transport_type == HTTP_TRANSPORT_OVER_SSL)',http_text.index('static esp_err_t _set_config'))
end=http_text.index('    if (client->timeout_ms == 0)',start)
branch=http_text[start:end]
assert branch.count('DEFAULT_HTTPS_PORT')==1 and branch.count('DEFAULT_HTTP_PORT')==1
content=extract(http_text,'esp_http_client_close')+'\n'+extract(http_text,'esp_http_client_set_url')+'\n'
content+='static esp_err_t config_transport_defaults(esp_http_client_handle_t client,const esp_http_client_config_t*config){\n    esp_err_t ret=ESP_OK;\n    client->connection_info.port=config->port;\n'+branch+'    return ret;\nerror:return ret;\n}\n'
(out/'http-init.inc').write_text(content,encoding='utf8')
records.append(dict(path='components/esp_http_client/esp_http_client.c',methods=['esp_http_client_close','esp_http_client_set_url'],exact_fragment='_set_config transport defaults',generated_sha256=sha(content.encode()),parser_and_string_helpers_mocked=True,full_http_init_not_executed=True))

(out/'source-verification.json').write_text(json.dumps(records,indent=2)+'\n')
