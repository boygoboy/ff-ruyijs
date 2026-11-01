"""双端信号采集：真 Firefox 159（geckodriver headless） vs ruyijs 引擎，同款 JS。"""
import json
import sys

COLLECT = r"""
(() => {
  const out = {};
  out.nav = {};
  for (const k of ["userAgent","platform","oscpu","buildID","appVersion","language","languages",
                   "hardwareConcurrency","maxTouchPoints","product","productSub","vendor",
                   "doNotTrack","pdfViewerEnabled","cookieEnabled","webdriver"]) {
    try { out.nav[k] = String(navigator[k]); } catch(e) { out.nav[k] = "ERR"; }
  }
  out.screen = {w: screen.width, h: screen.height, aw: screen.availWidth, ah: screen.availHeight,
                dpr: devicePixelRatio, cd: screen.colorDepth};
  out.tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
  out.locale = Intl.DateTimeFormat().resolvedOptions().locale;
  out.numFormat = new Intl.NumberFormat().format(12345.678);
  out.err = {};
  try { null.x; } catch(e) { out.err.typeError = e.message; }
  try { JSON.parse('{'); } catch(e) { out.err.syntax = e.message; }
  try { document.querySelectorAll(':x'); } catch(e) {
    out.err.domException = {name: e.name,
      members: Object.getOwnPropertyNames(Object.getPrototypeOf(e)).sort().join(',')};
  }
  out.math = {expm1: Math.expm1(1), sinh: Math.sinh(1), acos: Math.acos(0.5), pow: Math.pow(2, 53)};
  const c = document.createElement('canvas'); c.width = 240; c.height = 60;
  const g = c.getContext('2d');
  g.textBaseline = 'alphabetic';
  g.fillStyle = '#f60'; g.fillRect(100, 5, 80, 20);
  g.fillStyle = '#069'; g.font = '14px Arial'; g.fillText('Cwm fjordbank glyphs vext quiz', 2, 18);
  g.fillStyle = 'rgba(102,204,0,0.7)'; g.font = '18px Arial'; g.fillText('Cwm fjordbank glyphs vext quiz', 4, 30);
  g.beginPath(); g.arc(50, 40, 12, 0, Math.PI*2); g.strokeStyle = '#f00'; g.stroke();
  out.canvas2d = c.toDataURL().slice(-72);
  out.metrics = g.measureText('Cwm fjordbank glyphs vext quiz').width;
  const wc = document.createElement('canvas'); wc.width = 120; wc.height = 80;
  const gl = wc.getContext('webgl');
  if (gl) {
    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    out.webgl = {
      vendor: gl.getParameter(gl.VENDOR), renderer: gl.getParameter(gl.RENDERER),
      version: gl.getParameter(gl.VERSION),
      unmaskedVendor: dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : null,
      unmaskedRenderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : null,
      maxTex: gl.getParameter(gl.MAX_TEXTURE_SIZE)
    };
  }
  out.fonts = (() => {
    const base = ['monospace', 'sans-serif', 'serif'];
    const cands = ['Arial','Helvetica','Times New Roman','Courier New','Georgia','Verdana',
                   'Comic Sans MS','Segoe UI','Microsoft YaHei','PingFang SC','Hiragino Sans GB',
                   'Menlo','Monaco','SF Pro Text'];
    const s = document.createElement('span');
    s.style.cssText = 'position:absolute;left:-9999px;font-size:72px';
    s.textContent = 'mmmmmmmmmmlli';
    document.body.appendChild(s);
    const w = {};
    for (const b of base) { s.style.fontFamily = b; w[b] = s.offsetWidth; }
    const found = [];
    for (const f of cands) {
      let hit = false;
      for (const b of base) {
        s.style.fontFamily = "'" + f + "'," + b;
        if (s.offsetWidth !== w[b]) { hit = true; break; }
      }
      if (hit) found.push(f);
    }
    s.remove();
    return found;
  })();
  return JSON.stringify(out);
})()
"""

def collect_firefox():
    from selenium import webdriver
    from selenium.webdriver.firefox.options import Options
    opts = Options()
    opts.binary_location = "/Users/mac/Projects/PycharmProjects/ff-ruyijs/vendor/firefox-downloads/Firefox159.app/Contents/MacOS/firefox"
    opts.add_argument("-headless")
    driver = webdriver.Firefox(options=opts)
    try:
        driver.get("about:blank")
        import time; time.sleep(1)
        raw = driver.execute_script(
            "const src = arguments[0]; try { return eval(src); } catch(e) { return JSON.stringify({COLLECT_ERROR: String(e && (e.message || e)) + ' @ ' + ((e && e.stack) || '').slice(0, 160)}); }",
            COLLECT)
        data = json.loads(raw)
        if "COLLECT_ERROR" in data:
            raise RuntimeError("页内异常: " + data["COLLECT_ERROR"])
        if isinstance(data, dict) and "COLLECT_ERROR" in data:
            raise RuntimeError("页内异常: " + data["COLLECT_ERROR"])
        return data
    finally:
        driver.quit()

def collect_ruyijs():
    sys.path.insert(0, "/Users/mac/Projects/PycharmProjects/ff-ruyijs/python")
    from ruyijs import Context
    with Context(timeout=60) as context:
        context.load_page("https://example.com/", "<html><head></head><body></body></html>")
        return json.loads(context.eval(COLLECT))

def diff(a, b, path=""):
    lines = []
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            lines += diff(a.get(k), b.get(k), f"{path}.{k}" if path else k)
    elif a != b:
        lines.append(f"  {path}:\n    FF159: {str(a)[:90]}\n    ruyijs: {str(b)[:90]}")
    return lines

if __name__ == "__main__":
    ff = collect_firefox()
    rj = collect_ruyijs()
    json.dump({"firefox159": ff, "ruyijs": rj}, open("/tmp/signals.json", "w"), ensure_ascii=False, indent=1)
    lines = diff(ff, rj)
    print(f"差异 {len(lines)//1} 项（含多行）：")
    print("\n".join(lines) if lines else "完全一致")
