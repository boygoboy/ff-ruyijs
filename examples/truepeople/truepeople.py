"""TruePeopleSearch（Cloudflare 5s 盾 + Turnstile sitekey 0x4AAAAAAAmywfqBst8n7ro5）：
ruyijs+necko 原生环境通过托管质询并进入首页。

python truepeople.py
python truepeople.py --proxy http://127.0.0.1:7897 --output truepeople.json
python truepeople.py --direct

流程（与 Windows 侧实测一致）：navigate → 初始 403 质询页 → 引擎执行质询 JS
→ challenge POST 200（下发 cf_clearance）→ /InternalCaptcha 302 → 首页 200。
阶段判定全部取自 net_log 与页面自身状态：脚本不注入、不替换任何 API。

⚠️ 指纹之外还依赖出口 IP 信誉：被循环质询时先换 --proxy 出口，再怀疑环境。
"""

import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
if (ROOT / "dist" / ("js.exe" if os.name == "nt" else "js")).is_file() or os.environ.get("RUYIJS_SHELL"):
    sys.path.insert(0, str(ROOT / "python"))
from ruyijs import Context, JSError

URL = "https://www.truepeoplesearch.com/InternalCaptcha"
SITEKEY = "0x4AAAAAAAmywfqBst8n7ro5"
TITLE_MARK = "TruePeopleSearch"
CHALLENGE_MARK = "/cdn-cgi/challenge-platform/"
HOME = "https://www.truepeoplesearch.com/"


def auto_proxy():
    for name in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        if os.environ.get(name):
            return os.environ[name]
    for port in (7897, 7890):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.3):
                return f"http://127.0.0.1:{port}"
        except OSError:
            continue
    return None


def read_state(context):
    return context.eval("""(() => ({
      ready: document.readyState,
      title: document.title,
      url: location.href,
      sitekey: document.documentElement.innerHTML.indexOf('SITEKEY') >= 0,
      turnstile: !!document.querySelector('iframe[src*="challenges.cloudflare.com"]'),
      cfCookie: document.cookie.indexOf('cf_clearance') >= 0
    }))()""".replace("SITEKEY", SITEKEY))


def classify(log, home_paths=("/InternalCaptcha", "/")):
    """把 net_log 归约成截图同构的四阶段。"""
    stages = {"initial_403": None, "challenge_post": None, "redirect_302": None, "homepage_200": None}
    for row in log:
        path = urlsplit(row["url"]).path
        if path == "/InternalCaptcha" or path in home_paths:
            if row["status"] == 403 and stages["initial_403"] is None:
                stages["initial_403"] = row["url"]
            elif row["status"] == 302 and stages["redirect_302"] is None:
                stages["redirect_302"] = row["url"]
            elif row["status"] == 200 and stages["homepage_200"] is None:
                stages["homepage_200"] = row["url"]
        if CHALLENGE_MARK in row["url"] and row["method"] == "POST" and stages["challenge_post"] is None:
            stages["challenge_post"] = row["url"]
    return stages


def check_exit(proxy, url=URL):
    """2 秒预检出口等级：封锁 / 质询 / 放行。换节点后先跑这个再决定是否全流程。"""
    import urllib.request
    handler = urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {})
    opener = urllib.request.build_opener(handler)
    request = urllib.request.Request(url,
                                     headers={"User-Agent": "Mozilla/5.0", "Accept": "text/html"})
    try:
        with opener.open(request, timeout=30) as response:
            body = response.read(4096).decode("utf-8", "replace")
            status = response.status
    except urllib.error.HTTPError as error:
        status = error.code
        body = error.read(4096).decode("utf-8", "replace")
    except TimeoutError:
        return "exit_slow（30s 握手/响应超时，出口很慢但可能可用）" 
    except OSError as error:
        return f"exit_unreachable（{error}）"
    if status == 403 and ("Attention Required" in body or "error code: 10" in body):
        return "blocked（IP 被 Cloudflare 直接封锁，换出口）"
    if status == 403 and ("Captcha Challenge" in body or "challenge-platform" in body):
        return "site_captcha（站点验证页 403，正常入口，跑全流程）"
    if status == 403 and ("Just a moment" in body or "challenge-platform" in body):
        return "challenge（质询路径，可以跑全流程）"
    if status == 200:
        return "clean（直连放行）"
    return f"other（HTTP {status}）"


