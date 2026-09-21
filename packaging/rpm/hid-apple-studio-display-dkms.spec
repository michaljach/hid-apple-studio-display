%global modname hid-apple-studio-display

Name:           %{modname}-dkms
Version:        1.0.1
Release:        1%{?dist}
Summary:        Apple Studio Display brightness driver (DKMS)
License:        GPL-2.0-only
URL:            https://github.com/michaljach/hid-apple-studio-display
Source0:        %{url}/archive/v%{version}/%{modname}-%{version}.tar.gz
BuildArch:      noarch

Requires:       dkms
Requires:       gcc
Requires:       make
Requires:       kernel-devel

%description
Kernel driver that exposes the brightness of an Apple Studio Display or
Studio Display XDR as a standard /sys/class/backlight device, attached to
the DRM connector the display is on, so desktop environments (GNOME 48+)
show their brightness slider for it and brightness keys work.

This package uses DKMS to build the module for every installed kernel.

%prep
%autosetup -n %{modname}-%{version}

%build
# DKMS builds the module against the installed kernels at install time.

%install
install -d %{buildroot}%{_usrsrc}/%{modname}-%{version}
install -m 0644 %{modname}.c Makefile dkms.conf %{buildroot}%{_usrsrc}/%{modname}-%{version}/

%post
dkms add -m %{modname} -v %{version} -q || :
dkms build -m %{modname} -v %{version} -q || :
dkms install -m %{modname} -v %{version} -q || :
modprobe %{modname} >/dev/null 2>&1 || :

%preun
if [ "$1" -eq 0 ]; then
    modprobe -r %{modname} >/dev/null 2>&1 || :
    dkms remove -m %{modname} -v %{version} --all -q || :
fi

%files
%license LICENSE
%doc README.md
%{_usrsrc}/%{modname}-%{version}/

%changelog
* Mon Sep 21 2026 Michal Jach <michaljach@gmail.com> - 1.0.1-1
- Initial release.
