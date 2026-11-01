# Darwin Necko adapter spike

This branch tries to run the Windows runtime's `necko` / `ruyijs_necko` packages on macOS. It is not a maintenance release. `main`, `release/macos`, `tls/necko-core-arm64`, and `macos-core-proxy` are unchanged. Nothing here is pushed.

Products stay in gitignored `build/necko-darwin/`. `vendor/necko-derived/python` and `dist/js` were not replaced. The shell used for the smoke is the existing `dist/js`, with `RUYIJS_NECKO_PYTHON` pointed at the spike interpreter and `RUYIJS_XPCSHELL` pointed at a missing path.

## What ran

`tools/build_necko_darwin.py` downloads the `runtime.json` 0.3.0 Windows archive, checks its size and SHA-256, and extracts the text files of `necko` and `ruyijs_necko` into a Python 3.14 virtualenv. The four Linux patch anchors are applied with `libnecko_core.dylib` instead of `libnecko_core.so`. `ruyijs_necko/oneshot.py` is replaced by `src/resource_network/necko_oneshot.py`. Python 3.12 cannot import this adapter: `ruyijs_necko/codec.py` imports `compression.zstd`, which is in the 3.14 standard library and is not in the extracted packages.

The adapter's `_load_library` already looks for `libnecko_core.dylib` on Darwin. Import proceeds through `necko.adapters` and stops in `ctypes.CDLL` because that file was not produced.

## Link

`native/necko_posix/src` was compiled against `../firefox-159/obj-necko` NSS, with `-DXP_DARWIN` so the NSPR headers select `md/_darwin.cfg`. No NSS rebuild was started. HTTP/3 was not built.

The link stops in `native/necko_posix/src/socket.cpp`: `SOCK_NONBLOCK` and `SOCK_CLOEXEC` are not declared on Darwin. Those flags were not patched. `build/necko-darwin/link.log` has the compiler output.

## Smoke

`Context.eval('1+1')` returned 2, so the existing shell still runs. `navigate('https://example.com/')` returned the URL and `document.title` stayed empty. The page did not load. A local proxy was not contacted, because the helper dies while loading `libnecko_core.dylib`, before a request is sent.

## Still open

- The POSIX socket source does not compile on Darwin.
- No `libnecko_core.dylib`, so direct HTTPS, proxy, and certificate checks were not exercised.
- HTTP/3 and the Neqo bridge were not built. Handshake counts against `necko_tls` were not compared.
- Proxy behavior of the Windows adapter on Darwin is unknown until the library loads.
