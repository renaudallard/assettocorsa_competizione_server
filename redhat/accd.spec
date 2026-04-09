Name:           accd
Version:        0.1.0
Release:        1%{?dist}
Summary:        ACC dedicated server, clean-room reimplementation

License:        BSD-2-Clause
URL:            https://github.com/renaudallard/assettocorsa_competizione_server
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

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

%files
%license LICENSE
%doc README.md
/usr/bin/accd
