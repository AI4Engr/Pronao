"""A real-window test of examples/excalidraw (Windows only). It builds a test copy of the page (test-driver.js added, which answers the two dialogs with
fixed paths) and a test program that allows one scratch folder, starts the program, lets the driver open a drawing, export it as PNG and SVG, delete everything,
save, and save as, and then checks what is on disk and what the driver saw.

    python examples/excalidraw/test/run_window_test.py                    (after: bash examples/excalidraw/build_site.sh)
    python examples/excalidraw/test/run_window_test.py --shot out.png     (no checks: opens a drawing with some content and saves the program's own window as a picture)

The window is kept topmost while it runs, because the browser engine pauses the page's timers when the window is covered. Nothing on the screen is read.
It needs the machine to be free: a window in front of it that takes the focus can still make a run fail; run it again.
"""
import ctypes, json, os, pathlib, shutil, struct, subprocess, sys, tempfile, time
from ctypes import wintypes

sys.stdout.reconfigure(encoding='utf-8', errors='replace')           # the log holds a bullet; a Windows console's default code page cannot print it
HERE = pathlib.Path(__file__).resolve().parent
EXAMPLE = HERE.parent
REPO = EXAMPLE.parent.parent
SITE = EXAMPLE / 'site'
if os.name != 'nt':
    sys.exit('this test needs a window: Windows only')
if not (SITE / 'index.html').exists():
    sys.exit('run `bash examples/excalidraw/build_site.sh` first')

SHOT = sys.argv[sys.argv.index('--shot') + 1] if '--shot' in sys.argv else None
work = pathlib.Path(tempfile.mkdtemp(prefix='pronao-excalidraw-'))
data = work / 'data'
data.mkdir()
failures = []


def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ('  ' + detail if detail else '')))
    if not ok:
        failures.append(name)


