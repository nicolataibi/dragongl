# License: GPL-3.0-or-later
%global rpkg_srpm_build_method rpmautospec
%global _docdir_fmt %{name}
%global toolchain clang

Name:           dragongl
Version:        2026.09.03.05
Release:        %autorelease
Summary:        Multi-User Client-Server 3D RPG Engine — OpenGL (GLFW) & Vulkan

License:        GPL-3.0-or-later
URL:            https://github.com/nicolataibi/dragongl
Source0:        %{url}/archive/refs/tags/%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  clang
BuildRequires:  compiler-rt
BuildRequires:  pkgconfig(gl)
BuildRequires:  pkgconfig(glu)
BuildRequires:  pkgconfig(glfw3)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  glslc

# Automatic dependency generation handles libraries
Requires:       %{name}-data = %{version}-%{release}

%description
Dragon GL is a high-performance, 3D multi-user client-server dungeon
exploration simulator. The engine features a fully functional open Fantasy
RPG mechanical core and real-time front-ends built on both OpenGL (via GLFW) 
and Vulkan.


%package data
Summary:        Game assets for %{name}
BuildArch:      noarch
Requires:       %{name} = %{version}-%{release}

%description data
This package contains shaders and other runtime data
required by Dragon GL.


%package doc
Summary:        Documentation and user manuals for %{name}
BuildArch:      noarch

%description doc
This package contains user manuals, READMEs, HOWTOs,
and additional assets explaining the Dragon GL engine and game play.


%package tools
Summary:        Developer and DM tools for %{name}
BuildArch:      noarch
Requires:       python3

%description tools
This package contains developer and Dungeon Master (DM) utilities
for Dragon GL, including the combat log statistical analysis tool
(analyze_balance.py) used to evaluate game balance from server
session logs, and (generate_pdf_map.py) for maps.


%prep
%autosetup -n %{name}-%{version} -p1


%conf
%cmake


%build
%cmake_build


%install
%cmake_install


%files
%license LICENSE.txt
%{_bindir}/dragongl_server
%{_bindir}/dragongl_client
%{_bindir}/dragongl-server
%{_bindir}/dragongl-client
%{_bindir}/dragongl_reset_world

%{_mandir}/man1/dragongl_server.1*
%{_mandir}/man1/dragongl_client.1*
%{_mandir}/man1/dragongl-server.1*
%{_mandir}/man1/dragongl-client.1*
%{_mandir}/man1/dragongl_reset_world.1*


%files data
%doc HOWTO.txt
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/shaders/


%files doc
%license LICENSE.txt
%doc README.md HOWTO.txt
%doc readme_assets/


%files tools
%license LICENSE.txt
%{_datadir}/%{name}/tools/analyze_balance.py
%{_datadir}/%{name}/tools/generate_pdf_map.py
%{_mandir}/man1/analyze_balance.1*
%{_mandir}/man1/generate_pdf_map.1*


%changelog
%autochangelog
