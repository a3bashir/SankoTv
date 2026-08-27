#!/usr/bin/env python3
"""Record what is on disk for every SankoTV project. READ ONLY.

Written to replace the repair log that was lost: that log said what MOVED,
this says what IS. Run it again whenever a durable record is wanted; each
run writes a new dated file and never touches a project folder.

For every project it records the path, an intact/damaged verdict, and every
referenced file with its size and SHA-256. For every project FOLDER it also
records files that NO manifest references - orphans, which after the
shared-folder repair are the originals left behind on purpose. Naming them
here means they are identified rather than discovered later and wondered
about.

    python make_inventory.py                 # default roots
    python make_inventory.py --root <dir>    # ONLY these roots
    python make_inventory.py --out <file>    # override the output path
"""

import argparse
import collections
import glob
import hashlib
import json
import os
import time

DEFAULT_ROOTS = [
    r"C:/Users/User/Downloads/SankoTV_Save _test_Files",
    r"C:/Users/User/Downloads/SankoTV_test",
    r"C:/Users/User/Documents/SankoTV",
]


def referenced_files(manifest_path):
    """Both spellings: modern layers[].imageFile and LEGACY panel.pixmapFile.
    Missing the legacy one under-reports, which is exactly how an earlier
    survey mis-scored five projects as referencing nothing."""
    with open(manifest_path, encoding="utf-8") as handle:
        doc = json.load(handle)
    names = []
    for scene in doc.get("scenes", []) or []:
        for panel in scene.get("panels", []) or []:
            for layer in panel.get("layers", []) or []:
                if layer.get("imageFile"):
                    names.append(layer["imageFile"])
            if panel.get("pixmapFile"):
                names.append(panel["pixmapFile"])
    for entry in doc.get("consistency", []) or []:
        if entry.get("thumbnailFile"):
            names.append(entry["thumbnailFile"])
    seen, ordered = set(), []
    for name in names:
        if name not in seen:
            seen.add(name)
            ordered.append(name)
    return ordered, doc


