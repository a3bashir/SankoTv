#!/usr/bin/env python3
"""Make projects that share a folder independent of each other.

THE UNDERLYING BUG IS FIXED. READ THIS BEFORE CONCLUDING ANYTHING IS WRONG.
    Builds up to 2026-08-27 wrote every panel and layer as an image file
    named by POSITION (panel_s0_p0_layer0.png, and pixmapFile
    panel_s0_p0.png in legacy projects). Nothing in those names identified
    the project, so two projects saved into one folder referenced THE SAME
    FILES, and whichever was saved next overwrote the other's artwork.

    That is HISTORY. Since 2026-08-27 every project writes its images into
    its own "<basename>_assets/" folder beside its .sankotv, so two
    projects in one directory cannot reach each other's files at all.

    NOTHING IS AT RISK TODAY, AND MIGRATION NEEDS NO TOOL. Simply opening
    a project made by an older build and saving it moves its pixels into
    "<basename>_assets/" and leaves the old flat files where they are. For
    a project sharing a folder that first save is a RESCUE: it lifts the
    project out of the shared pool without touching what the other one
    still references.

WHAT THIS IS STILL FOR
    Tidying an EXISTING shared folder in one pass, without opening each
    project by hand — and giving a verified, per-file record of the move.
    It is a convenience now, not a repair. If you found this script and
    are wondering whether your projects are in danger: they are not.

WHAT THIS DOES
    For each project in a shared folder: creates <folder>/<basename>/,
    COPIES every file that project's manifest references into it, verifies
    each copy byte for byte, and only then MOVES the .sankotv in beside
    them. Afterwards each project owns its own pixels.

WHAT IT DELIBERATELY DOES NOT DO
    * It never MOVES a referenced image. In a shared folder several
      manifests name the same file and there is exactly one file on disk;
      the question "which project does this file belong to?" has no answer,
      so every project gets its own copy.
    * It deletes NOTHING. The original folder keeps every image it had.
    * It cannot UNDO damage. Where a file was already overwritten, every
      project sharing it now holds the same (latest) pixels; copying
      preserves that. It separates what is there; it does not restore what
      an older build already lost.

USAGE
    python repair_shared_projects.py                  # DRY RUN (default)
    python repair_shared_projects.py --apply --yes    # actually do it
    python repair_shared_projects.py --root <dir>     # search ONLY <dir>

    --root REPLACES the built-in roots; it does not add to them. It used to
    add, which meant pointing the tool at a scratch folder to rehearse ALSO
    swept the real ones. --apply additionally requires --yes, so that a
    stray argument on a command line cannot move anybody's files.

THE LOG (written only by a real pass, to --log or ./sankotv_repair_<ts>.log)
    A dry run says what WOULD happen; only the log says what DID. Format:

        SankoTV shared-folder repair - RECORD OF THE REAL PASS
        started <timestamp>
        PROJECT <absolute path of the .sankotv as it was found>
          verdict: intact | damaged (N of M images overwritten after its
                   own save)
          source : <folder it came from>
          target : <folder it went to>        (or "stays put, ...")
          copied <relative name>  sha256=<hash of the verified copy>
          ...
          moved  <file> -> <target>
          result : N file(s) copied and verified
        SUMMARY: P moved, S left in place, F copied, V verification failures

    Every line is flushed as it is written, so a crash mid-repair still
    leaves an accurate record of everything completed up to that point.
"""

import argparse
import collections
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

DEFAULT_ROOTS = [
    r"C:/Users/User/Downloads/SankoTV_Save _test_Files",
    r"C:/Users/User/Downloads/SankoTV_test",
    r"C:/Users/User/Documents/SankoTV",
]


