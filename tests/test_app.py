import http.client
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

from mechanical_tv.core import FRAME_BYTES, Library, Player
from mechanical_tv.server import Application, Handler, Server, password_hash


def auth_file(root):
    salt = 'a1' * 16
    (Path(root) / 'auth.json').write_text(json.dumps({'salt': salt, 'hash': password_hash('test-password-123', salt)}))


class PlayerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.library = Library(self.temp.name)
        self.ident = self.library.create('clip', 'fit')
        (self.library.root / self.ident / 'frames.raw').write_bytes(bytes([128]) * FRAME_BYTES * 20)
        self.library.update(self.ident, status='ready', duration=2, frames=20)
        self.now = 0
        self.player = Player(self.library, lambda: self.now)
        self.player.command('select', self.ident)

    def test_pause_seek_loop_and_blank_stop(self):
        self.player.command('play')
        self.now = 1.2
        self.assertAlmostEqual(self.player.snapshot()['position'], 1.2)
        self.player.command('pause')
        self.now = 8
        self.assertAlmostEqual(self.player.snapshot()['position'], 1.2)
        self.player.command('seek', 1.8)
        self.player.command('play')
        self.now = 8.5
        self.assertAlmostEqual(self.player.snapshot()['position'], .3)
        self.player.command('stop')
        self.assertEqual(set(self.player.snapshot()['pixels']), {0})

    def test_non_loop_ends_and_restart_is_idle(self):
        self.player.command('loop', False)
        self.player.command('play')
        self.now = 3
        self.assertEqual(self.player.snapshot()['state'], 'stopped')
        self.assertEqual(Player(self.library).snapshot()['state'], 'idle')

    def test_interrupted_job_recovery(self):
        ident = self.library.create('interrupted', 'fit')
        (self.library.root / ident / 'upload.part').write_bytes(b'partial')
        recovered = Library(self.temp.name)
        self.assertEqual(recovered.get(ident)['status'], 'failed')
        self.assertFalse((recovered.root / ident / 'upload.part').exists())

    def test_invalid_id_and_damaged_frames(self):
        with self.assertRaises(ValueError):
            self.library.get('../../etc/passwd')
        (self.library.root / self.ident / 'frames.raw').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            self.player.command('select', self.ident)


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'FFmpeg required')
class IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        auth_file(cls.root)
        cls.app = Application(cls.root)
        cls.server = Server(('127.0.0.1', 0), Handler)
        cls.server.app = cls.app
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.clip = cls.root / 'test.mp4'
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x120:rate=10', '-t', '1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', str(cls.clip)], check=True, timeout=30)

    @classmethod
    def tearDownClass(cls):
        cls.app.stopping.set()
        cls.app.worker.join(10)
        cls.server.shutdown()
        cls.server.server_close()
        cls.temp.cleanup()

    def request(self, method, path, body=None, cookie=None, extra=None):
        headers = {'X-MTV-Request': '1'}
        if cookie:
            headers['Cookie'] = cookie
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            headers['Content-Type'] = 'application/json'
        headers.update(extra or {})
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=20)
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def login(self):
        status, headers, _ = self.request('POST', '/api/login', {'password': 'test-password-123'})
        self.assertEqual(status, 200)
        return headers['Set-Cookie'].split(';')[0]

    def test_authentication_and_cross_origin_rejection(self):
        self.assertEqual(self.request('GET', '/api/state')[0], 401)
        self.assertEqual(self.request('POST', '/api/login', {'password':'wrong'})[0], 401)
        cookie = self.login()
        self.assertEqual(self.request('GET', '/api/state', cookie=cookie)[0], 200)
        self.assertEqual(self.request('POST', '/api/player', {'action':'play'}, cookie, {'Origin':'http://untrusted.example'})[0], 403)
        self.assertEqual(self.request('POST', '/api/player', {'action':'seek', 'value':float('nan')}, cookie)[0], 400)
        self.request('POST', '/api/logout', {}, cookie)
        self.assertEqual(self.request('GET', '/api/state', cookie=cookie)[0], 401)

    def test_upload_convert_preview_play_and_delete(self):
        cookie = self.login()
        status, _, body = self.request('POST', '/api/upload?name=example.mp4&fit=fit', self.clip.read_bytes(), cookie)
        self.assertEqual(status, 201, body)
        ident = json.loads(body)['id']
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            item = self.app.library.get(ident)
            if item['status'] in ('ready', 'failed'):
                break
            time.sleep(.1)
        self.assertEqual(item['status'], 'ready', item)
        self.assertEqual(item['frames'], 10)
        frame_file = self.app.library.root / ident / 'frames.raw'
        self.assertEqual(frame_file.stat().st_size, FRAME_BYTES * 10)
        self.assertGreater(len(set(frame_file.read_bytes())), 10)
        status, headers, body = self.request('GET', f'/api/preview/{ident}', cookie=cookie, extra={'Range':'bytes=0-15'})
        self.assertEqual(status, 206)
        self.assertEqual(len(body), 16)
        self.assertIn('Content-Range', headers)
        self.assertEqual(self.request('GET', f'/api/preview/{ident}', cookie=cookie, extra={'Range':'bytes=999999999-'})[0], 416)
        self.assertEqual(self.request('POST', '/api/player', {'action':'select','value':ident}, cookie)[0], 200)
        self.assertEqual(self.request('POST', '/api/player', {'action':'play'}, cookie)[0], 200)
        self.assertEqual(self.request('POST', '/api/delete', {'id':ident}, cookie)[0], 400)
        self.request('POST', '/api/player', {'action':'stop'}, cookie)
        self.assertEqual(self.request('POST', '/api/delete', {'id':ident}, cookie)[0], 200)
        self.assertFalse(frame_file.exists())

    def test_corrupt_video_fails_without_killing_worker(self):
        ident = self.app.library.create('broken.mp4', 'crop')
        (self.app.library.root / ident / 'original').write_bytes(b'not a video')
        self.app.library.prepare(ident)
        self.assertEqual(self.app.library.get(ident)['status'], 'failed')
        self.assertTrue(self.app.worker.is_alive())


if __name__ == '__main__':
    unittest.main()
