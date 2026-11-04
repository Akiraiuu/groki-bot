"""Extract unchanged method bodies from the experimental SDK source."""
from pathlib import Path
import json,hashlib,sys,re
source=Path(sys.argv[1]);out=Path(sys.argv[2]);s=source.read_text(encoding='utf-8')
def extract(name):
    pattern=re.compile(r'^(?:static )?(?:esp_err_t|bool|int|void) '+re.escape(name)+r'\(',re.M)
    for match in pattern.finditer(s):
        start=match.start();opening=s.find('{',start);semi=s.find(';',start)
        if semi>=0 and semi<opening:continue
        mode='code';depth=0;i=opening
        while i<len(s):
            c=s[i];pair=s[i:i+2]
            if mode=='line':
                if c=='\n':mode='code'
            elif mode=='block':
                if pair=='*/':mode='code';i+=1
            elif mode in ('string','char'):
                if c=='\\':i+=1
                elif c==('"' if mode=='string' else "'"):mode='code'
            elif pair=='//':mode='line';i+=1
            elif pair=='/*':mode='block';i+=1
            elif c=='"':mode='string'
            elif c=="'":mode='char'
            elif c=='{':depth+=1
            elif c=='}':
                depth-=1
                if depth==0:return s[start:i+1]
            i+=1
    raise ValueError(name)
names=['controlled_stop_requested','controlled_cleanup_lock','controlled_config_valid','controlled_bind_scope','esp_websocket_client_stop_observed',
       'esp_websocket_client_transport_close_confirmed',
       'esp_websocket_client_request_stop','esp_websocket_client_finish_cleanup','esp_websocket_client_stop_until','stop_wait_task','esp_websocket_client_is_connected',
       'esp_websocket_client_send_with_exact_opcode','esp_websocket_client_send_with_opcode',
       'esp_websocket_client_abort_connection','esp_websocket_client_recv','esp_websocket_client_task','esp_websocket_client_start','esp_websocket_client_destroy',
       'esp_websocket_client_destroy_on_exit','esp_websocket_client_send_close',
       'esp_websocket_client_close_with_optional_body']
out.write_text('\n\n'.join(extract(n) for n in names)+'\n',encoding='utf-8',newline='\n')
meta=dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
          extracted_sha256=hashlib.sha256(out.read_bytes()).hexdigest(),methods=names,
          actual_method_bodies=True,full_sdk_translation_unit=False)
out.with_suffix('.json').write_text(json.dumps(meta,indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(status='passed',methods=len(names))))
