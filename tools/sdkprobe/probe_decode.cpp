// Probe a clip with the RED R3D SDK and decode frame 0 at half res to a 16-bit PPM.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "R3DSDK.h"
#include "R3DSDKMetadata.h"
using namespace R3DSDK;

static int run(const char* libdir, const char* clipPath, const char* outPpm) {
    InitializeStatus st = InitializeSdk(libdir, OPTION_RED_NONE);
    printf("sdk version: %s (init status %d)\n", GetSdkVersion(), (int)st);
    if (st != ISInitializeOK) return 1;
    int rc = 0;
    {
        Clip clip(clipPath);
        if (clip.Status() != LSClipLoaded) { printf("load failed: %d\n", (int)clip.Status()); FinalizeSdk(); return 2; }
        printf("size %zux%zu frames %zu fps %.3f\n", clip.Width(), clip.Height(), clip.VideoFrameCount(), clip.VideoAudioFramerate());
        printf("timecode f0 %s  abs %s  edge %s\n", clip.Timecode(0), clip.AbsoluteTimecode(0), clip.EdgeTimecode(0));
        const char* keys[] = {RMD_CAMERA_MODEL, RMD_CAMERA_FIRMWARE_VERSION, RMD_REDCODE, RMD_SENSOR_NAME, RMD_FRAMERATE,
                              RMD_RECORD_FRAMERATE, RMD_ISO, RMD_WHITE_BALANCE_KELVIN, RMD_AUDIO_FORMAT,
                              RMD_LENS_NAME, RMD_CAMERA_PIN};
        for (const char* k : keys) printf("  %-28s = %s\n", k, clip.MetadataExists(k) ? clip.MetadataItemAsString(k).c_str() : "<absent>");
        printf("metadata items: %zu, audio channels: %zu, color version min %d default %d\n", clip.MetadataCount(), clip.AudioChannelCount(),
               (int)clip.MinimumColorVersion(), (int)clip.DefaultColorVersion());
        size_t w = clip.Width() / 2, h = clip.Height() / 2, need = w * h * 3 * 2;
        std::vector<unsigned char> buf(need + 16);
        unsigned char* p = buf.data(); while ((uintptr_t)p % 16) ++p;
        VideoDecodeJob job; job.OutputBufferSize = need; job.Mode = DECODE_HALF_RES_GOOD; job.OutputBuffer = p; job.PixelType = PixelType_16Bit_RGB_Interleaved;
        DecodeStatus ds = clip.DecodeVideoFrame(0, job);
        printf("decode frame 0 @ %zux%zu: status %d\n", w, h, (int)ds);
        if (ds != DSDecodeOK) rc = 3;
        else {
            FILE* f = fopen(outPpm, "wb"); fprintf(f, "P6\n%zu %zu\n65535\n", w, h);
            const unsigned short* s = (const unsigned short*)p;  // host-endian; PPM wants big-endian
            std::vector<unsigned char> row(w * 6);
            for (size_t y = 0; y < h; ++y) { for (size_t i = 0; i < w * 3; ++i) { unsigned short v = s[y * w * 3 + i]; row[2*i] = v >> 8; row[2*i+1] = v & 255; } fwrite(row.data(), 1, row.size(), f); }
            fclose(f); printf("wrote %s\n", outPpm);
        }
    }
    FinalizeSdk();
    return rc;
}
int main(int argc, char** argv) { if (argc < 4) { fprintf(stderr, "usage: probe_decode LIBDIR CLIP OUT.ppm\n"); return 1; } return run(argv[1], argv[2], argv[3]); }
