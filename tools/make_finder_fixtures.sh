#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Creates REAL Finder .DS_Store fixtures (SPEC F-SORT-08, F-SORT-03): the Finder itself
# writes them for neutral, artificial test folders - no user data.
#
#   tools/make_finder_fixtures.sh [--yes] [--out DIR] [--only CASE] [--ui-order] [--gui] [--keep] [--list]
#
#   --yes        do not ask before controlling Finder (needed without a terminal)
#   --out DIR    target directory (default tests/data/finder/macos-<major version>)
#   --only CASE  only this case (repeatable); --list shows all cases
#   --ui-order   also read the displayed row order of list views via System Events
#                (needs Accessibility permission for the terminal). Without it, the
#                expected order of list views is computed from Finder's rule (marked
#                "orderSource": "computed"); icon views always use Finder's own icon
#                positions ("finder-positions")
#   --gui        add the "Date Added" cases: Finder's AppleScript dictionary has no
#                "date added" column/arrangement, so these press View > Sort By >
#                Date Added (ctrl-opt-cmd-4) via System Events (Accessibility permission)
#   --keep       keep the temporary test folders (path is printed)
#
# What it does:
#  1. Creates a fresh temporary directory /private/tmp/lumaflip-finder.XXXXXX (neutral
#     path, no user name) with one folder <case>/Bilder per case. Each holds the same
#     8 small images (IMG_2.jpg, IMG_10.jpg, "IMG_2 (1).jpg", Ärger.png, "a b.jpg",
#     zebra.gif, Bild.tiff, apfel.heic) with fixed, pairwise different sizes,
#     modification and creation dates (SetFile -d if installed, otherwise touch) and
#     "date added" (files are moved in one per second, in a fixed order).
#  2. Uses AppleScript to open each folder in Finder, sets list view + sort column +
#     direction (name, modification date, creation date, size, kind; normal and
#     reversed) or icon view + arrangement (name, modification date, creation date,
#     size, kind, not arranged / snap to grid with set icon positions), reads back what
#     Finder reports and closes the window, so Finder writes its .DS_Store files.
#  3. Copies for each case into <out>/<case>/:
#       parent.DS_Store   <case>/.DS_Store       (settings of "Bilder" live in the parent,
#                                                 record name "Bilder")
#       folder.DS_Store   <case>/Bilder/.DS_Store (record ".", icon positions Iloc)
#       expected.json     macOS version, Finder setting as read back, file metadata,
#                         expected SortSpec (lf::SortKey name + direction) and order
#     The copies are checked for the user name, home path and computer name first.
#  4. Removes the temporary directory (unless --keep). Never deletes anything else.
#
# tests/unit/test_finder_fixtures.cpp checks every fixture against lf's .DS_Store reader
# and sort engine. Finder writes .DS_Store late; the script waits up to 10 s per case.
# The first run asks for permission to control Finder (System Settings > Privacy &
# Security > Automation). See docs/TESTING-M1.md section 4.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$(pwd -P)"

die() { printf 'make_finder_fixtures.sh: %s\n' "$*" >&2; exit 1; }
say() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }

[[ "$(uname)" == Darwin ]] || die "macOS only"
MACOS_VERSION="$(sw_vers -productVersion)"
MACOS_BUILD="$(sw_vers -buildVersion)"
MAJOR="${MACOS_VERSION%%.*}"
OUT="tests/data/finder/macos-$MAJOR"
YES=0; KEEP=0; UI_ORDER=0; GUI=0; LIST=0; ONLY=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --yes) YES=1 ;;
    --out) OUT="$2"; shift ;;
    --only) ONLY+=("$2"); shift ;;
    --ui-order) UI_ORDER=1 ;;
    --gui) GUI=1 ;;
    --keep) KEEP=1 ;;
    --list) LIST=1 ;;
    -h|--help) sed -n '3,45p' "$0"; exit 0 ;;
    *) die "unknown option $1" ;;
  esac; shift
done

