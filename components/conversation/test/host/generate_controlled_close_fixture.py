# SPDX-License-Identifier: BSL-1.0
"""Extract the current production close methods, without compiling M5/ESP-IDF."""
from pathlib import Path
import hashlib,json,sys
source=Path(sys.argv[1]);target=Path(sys.argv[2]);text=source.read_text(encoding='utf-8')
def method(marker):
    start = text.index(marker)
    opening = text.index('{', start)
    level = 0
    mode = 'code'
    i = opening
    while i < len(text):
        char = text[i]
        following = text[i:i+2]
        if mode == 'line':
            if char == '\n':
                mode = 'code'
        elif mode == 'block':
            if following == '*/':
                mode = 'code'
                i += 1
        elif mode in ('string', 'char'):
            if char == '\\':
                i += 1
            elif char == ('"' if mode == 'string' else "'"):
                mode = 'code'
        elif following == '//':
            mode = 'line'
            i += 1
        elif following == '/*':
            mode = 'block'
            i += 1
        elif char == '"':
            mode = 'string'
        elif char == "'":
            mode = 'char'
        elif char == '{':
            level += 1
        elif char == '}':
            level -= 1
            if level == 0:
                return text[start:i+1]
        i += 1
    raise ValueError('unterminated_candidate_method')

start=text.index('    using OperationLease=')
end=text.index('    // ---- event bridge:',start)
content=text[start:end]
target.parent.mkdir(parents=True,exist_ok=True)
target.write_text(content,encoding='utf-8')
settings_path=source.parent/'settings_sinks.cpp'
settings=settings_path.read_text(encoding='utf-8')
guard_start=settings.index('    if(g_exclusive_voice_audio.load',settings.index('void start_say_worker('))
guard_end=settings.index('    auto* owned =',guard_start)
(target.parent/'generated_local_speech_guard.inc').write_text(settings[guard_start:guard_end],encoding='utf-8')
app_main=(source.parent/'app_main.cpp').read_text(encoding='utf-8')
assert app_main.index('settings_sinks::set_exclusive_voice_audio(')<app_main.index('settings_sinks::register_ble_sinks(')
print(json.dumps(dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),extracted_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),actual_method_bodies=True,sdk_stubbed=True)))
