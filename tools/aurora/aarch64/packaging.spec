Name:       ru.androidcompatibilitylayer.appmanager
Summary:    AndroidCompatibilityLayer for Aurora OS
Version:    0.2.10
Release:    10
License:    GPL-3.0-only
URL:        https://github.com/Arpolend2/AndroidCompatibilityLayer
Source0:    %{name}-%{version}.tar.bz2

%global __provides_exclude_from ^%{_datadir}/%{name}/(fixtures|android-runtime)/.*$
%global __requires_exclude_from ^%{_datadir}/%{name}/android-runtime/.*$
%global debug_package %{nil}
%global _binary_payload w19.zstdio
%define __os_install_post %{nil}
%define _build_id_links none
%{!?acl_stage:%global acl_stage /nonexistent}

Requires:       sailfishsilica-qt5 >= 0.10.9
Requires:       qt5-plugin-imageformat-jpeg
Requires:       nemo-qml-plugin-notifications-qt5

%description
An experimental Megler port for Aurora OS with an Android 9 AArch64 runtime.
Built directly against the Platform SDK sysroot without sb2.

%prep
%build
%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
if [ ! -x "%{acl_stage}/usr/bin/%{name}" ]; then
    echo "no installed tree in %{acl_stage}" >&2
    exit 1
fi
cp -a "%{acl_stage}/." %{buildroot}/

%files
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
