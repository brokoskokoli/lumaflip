#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Untranslated-text check (F-I18N-01, F-I18N-05).

Scans the C++/Objective-C++ sources under src/ for string literals that look like
user-visible text but do not go through Qt's translation system: tr(), translate(),
QT_TR_NOOP/QT_TRANSLATE_NOOP or, for NSString texts, lf::mac::translate()
(src/platform/mac/MacTranslate.h).

Heuristics (deliberately simple, tuned to this code base):

  * A literal counts as "text" if it contains a non-ASCII letter (German umlauts,
    typographic quotes, ellipsis), or at least two words, or a single capitalised word
    of three or more letters, or a capitalised word followed by ':' or '%' ("Zoom: %1",
    "Name:").
  * Literals inside logging/diagnostics (qDebug, qWarning, qCInfo, Q_ASSERT, printf,
    trace marks …), object names, properties, settings keys, NSUserDefaults/plist keys,
    QLatin1String comparisons and similar code-only calls are ignored. Generic method
    names (value, insert, contains, data …) only count as code with an explicit receiver
    (`x.value(…)`, `p->insert(…)`, `Class::f(…)`), the trace marks only as `trace::once`/
    `trace::mark`: a bare `insert("Text")` in a widget is still reported.
  * Everything else that is reported must either be wrapped or listed in the allowlist
    (tools/i18n_allowlist.txt): one entry per line, ``path:literal`` (exact literal text
    as written in the source, without quotes) or ``path:*`` for a whole file; ``#`` starts
    a comment. Unused allowlist entries are reported as well (keeps the list honest).

With ``--lupdate <path to lupdate>`` the catalogues in translations/ are checked too
(F-I18N-05): they must be up to date with the sources (a scratch lupdate run finds no new
or obsolete messages, the numerus attribute matches; no tr() calls with a wrong context),
every finished translation keeps the source's placeholders (%1 … %99 as a multiset; %n in
at least one plural form) and every language must have at least 99 % of its messages
translated (F-I18N-02).

Always: every localizable text of packaging/macos/Info.plist.in (privacy usage
descriptions, copyright, document type names, UTTypeDescription) has an entry in every
packaging/macos/lproj/<lang>.lproj/InfoPlist.strings (and nothing stale), with the same
placeholders, and there is an .lproj folder for every CFBundleLocalizations entry.

Exit code 0 if nothing is reported, 1 otherwise. ``--list`` prints every finding
(including allowlisted ones) for auditing.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
ALLOWLIST = ROOT / "tools" / "i18n_allowlist.txt"

