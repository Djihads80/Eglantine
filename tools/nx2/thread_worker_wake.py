#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Checks that Common::ThreadWorker::WaitForRequests cannot miss its wake-up.

Compiles thread_worker_wake.cpp against src/common/thread_worker.h (from the working tree, or
from --rev) and runs it once for cancellation and once for normal completion, each in its own
process with a timeout. Linux with a dynamically linked glibc only.

    python3 -I tools/nx2/thread_worker_wake.py               # working tree: both should wake
    python3 -I tools/nx2/thread_worker_wake.py --rev <rev>   # a revision's header

Exit codes: 0 both cases woke, 1 a lost wake, 2 harness or build failure, 77 not supported.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCE = os.path.join(HERE, "thread_worker_wake.cpp")
HEADER = "src/common/thread_worker.h"
WARNINGS = ["-Wall", "-Wextra", "-Wmissing-declarations", "-Wshadow", "-Werror"]
RESULTS = {0, 1, 2, 77}


def run_case(binary, mode):
    try:
        run = subprocess.run([binary, mode], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 2, "harness: timed out"
    output = run.stdout.strip()
    if run.returncode in RESULTS:
        return run.returncode, output
    reason = (f"killed by signal {-run.returncode}" if run.returncode < 0
              else f"unexpected exit code {run.returncode}")
    details = "\n".join(text for text in (output, run.stderr.strip()) if text)
    return 2, f"harness: {reason}" + (f"\n{details}" if details else "")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rev", help="git revision whose thread_worker.h to test")
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or "g++")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        print("unsupported: Linux only")
        return 77

    with tempfile.TemporaryDirectory() as work:
        includes = []
        binary = os.path.join(work, "thread_worker_wake")
        try:
            if args.rev:
                show = subprocess.run(["git", "-C", ROOT, "show", f"{args.rev}:{HEADER}"],
                                      capture_output=True)
                if show.returncode != 0:
                    print(show.stderr.decode(errors="replace").strip())
                    return 2
                os.makedirs(os.path.join(work, "include", "common"))
                with open(os.path.join(work, "include", "common", "thread_worker.h"), "wb") as file:
                    file.write(show.stdout)
                includes.append("-I" + os.path.join(work, "include"))
            build = subprocess.run([args.cxx, "-std=c++20", "-O2", "-pthread", *WARNINGS,
                                    *includes, "-I" + os.path.join(ROOT, "src"), SOURCE, "-ldl",
                                    "-o", binary],
                                   capture_output=True, text=True)
        except OSError as error:
            print(f"harness: {error}")
            return 2
        if build.returncode != 0:
            print(build.stdout + build.stderr)
            return 2

        results = []
        for mode in ("cancel", "complete"):
            code, output = run_case(binary, mode)
            print(f"{mode}: {output}")
            results.append(code)
        if 77 in results:
            return 77
        if 2 in results:
            return 2
        return max(results)


if __name__ == "__main__":
    sys.exit(main())