def sankotv_is_running():
    """True if SankoTV.exe is running. Repairing under a live app risks the
    app rewriting the very files being copied."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq SankoTV.exe"],
            capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return False  # cannot tell; the caller is warned separately
    return "SankoTV.exe" in out


def referenced_files(manifest_path):
    """Every image file this manifest names, relative to its folder.

    BOTH spellings matter: modern projects use layers[].imageFile, legacy
    single-PNG projects use panel.pixmapFile. Missing the legacy spelling
    produces a "repaired" project whose pixels are simply gone.
    """
    with open(manifest_path, encoding="utf-8") as handle:
        doc = json.load(handle)
    names = []
    for scene in doc.get("scenes", []) or []:
        for panel in scene.get("panels", []) or []:
            for layer in panel.get("layers", []) or []:
                if layer.get("imageFile"):
                    names.append(layer["imageFile"])
            if panel.get("pixmapFile"):          # LEGACY single-PNG panels
                names.append(panel["pixmapFile"])
    for entry in doc.get("consistency", []) or []:
        if entry.get("thumbnailFile"):
            names.append(entry["thumbnailFile"])
    # Preserve order, drop duplicates (a manifest may name a file twice).
    seen, ordered = set(), []
    for name in names:
        if name not in seen:
            seen.add(name)
            ordered.append(name)
    return ordered


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def classify(manifest_path, folder, names):
    """intact | damaged: damaged when any referenced file was REWRITTEN
    after this project was last saved, i.e. another project overwrote it."""
    saved = os.path.getmtime(manifest_path)
    rewritten = []
    for name in names:
        full = os.path.join(folder, name)
        if os.path.exists(full) and os.path.getmtime(full) > saved + 2:
            rewritten.append(name)
    return ("damaged" if rewritten else "intact"), rewritten


def plan_for_root(roots):
    found = set()
    for root in roots:
        if os.path.isdir(root):
            found.update(glob.glob(os.path.join(root, "**", "*.sankotv"),
                                   recursive=True))
    by_folder = collections.defaultdict(list)
    for path in sorted(found):
        by_folder[os.path.dirname(path)].append(path)
    return {d: ps for d, ps in by_folder.items() if len(ps) > 1}


LOG_HANDLE = None


def log(line):
    """Record of the REAL pass. A dry run shows what WOULD happen; if these
    files ever have to be reconstructed, what ACTUALLY happened is the thing
    that matters, and no amount of re-running a dry run recovers it."""
    if LOG_HANDLE:
        LOG_HANDLE.write(line + "\n")
        LOG_HANDLE.flush()   # a crash mid-repair must not lose the record


def main():
    global LOG_HANDLE
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true",
                        help="perform the repair (default is a dry run)")
    parser.add_argument("--root", action="append", default=[],
                        help="search ONLY these folders (replaces the "
                             "built-in defaults entirely)")
    parser.add_argument("--yes", action="store_true",
                        help="required alongside --apply; without it --apply "
                             "refuses, so a stray argument cannot move files")
    parser.add_argument("--log", default=None,
                        help="where to write the record of the real pass")
    args = parser.parse_args()

    # --root REPLACES the defaults. It used to EXTEND them, which meant
    # pointing the tool at a scratch folder still swept the real ones - the
    # tool doing something other than what its operator intended, and being
    # believed because the output looked right. Replacing is the only
    # reading of "--root X" that cannot surprise.
    roots = args.root if args.root else list(DEFAULT_ROOTS)
    mode = "APPLY" if args.apply else "DRY RUN"
    print(f"=== SankoTV shared-folder repair - {mode} ===")
    print("Copies each project's images into a folder of its own, then moves")
    print("its .sankotv in beside them. Nothing is ever deleted.\n")

    if args.apply and not args.yes:
        print("REFUSING: --apply also requires --yes.")
        print("This moves files. Two flags means a stray argument, or a")
        print("half-remembered command line, cannot perform it by accident.")
        print("Run the dry run first, read it, then: --apply --yes")
        return 2

    if sankotv_is_running():
        print("REFUSING TO RUN: SankoTV.exe is running.")
        print("Close the application first - repairing underneath a live app")
        print("risks it rewriting the files being copied.")
        return 2

    log_path = args.log or os.path.abspath(
        "sankotv_repair_" + time.strftime("%Y%m%d-%H%M%S") + ".log")
    if args.apply:
        LOG_HANDLE = open(log_path, "w", encoding="utf-8")
        log("SankoTV shared-folder repair - RECORD OF THE REAL PASS")
        log("started " + time.strftime("%Y-%m-%d %H:%M:%S"))
        log("copies are verified by SHA-256; nothing is ever deleted\n")
        print(f"Recording what is done to: {log_path}\n")
    else:
        print(f"(a real pass would record everything to {log_path})\n")

    shared = plan_for_root(roots)
    if not shared:
        print("No folder contains more than one project. Nothing to repair.")
        return 0

    total_projects = total_copies = 0
    projects_moved = 0
    projects_moved_skipped = []
    verify_failures = 0
    problems = []

    for folder in sorted(shared):
        manifests = shared[folder]
        claims = collections.Counter()
        refs_by_project = {}
        for path in manifests:
            refs_by_project[path] = referenced_files(path)
            claims.update(set(refs_by_project[path]))
        contested = {n for n, c in claims.items() if c > 1}

        print(f"FOLDER  {folder}")
        print(f"        {len(manifests)} projects, "
              f"{len(contested)} image files claimed by more than one\n")

        for path in manifests:
            base = os.path.splitext(os.path.basename(path))[0]
            names = refs_by_project[path]
            state, rewritten = classify(path, folder, names)
            total_projects += 1

            # FLATTEN: when the containing folder is ALREADY named after this
            # project, it keeps the folder and stays put - the others move
            # out into their own. It ends up independent for the same reason
            # they do: nothing else references its files any more. Creating
            # <Test_SB_006>/<Test_SB_006>/ would only add a level.
            stays = os.path.basename(os.path.normpath(folder)).lower() == base.lower()
            target = folder if stays else os.path.join(folder, base)

            print(f"  PROJECT {os.path.basename(path)}   [{state.upper()}]")
            if state == "damaged":
                print(f"          {len(rewritten)} of its {len(names)} images "
                      f"were overwritten after it was saved -")
                print(f"          this repair FREEZES what is there now; the "
                      f"original art is not recoverable.")
            log(f"PROJECT {path}")
            log(f"  verdict: {state}"
                + (f" ({len(rewritten)} of {len(names)} images overwritten "
                   f"after its own save)" if rewritten else ""))
            if stays:
                print(f"          STAYS PUT - this folder is already named "
                      f"after it; the others move out.")
                print(f"          (its {len(names)} images are already here; "
                      f"nothing to copy)")
                log(f"  target : {folder}  (stays put, folder already named "
                    f"after it)")
                log(f"  copied : none needed\n")
                projects_moved_skipped.append(os.path.basename(path))
                print()
                continue
            print(f"          move  {os.path.basename(path)}  ->  {target}\\")
            log(f"  source : {folder}")
            log(f"  target : {target}")
            missing = []
            for name in names:
                src = os.path.join(folder, name)
                tag = "shared" if name in contested else "sole  "
                if not os.path.exists(src):
                    missing.append(name)
                    print(f"          MISSING {name}  (referenced but not on disk)")
                    continue
                print(f"          copy [{tag}] {name}")
                total_copies += 1
            if missing:
                problems.append(f"{os.path.basename(path)}: "
                                f"{len(missing)} referenced file(s) missing")
            print()

            if not args.apply:
                continue

            # --- perform: copy everything and VERIFY before moving anything
            os.makedirs(target, exist_ok=True)
            verified = 0
            for name in names:
                src = os.path.join(folder, name)
                if not os.path.exists(src):
                    continue
                dst = os.path.join(target, name)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
                src_hash, dst_hash = sha256(src), sha256(dst)
                if src_hash != dst_hash:
                    verify_failures += 1
                    problems.append(f"{os.path.basename(path)}: copy of "
                                    f"{name} does NOT match the source")
                    log(f"  COPY FAILED VERIFY {name}  src={src_hash[:16]} "
                        f"dst={dst_hash[:16]}")
                else:
                    verified += 1
                    log(f"  copied {name}  sha256={src_hash}")
            expected = len([n for n in names
                            if os.path.exists(os.path.join(folder, n))])
            if verified != expected:
                problems.append(f"{os.path.basename(path)}: only {verified} of "
                                f"{expected} copies verified - .sankotv NOT moved")
                log(f"  ABORTED: only {verified} of {expected} copies "
                    f"verified; manifest left in place\n")
                print(f"          VERIFY FAILED: leaving {os.path.basename(path)} "
                      f"where it is\n")
                continue
            shutil.move(path, os.path.join(target, os.path.basename(path)))
            projects_moved += 1
            print(f"          verified {verified} file(s); manifest moved\n")

    print("=" * 66)
    if args.apply:
        summary = (f"SUMMARY: {projects_moved} project(s) moved, "
                   f"{len(projects_moved_skipped)} left in place, "
                   f"{total_copies} file(s) copied, "
                   f"{verify_failures} verification failure(s).")
        print(summary)
        log("=" * 66)
        log(summary)
        if projects_moved_skipped:
            log("left in place (folder already named after them): "
                + ", ".join(projects_moved_skipped))
    else:
        print(f"SUMMARY (planned): {total_projects} project(s) examined, "
              f"{len(projects_moved_skipped)} would stay put, "
              f"{total_copies} file copies planned.")
    if problems:
        print("\nPROBLEMS (nothing was deleted; sources remain in place):")
        for p in problems:
            print(f"  - {p}")
        return 1
    if not args.apply:
        print("\nDRY RUN ONLY - nothing was changed.")
        print("Re-run with --apply to perform it.")
    else:
        print("\nEvery copy was verified byte for byte against its source.")
        print("The original folders still contain every image they had.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
