"""The command-line surface: `--version` and its contract with `--help`.

`--version` is the only place a built binary reports which omle and omle-runtime
sources went into it. publish.yml resolves those to each repository's latest
GitHub release rather than pinning them, so two builds of the same omle-server
version can differ and this output is the only thing that distinguishes them.

That makes the format worth pinning. It was reshaped three times while being
written, with nothing asserting on it — a later change could drop a field, or
print the raw git ref, and no test would notice.
"""

from __future__ import annotations

import pathlib
import re
import subprocess

import pytest

# 0.1.0 (built on omle=0.1.1, runtime=0.2.0)
#
# One line, the server's own version first, then each sibling. The refs are
# matched loosely on purpose: what they contain depends on how the binary was
# built — a tag for a release, "main@<sha>" for a CI test build, "unknown" for a
# plain local build — and the test should pin the shape, not the build.
VERSION_RE = re.compile(
    r"^(?P<version>\d+\.\d+\.\d+\S*) "
    r"\(built on omle=(?P<omle>[^,]+), runtime=(?P<runtime>[^)]+)\)$"
)


def _run(binary, *args):
    return subprocess.run([str(binary), *args], capture_output=True, text=True,
                          timeout=30, check=False)


def test_version_matches_the_documented_format(server_binary):
    r = _run(server_binary, "--version")
    assert r.returncode == 0, r.stderr
    assert r.stdout.count("\n") == 1, f"expected a single line, got {r.stdout!r}"

    m = VERSION_RE.match(r.stdout.strip())
    assert m, f"output does not match the documented format: {r.stdout!r}"
    assert m.group("omle"), "the omle ref is empty"
    assert m.group("runtime"), "the runtime ref is empty"


def test_version_refs_are_versions_not_raw_tags(server_binary):
    """A tag is written "v0.2.0" but reported as "0.2.0".

    This holds whatever the binary was built from: a release ref loses its v, and
    the other shapes ("main@<sha>", "unknown") never had one. So a leading v
    followed by a digit in the output means the stripping regressed.
    """
    m = VERSION_RE.match(_run(server_binary, "--version").stdout.strip())
    assert m
    for name in ("omle", "runtime"):
        ref = m.group(name)
        assert not re.match(r"^v\d", ref), (
            f"{name}={ref!r} still carries the tag's leading v"
        )


def test_short_and_long_version_flags_agree(server_binary):
    long_out = _run(server_binary, "--version").stdout
    short_out = _run(server_binary, "-V").stdout
    assert short_out == long_out, "-V and --version disagree"


def test_version_writes_nothing_to_stderr(server_binary):
    """Clean enough to capture in a script without filtering."""
    r = _run(server_binary, "--version")
    assert r.stderr == "", f"unexpected stderr: {r.stderr!r}"


def test_help_points_at_version_for_provenance(server_binary):
    """--help is usage, but it should say where the build info lives.

    Without this the only way to discover that --version reports the sibling refs
    is to run it, which nobody does when --help already answered their question.
    """
    r = _run(server_binary, "--help")
    assert r.returncode == 0, r.stderr
    assert "--version" in r.stdout
    assert "built against" in r.stdout, (
        "--help no longer mentions that --version reports the build sources"
    )


# ── model_dir: absent by default vs absent because you asked for it ───────────
#
# load() has exactly one failure: the directory not existing. A model that will
# not parse is reported and skipped. So "explicitly requested and missing" is
# always a path mistake, and a server that starts anyway answers every inference
# request with 404 while reporting healthy.
#
# The default being absent is different — a container's volume may mount a moment
# after the process starts — so that warns and comes up empty.

