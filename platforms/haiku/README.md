# platforms/haiku/

D-Modem on [Haiku](https://www.haiku-os.org/) (x86_64).  On Haiku itself
nothing here is needed: `cmake -B build` there.

| File | What it does |
|------|--------------|
| `haiku.cmake` | Toolchain file for building for Haiku from Linux, with Clang and lld |
| `haiku_sysroot.py` | Makes the sysroot `haiku.cmake` needs, from Haiku's own packages |
| `libre-haiku.patch` | libre's patch for Haiku, applied by `cmake/libre.cmake` |

    python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake \
        -DHAIKU_SYSROOT=$HOME/haiku-sysroot

The details are in [docs/haiku.md](../../docs/haiku.md) and
[docs/haiku-details.md](../../docs/haiku-details.md).
