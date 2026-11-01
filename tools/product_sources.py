"""Enumerate product translation units and embed the selected baseline profile."""

import json
import re
import sys
from provenance import write_if_changed

# Units only the Linux build compiles: the FreeType/Fontconfig/HarfBuzz fonts,
# OpenSSL WebCrypto and the codec/composition on Firefox's Skia
# (web_geometry/font_posix.cpp and the rest; tools/build_linux.py).
LINUX_ONLY = frozenset({
    "web_crypto/openssl_backend.cpp",
    "web_geometry/font_posix.cpp", "web_geometry/svg_path_posix.cpp",
    "web_media/image_codec_native.cpp", "web_media/image_composition_posix.cpp",
})
# Units Linux replaces with those: the DirectWrite / Core Text fonts, CNG /
# CommonCrypto WebCrypto, WIC / ImageIO codecs, and the stubs that complete the
# prebuilt Skia archive (Linux compiles Firefox's Skia sources instead).
NOT_LINUX = frozenset({
    "web_crypto/cng_backend.cpp", "web_crypto/digest_backend.cpp",
    "web_geometry/font_aliases.cpp", "web_geometry/font_collection.cpp",
    "web_geometry/font_face_mac.cpp", "web_geometry/font_missing.cpp",
    "web_geometry/font_preferences.cpp", "web_geometry/font_shaping.cpp",
    "web_geometry/system_font.cpp", "web_geometry/canvas_text.cpp",
    "web_geometry/canvas_text_path.cpp", "web_geometry/glyph_color.cpp",
    "web_geometry/glyph_mask.cpp", "web_geometry/glyph_palette.cpp",
    "web_geometry/glyph_runs.cpp", "web_geometry/glyph_style.cpp",
    "web_geometry/svg_length_font.cpp", "web_geometry/svg_path.cpp",
    "web_media/image_codec.cpp", "web_media/image_gif.cpp",
    "web_media/image_composition.cpp", "web_media/image_color.cpp",
    "web_canvas/skia_subset_stubs.cpp",
})


def platform_unit(relative, platform=None):
    """Whether src/<relative> is compiled for `platform` (sys.platform)."""
    linux = (platform or sys.platform).startswith("linux")
    return relative not in (NOT_LINUX if linux else LINUX_ONLY)

# Domains compiled into dist/js.exe. Adding a src/<domain>/ directory without
# listing it here is rejected by domains(); see docs/owners/BUILD_COVERAGE.md.
PRODUCT_DOMAINS = ("runtime", "interaction", "browser_environment", "document_model",
                   "execution_scheduling", "resource_network", "web_audio",
                   "web_crypto", "font_loading", "web_platform", "web_canvas",
                   "web_graphics", "web_styling", "web_rtc", "web_gpu",
                   "web_observers", "web_console", "web_media", "web_indexeddb", "web_geometry",
                   "web_workers", "web_trusted_types", "web_permissions", "web_broadcast",
                   "web_clipboard", "web_credentials", "web_messaging", "web_security",
                   "web_geolocation", "web_speech", "web_locks", "web_wake_lock", "web_midi", "web_service_worker", "web_gamepad", "web_animation")

# Domains deliberately outside the product build, with the reason each one is
# excluded. They are built and verified by their own harnesses instead.
EXCLUDED_DOMAINS = {
    "application": "Engine-independent source service contracts, compiled and "
                   "verified by the application MSVC GTest harness. Production "
                   "runtime assembly remains in runtime/Run.",
    "transport": "Necko worker sources are verified by artifacts/necko-inspection "
                 "harnesses; no product translation unit includes them yet.",
    "client": "Built separately by tools/client.py as the C ABI DLL, without "
              "SpiderMonkey linkage.",
    "domain": "Imported upstream domain headers, compiled through the owning "
              "domain's bridge translation units.",
}

