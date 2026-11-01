"""CreepJS（abrahamjuliot.github.io/creepjs）：ruyijs+necko 原生环境跑完整指纹检测并给出结果。

python creepjs.py
python creepjs.py --proxy http://127.0.0.1:7890 --output creepjs.json
python creepjs.py --direct

流程（与 Firefox trace jscall-detail-desktop-20261022-181949 一致）：页面加载 creep.js，脚本在主文档和一个同源
iframe（PHANTOM_DARKNESS）里依次检测 window、computed style、css media、math、timezone、fonts、intl、speech、
svg、rects、webgl、audio、worker、canvas 2d、resistance、navigator、headless、features，算出 FP ID 与 fuzzy hash。
各项是否通过取自页面自己的 console.log（"✔ (… ms) xxx passed" / "- xxx failed"），FP ID 取自页面渲染结果。
脚本不注入、不替换浏览器 API。

退出码 0 表示指纹计算完成、两个指纹（loose/stable）都通过，且没有失败项（与 Firefox 一致，含 service worker）。
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import sys
from time import monotonic

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
if (ROOT / "dist" / "js.exe").is_file() or (ROOT / "dist" / "js").is_file() or os.environ.get("RUYIJS_SHELL"):
    sys.path.insert(0, str(ROOT / "python"))
from ruyijs import Context, JSError

URL = "https://abrahamjuliot.github.io/creepjs/"
KNOWN_GAPS = set()
# Sections Firefox 158 passes in the trace; "worker" is whichever worker scope creepjs ended up using.
FIREFOX = ["window", "computed style", "css media", "math", "console errors", "timezone", "media", "fonts", "intl",
           "speech", "rects", "svg", "webgl", "audio", "service worker", "canvas 2d", "resistance", "navigator",
           "headless", "features"]
RESULT = re.compile(r"^%c(?:✔ (?:\(([\d.]+)ms\) )?(.+?) passed|- (.+?) failed)\b")
PAGE = """(() => { const h = document.querySelector('.fingerprint-header');
  const text = h ? h.textContent.replace(/\\s+/g, ' ').trim() : '';
  const id = text.match(/FP ID: ([0-9a-f]{64})/), fuzzy = text.match(/Fuzzy: ([0-9a-f]+)/);
  return {fpId: id ? id[1] : null, fuzzy: fuzzy ? fuzzy[1] : null,
          sections: document.querySelectorAll('#fingerprint-data strong').length}; })()"""


def auto_proxy():
    for name in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        if os.environ.get(name):
            return os.environ[name]
    try:
        with socket.create_connection(("127.0.0.1", 7890), timeout=.3):
            return "http://127.0.0.1:7890"
    except OSError:
        return None


def read_log(context, report):
    """Folds the page's console records into the report; returns True once both fingerprints are done."""
    for record in context.console_log(clear=True):
        text = record.get("text", "")
        match = RESULT.match(text)
        if match:
            if match.group(3):
                report["failed"].append(match.group(3))
            else:
                report["passed"][match.group(2)] = float(match.group(1)) if match.group(1) else None
        elif text.startswith("Fingerprinting complete in "):
            report["fingerprintMs"] = float(text.split()[-1].rstrip("ms"))
        elif record.get("level") == "error" and len(report["consoleErrors"]) < 20:
            report["consoleErrors"].append(text[:300])
    return "loose fingerprint" in report["passed"] and "stable fingerprint" in report["passed"] or \
        "loose fingerprint" in report["failed"] or "stable fingerprint" in report["failed"]


def run(args, report):
    deadline = monotonic() + args.timeout
    with Context(proxy=report["proxy"], timeout=60) as context:
        context.configure_clock("system")
        context.navigate(URL)
        done = False
        while not done and monotonic() < deadline:
            try:
                context.advance(250, max_tasks=500)
            except JSError as error:
                if len(report["scriptErrors"]) < 20:
                    report["scriptErrors"].append(error.message[:300])
            done = read_log(context, report)
        context.advance(500, max_tasks=500)  # the page renders its report after hashing
        read_log(context, report)
        report.update(context.eval(PAGE))
    report["failed"] = sorted(set(report["failed"]))
    ran = set(report["passed"]) | set(report["failed"])
    report["notRun"] = [name for name in FIREFOX if name not in ran and not (
        name == "service worker" and ("dedicated worker" in ran or "shared worker" in ran))]
    report["passed"] = dict(sorted(report["passed"].items()))
    report["ok"] = bool(report["fpId"]) and "loose fingerprint" in report["passed"] and \
        "stable fingerprint" in report["passed"] and set(report["failed"]) <= KNOWN_GAPS


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    proxy = parser.add_mutually_exclusive_group()
    proxy.add_argument("--proxy", help="HTTP(S) 代理，默认环境变量/本机 7890/直连")
    proxy.add_argument("--direct", action="store_true", help="直接连接")
    parser.add_argument("--timeout", type=float, default=60, help="总期限，秒")
    parser.add_argument("--output", type=Path, help="将 JSON 结果同时写入此文件")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout 必须是正有限数")
    report = {"ok": False, "transport": "ruyijs-page+necko-fetch", "proxy": None if args.direct else (args.proxy or auto_proxy()), "fpId": None,
              "fuzzy": None, "fingerprintMs": None, "passed": {}, "failed": [], "knownGaps": sorted(KNOWN_GAPS),
              "consoleErrors": [], "scriptErrors": []}
    started = monotonic()
    try:
        run(args, report)
    except Exception as error:
        report.update(ok=False, error=type(error).__name__ + ": " + str(error))
    report["elapsedSeconds"] = round(monotonic() - started, 3)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", "utf-8")
    print(text)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
