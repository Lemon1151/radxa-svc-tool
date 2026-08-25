# Radxa Control Center

Radxa Control Center is a native ARM64 Windows application for the public
version 1 user-mode interface exposed by `radxaplatform.sys`. It provides a
simple end-user view of performance mode, cooling, temperatures, and live
power data.

## Build

Open `RadxaControlCenter.sln` in Visual Studio 2022 with the ARM64 C++ tools and
Windows 10/11 SDK installed, or run:

```powershell
msbuild RadxaControlCenter.sln /m /p:Configuration=Release /p:Platform=ARM64
```

The output is written to `bin\ARM64\Release\RadxaControlCenter.exe`. The bundled
Radxa logo is converted to a multi-resolution Windows icon during the build and
embedded together with the administrator, Per-Monitor V2 DPI, and Common
Controls v6 manifest.

## Runtime behavior

The application requires a compatible `radxaplatform.sys` and elevates at
startup because the device is restricted to administrators. Device requests
run on a background thread. The interface is available in Simplified Chinese
and English; the initial language follows Windows and a user override is stored
under `HKCU\Software\Radxa\RadxaControlCenter`.
