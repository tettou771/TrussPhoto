#!/usr/bin/env python3
"""Memo <-> photo link smoke test (UTC matching, nearest-one, diary exclusion,
YAML block-list frontmatter). Uses copies of the test DNGs; one copy gets its
EXIF offset rewritten to +01:00 to stand in for a photo shot abroad."""
import json, os, shutil, sqlite3, subprocess, sys, time
import urllib.request
from datetime import datetime, timezone, timedelta

ROOT = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(ROOT)
APP = os.path.join(WT, "bin/TrussPhoto.app/Contents/MacOS/TrussPhoto")
CAT = f"{ROOT}/CatLinks"
SRC = f"{ROOT}/links_src"
VAULT = f"{ROOT}/links_vault"
MCP = 18853

# The app records --catalog in the global app_config.json; snapshot it so test
# catalogs never become the user's "last opened" catalog.
APP_CONFIG = os.path.expanduser("~/Library/Application Support/TrussPhoto/app_config.json")
try:
    _app_config_backup = open(APP_CONFIG).read()
except OSError:
    _app_config_backup = None

def restore_app_config():
    if _app_config_backup is not None:
        with open(APP_CONFIG, "w") as f:
            f.write(_app_config_backup)

for d in (CAT, SRC, VAULT):
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)

# --- photos: A keeps +09:00, B is rewritten to +01:00 (same wall clock) ---
shutil.copy2(f"{ROOT}/import_src/BF_08042.DNG", f"{SRC}/LINK_A.DNG")
shutil.copy2(f"{ROOT}/import_src/BF_08056.DNG", f"{SRC}/LINK_B.DNG")
subprocess.run(["exiftool", "-q", "-overwrite_original",
                "-OffsetTimeOriginal=+01:00", "-OffsetTimeDigitized=+01:00", "-OffsetTime=+01:00",
                f"{SRC}/LINK_B.DNG"], check=True)
# C: no EXIF offset at all (older bodies); its clock was set to local time abroad
shutil.copy2(f"{ROOT}/import_src/BF_08042.DNG", f"{SRC}/LINK_C.DNG")
subprocess.run(["exiftool", "-q", "-overwrite_original",
                "-OffsetTimeOriginal=", "-OffsetTimeDigitized=", "-OffsetTime=",
                f"{SRC}/LINK_C.DNG"], check=True)

def exif_utc(path):
    out = subprocess.run(["exiftool", "-s3", "-DateTimeOriginal", "-OffsetTimeOriginal", path],
                         capture_output=True, text=True, check=True).stdout.split("\n")
    dto, off = out[0].strip(), out[1].strip()
    sign = 1 if off[0] == "+" else -1
    tz = timezone(sign * timedelta(hours=int(off[1:3]), minutes=int(off[4:6])))
    return datetime.strptime(dto, "%Y:%m:%d %H:%M:%S").replace(tzinfo=tz)

A = exif_utc(f"{SRC}/LINK_A.DNG")   # 02:47:44Z
B = exif_utc(f"{SRC}/LINK_B.DNG")   # 10:48:37Z (would be 02:48:37Z if read as JST)

def iso(dt):
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

