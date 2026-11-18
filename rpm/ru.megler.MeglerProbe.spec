Name:       ru.megler.MeglerProbe
Summary:    Megler core diagnostic for Aurora OS
Version:    0.1.0
Release:    1
License:    GPL-3.0-only
URL:        https://github.com/Computershik73/megler-w10m
Source0:    %{name}-%{version}.tar.bz2

%global __provides_exclude_from ^%{_datadir}/%{name}/(fixtures|android-runtime)/.*$
%global __requires_exclude_from ^%{_datadir}/%{name}/android-runtime/.*$

Requires:       sailfishsilica-qt5 >= 0.10.9
Requires:       qt5-plugin-imageformat-jpeg
Requires:       nemo-qml-plugin-notifications-qt5
BuildRequires:  cmake
BuildRequires:  qt5-qttools-linguist
BuildRequires:  python3
BuildRequires:  pkgconfig(auroraapp)
BuildRequires:  pkgconfig(runtime-manager-qt5)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(Qt5Sensors)
BuildRequires:  pkgconfig(Qt5Positioning)

%description
An experimental Megler port for Aurora OS. It can import and attempt to run
standalone Android APKs, but Android application isolation is not implemented.

%prep
%autosetup

%build
%cmake -DMEGLER_BUILD_TEST_TARGETS=OFF
# apptool --configure overrides these macros to /bin/true.
if [ -f build.ninja ]; then
    %{__ninja}
elif [ -f Makefile ]; then
    %{__make} %{?_smp_mflags}
else
    echo "Unsupported CMake build generator" >&2
    exit 1
fi

%install
DESTDIR=%{buildroot} cmake --build . --target install

%files
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
