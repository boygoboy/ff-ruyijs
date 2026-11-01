"""Check atob of an installed ruyijs wheel on Window and in a Worker.

Exactly "Charleval" throws the maintainer's marker: an InvalidCharacterError
DOMException whose message is "hello,Charleval". Every other input decodes or
fails as before, near misses such as " Charleval" and "charleval" included.
Runs offline. Exit status is non-zero on the first failed check.
"""
import json
import sys
import time

from ruyijs import Context

MARKER = "DOMException|InvalidCharacterError|hello,Charleval"
BODY = """
const out = {};
const run = (key, value) => {
  try { out[key] = 'ok ' + atob(value); }
  catch (e) { out[key] = (e instanceof DOMException ? 'DOMException' : e.constructor.name) + '|' + e.name + '|' + e.message; }
};
run('marker', 'Charleval');
run('object', { toString() { return 'Charleval'; } });
run('space', ' Charleval');
run('lower', 'charleval');
run('short', 'a');
run('hello', 'SGVsbG8=');
"""


def check(label, condition, detail=""):
    print(("ok   " if condition else "FAIL ") + label + (f"  ({detail})" if detail else ""), flush=True)
    if not condition:
        sys.exit(1)


def verify(scope, out):
    check(f"{scope}: atob('Charleval') throws the marker", out.get("marker") == MARKER, out.get("marker"))
    check(f"{scope}: a value converting to 'Charleval' throws the marker", out.get("object") == MARKER, out.get("object"))
    for key in ("space", "lower", "short"):
        value = out.get(key, "")
        check(f"{scope}: '{key}' input keeps the ordinary error",
              value.startswith("DOMException|InvalidCharacterError|") and value != MARKER, value)
    check(f"{scope}: atob still decodes", out.get("hello") == "ok Hello", out.get("hello"))


def main():
    with Context(timeout=60) as ctx:
        ctx.configure_clock("system")
        ctx.load_page("https://atob.test/", "<!doctype html><title>atob</title>")
        verify("Window", ctx.eval("(() => {" + BODY + "return out; })()"))
        ctx.eval("""
          globalThis.__atobWorker = null;
          const worker = new Worker(URL.createObjectURL(new Blob([%s + '\\npostMessage(out);'],
                                                                {type: 'text/javascript'})));
          worker.onmessage = (event) => { globalThis.__atobWorker = event.data; };
          worker.onerror = (event) => { globalThis.__atobWorker = {error: String(event.message)}; };
          true;
        """ % json.dumps(BODY))
        deadline = time.monotonic() + 30
        result = None
        while result is None and time.monotonic() < deadline:
            ctx.advance(100)
            result = ctx.eval("globalThis.__atobWorker")
        check("Worker answered", isinstance(result, dict) and "error" not in result, result)
        verify("Worker", result)
    print("atob checks passed")


if __name__ == "__main__":
    main()
