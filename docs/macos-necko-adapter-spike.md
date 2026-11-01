# Darwin Necko adapter spike

This branch runs the Windows runtime's `necko` / `ruyijs_necko` packages on macOS. It is not a maintenance release. `main`, `release/macos`, `tls/necko-core-arm64`, and `macos-core-proxy` stay where they were. Nothing here is pushed, and no pull request is opened.

Products stay in gitignored `build/necko-darwin/`. `vendor/necko-derived/python` and `dist/js` were not replaced. The shell used for the smoke is the existing `dist/js`. `RUYIJS_NECKO_PYTHON` points at the spike interpreter and `RUYIJS_XPCSHELL` points at a missing path. When `NECKO_CORE_LIBRARY` is unset, `Context` uses `libnecko_core.dylib` next to that interpreter. The packaged `dist/necko_tls` library remains the default when `RUYIJS_NECKO_PYTHON` is unset.

## What is installed

`tools/build_necko_darwin.py` downloads the `runtime.json` 0.3.0 Windows archive, checks its size and SHA-256, and extracts the text files of `necko` and `ruyijs_necko` into a Python 3.14 virtualenv. The four Linux patch anchors are applied with `libnecko_core.dylib` instead of `libnecko_core.so`. `ruyijs_necko/oneshot.py` is `src/resource_network/necko_oneshot.py`. Python 3.12 cannot import this adapter: `ruyijs_necko/codec.py` imports `compression.zstd`, which the 3.14 standard library provides. The extracted `_tls_profiles.py` is the same 4883 bytes as `native/necko_tls/necko_win/_tls_profiles.py`.

The core library is `native/necko_posix/src` linked against the existing `../firefox-159/obj-necko` NSS. No NSS rebuild was started. Darwin has no `SOCK_NONBLOCK` or `SOCK_CLOEXEC`, so the socket opens with `fcntl` and `SO_NOSIGPIPE`. `SSL_DataPending` is a local symbol in the folded `libnss3.dylib`, so `necko_tls_pending` reports whether `PR_Recv` can peek one byte. The HTTP/2 pump only tests that sign. HTTP/3 and the Neqo bridge were not built.

The TLS option set follows the recovered `necko_core.dll` path in `native/necko_tls/necko_tls.c`. NSS defaults already leave OCSP stapling, delegated credentials, post-handshake auth, and the TLS 1.3 compatibility session id off. Enabling them added ClientHello extensions the captured Firefox hellos do not carry.

## What the smoke showed

`hello_probe.py` against the spike dylib, NSS 3.131:

| Profile | Ciphers | Groups | Key shares | Record | SCT | ECH | Compression |
|---|---|---|---|---|---|---|---|
| 100 | 17 | 6 | 2 (32, 65) | 517 | no | no | 0 |
| 159 | 15 | 5 | 3 (1216, 32, 65) | 1832 | yes | yes | 3 |

The extension list and those lengths match `dist/necko_tls/libnecko_core.dylib` for both profiles. Version 159's cipher order matches too. Version 100's cipher order follows the profile table (`0xc00a`, `0xc009`, `0xc013`, `0xc014`). The existing library's preference loop emits `0xc009`, `0xc013`, `0xc00a`, `0xc014` for that profile.

Through the existing shell, with xpcshell hidden:

- `navigate('https://example.com/')` produced `document.title` `Example Domain`. A direct adapter GET returned status 200 and HTTP/2 (`raw.version` 20).
- An HTTPS request through a local proxy sent `CONNECT example.com:443 HTTP/1.1` and loaded the same title. Two GETs in one adapter session used that one tunnel.
- An HTTP request through the same proxy sent `GET http://example.com/ HTTP/1.1` and the proxy's response became the document.
- A local HTTP document loaded directly. A local 302 from `/go` to `/land` ended on `Landed`.
- `https://self-signed.badssl.com/` failed certificate validation.
- A proxy URL with a path was refused before the proxy saw a connection, and the document title stayed empty.

Loading the Firefox roots prints a `CryptographyDeprecationWarning` for one certificate serial that is not positive. The page still verified.

## Still open

- HTTP/3 is not built, so Alt-Svc and the Neqo bridge are uncovered.
- `necko_tls_pending` can say that plaintext is present. It cannot return the buffered byte count while `SSL_DataPending` stays a local symbol.
- This note is the trial record. It does not change `docs/macos.md` or `runtime.json`.