# ---- cases: name | view | Finder setting | direction | needs --gui -----------------
CASES=(
  "list-name-normal|list|name column|normal|"
  "list-name-reversed|list|name column|reversed|"
  "list-modified-normal|list|modification date column|normal|"
  "list-modified-reversed|list|modification date column|reversed|"
  "list-created-normal|list|creation date column|normal|"
  "list-created-reversed|list|creation date column|reversed|"
  "list-size-normal|list|size column|normal|"
  "list-size-reversed|list|size column|reversed|"
  "list-kind-normal|list|kind column|normal|"
  "list-kind-reversed|list|kind column|reversed|"
  "list-added|list|date added|default|gui"
  "icon-name|icon|arranged by name||"
  "icon-modified|icon|arranged by modification date||"
  "icon-created|icon|arranged by creation date||"
  "icon-size|icon|arranged by size||"
  "icon-kind|icon|arranged by kind||"
  "icon-added|icon|date added||gui"
  "icon-none|icon|not arranged||"
  "icon-grid|icon|snap to grid||"
)
if [[ $LIST == 1 ]]; then
  for c in "${CASES[@]}"; do IFS='|' read -r name view what dir gui <<<"$c"; echo "$name  ($view: $what${dir:+, $dir}${gui:+, needs --gui})"; done
  exit 0
