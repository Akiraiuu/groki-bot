import unittest,tempfile,hashlib
from pathlib import Path
from unittest.mock import patch
from prepare_sdk import prepare,EXPECTED,PREFIX
class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='stopwatch-sdk-fixture-')
        self.root=Path(self.temp.name).resolve()
        assert self.root.parent==Path(tempfile.gettempdir()).resolve()
        self.addCleanup(self.temp.cleanup)
        self.source=self.root/'source';self.source.mkdir()
        self.project=self.root/'project';self.project.mkdir()
        self.manifest={'version':'1.8.0','files':[]}
        for path in sorted(EXPECTED):
            raw=('synthetic SDK source '+path).encode();f=self.source/path;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(raw)
            self.manifest['files'].append({'path':path,'url':PREFIX+path,'sha256':hashlib.sha256(raw).hexdigest()})
    def test_exact_inventory_and_no_network(self):
        with patch('urllib.request.urlopen',side_effect=AssertionError('offline fixture must not use network')):
            dest=prepare(self.project,self.source,self.manifest)
        self.assertEqual(len([f for f in dest.rglob('*') if f.is_file()]),6)
        for path in EXPECTED:self.assertEqual((dest/path).read_bytes(),(self.source/path).read_bytes())
        self.assertFalse((self.project/'managed_components').exists())
    def test_wrong_hash_writes_nothing(self):
        (self.source/'Kconfig').write_text('wrong',encoding='utf-8')
        with self.assertRaises(ValueError):prepare(self.project,self.source,self.manifest)
        self.assertFalse((self.project/'build-inputs').exists())
    def test_no_repeat_or_overwrite(self):
        dest=prepare(self.project,self.source,self.manifest);before=(dest/'LICENSE').read_bytes()
        with self.assertRaises(ValueError):prepare(self.project,self.source,self.manifest)
        self.assertEqual((dest/'LICENSE').read_bytes(),before)
    def test_path_and_version_are_pinned(self):
        self.manifest['files'][0]['path']='../escape'
        with self.assertRaises(ValueError):prepare(self.project,self.source,self.manifest)
        self.assertFalse((self.project/'build-inputs').exists())
    def test_unexpected_remote_is_refused(self):
        self.manifest['files'][0]['url']='https://invalid.example/code'
        with self.assertRaises(ValueError):prepare(self.project,self.source,self.manifest)
        self.assertFalse((self.project/'build-inputs').exists())
if __name__=='__main__':unittest.main()
