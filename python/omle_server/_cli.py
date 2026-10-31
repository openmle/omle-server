"""Console-script entry point for `omle-server`.

Replaces the current Python process with the native C++ binary so there is no
Python overhead at steady state.  Every argument is forwarded verbatim, so the
binary owns the whole interface — subcommands, flags and `--help` — and this
wrapper never needs to learn about a new option.  Run `omle-server --help` for
the current set.
"""
from __future__ import annotations

import os
import sys

from . import binary_path


def main() -> None:
    binary = binary_path()
    # exec replaces this process — no Python overhead after this point.
    os.execv(binary, [binary] + sys.argv[1:])