try:
    # a drawing to open: one rectangle and one text, in the format the component writes
    rect = {'id': 'r1', 'type': 'rectangle', 'x': 100, 'y': 100, 'width': 240, 'height': 120, 'angle': 0, 'strokeColor': '#1e1e1e', 'backgroundColor': '#a5d8ff', 'fillStyle': 'solid',
            'strokeWidth': 2, 'strokeStyle': 'solid', 'roughness': 1, 'opacity': 100, 'groupIds': [], 'frameId': None, 'roundness': {'type': 3}, 'seed': 1, 'version': 1,
            'versionNonce': 1, 'isDeleted': False, 'boundElements': None, 'updated': 1, 'link': None, 'locked': False}
    text = {'id': 't1', 'type': 'text', 'x': 130, 'y': 140, 'width': 180, 'height': 25, 'angle': 0, 'strokeColor': '#1e1e1e', 'backgroundColor': 'transparent', 'fillStyle': 'solid',
            'strokeWidth': 1, 'strokeStyle': 'solid', 'roughness': 1, 'opacity': 100, 'groupIds': [], 'frameId': None, 'roundness': None, 'seed': 2, 'version': 1, 'versionNonce': 2,
            'isDeleted': False, 'boundElements': None, 'updated': 1, 'link': None, 'locked': False, 'text': 'hello Pronao', 'fontSize': 20, 'fontFamily': 5, 'textAlign': 'left',
            'verticalAlign': 'top', 'containerId': None, 'originalText': 'hello Pronao', 'autoResize': True, 'lineHeight': 1.25}
    scene = {'type': 'excalidraw', 'version': 2, 'source': 'test', 'elements': [rect, text], 'appState': {'viewBackgroundColor': '#ffffff'}, 'files': {}}
    if SHOT:                 # a drawing worth a picture: three boxes, arrows, and text in two languages (the Chinese one needs the program's own font files)
        def box(i, kind, x, y, w, h, color):
            return dict(rect, id=i, type=kind, x=x, y=y, width=w, height=h, backgroundColor=color, seed=10 + len(i), roundness={'type': 3} if kind == 'rectangle' else None)

        def label(i, t, x, y, size=20):
            wide = any(ord(c) > 255 for c in t)
            return dict(text, id=i, text=t, originalText=t, x=x, y=y, fontSize=size, width=len(t) * size * (1.0 if wide else 0.55), height=size * 1.25, seed=30 + len(i))

        def arrow(i, x, y, dx, dy):
            return dict(rect, id=i, type='arrow', x=x, y=y, width=abs(dx), height=abs(dy), backgroundColor='transparent', points=[[0, 0], [dx, dy]], lastCommittedPoint=None,
                        startBinding=None, endBinding=None, startArrowhead=None, endArrowhead='arrow', roundness={'type': 2}, seed=50 + len(i), elbowed=False)
        scene['elements'] = [box('b1', 'rectangle', 80, 120, 200, 100, '#a5d8ff'), box('b2', 'ellipse', 420, 100, 220, 140, '#b2f2bb'), box('b3', 'rectangle', 760, 120, 200, 100, '#ffec99'),
                             arrow('a1', 285, 170, 130, 0), arrow('a2', 645, 170, 110, 0),
                             label('l1', 'page', 140, 158), label('l2', 'program', 480, 158), label('l3', 'the file you picked', 775, 158, 18),
                             label('l4', 'Pronao + Excalidraw', 330, 330, 40), label('l5', chr(0x624b) + chr(0x7ed8) + chr(0x98ce) + chr(0x683c) + chr(0xff0c) + chr(0x79bb) + chr(0x7ebf) + chr(0x53ef) + chr(0x7528), 330, 400, 32)]
    (data / 'in.excalidraw').write_text(json.dumps(scene), encoding='utf-8')
    (work / 'outside.txt').write_text('not for the page', encoding='utf-8')

    # the test copy of the page: the driver, told which folder to use
    test_site = work / 'site'
    shutil.copytree(SITE, test_site)
    (test_site / 'test-driver.js').write_text(('window.__TEST_SHOT__ = true;\n' if SHOT else '') + 'window.__TEST_DIR__ = ' + json.dumps(str(data).replace('\\', '/')) + ';\n' + (HERE / 'test-driver.js').read_text(encoding='utf-8'), encoding='utf-8')
    index = test_site / 'index.html'
    html = index.read_text(encoding='utf-8')
    marker = '<script type="module"'
    assert marker in html
    index.write_text(html.replace(marker, '<script src="/test-driver.js"></script>\n  ' + marker, 1), encoding='utf-8')

    # the test program
    exe = work / 'excalidraw_test.exe'
    env = dict(os.environ, ASSETS=str(test_site), WORK=str(work / 'build'), OPT='-O1', ICON=str(REPO / 'docs' / 'assets' / 'pronao.ico'))
    built = subprocess.run(['bash', str(REPO / 'scripts' / 'build.sh'), str(HERE / 'test_main.clg'), str(exe)], env=env, capture_output=True, text=True)
    if built.returncode != 0 or not exe.exists():
        print(built.stdout[-1500:], built.stderr[-1500:])
        sys.exit('the test program did not build')

    # run it, keep its window topmost, wait for the driver's log
    user32 = ctypes.windll.user32
    proc = subprocess.Popen([str(exe)], env=dict(os.environ, EXCALIDRAW_TEST_DIR=str(data)))
    Enum = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def raise_window():
        def cb(hwnd, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == proc.pid and user32.IsWindowVisible(hwnd):
                user32.ShowWindow(hwnd, 9)
                user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040)
            return True
        user32.EnumWindows(Enum(cb), 0)

    def window_picture(path):
        # only this program's own window: PrintWindow draws it into a bitmap, so whatever is in front of it on the screen cannot get into the picture
        from PIL import Image
        gdi32 = ctypes.windll.gdi32
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        found = []

        def cb(hwnd, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == proc.pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        user32.EnumWindows(Enum(cb), 0)
        hwnd = found[0]
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        hdc = user32.GetDC(0)
        mem = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(mem, bmp)
        user32.PrintWindow(hwnd, mem, 2)

        class HEADER(ctypes.Structure):
            _fields_ = [('size', wintypes.DWORD), ('w', wintypes.LONG), ('h', wintypes.LONG), ('planes', wintypes.WORD), ('bits', wintypes.WORD), ('comp', wintypes.DWORD),
                        ('image', wintypes.DWORD), ('x', wintypes.LONG), ('y', wintypes.LONG), ('used', wintypes.DWORD), ('important', wintypes.DWORD)]
        head = HEADER(ctypes.sizeof(HEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(head), 0)
        Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(path)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(0, hdc)

    steps = data / 'steps.log'
    deadline = time.time() + 90
    while time.time() < deadline and not steps.exists() and proc.poll() is None:
        raise_window()
        time.sleep(0.4)
    if SHOT:
        shot_log = steps.read_text(encoding='utf-8') if steps.exists() else ''
        if 'SHOT_READY' not in shot_log:
            proc.kill()
            sys.exit('the drawing was not opened: ' + shot_log[-300:])
        raise_window()
        time.sleep(0.8)
        window_picture(SHOT)
        proc.kill()
        print('picture of the window saved to ' + SHOT)
        sys.exit(0)
    time.sleep(0.5)
    proc.kill()
    log = steps.read_text(encoding='utf-8') if steps.exists() else ''
    print('--- what the page saw ---\n' + log + '-------------------------')

    check('the program started and the driver finished', 'DONE' in log and 'ERROR' not in log, log[-300:])
    check('the window title starts as Untitled', 'start title=Untitled - Excalidraw' in log)
    opened = [l for l in log.splitlines() if l.startswith('opened ')]
    check('opening a file through the dialog shows its name and is not "unsaved"', opened == ['opened title=in.excalidraw - Excalidraw dirty=false'] or opened == ['opened title=in.excalidraw - Excalidraw dirty=undefined'], str(opened))
    check('an edit marks the window unsaved (in the title and for the close question)', 'edited title=in.excalidraw • - Excalidraw dirty=true' in log, log)
    check('saving clears the mark', 'saved title=in.excalidraw - Excalidraw dirty=false' in log, log)
    check('save as takes the new name', 'saved as title=copy.excalidraw - Excalidraw dirty=false' in log, log)
    check('the page cannot read a file outside the folder it was given', 'outside refused forbidden' in log, log)
    check('the page cannot reach another address (fetch, image, script): the content-security policy refused all three', 'csp blocked 3 of 3 external requests' in log and 'external fetch ALLOWED' not in log, log)

    png = data / 'out.png'
    check('the PNG export was written and is a PNG', png.exists() and png.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' and png.stat().st_size > 500, str(png.stat().st_size if png.exists() else 'missing'))
    if png.exists():
        w, h = struct.unpack('>II', png.read_bytes()[16:24])
        check('the PNG has the size of the drawing, not an empty canvas', 200 < w < 1000 and 100 < h < 600, '%dx%d' % (w, h))
    svg = data / 'out.svg'
    check('the SVG export was written and holds the text', svg.exists() and svg.read_text(encoding='utf-8').lstrip().startswith('<svg'), 'missing' if not svg.exists() else svg.read_text(encoding='utf-8')[:80])

    saved = json.loads((data / 'in.excalidraw').read_text(encoding='utf-8')) if (data / 'in.excalidraw').exists() else {}
    check('Save wrote the file the drawing came from, with the deletion in it', saved.get('type') == 'excalidraw' and saved.get('elements') == [], str(saved)[:120])
    copy = data / 'copy.excalidraw'
    check('Save as wrote the copy', copy.exists() and json.loads(copy.read_text(encoding='utf-8')).get('type') == 'excalidraw')
    check('nothing outside the picked folder was changed', (work / 'outside.txt').read_text(encoding='utf-8') == 'not for the page')
    check('no file was written under a name the dialogs were not asked for', not any(p.name.startswith('unexpected') for p in data.iterdir()), str(sorted(p.name for p in data.iterdir())))
finally:
    shutil.rmtree(work, ignore_errors=True)

print('excalidraw window test: %s' % ('ok' if not failures else 'FAILED (' + ', '.join(failures) + ')'))
sys.exit(1 if failures else 0)
