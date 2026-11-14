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
| `dragongl-server` | binary | the server, its launchers, man pages, the systemd unit, `/etc/dragongl` |
| `dragongl-data` | noarch | the JSON datasets |
| `dragongl-tools` | noarch | the balance analyser, the PDF map generator, the Data Studio |
| `dragongl-doc` | noarch | the Markdown documentation |

## Guidelines, point by point

| Guideline | How the spec meets it | Checked by |
| :--- | :--- | :--- |
| Naming and versioning | `dragongl`, version `YYYY.MM.DD.NN` from one place (`Version:`), `%autorelease` and `%autochangelog` | `tools/check_release.py` in `%check` |
| License | `GPL-3.0-or-later` (SPDX). Nothing under another license is shipped: the artwork (CC BY 4.0) is not in the tarball, and the copy of cJSON (MIT) is deleted in `%prep` | `%license LICENSE.txt` in every package; rpmlint |
| No bundled libraries | cJSON is the only third-party code in the tree (`third_party/cjson`); the spec removes it and builds with `-DDRAGONGL_SYSTEM_CJSON=ON` against `pkgconfig(libcjson)` | the build fails if anything still needs the copy |
| Source | `Source0` is the archive of the tag on GitHub; `tools/release.sh` makes the same content with `git archive` | `rpmbuild`; `spectool -g` on Fedora |
| Build flags | `%cmake` with the flags of the distribution only: the project's own hardening flags and path mapping are off (`DRAGONGL_HARDENING=OFF`, `DRAGONGL_REPRODUCIBLE=OFF`), so `%optflags` and the debuginfo extraction are untouched | debuginfo and debugsource packages are built; rpmlint |
| Compiler | `%global toolchain clang`: upstream develops with clang first. gcc builds as well | CI: clang and gcc on Ubuntu and Fedora, `-Werror` |
| BuildRequires | `pkgconfig()` names for the libraries; tools named by package | `dnf builddep` in a clean container |
| Tests | `%check` runs the release and data checks and the unit suite, built with `%optflags` | `rpm` job |
| Desktop application | `io.github.nicolataibi.dragongl.desktop` (it runs in a terminal: the launcher asks for server, name and password before the window opens), a scalable icon in hicolor, AppStream metadata | `desktop-file-validate` and `appstream-util validate-relax` in `%check` |
| systemd | unit installed in `%{_unitdir}`, disabled by default, the scriptlets of `systemd-rpm-macros`; the service runs as a dynamic user, so no user is created | rpmlint |
| Configuration | `/etc/dragongl/dragongl-server.conf` is `%config(noreplace)`, mode 0600 because it holds passwords | rpmlint (one filter, explained) |
| File ownership | every directory the packages create is owned; `%{_datadir}/dragongl` is co-owned by the packages that do not depend on each other; the icon directories belong to `hicolor-icon-theme`, which is required | rpmlint |
| Documentation | man pages for every command in `%{_bindir}`; the Markdown guides in `-doc` | `mandoc -T lint` in CI |
| Architectures | no `ExcludeArch`: the code has no assembly and no assumption on the byte order of the host (the wire format and the save files are explicitly little-endian) | COPR: Fedora 44, 45 and rawhide on x86_64 and i386, EPEL 10 on x86_64 (2026.11.14.09) |

## rpmlint filters

`dragongl.rpmlintrc` has three filters, each with its reason; rpmlint
reports a filter that matches nothing, so none can outlive its cause.

| Filter | Why it is right |
| :--- | :--- |
| `non-readable /etc/dragongl/dragongl-server.conf 600` | the file holds the passwords of the server |
| `dragongl-data: no-documentation` | data only; the documentation is in `dragongl-doc` |
| `spelling-error` for Vulkan, shaders, changelog, unprivileged, DynamicUser | words the dictionary of rpmlint does not know |

## Running the checks

On Fedora, from a clean tree:

```bash
sudo dnf install -y rpm-build rpmlint fedora-review mock
tools/release.sh --srpm                       # dist/dragongl.spec, dist/*.src.rpm
sudo dnf builddep -y dist/*.src.rpm
rpmbuild --rebuild dist/*.src.rpm
rpmlint -r dragongl.rpmlintrc ~/rpmbuild/RPMS/*/dragongl*.rpm dist/*.src.rpm

# The checklist of the Fedora review, in a clean chroot (your user must be
# in the mock group):
cd dist && fedora-review --rpm-spec -n dragongl-*.src.rpm --mock-config fedora-rawhide-x86_64
```

`fedora-review` writes `review.txt`: every `[!]` is a point to fix or to
explain in the review request; `[ ]` are the points a human checks.

COPR builds the same spec. To cover every supported Fedora, enable in the
project settings the chroots `fedora-<N>-x86_64` of the releases still
supported, `fedora-rawhide-x86_64`, and `aarch64` for at least one of
them.

## Known gaps

- **Not run yet:** `fedora-review`, and a build on an architecture other
  than x86 (aarch64 in COPR would be the first). The table above is the
  author's reading of the guidelines, checked by the tools named in it;
  a reviewer may still ask for changes.
- **The man pages `analyze_balance(1)` and `generate_pdf_map(1)`** have
  generic names and document programs that are not in `%{_bindir}` (they
  live in `%{_datadir}/dragongl/tools`). A reviewer may ask for a
  `dragongl-` prefix.
- **No GPG signature** on the tarball: the release files carry a GitHub
  attestation instead (docs/RELEASING.md), which `Source0` verification
  in Fedora does not use.