# Calls whose string arguments are translated (or marked for translation).
TRANSLATE_CALLS = {
    "tr", "translate", "QT_TR_NOOP", "QT_TRANSLATE_NOOP", "QT_TRANSLATE_NOOP3", "QT_TR_N_NOOP",
    "QT_TRANSLATE_N_NOOP", "qtTrId", "nsTr", "trNoop", "lfTr",
}
# Calls whose string arguments are never shown to the user (free functions, macros,
# constructors and method names that only exist for code).
CODE_CALLS = {
    # logging / diagnostics
    "qDebug", "qInfo", "qWarning", "qCritical", "qFatal", "qCDebug", "qCInfo", "qCWarning",
    "qCCritical", "Q_ASSERT", "Q_ASSERT_X", "Q_LOGGING_CATEGORY", "printf", "fprintf", "snprintf",
    "NSLog", "qPrintable", "Q_UNREACHABLE",
    # identifiers, keys, properties
    "setObjectName", "setProperty", "objectName", "findChild", "findChildren",
    "setAccessibleIdentifier", "beginGroup",
    "QSettings", "objectForKey", "setObject", "valueForKey", "objectForInfoDictionaryKey",
    "typeWithIdentifier", "isEqualToString", "hasPrefix", "hasSuffix", "QUrl", "fromLocalFile",
    "QFile", "QDir", "QFileInfo", "QProcess", "startDetached", "setProgram",
    "QRegularExpression", "QLatin1String", "QLatin1StringView", "QStringView", "QByteArray",
    "QByteArrayLiteral", "QIcon", "fromTheme", "QPixmap", "QImage", "QColor", "QKeySequence",
    "setStyleSheet", "QFont", "setFamily", "QMimeData", "hasFormat",
    "qgetenv", "qputenv", "qEnvironmentVariable", "qEnvironmentVariableIsSet", "getenv",
    "QJsonObject", "QJsonValue", "addSearchPath", "setOrganizationName",
    "setApplicationName", "setOrganizationDomain", "QStandardPaths", "loadFromData",
    "selector", "NSSelectorFromString", "CFSTR", "imageNamed", "systemSymbolNamed",
    "imageWithSystemSymbolName", "setFeature", "connect", "SIGNAL", "SLOT", "metaObject",
    "QMetaObject", "invokeMethod", "addAppInfo", "QT_STRINGIFY", "registerMetaType",
    "qRegisterMetaType", "setAttribute", "setEnv", "insertEnv", "QStorageInfo",
}
# Generic method names: code only when called on an explicit receiver (`x.value("key")`,
# `map->insert("key", …)`, `QProcess::execute("…")`); a bare call could be a widget's own
# method showing the text (QLineEdit::insert, QComboBox::setData …).
RECEIVER_CODE_CALLS = {
    "value", "setValue", "contains", "remove", "insert", "property", "start", "execute",
    "startsWith", "endsWith", "compare", "indexOf", "lastIndexOf", "fromString", "setData",
    "data", "toObject",
}
# Calls that are code only with this qualifier (StartupTrace marks).
QUALIFIED_CODE_CALLS = {"once": ("trace::",), "mark": ("trace::",)}
# Never translated (SPEC 4.16: the product name stays).
PRODUCT_NAMES = {"Lumaflip"}
# Streamed logging: qCWarning(cat) << "text" << value;
LOGGING = re.compile(r"\b(qC?(Debug|Info|Warning|Critical)|NSLog|std::cerr|std::cout)\b")
WORD = re.compile(r"[A-Za-zÀ-ÖØ-öø-ÿ]{2,}")
NON_ASCII_LETTER = re.compile(r"[À-ÖØ-öø-ÿ„“”‘’…–—]")
IDENT = re.compile(r"[A-Za-z_]\w*\s*$")
CODEY = re.compile(r"^[\w.:/\-+*%#@$&|<>=?!\[\]{}(),;^~\\]*$")  # no spaces: keys, paths, ids
FORMATTY = re.compile(r"^[\s%\d\w.:,/\-+()\[\]×·|]*$")