def run(args, report):
    deadline = time.monotonic() + args.timeout
    proxy = None if args.direct else (args.proxy or auto_proxy())
    report["proxy"] = proxy
    with Context(proxy=proxy or None, timeout=args.timeout) as context:
        def call(method, *values, **kwargs):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("等待页面超时")
            context.timeout = max(1, remaining)
            return method(*values, **kwargs)

        def advance(ms=250):
            try:
                call(context.advance, ms, max_tasks=500)
            except JSError as error:
                # 质询期会有探测脚本被终止之类的噪声（实测 ExecutionTerminated），容忍不中断。
                entry = {"name": error.name, "message": error.message[:300]}
                if entry not in report["scriptErrors"] and len(report["scriptErrors"]) < 10:
                    report["scriptErrors"].append(entry)

        return_path = urlsplit(urlsplit(args.url).query if "returnUrl=" in args.url else "").path
        home_paths = ("/InternalCaptcha", "/", return_path) if return_path else ("/InternalCaptcha", "/")
        # 与真 Firefox 159/mac 的环境偏好一致（质询 VM 的 matchMedia 探针实测项）。
        call(context.configure_fingerprint,
             {"mediaFeatures": {"prefersColorScheme": "dark"}})
        call(context.configure_clock, "system")
        t = time.monotonic()
        call(context.navigate, args.url)
        report["navigateSeconds"] = round(time.monotonic() - t, 2)

        stall_signature, stall_since, re_navigated = None, None, False
        while time.monotonic() < deadline:
            advance()
            state = call(lambda: read_state(context))
            report["flow"] = classify(call(context.net_log), home_paths)
            # 质询页自己的标题也带站名；必须等 homepage_200 且不在 captcha 页才算到首页。
            if (report["flow"].get("homepage_200") and state.get("ready") == "complete"
                    and TITLE_MARK in str(state.get("title"))
                    and "captcha" not in str(state.get("title")).lower()):
                report["finalTitle"] = state["title"]
                report["finalUrl"] = state["url"]
                report["sitekeySeen"] = state.get("sitekey")
                report["turnstileWidget"] = state.get("turnstile")
                report["cfClearanceCookie"] = state.get("cfCookie")
                break
            # 无进展早退：净日志条数与页面状态连续 20s 零变化，说明质询已停滞。
            signature = (len(call(context.net_log)), str(state.get("title")), str(state.get("url")))
            if signature == stall_signature:
                if stall_since is None:
                    stall_since = time.monotonic()
                elif time.monotonic() - stall_since > 20:
                    # api.js 间歇性加载失败（实测 2/5）会让 turnstile 全局缺失、
                    # widget 永不渲染：重导航直到 API 就绪（各一次机会）。
                    api_missing = call(context.eval, "typeof turnstile") != "object"
                    if api_missing and not re_navigated:
                        re_navigated = True
                        stall_signature, stall_since = None, None
                        call(context.navigate, args.url)
                        continue
                    if not re_navigated and report["flow"].get("challenge_post"):
                        # 质询 POST 已 200 但页面停在原地：cf_clearance 已进罐时，
                        # 一次新 GET 走的就是截图流程的 302 → 首页跳（仅补一次）。
                        re_navigated = True
                        stall_signature, stall_since = None, None
                        call(context.navigate, args.url)
                        continue
                    report["lastState"] = {k: state.get(k) for k in ("ready", "title", "url") if k in state}
                    raise TimeoutError(f"质询 20s 无进展（净日志 {signature[0]} 条，页面未变化）")
            else:
                stall_signature, stall_since = signature, None
        else:
            report["lastState"] = {k: state.get(k) for k in ("ready", "title", "url") if k in state}
            raise TimeoutError("预算内未到达首页")

        stages = report["flow"]
        posts = [row for row in call(context.net_log)
                 if CHALLENGE_MARK in row["url"] and row["method"] == "POST"]
        report["challengePosts"] = [row["status"] for row in posts]
        if stages["initial_403"] and stages["homepage_200"]:
            report["passed"] = True
            report["stage"] = "passed_after_challenge_redirect"
        elif stages["homepage_200"]:
            report["passed"] = True
            report["stage"] = "passed_without_challenge"
        elif stages["challenge_post"] and any(row["status"] == 0 for row in posts):
            # 边缘冷却特征：首次 POST 200，重试 POST 网络层死亡（status 0）。
            # Cloudflare 边缘对每 IP 有 handshake-flood 冷却；等几分钟或换出口。
            report["stage"] = "edge_cooldown_suspected"
            report["advice"] = "等待几分钟后重试，或更换代理出口（同 IP 连续质询会进入边缘冷却）"
        else:
            report["passed"] = False
            report["stage"] = "incomplete"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default=URL)
    parser.add_argument("--timeout", type=float, default=150, help="单次尝试期限，秒")
    parser.add_argument("--retries", type=int, default=2,
                        help="质询未放行时的新会话重试次数（Cloudflare 逐请求判定有波动）")
    parser.add_argument("--proxy", help="HTTP(S) 代理 URL（默认 env，再探测本地端口）")
    parser.add_argument("--direct", action="store_true", help="不使用代理")
    parser.add_argument("--output", type=Path, help="把 JSON 结果同时写入此文件")
    parser.add_argument("--check-exit", action="store_true",
                        help="只做出口预检（封锁/质询/放行），不跑全流程")
    args = parser.parse_args(argv)
    if args.check_exit:
        proxy = None if args.direct else (args.proxy or auto_proxy())
        print(f"出口 {proxy or 'direct'}: {check_exit(proxy, args.url)}")
        return 0

    report = {"passed": False, "stage": "not-run", "transport": "ruyijs-page+gecko",
              "sitekey": SITEKEY, "scriptErrors": []}
    for attempt in range(1, args.retries + 2):
        report["attempt"] = attempt
        try:
            run(args, report)
        except BaseException as error:
            report.update(passed=False, error=type(error).__name__ + ": " + str(error))
        if report["passed"]:
            break
        if attempt <= args.retries:
            report.setdefault("attempts", []).append(
                {"attempt": attempt, "stage": report["stage"], "error": report.get("error")})
            report["passed"], report["stage"], report["error"] = False, "not-run", None
            report["flow"], report["scriptErrors"] = {}, []
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
