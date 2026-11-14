# Releasing Dragon GL

A release is a commit whose version, changelog and tag agree. Everything
else is made from that commit by scripts, so that two people releasing
the same commit publish the same files.

## What identifies a release

- **Version:** the `Version:` line of `dragongl.spec`, `YYYY.MM.DD.NN`
  (the date and a counter within the day). CMake, the binaries
  (`--version`), the man pages and the packages read it from there.
- **Changelog:** the first entry of `CHANGELOG.md` has that version; the
  entries are in the format of an RPM changelog.
- **Tag:** the version, on the commit that sets it.

`tools/check_release.py` checks the first two and the order of the
entries; CI runs it on every push, the spec runs it in `%check`.

## Steps

1. On `main`, with CI green: bump `Version:` in `dragongl.spec` and add
   the entry at the top of `CHANGELOG.md`, in the same commit as the
   change (or in the last commit of the release).

2. Check, on your machine:

   ```bash
   python3 tools/check_release.py
   ./build.sh -c -w -k                       # build, unit, integration and client tests
   tools/quality.sh commands man repro       # generated reference, man pages, reproducible build
   tools/release.sh --srpm                   # dist/: tarball, spec, source RPM, SHA256SUMS
   ```

   `tools/release.sh` refuses a tree with changes that are not committed
   and a version whose tag already exists on another commit.

3. Push the commit, wait for CI, then tag and push the tag (the script
   prints the two commands):

   ```bash
   git tag -a 2026.11.14.04 -m "Dragon GL 2026.11.14.04"
   git push origin 2026.11.14.04
   ```

4. The tag starts `.github/workflows/release.yml`: it checks that the tag
   is the version of the spec, builds the tarball and the source RPM with
   `tools/release.sh`, builds and lints the RPMs in a Fedora container
   (with `%check`), and publishes a GitHub release with those files,
   `SHA256SUMS` and the changelog entry as its notes. A tag on a commit
   with another version publishes nothing.

5. COPR builds the packages of the repository
   (`dnf copr enable nicolataibi/dragongl`) from the same tag, either with
   its rpkg/rpmautospec method on `dragongl.spec`, or with the *make srpm*
   method, which runs `.copr/Makefile` (it calls `tools/release.sh
   --srpm`). With a webhook from GitHub to COPR this needs no action.

## What can be verified afterwards

- The tarball: `tools/release.sh <tag>` on a clone gives a file with the
  SHA-256 published in `SHA256SUMS`. GitHub's own "Source code" archive
  of the tag has the same content, though GitHub does not promise the
  same bytes for it.
- The binaries: built twice from the tarball with the same compiler,
  libraries and options, they are identical (`tools/quality.sh repro`,
  see BUILD.md). The RPMs of the release are built by Fedora's toolchain
  of that day: rebuilding them identically needs the same package set.
- The files are not signed. `SHA256SUMS` protects against a damaged
  download, not against someone who can replace the files of the
  release; COPR signs the packages it builds with the key of the project
  there.

## If something goes wrong

- **The release job fails:** fix the cause on `main` with a new version
  (a tag that was pushed is never moved), or run the job again from the
  Actions page if the failure was not in the sources; a second run
  replaces the files of the release.
- **A release must be withdrawn:** delete the GitHub release (the tag
  stays, so the version is never used twice) and publish a new version.
