# olcPGEX_Media
Single header video player extension for the Pixel Game Engine https://github.com/OneLoneCoder/olcPixelGameEngine

Already functional, but work in progress (requires API polishing, documentation, as well as harmful and "exotic" video format handling).

TODO:
- Figure out if required files can be uploaded due license (such as static/dynamic libraries, FFMPEG headers so the user won't have to compile ffmpeg themselves).
- Test out library with other OS and compilers (currently library has only been tested with Windows10 + MSVC).

## Demo
This is envisioned extension usage, aimed to be as minimalistic as possible, with automatic video frame and audio synchronisation.
```cpp
#define OLC_PGE_APPLICATION
#include "olcPixelGameEngine.h"

#include "olcPGEX_Media.h"

class VideoPlayer : public olc::PixelGameEngine
{
    olc::Media media;

    bool OnUserCreate() override
    {
        media.Open("video.mp4");

        return true;
    }

    bool OnUserUpdate(float delta_time) override
    {
        olc::Decal* frame = media.GetVideoFrame(delta_time);
        if (frame != nullptr) {
            DrawDecal({ 0, 0 }, frame);
        }

        return true;
    }
};

int main()
{
    VideoPlayer demo;
    if (demo.Construct(1280, 720, 1, 1))
        demo.Start();
}
```

## Build setup info
You will need to download the following source code of libraries:
- [FFMPEG](https://github.com/FFmpeg/FFmpeg)
- [MiniAudio](https://github.com/mackron/miniaudio)

## Setting up FFMPEG
General setup instructions can be found here:
<https://ffmpeg.org//doxygen/4.3/md_INSTALL.html>

### MSVC
These are my notes from last FFMPEG setup on MSVC.
Information was gathered from here: https://trac.ffmpeg.org/wiki/CompilationGuide/MSVC
Step by step instruction to build ffmpeg on MSVC
- Open command prompt in MSVC (Tools -> Command Line -> Developer Command prompt)
- Navigate to folder you want to build
- Type `"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64`
- Then open msys2 mingw from `"C:\msys64\mingw64.exe"`
- Navigate to folder you want to build once again
- Expose MSVC compiler and linker with `export PATH="/c/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/MSVC/14.29.30133/bin/Hostx64/x64/":$PATH`
- Run configure script `./configure (some options)` (see options below)
- Run `make` (compile code)
- Run `make install` (create `.lib` and `.dll` if `--enable-shared` was passed at configure script)

When running `./configure`, run one of the snippets below (first one adds extra options that does not produce ffmpeg executables and documentation)
```
./configure --enable-asm --enable-yasm --disable-avdevice --disable-doc --disable-ffplay --disable-ffprobe --disable-ffmpeg --enable-shared --disable-static --disable-bzlib --disable-libopenjpeg --disable-iconv --disable-zlib --prefix=/usr/ffmpeg --toolchain=msvc --arch=amd64 --extra-cflags=-MDd --extra-ldflags='/NODEFAULTLIB:libcmt' --enable-debug
```
```
./configure --enable-asm --enable-yasm --enable-shared --disable-static --prefix=/usr/ffmpeg --toolchain=msvc --arch=amd64 --extra-cflags=-MDd --extra-ldflags='/NODEFAULTLIB:libcmt' --enable-debug
```

## Setting up MiniAudio
MiniAudio is single header library. So all you need to do, is to put `miniaudio.h` in same folder as `olcPGEX_Media.h`.

### Extra info
Great video on in-depth FFMPEG setup https://www.youtube.com/watch?v=MEMzo59CPr8
