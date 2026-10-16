# Third-party licenses

Binary builds of r3dss bundle or compile in the components below. This project's own code is
under the Apache License 2.0 (`LICENSE` next to this folder); it grants no rights to these
components, which keep their own licenses.

| Component | Use | License | Text |
|---|---|---|---|
| Qt 6.8 (Core, Gui, Widgets and their plugins) | GUI runtime DLLs next to `r3dss-gui.exe` | LGPL-3.0-only | `LGPL-3.0-only.txt`, which supplements `GPL-3.0-only.txt` |
| nlohmann/json | compiled into the executables | MIT | `nlohmann-json.txt` (Windows builds) |
| CLI11 | compiled into `r3dss-cli` | BSD-3-Clause | `cli11.txt` (Windows builds) |
| Microsoft Visual C++ runtime (vcruntime140*.dll, msvcp140*.dll, concrt140.dll) | Windows builds, next to the executables | Visual Studio redistributable ("Distributable Code") terms | Microsoft's license for the Visual Studio edition used to build |
| RED R3D SDK runtime | only in builds made with the SDK, under `bin/redsdk` | RED's SDK license agreement | not redistributable under this project's license; see `NOTICE` |

Qt is used unmodified and is dynamically linked, so the Qt DLLs can be replaced with other
builds of the same Qt version. Qt's source code is available from
https://download.qt.io/official_releases/qt/6.8/ and https://code.qt.io. Qt itself includes
third-party code under further licenses, listed at https://doc.qt.io/qt-6/licenses-used-in-qt.html.

The LGPL and GPL texts here are the copies Qt ships in its own source tree
(`qtbase/LICENSES`, tag v6.8.3).
