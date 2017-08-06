#
#    fty-metric-composite - Agent that computes new metrics from bunch of other metrics
#
#    Copyright (C) 2014 - 2017 Eaton
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License along
#    with this program; if not, write to the Free Software Foundation, Inc.,
#    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

# To build with draft APIs, use "--with drafts" in rpmbuild for local builds or add
#   Macros:
#   %_with_drafts 1
# at the BOTTOM of the OBS prjconf
%bcond_with drafts
%if %{with drafts}
%define DRAFTS yes
%else
%define DRAFTS no
%endif
Name:           fty-metric-composite
Version:        1.0.0
Release:        1
Summary:        agent that computes new metrics from bunch of other metrics
License:        GPL-2.0+
URL:            https://42ity.org
Source0:        %{name}-%{version}.tar.gz
Group:          System/Libraries
# Note: ghostscript is required by graphviz which is required by
#       asciidoc. On Fedora 24 the ghostscript dependencies cannot
#       be resolved automatically. Thus add working dependency here!
BuildRequires:  ghostscript
BuildRequires:  asciidoc
BuildRequires:  automake
BuildRequires:  autoconf
BuildRequires:  libtool
BuildRequires:  pkgconfig
BuildRequires:  systemd-devel
BuildRequires:  systemd
%{?systemd_requires}
BuildRequires:  xmlto
BuildRequires:  gcc-c++
BuildRequires:  zeromq-devel
BuildRequires:  czmq-devel
BuildRequires:  malamute-devel
BuildRequires:  lua-devel
BuildRequires:  fty-proto-devel
BuildRequires:  cxxtools-devel
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
fty-metric-composite agent that computes new metrics from bunch of other metrics.

%package -n libfty_metric_composite0
Group:          System/Libraries
Summary:        agent that computes new metrics from bunch of other metrics shared library

%description -n libfty_metric_composite0
This package contains shared library for fty-metric-composite: agent that computes new metrics from bunch of other metrics

%post -n libfty_metric_composite0 -p /sbin/ldconfig
%postun -n libfty_metric_composite0 -p /sbin/ldconfig

%files -n libfty_metric_composite0
%defattr(-,root,root)
%doc COPYING
%{_libdir}/libfty_metric_composite.so.*

%package devel
Summary:        agent that computes new metrics from bunch of other metrics
Group:          System/Libraries
Requires:       libfty_metric_composite0 = %{version}
Requires:       zeromq-devel
Requires:       czmq-devel
Requires:       malamute-devel
Requires:       lua-devel
Requires:       fty-proto-devel
Requires:       cxxtools-devel

%description devel
agent that computes new metrics from bunch of other metrics development tools
This package contains development files for fty-metric-composite: agent that computes new metrics from bunch of other metrics

%files devel
%defattr(-,root,root)
%{_includedir}/*
%{_libdir}/libfty_metric_composite.so
%{_libdir}/pkgconfig/libfty_metric_composite.pc
%{_mandir}/man3/*
%{_mandir}/man7/*

%prep
%setup -q

%build
sh autogen.sh
%{configure} --enable-drafts=%{DRAFTS} --with-systemd-units
make %{_smp_mflags}

%install
make install DESTDIR=%{buildroot} %{?_smp_mflags}

# remove static libraries
find %{buildroot} -name '*.a' | xargs rm -f
find %{buildroot} -name '*.la' | xargs rm -f

%files
%defattr(-,root,root)
%doc COPYING
%{_bindir}/fty-metric-composite
%{_mandir}/man1/fty-metric-composite*
%{_bindir}/fty-metric-composite-configurator
%{_mandir}/man1/fty-metric-composite-configurator*
%config(noreplace) %{_sysconfdir}/fty-metric-composite/fty-metric-composite.cfg
/usr/lib/systemd/system/fty-metric-composite@.service
%config(noreplace) %{_sysconfdir}/fty-metric-composite/fty-metric-composite-configurator.cfg
/usr/lib/systemd/system/fty-metric-composite-configurator.service
%dir %{_sysconfdir}/fty-metric-composite
%if 0%{?suse_version} > 1315
%post
%systemd_post fty-metric-composite@.service fty-metric-composite-configurator.service
%preun
%systemd_preun fty-metric-composite@.service fty-metric-composite-configurator.service
%postun
%systemd_postun_with_restart fty-metric-composite@.service fty-metric-composite-configurator.service
%endif

%changelog
