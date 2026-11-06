# SPDX-License-Identifier: BSL-1.0
"""Compile the unchanged coordinator connect body against explicit host boundaries."""
from pathlib import Path
import sys,hashlib,json
source=Path(sys.argv[1]);target=Path(sys.argv[2]);text=source.read_text()
start=text.index('    bool connect()\n')
end=text.index('    bool wait_for_wifi_for_reconnect(',start)
body=text[start:end]
assert body.count('fetch_agent_ticket(')==1 and body.count('client_->start(config_)')==1
assert body.count('connect_observer=')==2
target.write_text(body,encoding='utf-8')
http=(source.parent.parent/'components/wifi_config_service/http_handlers.cpp').read_text()
begin=http.index('esp_err_t handle_paid_voice_status(httpd_req_t* req)')
end=http.index('esp_err_t handle_paid_voice_control(httpd_req_t* req)',begin)
(target.parent/'generated_voice_status.inc').write_text(http[begin:end],encoding='utf-8')
print(json.dumps(dict(actual_coordinator_connect_body=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),sdk_and_operation_admission_stubbed=True)))