# --- memos ---
notes = {
    # Nearest to A in true UTC. Read as JST, B would sit 7 s away and win.
    "m_a.md": f"---\ntags: [photo-memo]\ncreated: {iso(A + timedelta(seconds=60))}\n---\n\nnear A\n",
    # Block-list tags + location, as Obsidian's Properties UI writes them.
    "m_b.md": (f"---\ntags:\n  - photo-memo\ncreated: {iso(B - timedelta(seconds=120))}\n"
               f"location:\n  - 47.4979\n  - 19.0402\n---\n\nnear B\n"),
    "m_diary_case.md": f"---\ntags: [Diary]\ncreated: {iso(A)}\n---\n\ndiary upper-case\n",
    "m_diary_nested.md": f"---\ntags:\n  - diary/2026\ncreated: {iso(B)}\n---\n\ndiary nested\n",
    "m_far.md": f"---\ntags: [photo-memo]\ncreated: {iso(A + timedelta(hours=2))}\n---\n\nno photo nearby\n",
    # Local offset recovered from the QuickMemo filename (local wall clock):
    # B's instant written at +01:00 -> 11:48:37 local. Diary so links stay unchanged.
    "20260218_114837.md": f"---\ntags: [diary]\ncreated: {iso(B)}\n---\n\nfilename offset (seconds)\n",
    "20260218_1148.md": f"---\ntags: [diary]\ncreated: {iso(B)}\n---\n\nfilename offset (minutes)\n",
    # Named when recording started (19:34 JST), saved 14 min 37 s later: still +09:00
    "20260218_1934.md": f"---\ntags: [diary]\ncreated: {iso(B)}\n---\n\ntranscription delay\n",
    # Written abroad at 11:48:00 local (+02:00, from the name): matches the
    # offset-less LINK_C whose wall clock reads 11:47:44
    "20260218_114800.md": "---\ntags: [photo-memo]\ncreated: 2026-02-18T09:48:00Z\n---\n\nnear C\n",
    # Offset written explicitly in `created`
    "iso_offset.md": "---\ntags: [diary]\ncreated: 2026-02-18T11:47:44+09:00\n---\n\nexplicit offset\n",
}
for name, body in notes.items():
    with open(f"{VAULT}/{name}", "w") as f:
        f.write(body)
# Same filename in two folders: two different notes, must stay two rows
for sub, mins in (("sub1", 300), ("sub2", 360)):
    os.makedirs(f"{VAULT}/{sub}")
    with open(f"{VAULT}/{sub}/dup.md", "w") as f:
        f.write(f"---\ntags: [diary]\ncreated: {iso(A + timedelta(minutes=mins))}\n---\n\n{sub} note\n")

for pid in subprocess.run(["lsof", "-ti", f":{MCP}"], capture_output=True, text=True).stdout.split():
    subprocess.run(["kill", "-9", pid])

logf = open(f"{ROOT}/links_smoke.log", "w")
proc = subprocess.Popen([APP, "--catalog", CAT],
                        env={**os.environ, "TRUSSC_MCP": "1", "TRUSSC_MCP_PORT": str(MCP)},
                        stdout=logf, stderr=logf)

reqid = 0
def rpc(method, params=None, timeout=120):
    global reqid
    reqid += 1
    msg = {"jsonrpc": "2.0", "id": reqid, "method": method}
    if params is not None: msg["params"] = params
    req = urllib.request.Request(f"http://localhost:{MCP}/mcp", data=json.dumps(msg).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())

def call(name, args=None):
    r = rpc("tools/call", {"name": name, "arguments": args or {}})
    if "error" in r: raise RuntimeError(f"{name}: {r['error']}")
    for c in r["result"].get("content", []):
        if c.get("type") == "text":
            try: return json.loads(c["text"])
            except json.JSONDecodeError: return c["text"]
    return r["result"]

def q(sql):
    con = sqlite3.connect(f"file:{CAT}/library.db?mode=ro", uri=True)
    try: return con.execute(sql).fetchall()
    finally: con.close()

failures = []
def check(desc, cond):
    print(("PASS: " if cond else "FAIL: ") + desc)
    if not cond: failures.append(desc)

