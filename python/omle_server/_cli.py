"""Console-script entry point: `omle-server [config.json]`

Replaces the current Python process with the native C++ binary so there is
no Python overhead at steady state.  All arguments are forwarded verbatim.
"""
from __future__ import annotations

import os
import sys

from . import binary_path


def main() -> None:
    binary = binary_path()
    # exec replaces this process — no Python overhead after this point.
    os.execv(binary, [binary] + sys.argv[1:])
