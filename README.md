# olcPGEX_Media
Video player for the Pixel Game Engine (http://onelonecoder.com/)

Already functional, but work in progress (requires API polishing, documentation, as well as harmful and "exotic" video format handling).

TODO:
- Figure out if required files can be uploaded due license (such as static/dynamic libraries, fffmpeg headers so the user won't have to compile ffmpeg themselves).
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