try:
    for _ in range(30):
        try:
            rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "links", "version": "1"}})
            break
        except Exception:
            time.sleep(1)
    time.sleep(2)

    call("load_folder", {"path": SRC})
    offs = dict(q("SELECT filename, offset_time FROM photos WHERE entry_type=0"))
    check(f"photo offsets extracted ({offs})",
          offs.get("LINK_A.DNG") == "+09:00" and offs.get("LINK_B.DNG") == "+01:00")

    r = call("import_obsidian", {"path": VAULT})
    check(f"12 memos imported ({r})", r.get("added") == 12)

    tags = dict(q("SELECT filename, tags FROM photos WHERE entry_type=1"))
    check(f"block-list tags parsed ({tags.get('m_b.md')})", tags.get("m_b.md") == '["photo-memo"]')
    loc = q("SELECT latitude, longitude FROM photos WHERE filename='m_b.md'")[0]
    check(f"block-list location parsed ({loc})", abs(loc[0] - 47.4979) < 1e-6 and abs(loc[1] - 19.0402) < 1e-6)
    dups = q("SELECT memo FROM photos WHERE entry_type=1 AND filename='dup.md' ORDER BY memo")
    check(f"same-named notes in two folders stay separate ({dups})",
          [d[0].strip() for d in dups] == ["sub1 note", "sub2 note"])
    memo = {f: (d, o) for f, d, o in q("SELECT filename, date_time_original, offset_time FROM photos WHERE entry_type=1")}
    check(f"memos carry a UTC offset ({sorted(set(o for _, o in memo.values()))})",
          all(o for _, o in memo.values()))
    check(f"offset from filename HHMMSS ({memo.get('20260218_114837.md')})",
          memo.get("20260218_114837.md") == ("2026:02:18 11:48:37", "+01:00"))
    check(f"offset from filename HHMM ({memo.get('20260218_1148.md')})",
          memo.get("20260218_1148.md") == ("2026:02:18 11:48:37", "+01:00"))
    check(f"filename offset survives a transcription delay ({memo.get('20260218_1934.md')})",
          memo.get("20260218_1934.md") == ("2026:02:18 19:48:37", "+09:00"))
    check(f"offset from created ISO ({memo.get('iso_offset.md')})",
          memo.get("iso_offset.md") == ("2026:02:18 11:47:44", "+09:00"))

    links = call("get_text_links")["links"]
    fname = dict(q("SELECT id, filename FROM photos"))
    by_name = {fname.get(k, k): [fname.get(p, p) for p in v] for k, v in links.items()}
    print("links:", by_name)
    check("m_a -> exactly LINK_A (UTC-nearest, not the +01:00 shot)", by_name.get("m_a.md") == ["LINK_A.DNG"])
    check("m_b -> exactly LINK_B (EXIF offset honoured)", by_name.get("m_b.md") == ["LINK_B.DNG"])
    check("diary (case/nested) never linked",
          "m_diary_case.md" not in by_name and "m_diary_nested.md" not in by_name)
    check("memo outside ±30 min not linked", "m_far.md" not in by_name)
    check("memo abroad -> offset-less LINK_C by wall clock", by_name.get("20260218_114800.md") == ["LINK_C.DNG"])
    check("exactly 3 links (diary memos excluded)", len(links) == 3)

    r2 = call("import_obsidian", {})
    check(f"re-import is a no-op ({r2})", r2.get("added") == 0 and r2.get("updated") == 0)

    # Rename and delete in the vault
    far_id = q("SELECT id FROM photos WHERE filename='m_far.md'")[0][0]
    os.rename(f"{VAULT}/m_far.md", f"{VAULT}/m_far_renamed.md")
    os.remove(f"{VAULT}/m_diary_case.md")
    r3 = call("import_obsidian", {})
    check(f"rename/delete re-import adds nothing ({r3})", r3.get("added") == 0)
    ren = q(f"SELECT filename, sync_state FROM photos WHERE id='{far_id}'")
    check(f"renamed note keeps its row ({ren})", ren == [("m_far_renamed.md", 0)])
    gone = q("SELECT sync_state FROM photos WHERE filename='m_diary_case.md'")
    check(f"deleted note is marked Missing ({gone})", gone == [(4,)])
    fname = dict(q("SELECT id, filename FROM photos"))

    # --- grid: memo cards interleaved, never opened as images ---
    grid = call("get_grid")
    items = grid["items"]
    kinds = [it["type"] for it in items]
    print("grid:", [(it["type"], fname.get(it["id"], it["id"])) for it in items])
    check(f"grid shows memo cards by default ({kinds.count('memo')} memos, {kinds.count('media')} media)",
          grid["showText"] and kinds.count("memo") == 12 and kinds.count("media") == 3)

    memo_id = next(it["id"] for it in items if it["type"] == "memo")
    r = call("open_photo", {"id": memo_id})
    check(f"opening a memo does nothing ({r})", r.get("single") is False)

    shot = rpc("tools/call", {"name": "tc_get_screenshot", "arguments": {"width": 1600}})
    for c in shot["result"].get("content", []):
        if c.get("type") == "image":
            import base64
            open(f"{ROOT}/links_grid.png", "wb").write(base64.b64decode(c["data"]))

    # Arrow keys in the single view step over memo cards
    ids = [it["id"] for it in items]
    media = [i for i, it in enumerate(items) if it["type"] == "media"]
    first, second = media[0], media[1]
    r = call("open_photo", {"id": ids[first]})
    check(f"open first photo ({fname.get(r.get('current', ''), '?')})", r.get("single") and r.get("current") == ids[first])
    between = [items[i]["type"] for i in range(first + 1, second)]
    RIGHT, LEFT = 262, 263
    call("tc_key_press", {"key": RIGHT}); call("tc_key_release", {"key": RIGHT})
    time.sleep(1.5)
    log = open(f"{ROOT}/links_smoke.log", errors="replace").read()
    want = fname[items[second]["id"]]
    check(f"RIGHT skips {len(between)} memo card(s) to {want}", log.rstrip().count(f"Opening: {want}") >= 1)
    call("tc_key_press", {"key": LEFT}); call("tc_key_release", {"key": LEFT})
    time.sleep(1.5)
    log = open(f"{ROOT}/links_smoke.log", errors="replace").read()
    back = fname[items[first]["id"]]
    check(f"LEFT skips back to {back}", log.count(f"Opening: {back}") >= 2)

    # Repopulate under an open single view (a new memo lands at the top and
    # shifts every index): arrows must still move from the photo on screen.
    r = call("open_photo", {"id": ids[first]})
    with open(f"{VAULT}/zz_newest.md", "w") as f:
        f.write(f"---\ntags: [photo-memo]\ncreated: {iso(A + timedelta(hours=6))}\n---\n\nnewest\n")
    call("import_obsidian", {})
    before = open(f"{ROOT}/links_smoke.log", errors="replace").read().count(f"Opening: {want}")
    call("tc_key_press", {"key": RIGHT}); call("tc_key_release", {"key": RIGHT})
    time.sleep(1.5)
    after = open(f"{ROOT}/links_smoke.log", errors="replace").read().count(f"Opening: {want}")
    check(f"after repopulate, RIGHT still goes {back} -> {want}", after == before + 1)
    call("tc_key_press", {"key": 256}); call("tc_key_release", {"key": 256})   # ESC -> grid
    time.sleep(0.5)

    r = call("set_memo", {"id": memo_id, "memo": "edited"})
    check(f"set_memo refuses memo rows ({r})", r.get("status") == "error")

    # Toggle memo cards off and on
    r = call("set_show_text", {"show": False})
    check(f"hiding memo cards leaves only photos ({r})", r.get("gridCount") == 3)
    r = call("set_show_text", {"show": True})
    check(f"showing memo cards again ({r})", r.get("gridCount") == 16)

    print("=" * 40)
    print("LINKS SMOKE: " + ("ALL PASS" if not failures else f"{len(failures)} FAILURE(S)"))
    if failures: sys.exit(1)
finally:
    proc.terminate()
    try: proc.wait(timeout=3)
    except Exception: proc.kill()
    restore_app_config()
