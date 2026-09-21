%global modname hid-apple-studio-display
%global udevdir %{_prefix}/lib/udev

Name:           %{modname}-dkms
Version:        1.1.0
Release:        1%{?dist}
Summary:        Apple Studio Display brightness and orientation driver (DKMS)
License:        GPL-2.0-only
URL:            https://github.com/michaljach/hid-apple-studio-display
Source0:        %{url}/archive/v%{version}/%{modname}-%{version}.tar.gz
BuildArch:      noarch

Requires:       dkms
Requires:       gcc
Requires:       make
Requires:       kernel-devel
Recommends:     python3-gobject
Recommends:     iio-sensor-proxy

%description
Kernel driver that exposes the brightness of an Apple Studio Display or
Studio Display XDR as a standard /sys/class/backlight device, attached to
the DRM connector the display is on, so desktop environments (GNOME 48+)
show their brightness slider for it and brightness keys work. The display's
orientation sensor is exposed as an IIO inclinometer, with an experimental
helper (asd-autorotate) that rotates the GNOME output to match.

This package uses DKMS to build the module for every installed kernel.

%prep
%autosetup -n %{modname}-%{version}

%build
# DKMS builds the module against the installed kernels at install time.

%install
install -d %{buildroot}%{_usrsrc}/%{modname}-%{version}
install -m 0644 %{modname}.c Makefile dkms.conf %{buildroot}%{_usrsrc}/%{modname}-%{version}/
%{_udevrulesdir}/90-hid-apple-studio-display.rules
%{udevdir}/asd-als-role
%{udevdir}/asd-bind-orientation
%{_bindir}/asd-autorotate
%{_userunitdir}/asd-autorotate.service
install -D -m 0644 contrib/90-hid-apple-studio-display.rules %{buildroot}%{_udevrulesdir}/90-hid-apple-studio-display.rules
install -D -m 0755 contrib/asd-als-role %{buildroot}%{udevdir}/asd-als-role
install -D -m 0755 contrib/asd-bind-orientation %{buildroot}%{udevdir}/asd-bind-orientation
install -D -m 0755 contrib/asd-autorotate %{buildroot}%{_bindir}/asd-autorotate
install -D -m 0644 contrib/asd-autorotate.service %{buildroot}%{_userunitdir}/asd-autorotate.service

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
%{_udevrulesdir}/90-hid-apple-studio-display.rules
%{udevdir}/asd-als-role
%{udevdir}/asd-bind-orientation
%{_bindir}/asd-autorotate
%{_userunitdir}/asd-autorotate.service

%changelog
* Mon Sep 21 2026 Michal Jach <michaljach@gmail.com> - 1.1.0-1
- Orientation sensor as an IIO inclinometer; asd-autorotate helper.
- udev rule so iio-sensor-proxy uses the front ambient light sensor.

* Mon Sep 21 2026 Michal Jach <michaljach@gmail.com> - 1.0.1-1
- Initial release.
