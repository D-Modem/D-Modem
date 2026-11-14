# platforms/haiku/

D-Modem on [Haiku](https://www.haiku-os.org/) (x86_64). On Haiku itself, you do not
need the files in this folder. On Haiku, use `cmake -B build`.

| File | What it does |
|------|--------------|
| `haiku.cmake` | Toolchain file to build for Haiku from Linux, with Clang and lld |
| `haiku_sysroot.py` | Makes the sysroot that `haiku.cmake` must have, from the packages of Haiku |
| `libre-haiku.patch` | The patch of libre for Haiku. `cmake/libre.cmake` applies it. |

    python3 platforms/haiku/haiku_sysroot.py ~/haiku-sysroot
    cmake -B build-haiku -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=platforms/haiku/haiku.cmake \
        -DHAIKU_SYSROOT=$HOME/haiku-sysroot

The details are in [docs/haiku.md](../../docs/haiku.md) and
[docs/haiku-details.md](../../docs/haiku-details.md).
