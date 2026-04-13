Name:           accd
Version:        0.2.3
Release:        1%{?dist}
Summary:        ACC dedicated server, clean-room reimplementation

License:        BSD-2-Clause
URL:            https://github.com/renaudallard/assettocorsa_competizione_server
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-rpm-macros

%description
An independent reimplementation of the Assetto Corsa Competizione
dedicated server, built so an unmodified ACC game client can connect
to it and play a private multiplayer session on Linux, without Wine.

%prep
%setup -q

%build
make -C accd %{?_smp_mflags}

%install
make -C accd install DESTDIR=%{buildroot} PREFIX=/usr
install -D -m 644 debian/accd.service %{buildroot}%{_unitdir}/accd.service

%post
%systemd_post accd.service

%preun
%systemd_preun accd.service

%postun
%systemd_postun_with_restart accd.service

%files
%license LICENSE
%doc README.md
/usr/bin/accd
/usr/share/man/man1/accd.1*
%{_unitdir}/accd.service