# Excluded domains that carry no translation unit of their own, so a product
# include proves nothing about linkage and must not demand registration.
HEADER_ONLY_DOMAINS = ("domain",)


def domains(root):
    """Return the product domains, rejecting any unclassified src/<domain>/.

    A domain that product code already includes must be built, or it would be
    compiled into nothing and silently disappear from dist/js.exe. A domain no
    product translation unit references yet may stay excluded while it is being
    wired, but the exclusion has to be explicit.
    """
    present = {path.name for path in (root / "src").iterdir() if path.is_dir()}
    missing = sorted(set(PRODUCT_DOMAINS) - present)
    if missing:
        raise ValueError("product domains have no sources: " + ", ".join(missing))
    unclassified = sorted(present - set(PRODUCT_DOMAINS) - set(EXCLUDED_DOMAINS))
    wired = _referenced(root, unclassified)
    if wired:
        raise ValueError("src/ domains are referenced by product code but not "
                         "built: " + ", ".join(wired))
    unwired = [name for name in unclassified if name not in wired]
    if unwired:
        raise ValueError("src/ domains are neither built nor excluded: "
                         + ", ".join(unwired))
    stale = sorted(name for name in EXCLUDED_DOMAINS
                   if name in present and name not in HEADER_ONLY_DOMAINS
                   and name in _referenced(root, [name]))
    if stale:
        raise ValueError("excluded src/ domains are referenced by product "
                         "code: " + ", ".join(stale))
    return PRODUCT_DOMAINS


def _referenced(root, candidates):
    """Names among candidates that a built domain's source already includes."""
    if not candidates:
        return []
    text = []
    for domain in PRODUCT_DOMAINS:
        for path in (root / "src" / domain).glob("*"):
            if path.suffix in (".cpp", ".h"):
                text.append(path.read_text(encoding="utf-8", errors="replace"))
    joined = "\n".join(text)
    # Only include directives count: string literals such as "application/xml"
    # or a JavaScript MIME type are not references to src/application.
    return [name for name in candidates
            if re.search(r'^\s*#\s*include\s+"' + re.escape(name) + '/', joined, re.MULTILINE)]


def sources(root):
    result = []
    for domain in domains(root):
        directory = root / "src" / domain
        # glob is deliberately flat; nested units would be skipped in silence.
        nested = sorted(path.relative_to(directory).as_posix()
                        for path in directory.rglob("*.cpp")
                        if path.parent != directory)
        if nested:
            raise ValueError(f"src/{domain} has unbuilt nested sources: "
                             + ", ".join(nested))
        for path in sorted(directory.glob("*.cpp")):
            if not platform_unit(f"{domain}/{path.name}"):
                continue
            result.append((path, domain + "_" + path.stem))
    if not any(name.startswith("runtime_") for _, name in result):
        raise ValueError("src/runtime must contain implementation files")
    return result


# Printable ASCII literals long enough that the linker keeps them verbatim and
# short enough to stay inside one string pool entry.
_LITERAL = re.compile(r'"((?:[\x20-\x21\x23-\x5b\x5d-\x7e]){24,100})"')


def residency(root, executable):
    """Prove every compiled domain left at least one literal in the executable.

    A domain can compile and link yet contribute nothing once /OPT:REF removes
    unreferenced sections. Counting literals per domain turns that silent loss
    into a build failure.
    """
    blob = executable.read_bytes()
    found = {}
    for domain in domains(root):
        literals = set()
        for path in sorted((root / "src" / domain).glob("*.cpp")):
            literals |= set(_LITERAL.findall(path.read_text(encoding="utf-8",
                                                            errors="replace")))
        if not literals:
            raise ValueError(f"domain {domain} has no probe literal to verify")
        hits = sorted(text for text in literals if text.encode("utf-8") in blob)
        if not hits:
            raise ValueError(f"domain {domain} contributes nothing to {executable.name}")
        found[domain] = {"literals": len(literals), "resident": len(hits),
                         "probe": hits[0]}
    return found


