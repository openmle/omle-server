"""Platform-tagged wheel for a package whose payload is a native executable.

All metadata lives in pyproject.toml; this file exists only to correct the wheel
tag. setuptools sees no extension module here — omle_server/bin/omle_server is
just a data file as far as it is concerned — so it builds `py3-none-any`. That
wheel would install happily on every platform and run on exactly one of them.

Two overrides fix it:

  has_ext_modules()      declaring the distribution impure is what makes the
                         platform tag appear (macosx_14_0_arm64,
                         manylinux_2_28_x86_64, ...).

  get_tag()              keeps the Python tag at py3/none. The binary is
                         exec'd, never imported, so it is independent of both
                         the interpreter version and its ABI — one wheel per
                         platform serves every supported Python.

Note on the first one: setting `root_is_pure = False` on the bdist_wheel
command also produces a platform tag, but it relocates every file into
`<name>.data/purelib/` instead of the wheel root. delocate and auditwheel
compute the vendored libraries' @loader_path/$ORIGIN offsets from the layout
inside the wheel, so that indirection makes them emit a path two levels wrong
and the installed binary fails at startup with "Library not loaded". Declaring
the distribution impure marks the wheel the same way while leaving the files
where the repair tools expect them.
"""

from setuptools import setup
from setuptools.dist import Distribution

try:  # setuptools >= 70.1 vendors its own copy
    from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
except ImportError:  # older setuptools defers to the wheel package
    from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:  # noqa: D102
        return True


class bdist_wheel(_bdist_wheel):
    def get_tag(self):
        _python, _abi, plat = super().get_tag()
        return "py3", "none", plat


setup(distclass=BinaryDistribution, cmdclass={"bdist_wheel": bdist_wheel})
