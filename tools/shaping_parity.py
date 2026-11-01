"""Shaping parity: run one identical JS probe under two engines and diff.

Engines:
  real     real Firefox 159 on this host, driven through geckodriver (WebDriver)
  ruyijs   this tree's engine (RUYIJS_SHELL or dist/js)

The probe measures per-character and whole-string canvas text metrics across
a font/sample matrix, letter-spacing cases, and canvas ink footprints. Numbers
are quantized to 1/60 CSS pixel (app units), the granularity both engines
round to, so parity means exact integer equality.

Usage:
  .venv/bin/python -I tools/shaping_parity.py            # collect + diff
  .venv/bin/python -I tools/shaping_parity.py --collect  # write JSON only
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PROBE = r"""
(() => {
  const q = v => v == null ? null : Math.round(v * 60) / 60;
  const c = document.createElement('canvas'); c.width = 600; c.height = 220;
  const g = c.getContext('2d');
  const SAMPLES = ['Hello world', 'CJK \u6d4b\u8bd5\u4e2d\u6587\u6587\u672c',
    '\ufb01 ligature fi ffi', 'emoji \ud83d\ude42\ud83d\udc4d\ud83c\udffd mixed',
    'mixed CJK+Latin \u6df7\u6392 Aa', '\u2026ellipsis\u2026 \u00d7\u00ae\u00a9',
    'AVTo Wa kerning', 'iiiiiiiiii iiiiiiiiii'];
  const FONTS = ['14px Arial', '14px Helvetica', '16px Times New Roman',
    '14px Hiragino Sans GB', '14px PingFang SC', 'italic 14px Georgia',
    'bold 14px Arial', 'bold 14px Hiragino Sans GB'];
  const out = {fonts: {}, spacing: {}, pixels: {}, baseline: {}};
  for (const f of FONTS) {
    g.font = f;
    out.fonts[f] = {};
    for (const s of SAMPLES) {
      const widths = [];
      for (const ch of s) widths.push(q(g.measureText(ch).width));
      const m = g.measureText(s);
      out.fonts[f][s] = {chars: widths, full: q(m.width),
        ascent: q(m.actualBoundingBoxAscent), descent: q(m.actualBoundingBoxDescent),
        left: q(m.actualBoundingBoxLeft), right: q(m.actualBoundingBoxRight)};
    }
  }
  for (const ls of ['0px', '2px', '-1px', '0.5px']) {
    try {
      g.letterSpacing = ls; g.font = '14px Arial';
      out.spacing[ls] = q(g.measureText('Hello CJK \u6df7\u6392').width);
    } catch (e) { out.spacing[ls] = 'ERR ' + e.name; }
  }
  try { g.letterSpacing = '0px'; } catch (e) {}
  const paint = (code) => {
    g.clearRect(0, 0, 600, 220); g.fillStyle = '#000'; g.strokeStyle = '#000';
    g.font = '14px Arial'; eval(code);
    const img = g.getImageData(0, 0, 600, 220).data;
    let ink = 0, minX = 600, maxX = -1, minY = 220, maxY = -1;
    for (let y = 0; y < 220; ++y) for (let x = 0; x < 600; ++x) {
      if (img[(y * 600 + x) * 4 + 3] !== 0) { ++ink;
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y; }
    }
    return {ink, box: maxX < 0 ? null : [minX, minY, maxX, maxY]};
  };
  out.pixels.fill14 = paint('g.fillText("Hello CJK \\u6df7\\u6392", 5, 30);');
  out.pixels.bold16 = paint('g.font = "bold 16px Georgia"; g.fillText("Wavy 2.0 Wg", 5, 90);');
  out.pixels.cjk18 = paint('g.font = "18px PingFang SC"; g.fillText("\\u6c38\\u4e1c\\u4e0d\\u591c\\u57ce", 5, 150);');
  for (const f of ['14px Arial', '16px Times New Roman', '14px PingFang SC']) {
    g.font = f;
    const m = g.measureText('Mg');
    out.baseline[f] = {alphabetic: q(m.fontBoundingBoxAlphabetic),
      hanging: q(m.fontBoundingBoxHanging), ideographic: q(m.fontBoundingBoxIdeographic),
      em: q(m.fontBoundingBoxAscent + m.fontBoundingBoxDescent)};
  }
  return out;
})()
"""


def ruyijs_collect(shell):
    sys.path.insert(0, str(ROOT / "python"))
    from ruyijs import Context
    with Context(timeout=60) as ctx:
        ctx.configure_clock("system")
        return ctx.eval(PROBE)


class WebDriver:
    """Minimal geckodriver client: session, data: page, one script eval."""

    def __init__(self, binary, firefox_binary=None):
        self.proc = subprocess.Popen(
            [binary, "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.port = None
        while self.port is None:
            line = self.proc.stdout.readline().decode(errors="replace")
            if not line:
                raise RuntimeError("geckodriver exited before announcing a port")
            if "istening on" in line:
                self.port = int(line.rstrip().rsplit(":", 1)[1])
        self.base = f"http://127.0.0.1:{self.port}"
        firefox = {"args": ["-headless"]}
        if firefox_binary:
            firefox["binary"] = str(firefox_binary)
        self.session = self._post("/session", {"capabilities": {
            "alwaysMatch": {"browserName": "firefox",
                            "moz:firefoxOptions": firefox}}})["value"]["sessionId"]

    def _post(self, path, payload):
        request = urllib.request.Request(
            self.base + path, json.dumps(payload).encode(), {"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read())

    def eval(self, script):
        self._post(f"/session/{self.session}/url", {"url": "data:text/html,<title>p</title>"})
        result = self._post(f"/session/{self.session}/execute/sync",
                            {"script": f"return ({script})", "args": []})
        return result["value"]

    def close(self):
        try:
            request = urllib.request.Request(self.base + f"/session/{self.session}",
                                             method="DELETE")
            urllib.request.urlopen(request, timeout=60)
        except OSError:
            pass
        finally:
            self.proc.terminate()


def flatten(node, path=""):
    if isinstance(node, dict):
        for key, value in node.items():
            yield from flatten(value, f"{path}.{key}" if path else key)
    else:
        yield path, node


def same(va, vb):
    if isinstance(va, (int, float)) and isinstance(vb, (int, float)):
        return abs(va - vb) < 1e-9
    if isinstance(va, list) and isinstance(vb, list) and len(va) == len(vb):
        return all(same(x, y) for x, y in zip(va, vb))
    return va == vb


def diff(real, engine):
    rows = []
    a = dict(flatten(real))
    b = dict(flatten(engine))
    for key in sorted(set(a) | set(b)):
        va, vb = a.get(key, "<missing>"), b.get(key, "<missing>")
        if not same(va, vb):
            rows.append((key, va, vb))
    return rows


def classify(rows):
    """Bucket differences by what produced them."""
    buckets = {}
    for key, va, vb in rows:
        if ".pixels." in key:
            name = "canvas ink footprint"
        elif ".spacing." in key:
            name = "letter-spacing"
        elif ".chars" in key or ".full" in key:
            name = "shaped advances"
        else:
            name = "metrics semantics (ink/baseline bounds)"
        buckets.setdefault(name, []).append((key, va, vb))
    return buckets


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--collect", action="store_true", help="write JSON only, no diff")
    parser.add_argument("--geckodriver", default=os.environ.get("GECKODRIVER", "geckodriver"))
    parser.add_argument("--firefox-binary",
                        default=os.environ.get(
                            "RUYIJS_FIREFOX_BIN",
                            str(ROOT / "vendor/firefox-downloads/Firefox159.app/Contents/MacOS/firefox")))
    parser.add_argument("--shell", default=os.environ.get("RUYIJS_SHELL", str(ROOT / "dist/js")))
    parser.add_argument("--out", type=Path, default=ROOT / "artifacts/shaping_parity")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    print(f"collecting real Firefox via {args.geckodriver} …", flush=True)
    driver = WebDriver(args.geckodriver, args.firefox_binary)
    try:
        real = driver.eval(PROBE)
    finally:
        driver.close()
    (args.out / "real.json").write_text(json.dumps(real, indent=1))

    print(f"collecting ruyijs via {args.shell} …", flush=True)
    engine = ruyijs_collect(args.shell)
    (args.out / "ruyijs.json").write_text(json.dumps(engine, indent=1))

    if args.collect:
        return 0
    rows = diff(real, engine)
    total = len(list(flatten(real)))
    print(f"\n{total - len(rows)}/{total} leaf values identical")
    if rows:
        buckets = classify(rows)
        print(f"{len(rows)} DIFFER, by class:")
        for name, items in sorted(buckets.items()):
            print(f"\n[{name}] {len(items)}")
            for key, va, vb in items[:8]:
                print(f"  {key}: real={va} ruyijs={vb}")
            if len(items) > 8:
                print(f"  … and {len(items) - 8} more")
        return 1
    print("PARITY: every measured value matches real Firefox 159")
    return 0


if __name__ == "__main__":
    sys.exit(main())