# The baseline always pins these five groups; every other group is optional and,
# when a group is listed, the schema requires it to be field-complete
# (src/browser_environment/schema.cpp Profile).
REQUIRED_PROFILE_GROUPS = ("navigator", "screen", "viewport", "location", "locale")
OPTIONAL_PROFILE_GROUPS = ("webgl", "webgpu", "permissions", "geolocation", "speech",
                           "storage", "media")


def profile(root, directory, name="firefox-environment.json"):
    source = root / "profiles" / name
    data = source.read_text(encoding="utf-8")
    parsed = json.loads(data)
    if (not isinstance(parsed, dict)
            or not set(REQUIRED_PROFILE_GROUPS) <= set(parsed)
            or set(parsed) - set(REQUIRED_PROFILE_GROUPS) - set(OPTIONAL_PROFILE_GROUPS)):
        raise ValueError("invalid Firefox environment baseline groups")
    delimiter = "RUYIJS_PROFILE"
    if f'){delimiter}"' in data:
        raise ValueError("profile collides with its C++ raw-string delimiter")
    target = directory / "firefox_environment_profile.h"
    write_if_changed(target,
        f'// Generated from profiles/{name}. Do not edit.\n'
        '#pragma once\nnamespace ruyijs::browser_environment {\n'
        f'inline constexpr char kFirefoxEnvironmentProfile[] = R"{delimiter}('
        f'{data}){delimiter}";\n}}\n', encoding="utf-8")
    return [source, target]


def inputs(root):
    paths = [path for path in (root / "src").rglob("*") if path.is_file()]
    paths += list((root / "profiles").rglob("*.json"))
    return paths


def needs_exceptions(source):
    if source.parent.name == "web_media":
        return source.stem in ("image_codec", "image_gif", "image_data", "image_avif", "image_color", "image_apng", "image_webp", "image_composition", "image_progress")
    if source.parent.name == "web_workers":
        return source.stem == "process_start"  # Native allocation boundary, no JSAPI.
    if source.parent.name == "resource_network" and source.stem.startswith("webtransport_"):
        return True  # Fallible typed transport conversion; guarded JS entry.
    if source.parent.name == "web_platform":
        return source.stem in ("text_decode", "object_url_store", "object_url_session", "form_query")  # Allocation boundaries.
    if source.parent.name == "web_indexeddb":
        return source.stem == "model" or source.stem.startswith("model_")
    # Only the engine-independent boundary contains imported throwing headers.
    if source.parent.name == "runtime" and source.stem in (
            "resource_buffer", "api_monitor", "pipe_async_state", "pipe_async_read"):
        return True
    if source.parent.name in ("document_model", "execution_scheduling", "resource_network", "web_audio"):
        return source.stem == "model" or source.stem.startswith("model_")
    if source.parent.name == "interaction":
        return source.stem == "model_transfer"
    if source.parent.name == "web_styling":
        return source.stem.startswith("model_") or source.stem in (
            "computed", "computed_source", "sheets", "css_namespace", "rule_declaration", "cascade_sources")
    # Only the WebRTC model bridge constructs the imported aggregate, whose
    # shared/entity.h base throws from its constructor.
    if source.parent.name == "web_rtc":
        return source.stem == "model"
    if source.parent.name == "browser_environment":
        return source.stem in ("model", "model_config", "model_history", "model_snapshot")
    # Only the WebGPU model bridge constructs the imported capability
    # aggregate, whose shared/aggregate_root.h base throws from its constructor.
    if source.parent.name == "web_gpu":
        return source.stem == "model"
    # The WebGL model includes the imported platform_apis header, and the
    # sanitize* units use <regex>; both are compiled with exceptions enabled.
    return source.parent.name == "web_graphics" and (
        source.stem == "model" or source.stem.startswith("sanitize"))