def _serve(binary, *args, wait=2.0):
    """Start the server, let it settle, stop it, and return (rc, output).

    rc is None when the process was still running and had to be stopped, which is
    the success signal for a server: it did not exit on its own.
    """
    import signal
    import time

    proc = subprocess.Popen([str(binary), "serve", *args],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    time.sleep(wait)
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        return None, out
    out, _ = proc.communicate()
    return proc.returncode, out


def test_explicit_missing_model_dir_is_fatal(server_binary, tmp_path):
    missing = tmp_path / "not-created"
    r = _run(server_binary, "serve", "--model-dir", str(missing),
             "--rest-port", "18091", "--grpc-port", "18090")
    assert r.returncode != 0, (
        "a --model-dir that does not exist should stop the server, not start one "
        "that serves nothing"
    )
    assert "Fatal" in r.stdout + r.stderr
    assert str(missing) in r.stdout + r.stderr, "the message should name the path"


def test_explicit_missing_model_dir_from_env_is_fatal(server_binary, tmp_path):
    import os

    env = dict(os.environ, OMLE_MODEL_DIR=str(tmp_path / "nope"))
    r = subprocess.run([str(server_binary), "serve",
                        "--rest-port", "18091", "--grpc-port", "18090"],
                       capture_output=True, text=True, timeout=30, env=env,
                       check=False)
    assert r.returncode != 0, "OMLE_MODEL_DIR is an explicit choice too"
    assert "Fatal" in r.stdout + r.stderr


def test_explicit_empty_model_dir_still_starts(server_binary, tmp_path):
    """Present but empty is a legitimate state, not a mistake."""
    empty = tmp_path / "empty"
    empty.mkdir()
    rc, out = _serve(server_binary, "--model-dir", str(empty),
                     "--rest-port", "18091", "--grpc-port", "18090")
    assert rc is None, f"server exited instead of starting: {out}"
    assert "Fatal" not in out, out


def test_default_missing_model_dir_warns_actionably(server_binary):
    """The default being absent warns, and the warning says how to fix it."""
    if pathlib.Path("/models").exists():
        pytest.skip("/models exists on this machine, so the default resolves")

    rc, out = _serve(server_binary, "--rest-port", "18091", "--grpc-port", "18090")
    assert rc is None, f"the default being absent should not stop the server: {out}"
    assert "Warning" in out and "Fatal" not in out
    # Actionable: names each way to set it.
    for hint in ("--model-dir", "OMLE_MODEL_DIR", "model_dir"):
        assert hint in out, f"the warning should mention {hint}: {out}"


# ── Drogon's upload path ──────────────────────────────────────────────────────

def test_starting_from_an_unwritable_directory_logs_no_errors(server_binary, tmp_path):
    """No error spam when the working directory cannot be written.

    Drogon builds a 256-way sharded temp tree under its upload path at startup,
    and its default is "uploads" relative to the working directory. Nothing here
    uploads files, but the tree is created regardless and Drogon offers no way to
    turn it off — setUploadPath is the only lever. Left at the default, the
    container (working directory /, unprivileged user) logged 256 "Permission
    denied" errors before serving a single request.

    Skipped as root, which bypasses the permission bits this relies on.
    """
    import os

    if hasattr(os, "geteuid") and os.geteuid() == 0:
        pytest.skip("running as root: no directory is unwritable")

    models = tmp_path / "models"
    models.mkdir()
    cwd = tmp_path / "readonly"
    cwd.mkdir()
    cwd.chmod(0o500)          # read + execute, no write
    try:
        proc = subprocess.Popen(
            [str(server_binary), "serve", "--model-dir", str(models),
             "--rest-port", "18087", "--grpc-port", "18086"],
            cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True)
        import signal
        import time
        time.sleep(3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        out, _ = proc.communicate(timeout=15)
    finally:
        cwd.chmod(0o700)      # so pytest can clean the tmp dir up

    assert "uploads" not in out, (
        "Drogon is still creating its upload tree relative to the working "
        f"directory:\n{out}"
    )
    assert "ERROR" not in out, f"unexpected errors at startup:\n{out}"


# ── --model: serve a single file ──────────────────────────────────────────────
#
# Removes the friction from checking one model: no directory to arrange, no file
# to move. The model is named after the file's stem, matching the flat-directory
# convention, so /v2/models/<stem> works the same either way.
#
# It never falls back. The caller named exactly one model, so a missing or
# unloadable file is a mistake, not a reason to start a server that serves
# nothing — the opposite of the default model_dir being absent.

def _model_fixture(tmp_path, name="quickcheck"):
    src = (pathlib.Path(__file__).parent.parent.parent / "tests" / "data"
           / "test_model_2f.omle")
    dst = tmp_path / f"{name}.omle"
    dst.write_bytes(src.read_bytes())
    return dst


def test_model_flag_serves_the_file_under_its_stem(server_binary, tmp_path):
    import socket

    import requests

    model = _model_fixture(tmp_path)

    def _free_port():
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    rest, grpc = _free_port(), _free_port()
    proc = subprocess.Popen(
        [str(server_binary), "serve", "--model", str(model),
         "--rest-port", str(rest), "--grpc-port", str(grpc)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        import time
        base = f"http://127.0.0.1:{rest}"
        for _ in range(80):
            if proc.poll() is not None:
                out, _ = proc.communicate()
                pytest.fail(f"server exited (rc={proc.returncode}):\n{out}")
            try:
                requests.get(f"{base}/v2/health/live", timeout=1)
                break
            except requests.RequestException:
                time.sleep(0.25)
        else:
            pytest.fail("server never answered")

        r = requests.get(f"{base}/v2/models/quickcheck/ready", timeout=5)
        assert r.status_code == 200, "the file's stem should be the model name"
        meta = requests.get(f"{base}/v2/models/quickcheck", timeout=5).json()
        assert meta["name"] == "quickcheck"
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def test_model_flag_rejects_a_missing_file(server_binary, tmp_path):
    r = _run(server_binary, "serve", "--model", str(tmp_path / "absent.omle"),
             "--rest-port", "18079", "--grpc-port", "18078")
    assert r.returncode != 0, "a named model that is absent must not start a server"
    assert "does not exist" in r.stdout + r.stderr


def test_model_flag_rejects_a_directory(server_binary, tmp_path):
    """The error names the flag that does take a directory."""
    r = _run(server_binary, "serve", "--model", str(tmp_path),
             "--rest-port", "18079", "--grpc-port", "18078")
    assert r.returncode != 0
    combined = r.stdout + r.stderr
    assert "is a directory" in combined
    assert "--model-dir" in combined, "the message should point at the right flag"


def test_model_flag_rejects_an_unloadable_file(server_binary, tmp_path):
    """Unlike a directory scan, there is nothing else to serve, so this is fatal."""
    bad = tmp_path / "corrupt.omle"
    bad.write_bytes(b"not a protobuf")
    r = _run(server_binary, "serve", "--model", str(bad),
             "--rest-port", "18079", "--grpc-port", "18078")
    assert r.returncode != 0
    assert "cannot load" in r.stdout + r.stderr


def test_model_and_model_dir_are_mutually_exclusive(server_binary, tmp_path):
    model = _model_fixture(tmp_path)
    r = _run(server_binary, "serve", "--model", str(model),
             "--model-dir", str(tmp_path),
             "--rest-port", "18079", "--grpc-port", "18078")
    assert r.returncode != 0, "an ambiguous request should be refused, not guessed"
    assert "mutually exclusive" in r.stdout + r.stderr


def test_model_flag_is_documented_in_help(server_binary):
    assert "--model PATH" in _run(server_binary, "--help").stdout
