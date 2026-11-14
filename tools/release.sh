#!/usr/bin/env bash
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# release.sh - the source tarball of a release, and optionally its source RPM.
#
#   tools/release.sh [--srpm] [--allow-dirty] [commit]
#
# Writes into dist/:
#   dragongl-<version>.tar.gz   `git archive` of the commit (default HEAD):
#                               the same commit always gives the same bytes
#   dragongl-<version>.spec     the spec for a plain rpmbuild (--srpm)
#   dragongl-<version>-1.src.rpm                               (--srpm)
#   SHA256SUMS                  of the files above
#
# It refuses to run when the release is not consistent (check_release.py),
# when the tree has changes that are not committed, or when the tag of the
# version already exists on another commit. It never tags and never
# pushes: the commands to do it are printed at the end.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
die() { printf 'release.sh: %s\n' "$*" >&2; exit 1; }

SRPM=0
DIRTY=0
COMMIT=HEAD
while (($#)); do
    case $1 in
    --srpm) SRPM=1 ;;
    --allow-dirty) DIRTY=1 ;;
    -h | --help) sed -n '5,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) die "unknown option '$1' (see --help)" ;;
    *) COMMIT=$1 ;;
    esac
    shift
done

cd "$ROOT"
command -v git >/dev/null || die "git not found"
git rev-parse --verify -q "$COMMIT^{commit}" >/dev/null || die "'$COMMIT' is not a commit"
if ((!DIRTY)) && [[ -n $(git status --porcelain) ]]; then
    die "the tree has changes that are not committed (git status); --allow-dirty to go on"
fi

# The version of the commit, not of the working tree.
VERSION="$(git show "$COMMIT:dragongl.spec" | sed -n 's/^Version:[[:space:]]*//p')"
[[ $VERSION =~ ^[0-9]{4}\.[0-9]{2}\.[0-9]{2}\.[0-9]{2}$ ]] || die "Version '$VERSION' is not YYYY.MM.DD.NN"
if [[ $(git rev-parse "$COMMIT") == $(git rev-parse HEAD) ]]; then
    python3 tools/check_release.py >/dev/null || { python3 tools/check_release.py; die "the release is not consistent"; }
fi
if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null &&
    [[ $(git rev-parse "refs/tags/$VERSION^{commit}") != $(git rev-parse "$COMMIT^{commit}") ]]; then
    die "tag $VERSION exists and is another commit: bump Version in dragongl.spec"
fi

OUT="$ROOT/dist"
mkdir -p "$OUT"
TARBALL="dragongl-$VERSION.tar.gz"
# tar.gz of git archive: no timestamp of the run, no user names; the
# export-ignore of .gitattributes leaves the artwork and the CI files out.
git archive --format=tar.gz --prefix="dragongl-$VERSION/" -o "$OUT/$TARBALL" "$COMMIT"
FILES=("$TARBALL")

if ((SRPM)); then
    command -v rpmbuild >/dev/null || die "--srpm needs rpmbuild"
    # %autorelease and %autochangelog are for rpmautospec (COPR, Koji). A
    # plain rpmbuild gets release 1 and the changelog of CHANGELOG.md.
    SPEC="$OUT/dragongl-$VERSION.spec"
    {
        git show "$COMMIT:dragongl.spec" |
            sed -e 's/^Release:.*/Release:        1%{?dist}/' -e '/^%autochangelog$/d'
        git show "$COMMIT:CHANGELOG.md" | sed -e '/^\* /s/$/-1/'
    } >"$SPEC"
    # A fixed name: rpm writes the path of its build directory into the
    # package, so a random one would make every source RPM different.
    TOP="/tmp/dragongl-release-$VERSION"
    rm -rf "$TOP"
    mkdir -m 700 "$TOP" || die "cannot create $TOP"
    trap 'rm -rf "$TOP"' EXIT
    # The time of the commit, not of the run, in the package and its files:
    # the same commit gives the same source RPM.
    SOURCE_DATE_EPOCH="$(git log -1 --format=%ct "$COMMIT")"
    export SOURCE_DATE_EPOCH
    touch -d "@$SOURCE_DATE_EPOCH" "$SPEC" "$OUT/$TARBALL"
    rpmbuild -bs --define "_topdir $TOP" --define "_sourcedir $OUT" --define "_srcrpmdir $OUT" \
        --define "dist %{nil}" --define "_buildhost localhost" \
        --define "use_source_date_epoch_as_buildtime 1" \
        --define "clamp_mtime_to_source_date_epoch 1" "$SPEC" >/dev/null
    FILES+=("dragongl-$VERSION.spec" "dragongl-$VERSION-1.src.rpm")
    [[ -f $OUT/dragongl-$VERSION-1.src.rpm ]] || die "rpmbuild did not write the source RPM"
fi

(cd "$OUT" && sha256sum "${FILES[@]}" >SHA256SUMS)
printf 'dist/ for %s (commit %s):\n' "$VERSION" "$(git rev-parse --short "$COMMIT")"
sed 's/^/  /' "$OUT/SHA256SUMS"
cat <<EOF

To publish (see docs/RELEASING.md):
  git tag -a $VERSION -m "Dragon GL $VERSION" $(git rev-parse --short "$COMMIT")
  git push origin $VERSION
EOF
