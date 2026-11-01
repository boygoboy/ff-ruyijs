"""DataDome interstitial device-check：ruyijs 跑通 plv2 + plv3 全流程并拿到首页。

python datadome.py
python datadome.py --proxy http://127.0.0.1:7890 --output result.json
python datadome.py --direct

流程：
  首页 403 → DataDome 注入 interstitial iframe（device-check 模式）
  → interstitial.js 采集 228+ 个环境字段（文档 plv2 字段全表）
  → 模块 516 生成 plv2（djb2 + xorshift32 + 双 XOR 流 + 自定义 base64）
  → 模块 872 内层 VM 生成 plv3（字节码 VM + 仿射变换 + 查表 XOR + base64url）
  → POST /interstitial/ 提交 payload(plv2) + plv3
  → 服务端返回 datadome cookie
  → 携 cookie 重新请求首页 → 200 + 完整 HTML

退出码 0 表示首页 200 拿到。

参考文档：
  DataDome plv2(payload).md      — plv2 算法完整闭合
  DataDome plv3(内层VM).md       — plv3 字节码 VM 逆向
  plv2 字段全表(228).md          — 228 个采集字段溯源
  DataDome 环境检测清单.md       — ruyijs 需要实现的浏览器 API 清单
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import sys
from time import monotonic
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
if (ROOT / "dist" / "js.exe").is_file() or (ROOT / "dist" / "js").is_file() or os.environ.get("RUYIJS_SHELL"):
    sys.path.insert(0, str(ROOT / "python"))
from ruyijs import Context, JSError  # noqa: E402

HOME = "https://www.mouser.cn/"
FRAME = "document.querySelector('iframe[src*=\"captcha-delivery\"]')"
ENVIRONMENT = {"screen": {"width": 1920, "height": 1080, "availWidth": 1920, "availHeight": 1040},
               "viewport": {"width": 1904, "height": 987}, "locale": {"timezone": "Asia/Shanghai"}}

READ_INTERSTITIAL = FRAME + """&&(()=>{
  const f=""" + FRAME + """,d=f.contentDocument,w=f.contentWindow;
  return {bodyClass:d.body&&d.body.className, title:d.title,
    ddm:!!w.ddm, interstitialCallback:typeof w.interstitialCallback,
    cid:w.ddm&&w.ddm.cid&&w.ddm.cid.slice(0,20)+'...'}})()"""


def auto_proxy():
    for name in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        if os.environ.get(name):
            return os.environ[name]
    try:
        with socket.create_connection(("127.0.0.1", 7890), timeout=0.3):
            return "http://127.0.0.1:7890"
    except OSError:
        return None


def main(argv=None):
    # A Chinese Windows console defaults to GBK; the help and log lines are UTF-8.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    proxy_group = parser.add_mutually_exclusive_group()
    proxy_group.add_argument("--proxy", help="HTTP(S) 代理；默认自动检测")
    proxy_group.add_argument("--direct", action="store_true", help="不使用代理直连")
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("datadome_result.json"),
                        help="JSON 结果输出路径")
    args = parser.parse_args(argv)
    selected = None if args.direct else (args.proxy or auto_proxy())

    report = {"status": "error", "proxy": selected or "direct", "steps": [], "scriptErrors": [],
              "payload": None, "plv3": None, "homepage": None}
    started = monotonic()

    def ts():
        return round(monotonic() - started, 2)

    def log(stage, **kw):
        entry = {"stage": stage, "t": ts(), **kw}
        report["steps"].append(json.loads(json.dumps(entry, default=str)))
        line = json.dumps(entry, ensure_ascii=False, default=str)
        print(line[:600], flush=True)

    print(f"[*] proxy={selected or 'direct'}", flush=True)

    with Context(timeout=90, proxy=selected, environment=ENVIRONMENT) as context:
        def run(ms=50, tasks=200):
            try:
                context.advance(ms, max_tasks=tasks)
            except JSError as error:
                if len(report["scriptErrors"]) < 20:
                    report["scriptErrors"].append(f"{error.name}: {error.message[:200]}")

        def ev(source):
            try:
                return context.eval(source)
            except JSError as error:
                return "JSERR " + str(error)[:200]

        # ── Step 1: navigate → DataDome 403 ──
        context.navigate(HOME)
        log("navigate", url=HOME)

        # ── Step 2: wait for interstitial iframe ──
        iframe_info = None
        for _ in range(200):
            run(200)
            state = ev(READ_INTERSTITIAL)
            if isinstance(state, dict) and state.get("ddm"):
                iframe_info = state
                break
        if not iframe_info:
            log("no_iframe")
            report["error"] = "DataDome interstitial iframe 未加载"
            raise SystemExit(1)
        log("interstitial", **iframe_info)

        # Read ddm (the three external inputs: hash, cid, Date.now)
        ddm = ev(f"""(()=>{{const d={FRAME}.contentWindow.ddm;
          return {{cid:d.cid,hash:d.hash,s:d.s,referer:d.referer}}}})()""")
        log("ddm", **(ddm if isinstance(ddm, dict) else {"raw": ddm}))

        # ── Step 3: wait for interstitial POST (plv2 + plv3) ──
        post_found = None
        for _ in range(300):
            run(300)
            posts = [r for r in context.net_log()
                     if r.get("method") == "POST" and "/interstitial/" in str(r.get("url", ""))
                     and "captcha-delivery" in str(r.get("url", ""))]
            if posts:
                post_found = posts[-1]
                break

        if not post_found:
            log("no_post", error="interstitial POST 未发出")
            report["error"] = "interstitial POST 未发出"
            raise SystemExit(1)

        body = str(post_found.get("requestBody", ""))
        params = parse_qs(body)
        payload = params.get("payload", [""])[0]
        plv3 = params.get("plv3", [""])[0]
        report["payload"] = payload
        report["plv3"] = plv3

        log("post", status=post_found.get("status"),
            payload_length=len(payload), plv3_length=len(plv3),
            payload_head=payload[:60] + "..." if len(payload) > 60 else payload,
            plv3_head=plv3[:60] + "..." if len(plv3) > 60 else plv3,
            other_params={k: v[0][:50] for k, v in params.items() if k not in ("payload", "plv3")})

        # ── Step 4: fetch homepage with datadome cookie ──
        cookie = str(ev("document.cookie") or "")
        dd_cookie = ""
        if "datadome=" in cookie:
            dd_cookie = cookie.split("datadome=")[-1].split(";")[0]
        log("cookie", datadome=dd_cookie[:40] + "..." if len(dd_cookie) > 40 else dd_cookie)

        # necko cookie jar already holds the datadome cookie — fetch with credentials
        ev(r"""
        fetch('https://www.mouser.cn/', {
            credentials: 'include',
            headers: {
                'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
                'Accept-Language': 'zh-CN,zh;q=0.9,en-US;q=0.8,en;q=0.7'
            }
        }).then(async r => {
            const text = await r.text();
            const m = text.match(/<title>([^<]*)<\/title>/);
            globalThis.__home = {
                status: r.status,
                length: text.length,
                title: m ? m[1].trim() : null,
                mouser: text.indexOf('Mouser Electronics') >= 0,
                head: text.slice(0, 500)
            };
        }, e => {
            globalThis.__home = {error: String(e)};
        });
        undefined
        """)

        home = None
        for _ in range(200):
            run(200)
            home = ev("globalThis.__home")
            if isinstance(home, dict):
                break

        if isinstance(home, dict) and home.get("status") == 200 and home.get("mouser"):
            report["status"] = "passed"
            report["homepage"] = {
                "status": home["status"],
                "length": home["length"],
                "title": home.get("title"),
            }
            log("homepage", status=200, length=home["length"], title=home.get("title"),
                head=home.get("head", "")[:200])
        elif isinstance(home, dict) and home.get("status") == 403:
            report["status"] = "submitted"
            report["homepage"] = {"status": 403}
            log("homepage", status=403, msg="datadome cookie 未放行，服务端要求更高验证")
        else:
            report["status"] = "submitted"
            log("homepage", raw=home)

        report["net"] = [{k: v for k, v in r.items() if k != "requestBody"}
                         for r in context.net_log()]
        report["console"] = context.console_log()[-20:]

    report["elapsedSeconds"] = round(monotonic() - started, 3)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1, default=str), "utf-8")

    ok = report["status"] == "passed"
    print(f"\n{'='*60}")
    print(f"[{'OK' if ok else 'PARTIAL'}] DataDome interstitial device-check")
    print(f"  plv2:      {len(report.get('payload') or '')} chars")
    print(f"  plv3:      {len(report.get('plv3') or '')} chars")
    hp = report.get("homepage") or {}
    if hp.get("status") == 200:
        print(f"  Homepage:  200  {hp.get('length')} bytes  {hp.get('title', '')}")
    else:
        print(f"  Homepage:  {hp.get('status', '?')}  (cookie 未通过服务端评分)")
    print(f"  Time:      {report['elapsedSeconds']}s")
    print(f"  Output:    {args.output}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