def strip_comments(src: str) -> str:
    """Blank out comments (keeping newlines and string/char literals)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c == "R" and src.startswith('R"', i) and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")):
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i)
            end = n if end < 0 else end + len(m.group(1)) + 2
            # raw strings hold code (scripts, shaders), never UI text: blank them out
            out.append('R""' + "".join(ch if ch == "\n" else " " for ch in src[i + 3:end]))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


LITERAL = re.compile(r'(@?)(?:u8|u|U|L)?"((?:[^"\\\n]|\\.)*)"')


def enclosing_calls(code: str, pos: int) -> list[str]:
    """Names of the calls (innermost first) whose parentheses enclose `pos`, within the
    current statement. A call with an explicit receiver is reported as ".name" (`x.f(`,
    `p->f(`) or "Qualifier::name" (`trace::once(`) in addition to its plain name."""
    names = []
    depth = 0
    i = pos - 1
    while i >= 0:
        c = code[i]
        if c in "\"'":  # skip literal backwards
            q = c
            i -= 1
            while i >= 0 and not (code[i] == q and code[i - 1] != "\\"):
                i -= 1
        elif c in ")]":
            depth += 1
        elif c in "([":
            if depth == 0:
                m = IDENT.search(code, 0, i) if c == "(" else None
                head = code[max(0, i - 80):i]
                m = re.search(r"([A-Za-z_]\w*)\s*(?:<[^<>()]*>)?\s*$", head)
                if m:
                    names.append(m.group(1))
                    before = head[:m.start(1)]
                    q = re.search(r"([A-Za-z_]\w*::)\s*$", before)
                    if q:
                        names.append(q.group(1) + m.group(1))
                    if re.search(r"(\.|->|::|\))\s*$", before):
                        names.append("." + m.group(1))
                elif c == "[":  # Objective-C message: [receiver selector:@"…"]
                    pass
            else:
                depth -= 1
        elif c in ";{}" and depth == 0:
            # an initializer list `{"key", tr("…")}` is not a statement end
            if c == "{":
                head = code[max(0, i - 80):i]
                m = re.search(r"([A-Za-z_]\w*)\s*$", head)
                if m and m.group(1) not in ("else", "do", "try"):
                    names.append(m.group(1))
                if code[max(0, i - 3):i].strip().endswith((",", "(", "=", "{")):
                    i -= 1
                    continue
            break
        i -= 1
    return names


def statement_head(code: str, pos: int) -> str:
    """The current statement up to `pos` (from the previous ';', '{' or '}')."""
    i = max(code.rfind(";", 0, pos), code.rfind("{", 0, pos), code.rfind("}", 0, pos))
    return code[i + 1:pos]


def objc_selectors(code: str, pos: int) -> list[str]:
    """Selector parts preceding `pos` on the same line (Objective-C messages)."""
    line_start = code.rfind("\n", 0, pos) + 1
    return re.findall(r"(\w+):\s*$", code[line_start:pos])


def looks_like_text(s: str) -> bool:
    if not re.search(r"[A-Za-zÀ-ÿ]", s):
        return False
    if NON_ASCII_LETTER.search(s):
        return True
    # "Name:", "Zoom: %1", "Seite %1": a capitalised word before ':' or '%' is a label
    if re.search(r"(?<![\w:#.\-])[A-ZÄÖÜ][a-zäöüß]{2,}\s*(:(?!:)|%)", s):
        return True
    if CODEY.match(s):
        # single token: a capitalised word ("Bild") is text, identifiers/keys are not
        return bool(re.fullmatch(r"[A-ZÄÖÜ][a-zäöüß]{2,}", s))
    words = WORD.findall(s)
    if len(words) < 2:
        return False
    if re.fullmatch(r"[\w.\-]+(\s+[\w.\-]+)*", s) and s == s.lower() and not re.search(r"[a-z]{4,}\s+[a-z]{4,}", s):
        return False
    return True


def is_code_call(calls: list[str]) -> bool:
    for c in calls:
        if c in CODE_CALLS:
            return True
        if c.startswith(".") and c[1:] in RECEIVER_CODE_CALLS:
            return True
        if "::" in c:
            qual, _, name = c.rpartition("::")
            if name in RECEIVER_CODE_CALLS or qual + "::" in QUALIFIED_CODE_CALLS.get(name, ()):
                return True
    return False


def scan_file(path: pathlib.Path) -> list[tuple[int, str, bool]]:
    """Findings: (line, literal, is_objc)."""
    src = path.read_text(encoding="utf-8")
    code = strip_comments(src)
    findings = []
    for m in LITERAL.finditer(code):
        start = m.start()
        # #include "…" and other preprocessor lines
        line_start = code.rfind("\n", 0, start) + 1
        line = code[line_start:code.find("\n", start)]
        if line.lstrip().startswith("#"):
            continue
        text = m.group(2)
        if not looks_like_text(text.replace("\\n", " ").replace("\\\"", '"')):
            continue
        if text in PRODUCT_NAMES:
            continue
        if LOGGING.search(statement_head(code, start)):
            continue
        calls = enclosing_calls(code, start)
        if any(c in TRANSLATE_CALLS for c in calls):
            continue
        if is_code_call(calls):
            continue
        if m.group(1) and any(s in ("isEqualToString", "objectForKey", "setObject", "forKey", "forName")
                               for s in objc_selectors(code, start)):
            continue
        # `"text"_L1 == x`, QStringLiteral used as a comparison key: comparisons are code
        tail = code[m.end():m.end() + 6]
        if re.match(r"\s*(==|!=)", tail) or re.search(r"(==|!=)\s*(QStringLiteral\(|QLatin1String\(|u)?$",
                                                         code[max(0, start - 20):start]):
            continue
        lineno = code.count("\n", 0, start) + 1
        findings.append((lineno, text, bool(m.group(1))))
    return findings


def load_allowlist(path: pathlib.Path) -> dict[tuple[str, str], int]:
    entries = {}
    if not path.exists():
        return entries
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        file, sep, literal = line.partition(":")
        if not sep:
            raise SystemExit(f"{path}:{n}: expected 'path:literal'")
        entries[(file.strip(), literal)] = n
    return entries


def check_literals(root: pathlib.Path, allowlist: pathlib.Path, list_all: bool) -> int:
    allow = load_allowlist(allowlist)
    used = set()
    bad = 0
    files = sorted(p for p in root.rglob("*") if p.suffix in (".cpp", ".h", ".mm"))
    for p in files:
        rel = p.relative_to(ROOT).as_posix() if p.is_relative_to(ROOT) else p.as_posix()
        for lineno, text, objc in scan_file(p):
            key = (rel, text)
            whole = (rel, "*")
            if key in allow or whole in allow:
                used.add(key if key in allow else whole)
                if list_all:
                    print(f"{rel}:{lineno}: (allowlisted) \"{text}\"")
                continue
            bad += 1
            kind = "NSString literal" if objc else "literal"
            print(f"{rel}:{lineno}: untranslated {kind}: \"{text}\"")
    stale = [k for k in allow if k not in used] if root == ROOT / "src" else []
    for k in stale:
        print(f"{allowlist.name}:{allow[k]}: unused allowlist entry {k[0]}:{k[1]}")
    if bad or stale:
        print(f"i18n check: {bad} untranslated text(s), {len(stale)} stale allowlist entr(y/ies). "
              "Wrap user-visible texts in tr()/QCoreApplication::translate()/lf::mac::translate() "
              "or add them to tools/i18n_allowlist.txt.", file=sys.stderr)
        return 1
    print(f"i18n check: {len(files)} files, no untranslated user-visible texts")
    return 0


# ---- .ts files (F-I18N-05): up to date with the sources, translated ----------------------

TS_DIR = ROOT / "translations"
MIN_COVERAGE = 0.99  # share of finished messages per language (F-I18N-02)


def ts_messages(path: pathlib.Path) -> dict[tuple[str, str, str], ET.Element]:
    """(context, source, disambiguation) -> <message>, without obsolete/vanished ones."""
    out = {}
    for ctx in ET.parse(path).getroot().iter("context"):
        name = ctx.findtext("name") or ""
        for msg in ctx.iter("message"):
            tr = msg.find("translation")
            if tr is not None and tr.get("type") in ("obsolete", "vanished"):
                continue
            out[(name, msg.findtext("source") or "", msg.findtext("comment") or "")] = msg
    return out


def shown(path: pathlib.Path) -> str:
    """`path` relative to the repository if it is inside, else as is (tests)."""
    return path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else path.as_posix()


PLACEHOLDER = re.compile(r"%(L?\d{1,2}|n)")


def placeholders(text: str) -> list[str]:
    """Sorted multiset of %1 … %99 (and %L1) in `text`; %n separately."""
    return sorted(p for p in PLACEHOLDER.findall(text or "") if p != "n")


def check_placeholders(ts: pathlib.Path, msg: ET.Element, key: tuple[str, str, str]) -> list[str]:
    """Problems of one finished message: placeholders differ from the source."""
    src = key[1]
    want = placeholders(src)
    tr = msg.find("translation")
    forms = [f.text or "" for f in tr.findall("numerusform")] if msg.get("numerus") == "yes" else [tr.text or ""]
    out = []
    for form in forms:
        got = placeholders(form)
        if got != want:
            out.append(f"{shown(ts)}: {key[0]}: placeholders {got} instead of {want} in "
                       f"{form!r} (source {src!r})")
    if "%n" in src and msg.get("numerus") == "yes" and not any("%n" in f or "%Ln" in f for f in forms):
        out.append(f"{shown(ts)}: {key[0]}: no plural form keeps %n (source {src!r})")
    return out


def is_finished(msg: ET.Element) -> bool:
    tr = msg.find("translation")
    if tr is None or tr.get("type") == "unfinished":
        return False
    if msg.get("numerus") == "yes":
        forms = tr.findall("numerusform")
        return bool(forms) and all((f.text or "").strip() for f in forms)
    return bool((tr.text or "").strip())


def check_ts(lupdate: str) -> int:
    """Runs lupdate into a scratch catalogue and compares it with translations/*.ts."""
    problems = 0
    with tempfile.TemporaryDirectory() as tmp:
        fresh = pathlib.Path(tmp) / "lumaflip_xx.ts"
        plural = pathlib.Path(tmp) / "lumaflip_de.ts"
        for out, extra in ((fresh, []), (plural, ["-pluralonly", "-target-language", "de"])):
            r = subprocess.run([lupdate, "-source-language", "de", "-locations", "none", "-no-obsolete",
                                "-silent", "-extensions", "cpp,h,mm", *extra, str(ROOT / "src"), "-ts", str(out)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                print(f"i18n check: lupdate failed ({lupdate})", file=sys.stderr)
                return 1
            # e.g. "Cannot invoke tr() like this", "Unbalanced opening parenthesis" (Objective-C
            # "[[" reads as a C++ attribute and derails the parser). Harmless: classes declared
            # in headers of the other platform.
            warnings = [w for w in r.stderr.splitlines()
                        if w.strip() and "Ignoring definition of undeclared qualified class" not in w]
            if warnings:
                print("\n".join(warnings))
                problems += 1
        expected = ts_messages(fresh)
        expected_plural = ts_messages(plural)
    for ctx, _, _ in expected:
        if ctx in ("", "lf", "Tr", "Tr::Tr") or ctx.startswith("Tr::"):
            print(f"i18n check: messages in context '{ctx}' - tr() outside a class needs a "
                  "Q_DECLARE_TR_FUNCTIONS struct (see InfoPanel.cpp)")
            problems += 1
    ts_files = sorted(TS_DIR.glob("lumaflip_*.ts"))
    if not ts_files:
        print(f"i18n check: no translations in {TS_DIR}", file=sys.stderr)
        return 1
    for ts in ts_files:
        have = ts_messages(ts)
        want = expected_plural if ts.name == "lumaflip_de.ts" else expected
        missing = [k for k in want if k not in have]
        extra = [k for k in have if k not in want]
        if missing or extra:
            problems += 1
            print(f"{ts.relative_to(ROOT)}: out of date ({len(missing)} new, {len(extra)} obsolete "
                  "messages) - run: cmake --build build/check --target lf_lupdate")
            for k in (missing + extra)[:5]:
                print(f"    {k[0]}: {k[1]!r}")
        for k in want:
            if k not in have:
                continue
            if (want[k].get("numerus") == "yes") != (have[k].get("numerus") == "yes"):
                problems += 1
                print(f"{ts.relative_to(ROOT)}: {k[0]}: numerus attribute differs from the source "
                      f"({k[1]!r}) - run lf_lupdate")
            if is_finished(have[k]):
                found = check_placeholders(ts, have[k], k)
                problems += len(found)
                for line in found:
                    print(line)
        done = sum(1 for k in want if k in have and is_finished(have[k]))
        coverage = done / len(want) if want else 1.0
        if coverage < MIN_COVERAGE:
            problems += 1
            print(f"{ts.relative_to(ROOT)}: only {coverage:.1%} translated (at least {MIN_COVERAGE:.0%})")
    if problems:
        print(f"i18n check: {problems} problem(s) in translations/ (translations/README.md)", file=sys.stderr)
        return 1
    print(f"i18n check: {len(ts_files)} catalogues up to date, {len(expected)} messages")
    return 0


# ---- InfoPlist.strings (localized Info.plist texts, packaging/macos/lproj) ----------------

INFO_PLIST = ROOT / "packaging" / "macos" / "Info.plist.in"
LPROJ_DIR = ROOT / "packaging" / "macos" / "lproj"
# Info.plist keys whose value is looked up by key in InfoPlist.strings.
LOCALIZED_BY_KEY = re.compile(r"^(NS\w+UsageDescription|NSHumanReadableCopyright)$")
# Keys whose *value* is the lookup key (document type names, UTI descriptions).
LOCALIZED_BY_VALUE = {"CFBundleTypeName", "UTTypeDescription"}
STRINGS_ENTRY = re.compile(r'^\s*"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"\s*;\s*$')


def plist_localizable() -> tuple[set[str], list[str]]:
    """(keys InfoPlist.strings must define, CFBundleLocalizations)."""
    root = ET.parse(INFO_PLIST).getroot()
    want: set[str] = set()
    localizations: list[str] = []

    def walk(d: ET.Element) -> None:
        items = list(d)
        for i, el in enumerate(items):
            if el.tag != "key":
                if el.tag in ("dict", "array"):
                    walk(el)
                continue
            value = items[i + 1] if i + 1 < len(items) else None
            if value is None:
                continue
            name = el.text or ""
            if name == "CFBundleLocalizations":
                localizations.extend(s.text or "" for s in value.iter("string"))
            elif value.tag == "string" and "${" not in (value.text or ""):
                if LOCALIZED_BY_KEY.match(name):
                    want.add(name)
                elif name in LOCALIZED_BY_VALUE:
                    want.add(value.text or "")
            if name == "NSHumanReadableCopyright":
                want.add(name)  # set by CMake (${MACOSX_BUNDLE_COPYRIGHT}), localized anyway

    walk(root)
    return want, localizations


def strings_entries(path: pathlib.Path) -> dict[str, str]:
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = STRINGS_ENTRY.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def check_info_plist_strings() -> int:
    if not INFO_PLIST.exists():
        return 0
    want, localizations = plist_localizable()
    problems = 0
    folders = {p.name[:-len(".lproj")] for p in LPROJ_DIR.glob("*.lproj")}
    for lang in localizations:
        if lang not in folders:
            problems += 1
            print(f"{shown(LPROJ_DIR)}: no {lang}.lproj/InfoPlist.strings (CFBundleLocalizations)")
    for strings in sorted(LPROJ_DIR.glob("*.lproj/InfoPlist.strings")):
        have = strings_entries(strings)
        rel = shown(strings)
        for k in sorted(want - have.keys()):
            problems += 1
            print(f"{rel}: missing \"{k}\" (localizable text of Info.plist.in)")
        for k in sorted(have.keys() - want):
            problems += 1
            print(f"{rel}: stale entry \"{k}\" (not in Info.plist.in)")
        for k, v in have.items():
            if not v.strip():
                problems += 1
                print(f"{rel}: empty translation for \"{k}\"")
    if problems:
        print(f"i18n check: {problems} problem(s) in the localized Info.plist texts "
              "(packaging/macos/lproj, translations/README.md)", file=sys.stderr)
        return 1
    print(f"i18n check: InfoPlist.strings complete ({len(want)} texts, {len(folders)} languages)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=pathlib.Path, default=ROOT / "src", help="directory to scan (default src/)")
    ap.add_argument("--allowlist", type=pathlib.Path, default=ALLOWLIST)
    ap.add_argument("--list", action="store_true", help="print all findings, also allowlisted ones")
    ap.add_argument("--lupdate", help="also check translations/*.ts against the sources with this lupdate")
    args = ap.parse_args()
    rc = check_literals(args.root, args.allowlist, args.list)
    rc |= check_info_plist_strings()
    if args.lupdate:
        rc |= check_ts(args.lupdate)
    return rc


if __name__ == "__main__":
    sys.exit(main())
