# TruePeopleSearch（Cloudflare 5s 盾 + Turnstile）

ruyijs+gecko 原生环境通过 Cloudflare 托管质询（5s 盾）并进入首页：
Turnstile sitekey `0x4AAAAAAAmywfqBst8n7ro5`，目标 `/InternalCaptcha`。

```bash
python truepeople.py --proxy http://127.0.0.1:7897 --output truepeople.json
python truepeople.py --direct        # 直连（该站对部分地区的 IP 直接封锁，不推荐）
```

## 流程与判定

```
navigate → 403 质询页 → 引擎执行质询 JS → POST /cdn-cgi/challenge-platform/(200)
→ cf_clearance 下发 → /InternalCaptcha 302 → 首页 200 → passed_after_challenge_redirect
```

阶段判定全部来自 net_log 与页面自身状态，脚本不注入任何 API。出口 IP 干净时
也可能跳过质询直接 200（`passed_without_challenge`），同样算通过。

## 两个已知的环境变量（不是引擎问题）

1. **出口 IP 信誉**：被循环质询时先换 `--proxy` 出口。直连 CN 出口会被
   Cloudflare 直接 IP 封锁（"Attention Required!"，连质询都不给）。
2. **边缘冷却（edge cooldown）**：Cloudflare 边缘对每 IP 有 handshake-flood
   冷却，特征是首次质询 POST 200、重试 POST 网络层死亡（status 0）。脚本会把
   这种状态判为 `edge_cooldown_suspected`；等几分钟或换出口再试，不要连打。

## 20261031 专项调试结论（质询卡在 clearance）

排除法实测（同一引擎同一出口）：

- **网络层健康**：质询 POST 多次全 200（含 3 连 200），无 status 0 —— 不是
  TLS/H2/H3 或连接问题。
- **SW 运行时健康**：本地最小 Service Worker（install/skipWaiting/activate）
  在 `https://test.local/` 注册安装成功。站点的 service-worker.js 安装失败
  是它的脚本在质询态被 Cloudflare 拦截/内容不符，属红鲱鱼。
- **指纹档案变体均无效**：mac 基线（含 buildID 修正为 159 构建日期）、完整
  Windows 档案、只换 navigator —— POST 都 200 但边缘不下发 cf_clearance。
- **剩余嫌疑**：质询信号评分。mac 宿主的派生信号（canvas 像素=CoreText+Skia、
  字体度量）与所声明档案的渲染栈不同源；Windows 引擎（截图通过案例）的派生
  信号与声明完全自洽。下一步需要逐信号与真 Firefox 对照（canvas 哈希、字体
  枚举、时序），或接受"mac 宿主跑 Windows 指纹过 CF 托管质询"是派生面边界。
- 每次失败尝试都会加重该 IP 的评分损耗：调试期间务必间隔数分钟、控制次数。
- **MOZ_LOG 终审（nsHttp:5/socket:5 全量）**：net_log 里的 status 0 全部是
  NS_BINDING_ABORTED —— 质询 JS 自己中止被取代的探测请求（正常行为）；
  日志中零连接层错误（无 refused/reset/timeout）。网络端到端健康，
  瓶颈确认在边缘的 verdict 评分，不在引擎的任何网络环节。
  复现工具：MOZ_LOG="nsHttp:5,nsSocketTransport:5" MOZ_LOG_FILE=/tmp/mozlog
  （host 会写 /tmp/mozlog.moz_log）。
- **质询 POST 响应体实测**：HTTP 200 + `text/plain` 加密二进制载荷（质询 VM
  的加密指令流，无可读 JSON），无 set-cookie —— verdict 不发 clearance 就
  是边缘的明确表态。network.js 已内置转储（/tmp/chall_resp.log，仅质询 URL）。
- **真 Firefox 159 对照（同出口）**：geckodriver 驱动官方 Firefox 159 走同一
  质询，同样未通过 —— 引擎行为与真浏览器一致（Turnstile api.js/iframe/内部
  POST 全部加载运行，边缘不发放 clearance）。`tools/signal_parity.py` 可随时
  双端比对 canvas/webgl/字体/Intl/异常面等信号（今晚已对齐 DOMException 六成员
  与 webgl 基线，字体 11/11、错误消息一致）。