fi
SELECTED=()
for c in "${CASES[@]}"; do
  IFS='|' read -r name _ _ _ gui <<<"$c"
  if [[ ${#ONLY[@]} -gt 0 ]]; then
    printf '%s\n' "${ONLY[@]}" | grep -qxF "$name" || continue
  elif [[ -n "$gui" && $GUI == 0 ]]; then
    continue
  fi
  SELECTED+=("$c")
done
[[ ${#SELECTED[@]} -gt 0 ]] || die "no case selected (--list shows the cases)"

# ---- the test files: name | sips format | size rank | created | modified | added rank ----
# Every column differs pairwise and gives an order different from the name order.
FILES=(
  "a b.jpg|jpeg|3|202001011000|202403051200|5"
  "apfel.heic|heic|7|202101011000|202401011200|2"
  "Ärger.png|png|1|201901011000|202406301200|7"
  "Bild.tiff|tiff|8|202201011000|202302011200|1"
  "IMG_10.jpg|jpeg|2|201801011000|202405011200|3"
  "IMG_2.jpg|jpeg|5|202301011000|202312011200|8"
  "IMG_2 (1).jpg|jpeg|6|201701011000|202402011200|4"
  "zebra.gif|gif|4|202401011000|202404011200|6"
)
# Icon positions for "not arranged" / "snap to grid" (name x y): 4 columns x 2 rows,
# row-major order deliberately different from every other order.
POSITIONS=(
  "zebra.gif" 80 60   "IMG_10.jpg" 240 60   "a b.jpg" 400 60      "Bild.tiff" 560 60
  "Ärger.png" 80 220  "apfel.heic" 240 220  "IMG_2 (1).jpg" 400 220  "IMG_2.jpg" 560 220
)

echo "Lumaflip Finder fixtures for macOS $MACOS_VERSION ($MACOS_BUILD) -> $OUT"
echo "Cases: $(for c in "${SELECTED[@]}"; do printf '%s ' "${c%%|*}"; done)"
echo
echo "This script opens and closes Finder windows for temporary test folders and changes"
echo "their view settings (only those folders). Do not use Finder while it runs (~1-2 min)."
echo "macOS will ask once whether the terminal may control Finder$([[ $GUI == 1 || $UI_ORDER == 1 ]] && echo ' and System Events (Accessibility)')."
if [[ $YES == 0 ]]; then
  [[ -t 0 ]] || die "not interactive - pass --yes"
  read -r -p "Continue? [y/N] " answer
  [[ "$answer" == [yYjJ]* ]] || exit 1
fi

for tool in osascript sips python3; do command -v "$tool" >/dev/null || die "$tool not found"; done

# ---- temporary root (own mktemp directory, removed on exit unless --keep) ----------
ROOT="$(mktemp -d /private/tmp/lumaflip-finder.XXXXXX)"
cleanup() {
  if [[ $KEEP == 1 ]]; then
    echo "Test folders kept: $ROOT"
  elif [[ -n "${ROOT:-}" && -d "$ROOT" && ! -L "$ROOT" && "$ROOT" == /private/tmp/lumaflip-finder.?????? ]]; then
    rm -r -- "$ROOT"
  fi
}
trap cleanup EXIT

# ---- 1. images ----------------------------------------------------------------------
say "Creating test images in $ROOT"
SRC="$ROOT/_src"; mkdir "$SRC"
ICON=/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericDocumentIcon.icns
sips -s format png -z 48 48 "$ICON" --out "$SRC/base.png" >/dev/null 2>&1 \
  || die "sips could not render $ICON"
for f in "${FILES[@]}"; do
  IFS='|' read -r name fmt rank _ _ _ <<<"$f"
  sips -s format "$fmt" "$SRC/base.png" --out "$SRC/$name" >/dev/null 2>&1 || die "sips: $name ($fmt)"
  # Logical sizes 40 000 + rank * 6 000 bytes (also different in 4 KiB blocks); the
  # padding after the image data is ignored by image decoders.
  target=$((40000 + rank * 6000))
  size=$(stat -f %z "$SRC/$name")
  (( size < target )) || die "$name: $size bytes, more than the target $target"
  head -c $((target - size)) /dev/zero >> "$SRC/$name"
done

# Fixed creation/modification dates. touch fallback: an mtime before the birth time
# moves the birth time back (APFS), then the real mtime is set.
set_dates() { # file created(CCYYMMDDhhmm) modified
  local file="$1" c="$2" m="$3"
  if command -v SetFile >/dev/null; then
    SetFile -d "${c:4:2}/${c:6:2}/${c:0:4} ${c:8:2}:${c:10:2}:00" "$file"
    SetFile -m "${m:4:2}/${m:6:2}/${m:0:4} ${m:8:2}:${m:10:2}:00" "$file"
  else
    touch -t "$c" "$file"
    touch -m -t "$m" "$file"
  fi
}

say "Creating ${#SELECTED[@]} case folders"
for c in "${SELECTED[@]}"; do
  case_name="${c%%|*}"
  mkdir -p "$ROOT/$case_name/Bilder" "$ROOT/_stage/$case_name"
  for f in "${FILES[@]}"; do
    IFS='|' read -r name _ _ created modified _ <<<"$f"
    cp "$SRC/$name" "$ROOT/_stage/$case_name/$name"
    set_dates "$ROOT/_stage/$case_name/$name" "$created" "$modified"
  done
done
# "Date added" = time the file was moved into the folder: one rank per second.
for rank in 1 2 3 4 5 6 7 8; do
  for f in "${FILES[@]}"; do
    IFS='|' read -r name _ _ _ _ added <<<"$f"
    [[ "$added" == "$rank" ]] || continue
    for c in "${SELECTED[@]}"; do
      case_name="${c%%|*}"
      mv "$ROOT/_stage/$case_name/$name" "$ROOT/$case_name/Bilder/$name"
    done
  done
  sleep 1.1
done

# ---- 2. Finder ----------------------------------------------------------------------
# argv: folder view what direction uiOrder [name x y]...
# Output lines: SETTING<tab>text | POS<tab>name<tab>x<tab>y | ORDER<tab>name | UIORDER-FAILED<tab>why
read -r -d '' FINDER_SCRIPT <<'APPLESCRIPT' || true
on run argv
	set folderPath to item 1 of argv
	set viewName to item 2 of argv
	set what to item 3 of argv
	set dirName to item 4 of argv
	set uiOrder to (item 5 of argv) is "1"
	set posArgs to {}
	if (count of argv) > 5 then set posArgs to items 6 thru -1 of argv
	set out to {}
	tell application "Finder"
		activate
		open ((POSIX file folderPath) as alias)
		delay 1
		set w to Finder window 1
		set fo to target of w
		set bounds of w to {100, 100, 900, 600}
		if viewName is "list" then
			set current view of w to list view
			delay 0.5
			if what is "date added" then
				my sortByDateAdded()
				set end of out to "SETTING" & tab & "View > Sort By > Date Added (ctrl-opt-cmd-4)"
			else
				set lvo to list view options of w
				if what is "name column" then
					set c to column id name column of lvo
				else if what is "modification date column" then
					set c to column id modification date column of lvo
				else if what is "creation date column" then
					set c to column id creation date column of lvo
				else if what is "size column" then
					set c to column id size column of lvo
				else if what is "kind column" then
					set c to column id kind column of lvo
				else
					error "unknown column " & what
				end if
				set visible of c to true
				set sort column of lvo to c
				if dirName is "reversed" then
					set sort direction of c to reversed
				else
					set sort direction of c to normal
				end if
				delay 0.5
				set sc to sort column of lvo
				set end of out to "SETTING" & tab & ((name of sc) as text) & ", " & ((sort direction of sc) as text)
			end if
		else
			set current view of w to icon view
			delay 0.5
			set ivo to icon view options of w
			if what is "date added" then
				my sortByDateAdded()
			else if what is "arranged by name" then
				set arrangement of ivo to arranged by name
			else if what is "arranged by modification date" then
				set arrangement of ivo to arranged by modification date
			else if what is "arranged by creation date" then
				set arrangement of ivo to arranged by creation date
			else if what is "arranged by size" then
				set arrangement of ivo to arranged by size
			else if what is "arranged by kind" then
				set arrangement of ivo to arranged by kind
			else if what is "not arranged" or what is "snap to grid" then
				if what is "not arranged" then
					set arrangement of ivo to not arranged
				else
					set arrangement of ivo to snap to grid
				end if
				repeat with i from 1 to (count of posArgs) by 3
					set n to item i of posArgs
					set position of item n of fo to {(item (i + 1) of posArgs) as integer, (item (i + 2) of posArgs) as integer}
				end repeat
			else
				error "unknown arrangement " & what
			end if
			delay 1
			if what is "date added" then
				set end of out to "SETTING" & tab & "View > Sort By > Date Added (ctrl-opt-cmd-4); arrangement " & ((arrangement of ivo) as text)
			else
				set end of out to "SETTING" & tab & ((arrangement of ivo) as text)
			end if
			set ns to name of every item of fo
			set ps to position of every item of fo
			repeat with i from 1 to count of ns
				set p to item i of ps
				set end of out to "POS" & tab & (item i of ns) & tab & ((item 1 of p) as text) & tab & ((item 2 of p) as text)
			end repeat
		end if
	end tell
	if uiOrder and viewName is "list" then set out to out & my listOrder()
	tell application "Finder" to close w
	set AppleScript's text item delimiters to linefeed
	return out as text
end run

on sortByDateAdded()
	-- View > Sort By > Date Added; key code 21 = "4" on every keyboard layout
	tell application "System Events" to key code 21 using {control down, option down, command down}
	delay 1
end sortByDateAdded

-- Displayed row order of the front Finder list view (Accessibility).
on listOrder()
	set res to {}
	try
		tell application "System Events" to tell process "Finder"
			set o to missing value
			repeat with e in (entire contents of window 1)
				try
					if role of e is "AXOutline" then
						set o to e
						exit repeat
					end if
				end try
			end repeat
			if o is missing value then return {"UIORDER-FAILED" & tab & "no outline found"}
			repeat with r in rows of o
				set n to missing value
				try
					set n to value of text field 1 of UI element 1 of r
				end try
				if n is missing value then
					try
						set n to value of text field 1 of r
					end try
				end if
				if n is missing value then return {"UIORDER-FAILED" & tab & "row without a name"}
				set end of res to "ORDER" & tab & n
			end repeat
		end tell
	on error msg
		return {"UIORDER-FAILED" & tab & msg}
	end try
	return res
end listOrder
APPLESCRIPT

STAGED="$ROOT/_out"; mkdir "$STAGED"
for c in "${SELECTED[@]}"; do
  IFS='|' read -r case_name view what dir _ <<<"$c"
  folder="$ROOT/$case_name/Bilder"
  say "$case_name: $view view, $what${dir:+ ($dir)}"
  marker="$ROOT/_stage/$case_name.marker"; : > "$marker"; sleep 1.1
  args=("$folder" "$view" "$what" "${dir:-}" "$UI_ORDER")
  [[ "$what" == "not arranged" || "$what" == "snap to grid" ]] && args+=("${POSITIONS[@]}")
  if ! osascript - "${args[@]}" <<<"$FINDER_SCRIPT" > "$ROOT/_stage/$case_name.finder" 2> "$ROOT/_stage/$case_name.err"; then
    cat "$ROOT/_stage/$case_name.err" >&2
    die "$case_name: AppleScript failed (Automation permission? System Settings > Privacy & Security > Automation)"
  fi
  sed -n 's/^SETTING\t/    Finder reports: /p' "$ROOT/_stage/$case_name.finder"
  # Finder writes .DS_Store asynchronously after closing the window.
  for _ in $(seq 1 40); do
    [[ "$ROOT/$case_name/.DS_Store" -nt "$marker" || "$folder/.DS_Store" -nt "$marker" ]] && break
    sleep 0.25
  done
  sleep 1 # let Finder finish writing both files
  [[ -f "$ROOT/$case_name/.DS_Store" || -f "$folder/.DS_Store" ]] || die "$case_name: Finder wrote no .DS_Store"
  mkdir "$STAGED/$case_name"
  [[ -f "$ROOT/$case_name/.DS_Store" ]] && cp "$ROOT/$case_name/.DS_Store" "$STAGED/$case_name/parent.DS_Store"
  [[ -f "$folder/.DS_Store" ]] && cp "$folder/.DS_Store" "$STAGED/$case_name/folder.DS_Store"
  osascript -l JavaScript "$REPO/tools/finder_fixture_meta.js" "$folder" > "$ROOT/_stage/$case_name.meta.json"
done

# ---- 3. expected.json + privacy check, then copy to $OUT ----------------------------
say "Writing expected.json, checking for private data"
FILE_TABLE="$(printf '%s\n' "${FILES[@]}")"
export FILE_TABLE MACOS_VERSION MACOS_BUILD
python3 -I - "$ROOT" "$STAGED" "${SELECTED[@]}" <<'PY'
import datetime, json, os, pathlib, subprocess, sys

root, staged, cases = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3:]
KEY = {"name column": "Name", "modification date column": "DateModified",
       "creation date column": "DateCreated", "size column": "Size", "kind column": "Kind",
       "date added": "DateAdded", "arranged by name": "Name",
       "arranged by modification date": "DateModified", "arranged by creation date": "DateCreated",
       "arranged by size": "Size", "arranged by kind": "Kind",
       "not arranged": "FinderIconPosition", "snap to grid": "FinderIconPosition"}
# Finder's default direction: names/kinds ascending, dates and sizes newest/largest first.
DEFAULT_ASC = {"Name": True, "Kind": True, "DateModified": False, "DateCreated": False,
               "DateAdded": False, "Size": False, "FinderIconPosition": True}

def utc(stamp):  # CCYYMMDDhhmm local time -> ISO UTC
    t = datetime.datetime.strptime(stamp, "%Y%m%d%H%M").astimezone(datetime.timezone.utc)
    return t.strftime("%Y-%m-%dT%H:%M:%S.000Z")

intended = {}
for line in os.environ["FILE_TABLE"].splitlines():
    name, _fmt, _rank, created, modified, _added = line.split("|")
    intended[name] = (utc(created), utc(modified))

needles = {os.environ.get("USER", ""), os.environ.get("HOME", ""), os.path.expanduser("~")}
for cmd in (["scutil", "--get", "ComputerName"], ["scutil", "--get", "LocalHostName"], ["id", "-F"]):
    try:
        needles.add(subprocess.run(cmd, capture_output=True, text=True).stdout.strip())
    except OSError:
        pass
needles = {n for n in needles if len(n) >= 3}

failed = False
for spec in cases:
    case, view, what, direction, _gui = spec.split("|")
    stage = root / "_stage"
    meta = json.loads((stage / f"{case}.meta.json").read_text())
    files = {f["name"]: f for f in meta["files"]}
    for name, (created, modified) in intended.items():
        f = files.get(name)
        if not f or f["created"] != created or f["modified"] != modified:
            sys.exit(f"{case}: {name}: dates not as intended ({f and f['created']}, {f and f['modified']}"
                     f" instead of {created}, {modified})")
    lines = (stage / f"{case}.finder").read_text().splitlines()
    setting = "; ".join(l.split("\t", 1)[1] for l in lines if l.startswith("SETTING\t"))
    key = KEY[what]
    observed, source, note = None, "computed", ""
    pos = [l.split("\t") for l in lines if l.startswith("POS\t")]
    if pos:
        pts = {n: (float(x), float(y)) for _, n, x, y in pos if n in files}
        if len(pts) == len(files) and len(set(pts.values())) == len(pts):
            observed = sorted(pts, key=lambda n: (pts[n][1], pts[n][0]))
            source = "finder-positions"
        else:
            note = "icon positions incomplete or overlapping"
    rows = [l.split("\t", 1)[1] for l in lines if l.startswith("ORDER\t")]
    if rows:
        stems = {os.path.splitext(n)[0]: n for n in files}  # "hide extensions"
        names = [r if r in files else stems.get(r, r) for r in rows]
        if sorted(names) == sorted(files):
            observed, source = names, "finder-ui"
        else:
            note = "list rows do not match the files"
    for l in lines:
        if l.startswith("UIORDER-FAILED\t"):
            note = "UI order not available: " + l.split("\t", 1)[1]
    ascending = DEFAULT_ASC[key] if direction != "reversed" else not DEFAULT_ASC[key]
    spec_source = "assumed (normal = Finder's default direction of the column)" if view == "list" \
        else "assumed (Finder's fixed direction of the arrangement)"
    if key == "FinderIconPosition":
        spec_source = "icon positions"
        order = observed
    else:
        orders = meta["orders"][key]
        order = orders["asc" if ascending else "desc"]
        if observed is not None:
            if observed == orders["asc"] or observed == orders["desc"]:
                ascending = observed == orders["asc"]
                spec_source = "derived from the displayed order"
            else:
                print(f"WARNING {case}: Finder's order {observed} matches neither direction of "
                      f"{key} - recorded as is, the fixture test will fail", file=sys.stderr)
            order = observed
    if order is None:
        sys.exit(f"{case}: no order ({note})")
    expected = {
        "generator": "tools/make_finder_fixtures.sh",
        "case": case,
        "macos": os.environ["MACOS_VERSION"],
        "macosBuild": os.environ["MACOS_BUILD"],
        "view": view,
        "requested": what + (f", {direction}" if direction else ""),
        "finderSetting": setting,
        "folder": "Bilder",
        "expected": {"key": key, "ascending": ascending, "source": spec_source},
        "order": order,
        "orderSource": source,
        "files": [{k: f[k] for k in ("name", "size", "modified", "created", "added", "kind")}
                  for f in sorted(meta["files"], key=lambda f: f["name"])],
    }
    if note:
        expected["note"] = note
    (staged / case / "expected.json").write_text(json.dumps(expected, ensure_ascii=False, indent=2) + "\n")
    for ds in (staged / case).glob("*.DS_Store"):
        data = ds.read_bytes()
        for n in needles:
            if n.encode("utf-8") in data or n.encode("utf-16-be") in data:
                print(f"PRIVATE DATA: {ds.name} of {case} contains {n!r}", file=sys.stderr)
                failed = True
    print(f"  {case}: {key} {'asc' if ascending else 'desc'} ({spec_source}); order from {source}")
sys.exit(1 if failed else 0)
PY

mkdir -p "$OUT"
for c in "${SELECTED[@]}"; do
  case_name="${c%%|*}"
  mkdir -p "$OUT/$case_name"
  rm -f "$OUT/$case_name/parent.DS_Store" "$OUT/$case_name/folder.DS_Store" "$OUT/$case_name/expected.json"
  cp "$STAGED/$case_name/"* "$OUT/$case_name/"
done
say "Done: $OUT"
echo "Check: build/check/tests/lf_unit_tests --gtest_filter='FinderFixtures.*'"
echo "Then commit $OUT (no personal data: only the temporary test folders were used)."
