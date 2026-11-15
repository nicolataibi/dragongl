# Packaging Dragon GL

`dragongl.spec` builds five packages on Fedora. This page says how the
spec meets the Fedora packaging guidelines, what a reviewer will ask, and
how to run the tools that check it. The upstream tree and the spec are
kept together: a change that breaks a point below fails the `rpm` job of
CI (`rpmbuild` with `%check`, then `rpmlint`).

## The packages

| Package | Arch | Contents |
| :--- | :--- | :--- |
| `dragongl` | binary | the Vulkan client, its launcher, shaders, man pages, menu entry, icon, AppStream metadata |
| `dragongl-server` | binary | the server, its launchers, man pages, the systemd unit, the firewalld service, `/etc/dragongl` |
| `dragongl-data` | noarch | the JSON datasets |
| `dragongl-tools` | noarch | the balance analyser and the PDF map generator (commands `dragongl-analyze-balance`, `dragongl-generate-pdf-map`), the Data Studio |
| `dragongl-doc` | noarch | the Markdown documentation |

## Guidelines, point by point

| Guideline | How the spec meets it | Checked by |
| :--- | :--- | :--- |
| Naming and versioning | `dragongl`, version `YYYY.MM.DD.NN` from one place (`Version:`), `%autorelease` and `%autochangelog` | `tools/check_release.py` in `%check` |
| License | `GPL-3.0-or-later AND CC0-1.0` (SPDX) for the client package, whose AppStream metadata is CC0-1.0 as AppStream asks; `GPL-3.0-or-later` for every subpackage, each with its own `License:`. The artwork (CC BY 4.0) is not in the tarball, and the copy of cJSON (MIT) is deleted in `%prep` | `%license LICENSE.txt` in every package; rpmlint |
| No bundled libraries | cJSON is the only third-party code in the tree (`third_party/cjson`); the spec removes it and builds with `-DDRAGONGL_SYSTEM_CJSON=ON` against `pkgconfig(libcjson)` | the build fails if anything still needs the copy |
| Source | `Source0` is the archive of the tag on GitHub; `tools/release.sh` makes the same content with `git archive` | `rpmbuild`; `spectool -g` on Fedora |
| Build flags | `%cmake` with the flags of the distribution only: build type `None` (no optimization flags of CMake's own; an empty build type would default to Release and add `-O3`), the project's own hardening flags and path mapping off (`DRAGONGL_HARDENING=OFF`, `DRAGONGL_REPRODUCIBLE=OFF`), so `%optflags` and the debuginfo extraction are untouched | debuginfo and debugsource packages are built; rpmlint |
| Compiler | gcc, the default of the distribution (`BuildRequires: gcc`, `-DDRAGONGL_USE_CLANG=OFF` so the project's preference for clang does not apply) | CI: clang and gcc on Ubuntu and Fedora, `-Werror`; the integration suite with gcc and the Fedora flags |
| BuildRequires / Requires | `pkgconfig()` names for the libraries; tools named by package. The Python scripts of `-tools` live in `%{_datadir}`, where rpm generates no dependency, so `python3` and `python3-reportlab` are listed by hand | `dnf builddep` in a clean container |
| Tests | `%check` runs the release and data checks, the unit suite and the server integration suite (in-process harness, protocol vectors, a real server driven over TLS on the loopback interface: no outside network), all built with gcc, `%optflags` and `%build_ldflags` | `rpm` job |
| Desktop application | `io.github.nicolataibi.dragongl.desktop` (`Terminal=true` on purpose: the launcher asks for server, name and password before the window opens), a scalable icon in hicolor, AppStream metadata | `desktop-file-validate` and `appstream-util validate-relax --nonet` (the check the AppStream guidelines of Fedora ask for) in `%check` |
| systemd | unit installed in `%{_unitdir}`, disabled by default, the scriptlets of `systemd-rpm-macros` (no `Requires(post)` on systemd: obsolete); the service runs as a dynamic user, so no user is created; the passwords are systemd credentials (`LoadCredential=`) | rpmlint; `systemd-analyze verify` |
| Configuration | `/etc/dragongl/dragongl-server.conf` is `%config(noreplace)`, mode 0644: it holds options only. The password files are created by the administrator and are not shipped | rpmlint |
| Firewall | a firewalld service `dragongl-server` (8080/tcp) in `%{_prefix}/lib/firewalld/services`, `firewalld-filesystem` as BuildRequires, Requires and Requires(post), `%firewalld_reload` in `%post`; the port is not opened by the package | rpmlint |
| File ownership | every directory the packages create is owned; `%{_datadir}/dragongl` is co-owned by the packages that do not depend on each other; the icon directories belong to `hicolor-icon-theme`, which is required | rpmlint |
| Documentation | a man page for every command in `%{_bindir}`, each named after its command; the Markdown guides in `-doc` | `mandoc -T lint` in CI |
| Architectures | no `ExcludeArch`: the code has no assembly and no assumption on the byte order of the host (the wire format and the save files are explicitly little-endian) | COPR: Fedora 44, 45 and rawhide on x86_64 and i386, EPEL 10 on x86_64 (2026.11.14.09) |

## rpmlint filters

`dragongl.rpmlintrc` has two filters, each with its reason; rpmlint
reports a filter that matches nothing, so none can outlive its cause.

| Filter | Why it is right |
| :--- | :--- |
| `dragongl-data: no-documentation` | data only; the documentation is in `dragongl-doc` |
| `spelling-error` for Vulkan, unprivileged, DynamicUser | names the dictionary of rpmlint does not know (Vulkan is the graphics API) |

## Running the checks

On Fedora, from a clean tree:

```bash
sudo dnf install -y rpm-build rpmlint fedora-review mock rpmdevtools
tools/release.sh --srpm                       # dist/dragongl.spec, dist/*.src.rpm
sudo dnf builddep -y dist/*.src.rpm
rpmbuild --rebuild dist/*.src.rpm
rpmlint -r dragongl.rpmlintrc ~/rpmbuild/RPMS/*/dragongl*.rpm dist/*.src.rpm

# The checklist of the Fedora review, in a clean chroot (your user must be
# in the mock group). The source RPM of a review request carries the
# tarball of Source0 as GitHub serves it, so that fedora-review finds the
# same checksum upstream (tools/release.sh makes the same content, but
# GitHub does not promise the bytes of its archives):
cd dist && spectool -g dragongl.spec && rpmbuild -bs --define "_sourcedir $PWD" \
    --define "_srcrpmdir $PWD" dragongl.spec
fedora-review --rpm-spec -n dragongl-*.src.rpm --mock-config fedora-rawhide-x86_64
```

`fedora-review` writes `review.txt`: every `[!]` is a point to fix or to
explain in the review request; `[ ]` are the points a human checks.

COPR builds the same spec. To cover every supported Fedora, enable in the
project settings the chroots `fedora-<N>-x86_64` of the releases still
supported, `fedora-rawhide-x86_64`, and `aarch64` for at least one of
them.

## fedora-review of 2026.11.14.10

Run on Fedora rawhide (fedora-review 0.12.0, mock): no item failed (`[!]`).
What it reported, and what was done:

| Report | Answer |
| :--- | :--- |
| rpmlint `unused-direct-shlib-dependency`: the client linked `libcjson` | fixed in 2026.11.14.11: the client compiles only the shared code it uses and reads no JSON |
| rpmlint `spelling-error` for *shaders* and *changelog* | the descriptions no longer use them; *Vulkan* stays (a name) |
| rpmlint `non-readable /etc/dragongl/dragongl-server.conf 600` | gone in 2026.11.15.05: the passwords moved to systemd credentials and the file is 0644 |
| rpmlint `no-documentation` on `dragongl-data` | intended: data only, documentation in `dragongl-doc` |
| licensecheck: 75 files without a license notice | the source files that lacked one (shaders, tests, Data Studio, man pages, packaging files, CMake files) carry an SPDX line since 2026.11.14.11; what is left are the JSON data, which cannot hold comments, the Markdown documents and the configuration of the tools and CI. All of it is GPL-3.0-or-later (`LICENSE.txt`) |
| licensecheck: "CC-BY-4.0 and/or GPL-3" in `README.md` and `LORE.md` | they mention the license of the artwork in `readme_assets/`, which is not in the tarball |
| source checksum differs from upstream, "diff -r shows no differences" | the source RPM was made by `tools/release.sh`; for a review request, use the downloaded `Source0` (commands above) |
| "Fully versioned dependency in subpackages" | not applicable: no subpackage requires the base package; `-server` and `-tools` require `-data` at the same version-release |
| `gpgverify` not used | upstream publishes no signature: GitHub attestations instead (see below) |

## Known gaps

- **Not run yet:** a build on an architecture other than x86 (aarch64 in
  COPR would be the first). The table above is the author's reading of
  the guidelines, checked by the tools named in it; a reviewer may still
  ask for changes.
- **No GPG signature** on the tarball: the release files carry a GitHub
  attestation instead (docs/RELEASING.md), which `Source0` verification
  in Fedora does not use.

## Questions a reviewer may ask

**Why two commands per program (`dragongl-client` and `dragongl_client`,
`dragongl-server` and `dragongl_server`)?** The hyphenated ones are bash
launchers for people: they print the banner, ask for what is missing
(server address, player name, passwords) and set defaults. The ones with
an underscore are the programs, run by the launchers, by scripts and by
the systemd unit; they never prompt unless asked. Each has its man page.

**Why `Terminal=true` in the desktop entry?** The launcher asks for the
server and the credentials in the terminal before the Vulkan window
opens; without a terminal the player could not answer.

**Why not clang?** The spec uses gcc, the default of Fedora. Upstream
prefers clang for development (`DRAGONGL_USE_CLANG`, default ON, only
when no compiler is chosen); the spec turns it off, and CI builds and
tests with both compilers.

**Where do the passwords of the service go?** In
`/etc/dragongl/server-password` and `/etc/dragongl/dm-password`, files
the administrator creates (root-only). The unit loads them with
`LoadCredential=`: systemd gives the dynamic user a private copy under
`/run/credentials/dragongl-server.service/`, passed to the server with
`--password-file`/`--dm-password-file`. They never pass through the
environment, the command line or a world-readable file. A missing file
is not an error for systemd (`SetCredential=` gives an empty default):
the server then exits with status 2 and a message naming the file
(server password), or disables the `dm` account (DM password).
`RestartPreventExitStatus=2` stops a restart loop.

**Does the package open a port?** No. It ships the firewalld service
`dragongl-server` (8080/tcp); the administrator enables it with
`firewall-cmd --permanent --add-service=dragongl-server`.

**Why is `/usr/bin/dragongl-analyze-balance` a symlink?** The two tools
are Python scripts installed with their data in
`%{_datadir}/dragongl/tools`; the commands in `%{_bindir}` link to them
(relative symlinks), and their man pages carry the command names. The
scripts use the standard library and `reportlab`, both required
explicitly because rpm generates no dependency for scripts outside the
Python paths.

**Why `validate-relax` and not `validate` for the AppStream file?** It
is what the Fedora AppStream guidelines ask for; `validate` also wants to
download the screenshots, and the build has no network.

**Do the tests need the network?** No. The integration suite starts a
server on a free port of the loopback interface and connects to it over
TLS; nothing leaves the build host. The Vulkan compute test skips itself
without a Vulkan driver.
