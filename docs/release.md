# flower release process

This page explains how a version gets tagged, built, packaged, and published. The scripts are the source of truth. This page is the map. Everything runs from the repo root on a machine with `gh` (installed and authenticated with `gh auth login`). Docker is optional. Without docker, the release ships the plain tarball only, with a note.

## version stamping

The version lives in `FLOWER_VERSION` in `src/flower.h`, guarded by `#ifndef`. A plain `make` builds the default `0.0.1`; the release stamp is a define:

```sh
make VERSION=1.2.3
./flower version        # -> flower 1.2.3
```

## the one command

```sh
make release TAG=v1.2.3        # or: tools/release.sh v1.2.3
```

[tools/release.sh](../tools/release.sh) runs these steps in order:

1. Guards. The script refuses to start unless all of these hold: the tag matches `v1.2`/`v1.2.3`, the tag does not exist yet, the working tree is clean (no uncommitted and no untracked files), HEAD is not ahead of the upstream branch, and `gh` is installed and authenticated. Everything fails before anything is built or tagged.
2. Tests. `make check` runs the 154-check selfcheck suite.
3. Build. The binary is rebuilt with the version stamped in (`make clean`, `make VERSION=1.2.3`). `flower version` must report `1.2.3` or the script dies.
4. Tarball. The build produces `dist/flower-v1.2.3-<os>-<arch>.tar.gz` with the binary, `LICENSE.md`, and `README.md` inside a top-level directory.
5. Distro packages. When docker is up, [tools/package.sh](../tools/package.sh) builds the matrix below. Set `PACKAGES=0` to skip this step. No docker skips with a warning.
6. Checksums. One `dist/checksums-v1.2.3.txt` covers every artifact (sha256sum, with a shasum fallback for hosts without it).
7. Publish. The script creates the annotated tag, pushes the tag, and runs `gh release create` with everything in `dist/` uploaded. The release body comes from [tools/release-notes.sh](../tools/release-notes.sh) via `--notes-file`: a which-file-for-which-system table generated from what is actually in `dist/` (rows appear only for artifacts that were built), followed by the auto-generated changelog.

The tag and the GitHub release only happen at the very end. Any earlier failure leaves nothing behind to clean up.

To fix the text of a release that is already out:

```sh
gh release edit v1.0.0 --notes-file /tmp/n.md
```

## distro packages only

```sh
tools/package.sh v1.2.3            # all six targets
tools/package.sh v1.2.3 deb arch   # just these
make packages TAG=v1.2.3           # same, via make
```

| target | container | artifact | installs on |
|---|---|---|---|
| `deb` | `ubuntu:22.04` | `flower_1.2.3-1_amd64.deb` | ubuntu 22.04/24.04+, debian 12+ |
| `rpm-fc` | `fedora:41` | `flower-1.2.3-1.fc41.x86_64.rpm` | fedora 41+ |
| `rpm-el9` | `rockylinux:9` | `flower-1.2.3-1.el9.x86_64.rpm` | rhel / alma / rocky 9+ |
| `arch` | `archlinux:base-devel` | `flower-1.2.3-1-x86_64.pkg.tar.zst` | arch, via `pacman -U` |
| `win` | `africanfuture/msys2-base:linux-latest` | `flower-1.2.3-windows-x86_64.zip` | windows 10+ 64-bit |
| `mac` | `flower-osxcross:11.3` (local build) | `flower-1.2.3-macos-x86_64.zip` | macOS 10.13+, rosetta 2 on apple silicon |

Install commands for users: `apt install ./flower_..._.deb`,
`dnf install ./flower-..._.rpm`, `pacman -U flower-..._.pkg.tar.zst`.
The windows zip unzips in place. `flower.exe` carries everything it needs.

Each target builds inside the container of its own distro, with the packaging tool of that distro (`dpkg-deb`, `rpmbuild`, `makepkg`, mingw, osxcross). The binary has zero external dependencies beyond libc (`LDLIBS = -pthread`, and libpthread is part of glibc), so the packages depend on nothing either. The windows and macos builds are plain cross-compiles of the same binary.

### the glibc rule

A dynamically linked binary runs on the glibc of its build host or newer, never older. Each target therefore builds on the oldest release it must support. The image list above is also the compatibility list. To raise or lower the floor for a format, change the image in `tools/package.sh` (one variable per target). For example, a `deb` build on `debian:bookworm` drops ubuntu 22.04 but keeps debian 12+.

## adding a target

A new distro is one `case` entry in `tools/package.sh` (image + which script to run). If the format is new, add one script in `tools/pkg/` that follows the existing ones: install nothing (no deps to install), build with `make VERSION=...`, verify the stamp, package, and drop the artifact in `dist/`.

## gotchas already hit

- The package scripts must not run `make clean`. Clean removes `dist/`, which already holds the release tarball at that point. They `rm -f` the binaries instead.
- The checksums step must skip its own output file.
- `update` self-update (`src/update.c`) is a template stub: after tagging, point its `DL_BASE` at the real release artifacts and wire the sha256 check.
