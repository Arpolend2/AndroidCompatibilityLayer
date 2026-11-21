Name:       ru.androidcompatibilitylayer.appmanager
Summary:    AndroidCompatibilityLayer for Aurora OS
Version:    0.3.1
Release:    1
License:    GPL-3.0-only
URL:        https://github.com/Arpolend2/AndroidCompatibilityLayer
Source0:    %{name}-%{version}.tar.bz2

%global __provides_exclude_from ^%{_datadir}/(%{name}/fixtures|common/ru.androidcompatibilitylayer/appmanager/android-runtime)/.*$
%global __requires_exclude_from ^%{_datadir}/common/ru.androidcompatibilitylayer/appmanager/android-runtime/.*$

Requires:       sailfishsilica-qt5 >= 0.10.9
Requires:       qt5-plugin-imageformat-jpeg
Requires:       nemo-qml-plugin-notifications-qt5
Requires:       ru.auroraos.webview
BuildRequires:  cmake
BuildRequires:  qt5-qttools-linguist
BuildRequires:  python3
BuildRequires:  pkgconfig(auroraapp)
BuildRequires:  pkgconfig(aurorawebview)
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
%cmake -DANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS=OFF \
    -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_NATIVE_EGL=ON \
    -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_COMPOSITION=ON \
    -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_CANVAS=ON
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
%{_datadir}/common/ru.androidcompatibilitylayer/appmanager
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
