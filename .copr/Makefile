# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# Source RPM for COPR ("make srpm" build method): COPR clones the repository
# and runs, in its top directory,
#   make -f .copr/Makefile srpm outdir=<dir> spec=<path>
# The tarball and the spec come from tools/release.sh, the same script that
# makes the files of a GitHub release.

outdir ?= $(CURDIR)/dist

.PHONY: srpm
srpm:
	command -v git >/dev/null && command -v rpmbuild >/dev/null && command -v python3 >/dev/null || \
		dnf -y install git-core rpm-build python3
	git config --global --add safe.directory "$(CURDIR)"
	tools/release.sh --srpm --allow-dirty
	mkdir -p "$(outdir)"
	cp dist/*.src.rpm "$(outdir)"/
