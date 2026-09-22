#!/usr/bin/env python3
"""Extract the game data for the Monster Shooter PS Vita port from your own APK.

This port ships only the loader. The game itself is not redistributed: you need
a copy of the original Monster Shooter Android APK, which this script unpacks
into the layout the Vita expects.

    python3 extract_apk.py monster_shooter.apk

It writes ./monstershooter/ - copy that folder to ux0:data/ on your Vita, so you
end up with ux0:data/monstershooter/libClawNativeApp.so and so on.
"""

import hashlib
import sys
import zipfile
from pathlib import Path

# Where each APK member goes, relative to the output folder.
WANTED = {
    "lib/armeabi-v7a/libClawNativeApp.so": "libClawNativeApp.so",
    "lib/armeabi-v7a/libstlport_shared.so": "libstlport_shared.so",
    "assets/data.pak": "assets/data.pak",
    "assets/data-android.pak": "assets/data-android.pak",
    "assets/data-normal.pak": "assets/data-normal.pak",
    "assets/data-shop.pak": "assets/data-shop.pak",
    "res/raw/android_intro.mp4": "movie/android_intro.mp4",
    "res/raw/android_cs1.mp4": "movie/android_cs1.mp4",
    "res/raw/android_cs2.mp4": "movie/android_cs2.mp4",
    "res/raw/android_cs3.mp4": "movie/android_cs3.mp4",
}

# data-retina.pak is deliberately left out: it holds the tablet-resolution
# atlases, 18 MB the Vita's 960x544 screen never reads.

# The two binaries this port was developed and tested against. A mismatch is not
# fatal - other APK revisions may work - but it is worth knowing about.
KNOWN = {
    "libClawNativeApp.so": "7d571f720085262e",
    "libstlport_shared.so": "03f53b771e428232",
}


class ApkError(Exception):
    """A problem with the APK, worded for the person who supplied it."""


def open_apk(path):
    """Open path as a Monster Shooter APK and check it.

    Returns (zipfile, notes): the open archive and a list of non-fatal notes
    about binaries that differ from the tested build. Raises ApkError when the
    file cannot be used at all. Shared with the setup tool, so both reject the
    same files with the same words.
    """
    path = Path(path)
    if not path.is_file():
        raise ApkError("%s is not a file" % path)
    try:
        zf = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        raise ApkError("%s is not a valid APK (APKs are zip archives)" % path.name)

    names = set(zf.namelist())
    missing = [m for m in WANTED if m not in names]
    if missing:
        zf.close()
        raise ApkError("%s does not look like the Monster Shooter APK. Missing:\n  %s"
                       % (path.name, "\n  ".join(missing)))

    notes = []
    for member, target in WANTED.items():
        expected = KNOWN.get(target)
        if expected:
            got = hashlib.sha256(zf.read(member)).hexdigest()[:16]
            if got != expected:
                notes.append("%s differs from the tested build (expected %s, got %s)"
                             " - it may still work" % (target, expected, got))
    return zf, notes


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2

    try:
        zf, notes = open_apk(argv[1])
    except ApkError as e:
        print("error: %s" % e)
        return 1

    out = Path("monstershooter")
    with zf:
        for n in notes:
            print("note: " + n)
        total = 0
        for member, target in WANTED.items():
            data = zf.read(member)
            dst = out / target
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
            total += len(data)
            print("  %s  (%s bytes)" % (target, format(len(data), ",")))

    print()
    print("Extracted %d files, %.1f MB, into %s/" % (len(WANTED), total / 1024 / 1024, out))
    print()
    print("Now copy that folder to your Vita so that it becomes:")
    print("    ux0:data/monstershooter/")
    print("then install the VPK and launch the game.")
    return 0


def _selftest():
    """Build a fake APK in a temp dir and check both the happy and sad path."""
    import os
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        # Back out of tmp before it is deleted: Windows refuses to remove
        # the current directory.
        old = os.getcwd()
        os.chdir(tmp)
        try:

            # An archive missing everything must be rejected, not half-extracted.
            bad = Path("bad.apk")
            with zipfile.ZipFile(bad, "w") as z:
                z.writestr("AndroidManifest.xml", "nope")
            assert main(["x", str(bad)]) == 1, "a wrong APK must fail"
            assert not Path("monstershooter").exists(), "must not write anything on failure"

            # A complete archive extracts into the documented layout.
            good = Path("good.apk")
            with zipfile.ZipFile(good, "w") as z:
                for member in WANTED:
                    z.writestr(member, b"x" * 32)
            assert main(["x", str(good)]) == 0, "a complete APK must succeed"
            for target in WANTED.values():
                assert Path("monstershooter", target).is_file(), "%s not extracted" % target

            # A non-archive must be refused with a message, not a traceback.
            Path("notazip.apk").write_bytes(b"definitely not a zip")
            assert main(["x", "notazip.apk"]) == 1, "a non-zip must fail cleanly"
        finally:
            os.chdir(old)

    print("selftest ok")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        _selftest()
    else:
        sys.exit(main(sys.argv))
