# RED R3D SDK probe

Minimal standalone check that the RED R3D SDK can open and decode a clip. Not part of the
CMake build; the SDK is proprietary and must be downloaded from red.com and kept out of git.

Linux:

    g++ -std=c++17 -O1 -I$R3DSDK_ROOT/Include probe_decode.cpp \
        $R3DSDK_ROOT/Lib/linux64/libR3DSDKPIC-cpp11.a -ldl -lpthread -o probe_decode
    ./probe_decode $R3DSDK_ROOT/Redistributable/linux CLIP.R3D frame0.ppm

Windows (x64 Native Tools prompt):

    cl /std:c++17 /EHsc /MD /I%R3DSDK_ROOT%\Include probe_decode.cpp %R3DSDK_ROOT%\Lib\win64\R3DSDK-2017MD.lib
    probe_decode %R3DSDK_ROOT%\Redistributable\win CLIP.R3D frame0.ppm

Verified 2026-10-14 with SDK 9.2.1 (Linux x64, CPU decode) on the `samples-v1` Komodo clip.
