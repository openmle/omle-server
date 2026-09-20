"""omle-server — the OIP inference server, delivered as a Python package.

This package exists only to ship the compiled ``omle_server`` binary somewhere
``pip`` can put it. It is a delivery vehicle, not a Python API: installing it
puts ``omle-server`` on PATH, and that console script ``execv``s the binary, so
no Python remains in the process once the server is running.

    pip install omle-server
    omle-server                       # uses configs/server.json
    omle-server /etc/omle/server.json # or an explicit config

Everything the server accepts is handled by the binary itself — the config file
plus the ``OMLE_MODEL_DIR``, ``OMLE_REST_PORT`` and ``OMLE_GRPC_PORT``
environment variables (see ``src/main.cpp``). There is deliberately no Python
wrapper translating arguments into those, because it could only ever restate
what the binary already does, and would drift from it.

To launch the server from Python, run the binary as a subprocess like any other
program:

    import subprocess, omle_server
    proc = subprocess.Popen([omle_server.binary_path()],
                            env={**os.environ, "OMLE_MODEL_DIR": "/models"})
"""

from __future__ import annotations

import pathlib
import sys

try:
    from ._version import __version__
except ImportError:  # running from a source tree with no build yet
    __version__ = "0.0.0.dev0"

__all__ = ["__version__", "binary_path"]


def binary_path() -> str:
    """Absolute path to the bundled ``omle_server`` executable.

    Raises FileNotFoundError when the package was installed without the binary
    staged into it, which is what an editable install off a fresh checkout
    looks like.
    """
    name = "omle_server.exe" if sys.platform == "win32" else "omle_server"
    candidate = pathlib.Path(__file__).parent / "bin" / name
    if not candidate.exists():
        raise FileNotFoundError(
            f"omle_server binary not found at {candidate}.\n"
            "Build it with `python scripts/build.py` from the repository root, "
            "then reinstall this package."
        )
    return str(candidate)
