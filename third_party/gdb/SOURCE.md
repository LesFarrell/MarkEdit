# Bundled GNU GDB source and provenance

`bin/gdb.exe` is the standalone 64-bit Windows GDB 17.1 executable from
w64devkit 2.7.0. MarkEdit starts it as a separate process; it is not linked
into MarkEdit.

The MarkEdit source repository's `third_party/gdb/source/` directory contains
the complete source inputs for this binary. It is intentionally omitted from
the binary package; it remains available alongside the corresponding MarkEdit
release at <https://github.com/LesFarrell/MarkEdit/tree/main/third_party/gdb/source>:

- `gdb-17.1.tar.xz` — GNU GDB 17.1 from <https://ftp.gnu.org/gnu/gdb/>.
- `expat-2.7.5.tar.xz` — Expat 2.7.5, statically linked by the build.
- `libiconv-1.19.tar.gz` — GNU libiconv 1.19, statically linked by the build.
- `PDCurses-3.9.tar.gz` — PDCurses 3.9, statically linked by the build.
- `w64devkit/` — the w64devkit 2.7.0 Dockerfile, version/readme, and complete
  recipe support-file directory, including the two GDB patches. The Dockerfile
  records the configure flags and its additional `quiet = 1` source edit.

The archive hashes below are the values pinned by the included w64devkit
Dockerfile and are also recorded in `SHA256SUMS.txt`. The GDB licence is in
`COPYING.txt`; licences and notices for the source dependencies are included
inside their respective source archives. MinGW-w64 runtime notices are in
`COPYING.MinGW-w64-runtime.txt`.

To reproduce the executable, place the four archives in the matching download
stages of the included w64devkit 2.7.0 recipe (or allow that recipe to download
the same hashed inputs), then build its `build-gdb` stage for x86-64 Windows.
The produced `/out/bin/gdb.exe` should match the runtime described here.