def is_sankotv_artefact(name):
    """A file SankoTV itself wrote: panel flattens, layer images, and
    consistency thumbnails. Anything else in a folder is the user's."""
    low = name.lower()
    return (low.startswith("panel_s") or low.startswith("consistency_"))         and low.endswith(".png")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", default=[],
                        help="search ONLY these folders (replaces defaults)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    roots = args.root if args.root else list(DEFAULT_ROOTS)
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = args.out or os.path.join(
        here, "inventory", "inventory_" + time.strftime("%Y%m%d") + ".md")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    manifests = set()
    for root in roots:
        if os.path.isdir(root):
            manifests.update(glob.glob(os.path.join(root, "**", "*.sankotv"),
                                       recursive=True))

    by_folder = collections.defaultdict(list)
    for path in sorted(manifests):
        by_folder[os.path.dirname(path)].append(path)

    # Also visit folders that hold images but NO manifest. After the
    # shared-folder repair those are exactly where the left-behind originals
    # sit, and a per-manifest walk would never look at them - which is how
    # orphans get discovered months later and wondered about.
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, files in os.walk(root):
            if dirpath in by_folder:
                continue
            # Only SankoTV's OWN artefacts count. A folder of reference
            # images or brush files is not an orphaned project - flagging
            # it would bury the real leftovers in noise.
            if any(is_sankotv_artefact(f) for f in files):
                by_folder[dirpath] = []

    lines = []
    lines.append("# SankoTV project inventory")
    lines.append("")
    lines.append(f"Taken {time.strftime('%Y-%m-%d %H:%M:%S')} - READ ONLY; no "
                 f"project folder was modified.")
    lines.append("")
    lines.append("Roots searched:")
    for root in roots:
        lines.append(f"  - `{root}`" + ("" if os.path.isdir(root)
                                        else "  (not present)"))
    lines.append("")
    lines.append("`damaged` means at least one image this project references "
                 "was rewritten AFTER")
    lines.append("the project was last saved - i.e. another project "
                 "overwrote it back when they")
    lines.append("shared a folder. It is a historical fact, not something the "
                 "current layout can undo.")
    lines.append("")

    total_projects = total_files = total_orphans = 0
    damaged_names = []

    for folder in sorted(by_folder):
        paths = by_folder[folder]
        lines.append("---")
        lines.append("")
        lines.append(f"## `{folder}`")
        lines.append("")
        if not paths:
            lines.append("**No project file here — every file below is an "
                         "orphan.**")
            lines.append("")
        elif len(paths) > 1:
            lines.append(f"**{len(paths)} projects share this folder — they "
                         f"share pixel files.**")
            lines.append("")
        claimed = set()
        for path in paths:
            total_projects += 1
            names, doc = referenced_files(path)
            saved = os.path.getmtime(path)
            rewritten = []
            for name in names:
                full = os.path.join(folder, name)
                if os.path.exists(full) and os.path.getmtime(full) > saved + 2:
                    rewritten.append(name)
            verdict = "DAMAGED" if rewritten else "intact"
            if rewritten:
                damaged_names.append(os.path.basename(path))
            claimed.update(os.path.normcase(n) for n in names)

            lines.append(f"### {os.path.basename(path)} — **{verdict}**")
            lines.append("")
            lines.append(f"- path: `{path}`")
            lines.append(f"- project name in file: `{doc.get('projectName')}`"
                         f"  fps: `{doc.get('fps')}`  canvas: "
                         f"`{doc.get('canvasWidth')}x{doc.get('canvasHeight')}`")
            lines.append(f"- last saved: "
                         f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(saved))}")
            if rewritten:
                lines.append(f"- **{len(rewritten)} of {len(names)} referenced "
                             f"images were overwritten after this project's "
                             f"own save**")
            lines.append(f"- references {len(names)} file(s):")
            lines.append("")
            lines.append("| file | bytes | sha256 |")
            lines.append("|---|---:|---|")
            for name in names:
                full = os.path.join(folder, name)
                if os.path.exists(full):
                    lines.append(f"| `{name}` | {os.path.getsize(full)} | "
                                 f"`{sha256(full)}` |")
                    total_files += 1
                else:
                    lines.append(f"| `{name}` | — | **MISSING** |")
            lines.append("")

        # Orphans: files in the folder that no manifest here references.
        orphans = []
        for entry in sorted(os.listdir(folder)):
            full = os.path.join(folder, entry)
            if not os.path.isfile(full) or entry.lower().endswith(".sankotv"):
                continue
            if os.path.normcase(entry) in claimed:
                continue
            if not paths and not is_sankotv_artefact(entry):
                continue  # unrelated file in a folder with no project
            orphans.append(entry)
        if orphans:
            total_orphans += len(orphans)
            lines.append(f"### Orphans in this folder — {len(orphans)} file(s) "
                         f"no manifest here references")
            lines.append("")
            lines.append("These are safe to leave. After the shared-folder "
                         "repair they are the originals")
            lines.append("kept deliberately: each project got its own COPY, "
                         "and nothing was deleted.")
            lines.append("")
            lines.append("| file | bytes | sha256 |")
            lines.append("|---|---:|---|")
            for name in orphans:
                full = os.path.join(folder, name)
                lines.append(f"| `{name}` | {os.path.getsize(full)} | "
                             f"`{sha256(full)}` |")
            lines.append("")

    summary = (f"{total_projects} project(s), {total_files} referenced file(s), "
               f"{total_orphans} orphan(s), {len(damaged_names)} damaged.")
    header = [f"**{summary}**", ""]
    if damaged_names:
        header += ["Damaged: " + ", ".join(sorted(damaged_names)), ""]
    at = lines.index("Roots searched:")   # summary sits above the roots list
    lines[at:at] = header

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print(summary)
    print("written to", out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
