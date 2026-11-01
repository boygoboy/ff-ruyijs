"""Spike: load the Windows Necko Python adapter on Darwin.

Products stay under build/necko-darwin/. This does not replace
vendor/necko-derived/python or dist/js, and it does not build HTTP/3.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compression_native

DEPENDENCIES = ["requests==2.34.2", "urllib3==2.7.0", "h2==4.4.1",
                "hpack==4.2.0", "hyperframe==6.1.0", "brotli==1.2.0",
                "cryptography==50.0.1", "publicsuffixlist==1.0.2.20260922"]
UPSTREAM = "vendor/necko-derived/python/Lib/site-packages/"
PATCHES = {
    "ruyijs_necko/brotli_native.py": [
        ("import ctypes as ct\n", "import ctypes as ct\nimport os\n"),
        ('        path = Path(__file__).with_name("ruyijs_brotli.dll")\n',
         '        path = (Path(__file__).with_name("ruyijs_brotli.dll") if os.name == "nt" else\n'
         '                Path(__file__).parent.parent / "necko/libnecko_core.dylib")\n'),
    ],
    "ruyijs_necko/launch.py": [
        ("import os\n", "import os\nimport sys\n"),
        ('    executable = runtime / "python/python.exe"\n',
         '    executable = runtime / "python/python.exe" if os.name == "nt" else Path(sys.executable)\n'),
    ],
    "ruyijs_necko/connection_lifetime.py": [
        ("def h3_finished(connection, udp):\n", "def h3_finished(connection, udp, selector=None):\n"),
        ("        connection._ruyijs_retained_udp = udp\n",
         "        connection._ruyijs_retained_udp = udp\n"
         "    if selector is not None:\n"
         "        connection._ruyijs_retained_selector = selector\n"),
        ('    for attribute in ("_ruyijs_retained_udp", "_wake_read", "_wake_write", "_dll_directory"):\n',
         '    for attribute in ("_ruyijs_retained_selector", "_ruyijs_retained_udp", "_wake_read", '
         '"_wake_write", "_dll_directory"):\n'),
    ],
    "necko/_http3.py": [
        ('                profile=PROFILES[self.firefox]["quic"],\n',
         '                profile=PROFILES[self.firefox]["quic"],\n'
         '                firefox=self.firefox,\n'),
    ],
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def runtime_archive(root, build):
    manifest = json.loads((root / "runtime.json").read_text(encoding="utf-8"))
    archive = build / manifest["archive"]
    if not archive.is_file():
        request = urllib.request.Request(manifest["url"], headers={"User-Agent": "ruyijs-build"})
        with urllib.request.urlopen(request, timeout=300) as source, archive.open("wb") as output:
            shutil.copyfileobj(source, output)
    if archive.stat().st_size != manifest["size"] or digest(archive.read_bytes()) != manifest["sha256"]:
        archive.unlink()
        raise ValueError("Pinned runtime archive size or SHA-256 differs from runtime.json")
    return manifest, archive


def install_packages(root, build, site):
    manifest, archive = runtime_archive(root, build)
    inventory = {}
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            if not entry.filename.startswith(UPSTREAM):
                continue
            relative = Path(entry.filename[len(UPSTREAM):])
            if (not relative.parts or relative.parts[0] not in ("necko", "ruyijs_necko") or
                    relative.suffix.lower() not in (".py", ".json", ".pem", ".dat", ".html", ".txt")):
                continue
            content = source.read(entry)
            inventory[relative.as_posix()] = digest(content)
            text_name = relative.as_posix()
            if text_name in PATCHES:
                text = content.decode("utf-8").replace("\r\n", "\n")
                for old, new in PATCHES[text_name]:
                    if text.count(old) != 1:
                        raise ValueError(f"Darwin patch anchor changed in {text_name}: {old!r}")
                    text = text.replace(old, new)
                content = text.encode("utf-8")
            target = site / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
    missing = sorted(set(PATCHES) - set(inventory))
    if missing:
        raise ValueError(f"Pinned runtime lacks {missing}")
    oneshot = site / "ruyijs_necko/oneshot.py"
    oneshot.write_bytes((root / "src/resource_network/necko_oneshot.py").read_bytes())
    leftovers = []
    for path in site.rglob("*.py"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in (".dll", ".so", "python.exe"):
            if token in text:
                leftovers.append(f"{path.relative_to(site)}: {token}")
    (build / "upstream.json").write_text(json.dumps({
        "archive": manifest["url"], "sha256": manifest["sha256"], "files": inventory,
        "patched": sorted(PATCHES),
        "replaced": {"ruyijs_necko/oneshot.py": "src/resource_network/necko_oneshot.py"},
        "platform_tokens": leftovers,
    }, indent=2) + "\n", encoding="utf-8")
    return leftovers


def compile_core(root, build, site, nss_obj):
    nss_obj = Path(nss_obj).resolve()
    libdir = nss_obj / "dist" / "bin"
    include = nss_obj / "dist" / "include"
    nspr_source = nss_obj.parent / "nsprpub" / "pr" / "include"
    if not nspr_source.is_dir():
        nspr_source = root / "firefox" / "nsprpub" / "pr" / "include"
    if not (libdir / "libnss3.dylib").is_file():
        raise FileNotFoundError(libdir / "libnss3.dylib")
    vendor = root / "native/necko_posix/src"
    compression = root / "vendor/compression-native"
    compression_native.frozen_inputs(compression)
    objects = build / "objects"
    objects.mkdir(parents=True, exist_ok=True)
    decoder = []
    cc = os.environ.get("CC", "clang")
    for unit in compression_native.brotli_units(compression):
        if unit.startswith(("common/", "dec/")):
            output = objects / (unit.replace("/", "_")[:-2] + ".o")
            subprocess.run([cc, "-std=c11", "-fPIC", "-O2",
                            *compression_native.brotli_flags(compression),
                            "-c", str(compression / "brotli" / unit), "-o", str(output)],
                           check=True)
            decoder.append(output)
    target = site / "necko"
    target.mkdir(parents=True, exist_ok=True)
    output = build / "libnecko_core.dylib"
    command = [
        os.environ.get("CXX", "clang++"), "-std=c++20", "-dynamiclib", "-fPIC", "-O2",
        "-DXP_DARWIN", "-DXP_UNIX",
        "-I", str(include / "nspr"), "-I", str(include / "nss"), "-I", str(nspr_source),
        "-I", str(compression / "brotli/include"),
        *sorted(str(path) for path in vendor.glob("*.cpp")),
        *map(str, decoder),
        "-L", str(libdir),
        "-lnss3", "-lmozglue",
        "-Wl,-rpath,@loader_path",
        "-Wl,-install_name,@rpath/libnecko_core.dylib",
        "-o", str(output),
    ]
    log = build / "link.log"
    result = subprocess.run(command, text=True, capture_output=True)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"link failed, see {log}")
    installed = target / "libnecko_core.dylib"
    shutil.copy2(output, installed)
    for name in ("libnss3.dylib", "libmozglue.dylib", "libfreebl3.dylib", "libsoftokn3.dylib"):
        link = target / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to((libdir / name).resolve())
    return installed


def probe_imports(python, report):
    code = r"""
