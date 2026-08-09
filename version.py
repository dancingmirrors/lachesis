#!/usr/bin/env python3

import os
import subprocess
import sys

DESCRIBE = [
    "git",
    "-c",
    "safe.directory=*",
    "describe",
    "--match",
    "v[0-9]*",
    "--tags",
    "--long",
    "--always",
    "--abbrev=40",
    "--dirty",
]


def describe(cwd):
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    try:
        p = subprocess.Popen(
            DESCRIBE,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
    except OSError:
        return ""
    out = p.communicate()[0]
    if p.wait() != 0:
        return ""
    return out.decode("utf-8", "replace").strip()


def main(argv):
    version_h = ""
    extra = ""
    cwd = os.getcwd()

    for arg in argv:
        val = arg.split("=", 1)[1] if "=" in arg else ""
        if arg.startswith("--extra="):
            extra = "-" + val
        elif arg.startswith("--versionh="):
            version_h = os.path.abspath(val)
        elif arg.startswith("--cwd="):
            cwd = val
        else:
            sys.stderr.write("Unknown parameter: %s\n" % arg)
            return 1

    if not os.path.isdir(cwd):
        sys.stderr.write("Not a directory: %s\n" % cwd)
        return 1

    version = describe(cwd)

    if not version_h:
        print("%s%s" % (version or "UNKNOWN", extra))
        return 0

    if not version:
        if os.path.exists(version_h):
            return 0
        version = "UNKNOWN"

    new = ('#define VERSION "%s%s"\n' % (version, extra)).encode("utf-8")
    try:
        with open(version_h, "rb") as f:
            old = f.read()
    except OSError:
        old = None

    if old != new:
        with open(version_h, "wb") as f:
            f.write(new)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
