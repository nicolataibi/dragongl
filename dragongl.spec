# License: GPL-3.0-or-later
%global rpkg_srpm_build_method rpmautospec
# One documentation directory for all the subpackages.
%global _docdir_fmt %{name}
# Upstream develops and tests with clang first (CMake picks it by default);
# gcc builds too and CI keeps it so.
%global toolchain clang
%global app_id io.github.nicolataibi.dragongl

Name:           dragongl
Version:        2026.11.14.11
Release:        %autorelease
Summary:        Multi-user 3D dungeon RPG: Vulkan client

# The artwork in readme_assets/ (CC BY 4.0) is not in the release tarball
# (export-ignore) and is not packaged: everything shipped is GPL-3.0-or-later.
License:        GPL-3.0-or-later
URL:            https://github.com/nicolataibi/dragongl
Source0:        %{url}/archive/refs/tags/%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  clang
BuildRequires:  glslc
BuildRequires:  pkgconfig(glfw3)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  pkgconfig(libsodium)
# The JSON parser: the copy in third_party/ is deleted in the prep section.
BuildRequires:  pkgconfig(libcjson) >= 1.7.17
# TLS 1.3 between client and server
BuildRequires:  pkgconfig(openssl)
BuildRequires:  python3
# Needed by tools/check_data.py (check section): data/*.json against data/schema.
BuildRequires:  python3-jsonschema
BuildRequires:  systemd-rpm-macros
# Menu entry and AppStream metadata of the client, validated in the check section.
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

# Owns the icon directories.
Requires:       hicolor-icon-theme

# The client renders with Vulkan 1.3: it needs a Vulkan driver.
Recommends:     mesa-vulkan-drivers

%description
Dragon GL is a multi-user client-server dungeon exploration RPG with a
d20-style rules engine, 100 randomly generated floors and a real-time
3D client. This package contains the client, rendered with Vulkan 1.3
(GPU-driven: culling and mesh generation run on the GPU).
The game server is in the %{name}-server package.


%package server
Summary:        Multi-user 3D dungeon RPG: game server
Requires:       %{name}-data = %{version}-%{release}
# dm_pdf and dm_mapfloor run generate_pdf_map.py from the tools package.
Recommends:     %{name}-tools = %{version}-%{release}
%{?systemd_requires}

%description server
The authoritative Dragon GL game server: world simulation, NPC AI, combat,
persistence of characters and world. Clients connect over TLS 1.3: the
server creates its own certificate at the first start, or uses the one
it is given. It needs no graphics stack. A systemd
unit (dragongl-server.service, disabled by default) runs it as an
unprivileged dynamic user with its state in /var/lib/dragongl; passwords
go in its configuration file in /etc/dragongl.


%package data
Summary:        Game data for %{name}
BuildArch:      noarch

%description data
The read-only game datasets of Dragon GL (bestiary, items and spells, in
JSON), used by the server and by the Data Studio of %{name}-tools.


%package tools
Summary:        Dungeon Master and balance tools for %{name}
BuildArch:      noarch
Requires:       python3
Requires:       %{name}-data = %{version}-%{release}

%description tools
Dungeon Master and developer utilities for Dragon GL: the combat log
analyzer used to evaluate game balance, the PDF floor map generator used
by the DM commands of the server, and the Data Studio, an HTML suite to
inspect and edit the JSON datasets.


%package doc
Summary:        Documentation for %{name}
BuildArch:      noarch

%description doc
User manual, quick start, build and installation guides, protocol and
security documentation, game lore and the release notes of Dragon GL.


%prep
%autosetup -p1
# No bundled library: cJSON comes from the system (DRAGONGL_SYSTEM_CJSON).
rm -r third_party/cjson


%build
# The distribution's flags are the only compiler flags: build type None
# adds no optimization of its own (an empty build type would default to
# Release, -O3), and the hardening flags of the project are off.
# DRAGONGL_REPRODUCIBLE=OFF: rpm maps the build directory itself (to
# /usr/src/debug) and collects the sources of the debugsource package
# from the paths in the debug information; with the project's own
# -ffile-prefix-map it finds none and the build fails (empty
# debugsourcefiles.list).
%cmake -DCMAKE_BUILD_TYPE=None -DDRAGONGL_HARDENING=OFF -DDRAGONGL_REPRODUCIBLE=OFF \
       -DDRAGONGL_SYSTEM_CJSON=ON -DDRAGONGL_SYSTEMD_UNITDIR=%{_unitdir}
%cmake_build


%install
%cmake_install


%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{app_id}.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/%{app_id}.metainfo.xml
python3 tools/check_release.py
python3 tools/check_data.py
# Unit tests (the Vulkan compute test skips itself without a Vulkan driver).
cmake -S tests -B %{_vpath_builddir}-tests -DCMAKE_BUILD_TYPE=None \
      -DDRAGONGL_HARDENING=OFF -DDRAGONGL_SYSTEM_CJSON=ON \
      -DCMAKE_C_FLAGS="%{optflags}" \
      -DDRAGONGL_TEST_SHADER_DIR="$PWD/%{_vpath_builddir}/shaders"
cmake --build %{_vpath_builddir}-tests %{?_smp_mflags}
ctest --test-dir %{_vpath_builddir}-tests --output-on-failure


%post server
%systemd_post dragongl-server.service

%preun server
%systemd_preun dragongl-server.service

%postun server
%systemd_postun_with_restart dragongl-server.service


%files
%license LICENSE.txt
%{_bindir}/dragongl_client
%{_bindir}/dragongl-client
%{_mandir}/man1/dragongl_client.1*
%{_mandir}/man1/dragongl-client.1*
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/shaders/
%{_datadir}/applications/%{app_id}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_metainfodir}/%{app_id}.metainfo.xml


%files server
%license LICENSE.txt
%{_bindir}/dragongl_server
%{_bindir}/dragongl-server
%{_bindir}/dragongl-reset-world
%{_mandir}/man1/dragongl_server.1*
%{_mandir}/man1/dragongl-server.1*
%{_mandir}/man1/dragongl-reset-world.1*
%{_unitdir}/dragongl-server.service
%dir %{_sysconfdir}/%{name}
%config(noreplace) %attr(0600,root,root) %{_sysconfdir}/%{name}/dragongl-server.conf


%files data
%license LICENSE.txt
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/data/


%files tools
%license LICENSE.txt
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/tools/
%{_mandir}/man1/analyze_balance.1*
%{_mandir}/man1/generate_pdf_map.1*


%files doc
%license LICENSE.txt
%doc README.md HOWTO.md INSTALL.md BUILD.md DEVELOPMENT.md PROTOCOL.md SECURITY.md
%doc CONTRIBUTING.md CODE_OF_CONDUCT.md ACCESSIBILITY.md LORE.md docs/GPU_TESTING.md docs/COMMANDS.md


%changelog
%autochangelog