import traceback
missing = []
try:
    from necko._transport import _load_library
    library = _load_library()
    print("loaded", library)
    for name in ("necko_tls_nss_version", "necko_tls_nspr_version"):
        print(name, getattr(library, name)().decode())
except Exception:
    traceback.print_exc()
"""
    result = subprocess.run([str(python), "-I", "-B", "-c", code], text=True,
                            capture_output=True)
    report.write_text(result.stdout + result.stderr, encoding="utf-8")
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nss-obj", type=Path, required=True)
    parser.add_argument("--python", default="python3.12")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This spike runs on Darwin")
    root = Path(__file__).resolve().parents[1]
    build = root / "build/necko-darwin"
    build.mkdir(parents=True, exist_ok=True)
    runtime = build / "python"
    python = runtime / "bin/python3"
    if not (runtime / "bin/pip").exists():
        subprocess.run([args.python, "-m", "venv", str(runtime)], check=True)
    subprocess.run([str(python), "-m", "pip", "install", "--disable-pip-version-check",
                    *DEPENDENCIES], check=True)
    site = Path(subprocess.check_output(
        [str(python), "-c", "import sysconfig; print(sysconfig.get_path('purelib'))"],
        text=True).strip())
    leftovers = install_packages(root, build, site)
    print("platform tokens:")
    for line in leftovers:
        print(" ", line)
    try:
        installed = compile_core(root, build, site, args.nss_obj)
        print("dylib", installed)
    except Exception as error:
        print("COMPILE_FAILED", error)
    code = probe_imports(python, build / "import-report.txt")
    print("import_ec", code)
    print((build / "import-report.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
