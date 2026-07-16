#!/usr/bin/env python3
"""WS2 smoke on the integrated main build: schema v22 + Obsidian import +
idempotency + text embeddings, against the real vault (READ-ONLY)."""
import json, os, shutil, sqlite3, subprocess, sys, time
import urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(ROOT)
APP = os.path.join(WT, "bin/TrussPhoto.app/Contents/MacOS/TrussPhoto")
VAULT = "/Users/toru/Nextcloud/Obsidian/QuickMemo"
CAT = f"{ROOT}/CatWS2"
MCP = 18852

shutil.rmtree(CAT, ignore_errors=True)
os.makedirs(CAT)

out = subprocess.run(["lsof", "-ti", f":{MCP}"], capture_output=True, text=True).stdout
for pid in out.split():
    subprocess.run(["kill", "-9", pid])

logf = open(f"{ROOT}/ws2_smoke.log", "w")
proc = subprocess.Popen([APP, "--catalog", CAT],
                        env={**os.environ, "TRUSSC_MCP": "1", "TRUSSC_MCP_PORT": str(MCP)},
                        stdout=logf, stderr=logf)

reqid = 0
def rpc(method, params=None, timeout=120):
    global reqid
    reqid += 1
    msg = {"jsonrpc": "2.0", "id": reqid, "method": method}
    if params is not None: msg["params"] = params
    req = urllib.request.Request(f"http://localhost:{MCP}/mcp",
        data=json.dumps(msg).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())

def call(name, args=None, timeout=120):
    r = rpc("tools/call", {"name": name, "arguments": args or {}}, timeout)
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
    for i in range(30):
        try:
            rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "smoke", "version": "1"}})
            break
        except Exception:
            time.sleep(1)
    time.sleep(2)

    r1 = call("import_obsidian", {"path": VAULT})
    print(f"import 1: {r1}")
    check("schema v22", q("PRAGMA user_version")[0][0] == 22)
    n_text = q("SELECT COUNT(*) FROM photos WHERE entry_type=1")[0][0]
    check(f"text entries imported ({n_text})", n_text >= 70)
    n_gps = q("SELECT COUNT(*) FROM photos WHERE entry_type=1 AND latitude != 0")[0][0]
    check(f"text entries with GPS ({n_gps})", n_gps >= 50)
    check("added>0 on first import", r1.get("added", 0) >= 70)

    r2 = call("import_obsidian", {})
    print(f"import 2: {r2}")
    n_text2 = q("SELECT COUNT(*) FROM photos WHERE entry_type=1")[0][0]
    check(f"idempotent (count {n_text}->{n_text2}, added={r2.get('added')})",
          n_text2 == n_text and r2.get("added", -1) == 0)

    time.sleep(20)  # text embedding generation
    n_emb = q("SELECT COUNT(*) FROM embeddings WHERE source='text'")[0][0]
    check(f"text embeddings stored ({n_emb})", n_emb >= 70)

    memoPaths = json.load(open(f"{CAT}/catalog.json")).get("memoImportPaths", [])
    check(f"memoImportPaths persisted ({memoPaths})", VAULT in memoPaths)

    print("=" * 40)
    print("WS2 SMOKE: " + ("ALL PASS" if not failures else f"{len(failures)} FAILURE(S)"))
    if failures: sys.exit(1)
finally:
    proc.terminate()
    try: proc.wait(timeout=3)
    except Exception: proc.kill()
