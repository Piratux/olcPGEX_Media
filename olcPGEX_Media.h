// olcPGEX_Media Version 1.0.0

// IMPORTANT:
// - This extension was made to work with PGE version 2.15 (it can also be used without PGE with little modifications, if needed)
// - To use it with previous versions, you may need to do some API changes
// - Only one media file can be played per single Media instance

// Define OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK to not use default miniaud.io playback and play the audio yourself

// TODO:
// - Check if video/audio is opened before every function related to video/audio (?)
// - Test custom audio playback feature
// - Tidy up print info functions
// - Check if multiple Media instances can co-exist
// - Figure out how to handle memory leak upon decoding thread error
// - Make audio files display images if exists
// - Add attached pic notice as explanation for mp3

#ifndef OLCPGEX_MEDIA_H
#define OLCPGEX_MEDIA_H

extern "C" {
// Video and audio dependencies
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

// Video dependencies
#include <libswscale/swscale.h>
#include <inttypes.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>

// Audio dependencies
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

#ifndef OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK
// FFMPEG will be the one that handles decoding so this makes result executable a little bit smaller according to docs
#define MA_NO_DECODING
#define MA_NO_ENCODING

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#endif // OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK


#include <iostream>
#include <string>
#include <cstdint>
#include <locale>
// Although this header is considered deprecated since C++17, according to https://stackoverflow.com/a/18597384 it's still considered "safe and portable"
#include <codecvt>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <algorithm>


#ifdef _WIN32
// Use windows specific API to handle IO
#include <Windows.h>
#else 
// Use C IO for other platforms
#include <cstdio>
#endif // _WIN32


// This definition usually gets set automatically by the IDEs
#ifdef NDEBUG
// Release macro
#define OLC_MEDIA_ASSERT(condition, message) if(!(condition)){return Result::Error;}
#else
// Debug macro
#define OLC_MEDIA_ASSERT(condition, message) if(!(condition)){std::cerr << "[OLC_MEDIA]: " << (message) << '\n'; return Result::Error;}
#endif

// Declarations
namespace olc {
	class Media {
    public:
        enum class Result {
            Success = 0,
            Error = 1,
        };

        // Formats that are supported by both - ffmpeg and miniaud.io libraries.
        // Miniaud.io doesn't support planar (non-interleaved) audio formats, so all formats will be stored as non planar
        // NOTE: Beware, that when using much lower quality audio format than the original, output audio may contain noise and glitches(short, high pitch sounds)
        enum class AudioFormat {
            Default,   // The audio will be the same as original, unless it's none of the below supported formats. Then it will default to F32.
            U8,        // Unsigned 8 bits
            S16,       // Signed 16 bits
            S32,       // Signed 32 bits
            F32        // Float
        };

        // All settings must have default value
        struct Settings {
            // Audio and video buffer scaler. Only suggested to increase it, if you notice some video/audio 
            // frames are skipped, which could happen if video/audio frames aren't interleaved as often.
            uint8_t preloaded_frames_scale = 1;

            // Output format of the audio.
            AudioFormat audio_format = AudioFormat::Default;
        };

    private:
        // Thread safe "queue" that uses circular buffer
        class VideoQueue {
        private:
            uint16_t _size = 0;
            uint16_t _capacity = 0;

            uint16_t _insert_idx = 0; // idx that points to location where new element will be inserted
            uint16_t _delete_idx = 0; // idx that points to location where oldest element will be deleted from

            AVFrame** _data = nullptr;

            mutable std::mutex _mut;

        public:
            VideoQueue() {
            }

            ~VideoQueue() {
                clear();
                free();
            }

            // Suggested to set capacity to fps (but min capacity must be 2)
            Result init(uint16_t capacity) {
                // Always reset the values
                _size = 0;
                _insert_idx = 0;
                _delete_idx = 0;

                // If video fifo was already in use, reset it first
                if (_data != nullptr) {
                    clear();
                    free();
                }

                if (capacity <= 1)
                    return Result::Error;

                _data = new AVFrame * [capacity];
                if (_data == nullptr)
                    return Result::Error;

                _capacity = capacity;
                for (uint16_t i = 0; i < _capacity; ++i) {
                    _data[i] = av_frame_alloc();

                    // Memory might run out if capacity is too big (which might happen if video fps is insanely large)
                    if (_data[i] == nullptr)
                        return Result::Error;
                }

                return Result::Success;
            }

            AVFrame* back() const {
                std::unique_lock<std::mutex> lock(_mut);
                return _data[_insert_idx];
            }

            AVFrame* front() const {
                std::unique_lock<std::mutex> lock(_mut);
                return _data[_delete_idx];
            }

            // Push updated AVFrame from "back()"
            void push() {
                std::unique_lock<std::mutex> lock(_mut);

                _insert_idx = (_insert_idx + 1) % _capacity;
                ++_size;
            }

            // Pop AVFrame from the front() unreferencing it
            // If size is 0, does nothing
            void pop() {
                std::unique_lock<std::mutex> lock(_mut);

                if (_size > 0) {
                    av_frame_unref(_data[_delete_idx]);

                    _delete_idx = (_delete_idx + 1) % _capacity;
                    --_size;
                }
            }

            size_t size() const {
                std::unique_lock<std::mutex> lock(_mut);
                return _size;
            }

            size_t capacity() const {
                std::unique_lock<std::mutex> lock(_mut);
                return _capacity;
            }

            void clear() {
                if (_data != nullptr) {
                    for (uint16_t i = 0; i < _capacity; ++i) {
                        av_frame_unref(_data[i]);
                    }
                    _size = 0;
                    _insert_idx = 0;
                    _delete_idx = 0;
                }
            }

            // De-allocates fifo structure
            void free() {
                if (_data != nullptr) {
                    for (uint16_t i = 0; i < _capacity; ++i) {
                        av_frame_free(&_data[i]);
                    }

                    delete[] _data;
                    _data = nullptr;

                    _size = 0;
                    _insert_idx = 0;
                    _delete_idx = 0;
                }
            }
        };

        // Thread safe wrapper for audio fifo read/write
        class AudioQueue {
        private:
            AVAudioFifo* _fifo = nullptr;
            mutable std::mutex _mut;

        public:
            AudioQueue() {
            }

            ~AudioQueue() {
                clear();
                free();
            }

            // Suggested to set capacity to sample rate
            Result init(AVSampleFormat format, int channels, int capacity) {
                // If these parameters are 0, there is something wrong
                if (capacity == 0 || channels == 0)
                    return Result::Error;

                if (_fifo != nullptr) {
                    clear();
                    free();
                }

                _fifo = av_audio_fifo_alloc(format, channels, capacity);

                if (_fifo == nullptr)
                    return Result::Error;

                return Result::Success;
            }

            int push(void** data, int samples) {
                std::lock_guard<std::mutex> lock(_mut);

                int space = av_audio_fifo_space(_fifo);

                // If capacity is reached, drain some audio samples
                if (samples > space) {
                    av_audio_fifo_drain(_fifo, std::min(av_audio_fifo_size(_fifo), samples - space));
                }

                return av_audio_fifo_write(_fifo, data, samples);
            }

            int pop(void** data, int samples) {
                std::unique_lock<std::mutex> lock(_mut);
                return av_audio_fifo_read(_fifo, data, samples);
            }

            void drain(int samples) {
                std::unique_lock<std::mutex> lock(_mut);
                av_audio_fifo_drain(_fifo, samples);
            }

            int size() {
                std::unique_lock<std::mutex> lock(_mut);
                return av_audio_fifo_size(_fifo);
            }

            int capacity() {
                std::unique_lock<std::mutex> lock(_mut);
                return av_audio_fifo_space(_fifo) + av_audio_fifo_size(_fifo);
            }

            // Empties out all the frames
            void clear() {
                if (_fifo != nullptr)
                    av_audio_fifo_drain(_fifo, av_audio_fifo_size(_fifo));
            }

            // De-allocates fifo structure
            void free() {
                av_audio_fifo_free(_fifo);
                _fifo = nullptr;
            }
        };

        // Platform specific IO setup for FFMPEG
        // Big thanks to Desp4
#ifdef _WIN32
        typedef DWORD FilePointer;
        typedef HANDLE FileHandle;

    public:
        class FileName
        {
        public:
            FileName(const std::wstring& name)
            {
                filename = name;
            }

            FileName(const std::string& name)
            {
                std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
                filename = converter.from_bytes(name);
            }

            operator const wchar_t* () const
            {
                return filename.c_str();
            }

        private:
            std::wstring filename;
        };
    private:
#else
        typedef uint64_t FilePointer;
        typedef FILE* FileHandle;
    public:
        typedef const char* FileName;
    private:
#endif // _WIN32

        class IOContext {
        private:
#ifdef _WIN32
            static FileHandle openFile(const FileName& path)
            {
                HANDLE ret = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                return ret != INVALID_HANDLE_VALUE ? ret : nullptr;
            }

            static bool closeFile(FileHandle file)
            {
                return !CloseHandle(file);
            }

            static bool seekFile(FileHandle file, int64_t offset, int origin)
            {
                // Origin macros match those of fseek
                return SetFilePointer(file, (LONG)offset, NULL, origin) == INVALID_SET_FILE_POINTER;
            }

            static FilePointer readFile(FileHandle file, void* buffer, FilePointer size)
            {
                DWORD len;
                return ReadFile(file, buffer, size, &len, NULL) != 0 ? len : 0;
            }

            static FilePointer filePos(FileHandle file)
            {
                return SetFilePointer(file, 0, NULL, FILE_CURRENT);
            }

#else

            static FileHandle openFile(const FileName& path)
            {
                return fopen(path, "rb");
            }

            static bool closeFile(FileHandle file)
            {
                return fclose(file);
            }

            static bool seekFile(FileHandle file, int64_t offset, int origin)
            {
                return fseek(file, offset, origin);
            }

            static FilePointer readFile(FileHandle file, void* buffer, FilePointer size)
            {
                return fread(buffer, 1, size, file);
            }

            static FilePointer filePos(FileHandle file)
            {
                return ftell(file);
            }

#endif // _WIN32

        public:
            // Recommended buffer size for i/o contexts by ffmpeg docs
            IOContext(uint64_t buffersize = 4096) :
                bufferSize(buffersize),
                buffer(static_cast<uint8_t*>(av_malloc(buffersize)))
            {
            }

            ~IOContext()
            {
                closeIO();
                if (buffer)
                    av_freep(&buffer);
            }

            // Returns true on success
            bool initAVFmtCtx(const FileName& filename, AVFormatContext* fmtCtx)
            {
                closeIO();
                file = openFile(filename);

                if (!file)
                    return false;

                ioCtx = avio_alloc_context(
                    buffer,         // Internal buffer
                    (int)bufferSize,     // Buffer size
                    0,              // Write flag(1=true, 0=false) 
                    this,           // User data, will be passed to the callback functions
                    ioread,         // Read callback
                    nullptr,        // No write callback
                    ioseek);        // Seek callback


                fmtCtx->pb = ioCtx;
                fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

                // Read the file and let ffmpeg guess it.
                FilePointer len = readFile(file, buffer, (FilePointer)bufferSize);
                if (!len)
                    return false;

                // Seek to the beginning.
                seekFile(file, 0, SEEK_SET);

                // Set up a probe.
                AVProbeData probeData;
                probeData.buf = buffer;
                probeData.buf_size = len;
                probeData.filename = "";
                probeData.mime_type = NULL;

                fmtCtx->iformat = av_probe_input_format(&probeData, 1);

                return true;
            }

            void closeIO()
            {
                if (file)
                {
                    closeFile(file);
                    file = nullptr;
                }
                if (ioCtx)
                    avio_context_free(&ioCtx);
            }

        protected:
            static int ioread(void* data, uint8_t* buf, int bufSize)
            {
                IOContext* thisio = reinterpret_cast<IOContext*>(data);
                FilePointer len = readFile(thisio->file, buf, bufSize);
                if (!len)
                    return AVERROR_EOF; // Probably reached EOF, let ffmpeg know.
                return len;
            }

            // Whence: SEEK_SET, SEEK_CUR, SEEK_END and AVSEEK_SIZE
            static int64_t ioseek(void* data, int64_t pos, int whence)
            {
                if (whence == AVSEEK_SIZE)
                    return -1; // Don't support - return a negative.

                IOContext* thisio = reinterpret_cast<IOContext*>(data);
                if (seekFile(thisio->file, pos, whence))
                    return -1;

                return filePos(thisio->file);
            }

        private:
            uint8_t* buffer;
            uint64_t bufferSize;
            AVIOContext* ioCtx = nullptr;
            FileHandle file = nullptr;
        };

    private:
        // -- Private internal state --
        AVFormatContext* av_format_ctx = nullptr;
        IOContext ioCtx;

        Settings settings;

        bool is_paused;

        // If false, video capture wasn't opened, or last frame was put into queue
        std::atomic<bool> finished_reading = false;

        // When true, the frame loading thread keeps on working
        // When set to false, and conditional is called, frame loading thread is halted
        std::atomic<bool> keep_loading = true;
        
        std::thread frame_loader;
        std::mutex mutex;
        std::condition_variable conditional;

        
        // -- Video stuff --
        int video_stream_index = -1;
        AVRational video_time_base;
        VideoQueue video_fifo;
        const AVCodec* av_video_codec = nullptr;
        AVCodecContext* av_video_codec_ctx = nullptr;
        SwsContext* sws_video_scaler_ctx = nullptr;
        AVFrame* temp_video_frame = nullptr; // Used to temporary store converted video frame
        olc::Renderable video_frame;
        int video_width = 0;
        int video_height = 0;
        int video_delay = 0;
        float delta_time_accumulator = 0.0f;
        double last_video_pts = 0.0;
        bool video_opened = false;
        bool attached_pic = false; // True if video stream is a single attached picture (for example album art in mp3 metadata)

        // -- Audio stuff --
        int audio_stream_index = -1;
        AVRational audio_time_base;
        AudioQueue audio_fifo;
        const AVCodec* av_audio_codec = nullptr;
        AVCodecContext* av_audio_codec_ctx = nullptr;
        SwrContext* swr_audio_resampler = nullptr;
        size_t audio_frames_consumed = 0;
        std::atomic<double> audio_time = 0.0;
        AVSampleFormat audio_format;
        int audio_sample_size = 0;
        int audio_sample_rate = 0;
        int audio_channel_count = 0;
        // Default volume is 1 (max) for miniaud.io, so it's better to have video playing quieter than louder
        float audio_volume = 0.5f;
        bool audio_opened = false;

#ifndef OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK
        ma_device audio_device;
#endif //OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK

	public:
        Media();
        ~Media();

        // -- Video and audio functions --

        // If media is already open, closes it first.
        // - preloaded_frames_scale: Affects how many seconds of pre-decoded video/audio frames should be stored. (only suggested to 
        //   increase it, when you notice that some of your video/audio frames are skipped).
        // - settings: Playback settings. Pass nullptr to use default settings.
        // NOTE: You shouldn't use this class to load static image formats as they most likely won't work (such as ".jpg" or ".png")
        Result Open(const std::string& filename, bool open_video, bool open_audio, Settings* settings);

#ifdef _WIN32
        // Windows exclusive function, because windows allows filenames to have unicode characters.
        Result Open(const std::wstring& filename, bool open_video, bool open_audio, Settings* settings);
#endif // _WIN32

        // If media is currently open, closes it and frees up all the resources.
        void Close();

        // Returns true if any of the following is true:
        // - Media file wasn't opened;
        // - No more frames are available in the media file;
        // - Error occured when trying to receive a packet.
        bool FinishedReading();

        // If media was playing, pauses it.
        // Does nothing if media isn't open.
        void Pause();

        // If media was paused, continues playing it.
        // Does nothing if media isn't open.
        void Play();

        // Returns true if media is currently paused.
        bool IsPaused();

        // Seeks the media file to specified timepoint.
        // - new_time: wanted timestamp in seconds
        Result Seek(double new_time);

        // Returns current position in media that is being played.
        // If media isn't open, returns 0.0
        double GetCurrentPlaybackTime();


        // -- Video functions --
        
        // Returns next video frame, unless media is paused.
        // When only video is playing, delta_time is used to synchronise the video.
        // If video is played together with audio, delta_time is ignored, and video is synchronised based on
        // how many audio frames were consumed.
        // If media was paused before a single video frame could be decoded, the returned frame might be empty.
        // 
        // NOTE: Returned decal's pixel data might change when you call one of "GetVideoFrame" functions again.
        olc::Decal* GetVideoFrame(float delta_time);

        // Returns next video frame even when media is paused.
        // 
        // NOTE: Returned decal's pixel data might change when you call one of "GetVideoFrame" functions again.
        // NOTE: Only use this function, if you want to implement video synchronisation yourself.
        olc::Decal* GetVideoFrame();

        // Pops video frame from the internal queue.
        // If one of the following is true, function does nothing and returns Error enum:
        // - Video wasn't opened;
        // - No more frames are avialable in the video;
        // - Decoder didn't decode a frame yet.
        // 
        // NOTE: Only use this function, if you want to implement video synchronisation yourself.
        Result SkipVideoFrame();

        // Returns true if video was sucessfully opened with "Open()" function and "Close()" wasn't called.
        bool IsVideoOpened();

        // Not all videos have frames of equal length, so FPS can only be average.
        double GetAverageVideoFPS();

        // Prints video info to console.
        // 
        // NOTE: The printed information can change between versions.
        void PrintVideoInfo();

        
		//bool seek_frame(int64_t ts);
		

        // -- Audio functions --
        // Returns amount of samples that were read (if not all samples were written, the rest will be filled with 0s (silence))
        // or -1 if an error occured due to bad parameters or other reasons.
        // output: pointer to byte array pointed by "void*", where the byte array size must be "channel_count * sample_count * sample_size".
        // entire outptu buffer will be filled with silence by default.
        // 
        // NOTE: Only use this function if you intend to play the audio yourself. If you do decide to handle audio yourself,
        // note, that when video and audio is played together, video is synchronised according to how many audio samples have been read.
        int GetAudioFrame(void** output, int sample_count);

        // Returns true if audio was sucessfully opened with "Open()" function and "Close()" wasn't called.
        bool IsAudioOpened();

        // Prints audio info to console.
        // 
        // NOTE: The printed information can change between versions.
        void PrintAudioInfo();

        // If audio isn't opened, does nothing.
        // new_volume: value between 0 (silence) and 1 (full volume). The value is clamped if it exceeds bounds.
        //
        // NOTE: This doesn't affect audio output frame volume when using your own audio backend.
        void SetAudioVolume(float new_volume);

        // Returns value between 0 (silence) and 1 (full volume). The value is clamped if it exceeds bounds.
        // If audio isn't open, returns 0.
        //
        // NOTE: This doesn't affect audio output frame volume when using your own audio backend.
        float GetAudioVolume();

        // Returns audio format that will be converted (if needed) and stored when reading audio data, that 
        // you can expect to get from "GetAudioFrame()" function.
        // If audio isn't open, returns AV_SAMPLE_FMT_NONE.
        AVSampleFormat GetAudioOutputFormat();

        // Returns original audio format that was stored in the media.
        // If audio isn't open, returns AV_SAMPLE_FMT_NONE.
        AVSampleFormat GetAudioOriginalFormat();

        // Returns 
        int GetAudioSampleSize();

        int GetAudioSampleRate();

        int GetAudioChannelCount();

        // Returns true if audio format has attached album art (for example album art in .mp3 metadata section)
        bool HasAlbumArt();


    private:
        // -- Video and audio functions --
        Result Open(const FileName& filename, bool open_video, bool open_audio, Settings* settings);
        Result OpenFile(const FileName& filename);
        void CloseFile();
        void StartDecodingThread();
        void StopDecodingThread();
        Result DecodingThread();
        // Perform position adjustion after seeking, by consuming the frames up to specified timepoint.
        // This function works similarly to "DecodingThread()"
        Result AdjustSeekedPosition(double wanted_timepoint);
        static const char* GetError(int errnum);
        // Returns success, if all settings are valid
        Result ApplySettings();
        

        // -- Video functions --
        Result InitVideo();
        void CloseVideo();
        void ConvertFrameToRGBASprite(AVFrame* frame, olc::Sprite* target);
        // Send updated pixel data in olc::Sprite to GPU
        void UpdateResultSprite();
        // Calculates video pts in seconds
        double CalculateVideoPts(const AVFrame* frame);
        Result HandleVideoDelay();
        const AVFrame* PeekFrame();
        static AVPixelFormat CorrectDeprecatedPixelFormat(AVPixelFormat pix_fmt);

        // -- Audio functions --
        Result InitAudio();
        void CloseAudio();
        // Calculates audio pts in seconds
        double CalculateAudioPts(const AVFrame* frame);
        Result ChooseAudioFormat();
        Result InitialiseAndStartMiniaudio();
	};
}

// Definitions
namespace olc {
    Media::Media() {
    }

    Media::~Media() {
        Close();
    }

    Media::Result Media::Open(const std::string& filename, bool open_video, bool open_audio, Settings* settings) {
        return Open(FileName{ filename.c_str() }, open_video, open_audio, settings);
    }

#ifdef _WIN32
    Media::Result Media::Open(const std::wstring& filename, bool open_video, bool open_audio, Settings* settings) {
        return Open(FileName{ filename.c_str() }, open_video, open_audio, settings);
    }
#endif // _WIN32

    void Media::Close() {
        StopDecodingThread();
        CloseFile();
        CloseVideo();
        CloseAudio();
    }

    bool Media::FinishedReading() {
        // If neither of the streams were open, return false
        if (IsVideoOpened() == false && IsAudioOpened() == false)
            return false;

        // Check if decoding thread has finished and all the frames were read from the streams that were open
        if (finished_reading) {
            bool video_finished = true;
            bool audio_finished = true;

            if (IsVideoOpened() && video_fifo.size() > 0) {
                video_finished = false;
            }

            if (IsAudioOpened() && audio_fifo.size() > 0) {
                audio_finished = false;
            }

            return video_finished && audio_finished;
        }

        return false;
    }

    void Media::Pause() {
        if ((IsAudioOpened() || IsVideoOpened()) == false)
            return;

        if (IsPaused())
            return;

        if (IsAudioOpened())
            ma_device_stop(&audio_device);

        is_paused = true;
    }
    
    void Media::Play() {
        if ((IsAudioOpened() || IsVideoOpened()) == false)
            return;

        if (IsPaused() == false)
            return;

        if (IsAudioOpened())
            ma_device_start(&audio_device);

        is_paused = false;
    }

    bool Media::IsPaused() {
        return is_paused;
    }

    Media::Result Media::Seek(double new_time) {
        PiraTimer::start("BigSeek");

        if ((IsVideoOpened() || IsAudioOpened()) == false)
            return Result::Error;

        Pause();
        StopDecodingThread();

        Result result = Result::Success;
        int response = av_seek_frame(av_format_ctx, -1, int64_t(AV_TIME_BASE * new_time), AVSEEK_FLAG_BACKWARD);
        //int response = av_seek_frame(av_format_ctx, -1, int64_t(AV_TIME_BASE * new_time), AVSEEK_FLAG_ANY);

        // If seeking was successful
        if (response >= 0) {
            if (IsVideoOpened()) {
                video_fifo.clear();
                avcodec_flush_buffers(av_video_codec_ctx);
            }

            if (IsAudioOpened()) {
                audio_fifo.clear();
                avcodec_flush_buffers(av_audio_codec_ctx);
            }

            PiraTimer::start("Seek");
            // "av_seek_frame" won't actually make next received frames to be what we want, instead, it will 
            // seek back to nearest keyframe from the given timepoint.
            // So we have to consume the frames up to the timepoint we want.
            result = AdjustSeekedPosition(new_time);
            PiraTimer::end("Seek");
        }
        else {
            result = Result::Error;
        }

        // Continue decoding and playing even if seek wasn't successful
        StartDecodingThread();
        Play();

        PiraTimer::end("BigSeek");

        return result;
    }

    double Media::GetCurrentPlaybackTime() {
        double time = 0.0;

        if (IsVideoOpened()) {
            time = last_video_pts;
        }
        else if (IsAudioOpened()) {
            time = audio_time;
        }

        return time;
    }

    olc::Decal* Media::GetVideoFrame(float delta_time) {
        //printf("video fifo size %llu\n", video_fifo.size());

        if (IsVideoOpened() == false) {
            printf("Video isn't open\n");
            return nullptr;
        }

        if (FinishedReading()) {
            //printf("Finished reading video\n");
            return GetVideoFrame();
        }

        // This returned video frame might be empty
        if (IsPaused()) {
            return video_frame.Decal();
        }

        if (HasAlbumArt()) {
            return GetVideoFrame();
        }

        double time_reference;

        // If audio is opened, synchronise video with audio
        if (IsAudioOpened()) {
            time_reference = audio_time;
        }
        // Otherwise synchronise it based on how much time has passed between function calls (or allow user to mess with delta time if he wants)
        else {
            delta_time_accumulator += delta_time;
            time_reference = delta_time_accumulator;
        }

        //printf("tr: %lf\n", time_reference);

        //printf("tr: %lf\n", time_reference);
        //printf("lvpts: %lf\n", last_video_pts);

        // If enough time hasn't passed yet, return the same frame
        if (time_reference < last_video_pts)
            return video_frame.Decal();

        while (true) {
            const AVFrame* next_frame = PeekFrame();

            // Check if Decoding thread has a next video frame at all
            if (next_frame == nullptr)
                return video_frame.Decal();

            last_video_pts = CalculateVideoPts(next_frame);

            // Test Decoding thread later by changing "<=" to ">="
            if (time_reference <= last_video_pts)
                break;

            SkipVideoFrame();
        }

        return GetVideoFrame();
    }

    olc::Decal* Media::GetVideoFrame() {
        if (IsVideoOpened() == false) {
            printf("Video isn't open\n");
            //return nullptr;
            return video_frame.Decal();
        }

        if (FinishedReading()) {
            //printf("Finished reading video\n");
            return video_frame.Decal();
        }

        // If decoding thread wasn't quick enough to decode frames return same image.
        // (We don't know if decoding thread isn't quick enough, or if last video frame 
        // was decoded, and there are other frames left over, like audio frames)
        if (video_fifo.size() > 0) {
            //printf("v-\n");

            AVFrame* frame_ref = video_fifo.front();

            ConvertFrameToRGBASprite(frame_ref, video_frame.Sprite());
            //UpdateResultSprite();

            //printf("vt: %lf\n", double(frame_ref->best_effort_timestamp * video_time_base.num) / double(video_time_base.den));

            video_fifo.pop();

            conditional.notify_one();
        }

        return video_frame.Decal();
    }

    // TODO: return error when no more frames are available and there is nothing to skip
    Media::Result Media::SkipVideoFrame() {
        if (IsVideoOpened() == false) {
            printf("Video isn't open\n");
            return Result::Error;
        }

        if (FinishedReading()) {
            printf("Finished reading video\n");
            return Result::Error;
        }

        // If decoding thread wasn't quick enough to decode frames don't do anything.
        // (We don't know if decoding thread isn't quick enough, or if last video frame 
        // was decoded, and there are other frames left over, like audio frames)
        if (video_fifo.size() > 0) {
            video_fifo.pop();

            conditional.notify_one();

            return Result::Success;
        }

        return Result::Error;
    }

    bool Media::IsVideoOpened() { 
        return video_opened; 
    }

    double Media::GetAverageVideoFPS() {
        return av_q2d(av_format_ctx->streams[video_stream_index]->avg_frame_rate); 
    }

    void Media::PrintVideoInfo() {
        if (IsVideoOpened() == false) {
            printf("Video isn't open\n");
            return;
        }

        AVStream* video_stream = av_format_ctx->streams[video_stream_index];
        double frame_rate = av_q2d(video_stream->avg_frame_rate);
        int time_base_num = video_stream->time_base.num;
        int time_base_den = video_stream->time_base.den;
        int frame_rate_num = video_stream->avg_frame_rate.num;
        int frame_rate_den = video_stream->avg_frame_rate.den;

        int64_t duration_origin = av_format_ctx->duration;
        int64_t duration = duration_origin / AV_TIME_BASE;
        int64_t duration_h = duration / 3600;
        int64_t duration_min = (duration % 3600) / 60;
        int64_t duration_sec = duration % 60;

        // Print time base and fps
        printf("----------------------\n");
        printf("Video info\n");
        printf("Codec: %s\n", av_video_codec->long_name);
        printf("Pixel fmt: %s\n", av_get_pix_fmt_name(av_video_codec_ctx->pix_fmt));
        printf("Width: %i   Height: %i\n", video_width, video_height);
        printf("Duration_origin: %lli\n", duration_origin);
        printf("Duration: %lli:%lli:%lli h:min:sec\n", duration_h, duration_min, duration_sec);
        printf("Frame rate: %lf\n", frame_rate);
        printf("Time base num: %i\n", time_base_num);
        printf("Time base den: %i\n", time_base_den);
        printf("Frame rate num: %i\n", frame_rate_num);
        printf("Frame rate den: %i\n", frame_rate_den);
        printf("Video delay: %i\n", video_delay);
        printf("----------------------\n");
    }

    int Media::GetAudioFrame(void** output, int sample_count) {
        // Do some error checking
        if (!output || !(*output) || sample_count < 0)
            return -1;

        // Fill buffer with silence, in case less samples are stored than requested
        memset(*output, 0, audio_channel_count * audio_sample_size * sample_count);

        //printf("as: %i\n", audio_sample_size);

        int samples_read = audio_fifo.pop(output, sample_count);
        conditional.notify_one();

        // "pop()" can return negative error code so we will convert it to -1
        if (samples_read < 0)
            return -1;
        
        audio_frames_consumed += samples_read;

        // I'm only storing raw audio data, so this is the only way I can calculate audio time stamp without relying on "pts" in the frame
        audio_time = double(audio_frames_consumed) / double(audio_sample_rate);

        //double a_time = audio_time;
        //printf("at: %lf\n", a_time);

        //printf("sr: %i\n", samples_read);

        //printf("tot: %llu\n", audio_frames_consumed);
        //printf("-----------\n");

        // TEMP
        //memset(*output, 0, audio_channel_count * audio_sample_size * sample_count);

        return samples_read;
    }

    bool Media::IsAudioOpened() {
        return audio_opened; 
    }

    void Media::PrintAudioInfo() {
        if (IsAudioOpened() == false) {
            printf("Audio isn't open\n");
            return;
        }
        //av_dump_format(av_format_ctx, )
        //printf("stream count: %u\n", av_format_ctx->nb_streams);

        AVStream* audio_stream = av_format_ctx->streams[audio_stream_index];
        int frame_size = av_format_ctx->streams[audio_stream_index]->codecpar->frame_size;
        int sample_rate = av_format_ctx->streams[audio_stream_index]->codecpar->sample_rate;
        int channels = av_format_ctx->streams[audio_stream_index]->codecpar->channels;
        int time_base_num = audio_stream->time_base.num;
        int time_base_den = audio_stream->time_base.den;
        int pkt_time_base_num = av_audio_codec_ctx->pkt_timebase.num;
        int pkt_time_base_den = av_audio_codec_ctx->pkt_timebase.den;
        int ctx_sample_rate = av_audio_codec_ctx->sample_rate;

        int64_t duration_origin = av_format_ctx->duration;
        int64_t duration = duration_origin / AV_TIME_BASE;
        int64_t duration_h = duration / 3600;
        int64_t duration_min = (duration % 3600) / 60;
        int64_t duration_sec = duration % 60;

        // Print time base and fps
        printf("----------------------\n");
        printf("Audio info\n");

        printf("Codec: %s\n", av_audio_codec->long_name);
        printf("Frame size: %i\n", frame_size);
        printf("Original format type: %s\n", av_get_sample_fmt_name(GetAudioOriginalFormat()));
        printf("Output format type: %s\n", av_get_sample_fmt_name(GetAudioOutputFormat()));
        printf("Duration_origin: %lli\n", duration_origin);
        printf("Duration: %lli:%lli:%lli h:min:sec\n", duration_h, duration_min, duration_sec);
        printf("Sample rate: %i\n", sample_rate);
        printf("Channels: %i\n", channels);
        printf("Time base num: %i\n", time_base_num);
        printf("Time base den: %i\n", time_base_den);
        printf("Packet time base num: %i\n", pkt_time_base_num);
        printf("Packet time base den: %i\n", pkt_time_base_den);
        printf("Ctx sample rate: %i\n", ctx_sample_rate);
        printf("block_align: %i\n", av_format_ctx->streams[audio_stream_index]->codecpar->block_align);
        printf("initial_padding: %i\n", av_format_ctx->streams[audio_stream_index]->codecpar->initial_padding);
        printf("trailing_padding: %i\n", av_format_ctx->streams[audio_stream_index]->codecpar->trailing_padding);
        printf("seek_preroll: %i\n", av_format_ctx->streams[audio_stream_index]->codecpar->seek_preroll);
        printf("----------------------\n");
    }

    void Media::SetAudioVolume(float new_volume) {
        if (IsAudioOpened() == false)
            return;

        // Clamp volume
        if (new_volume < 0.0f)
            new_volume = 0.0f;
        else if (new_volume > 1.0f)
            new_volume = 1.0f;

        audio_volume = new_volume;
        ma_device_set_master_volume(&audio_device, audio_volume);
    }

    float Media::GetAudioVolume() {
        if (IsAudioOpened() == false)
            return 0.0;

        return audio_volume;
    }

    AVSampleFormat Media::GetAudioOutputFormat() {
        assert(IsAudioOpened());

        return audio_format;
    }

    AVSampleFormat Media::GetAudioOriginalFormat() {
        assert(IsAudioOpened());

        return (AVSampleFormat)av_format_ctx->streams[audio_stream_index]->codecpar->format;
    }

    int Media::GetAudioSampleSize() {
        assert(IsAudioOpened());

        return audio_sample_size;
    }

    int Media::GetAudioSampleRate() {
        assert(IsAudioOpened());

        return audio_sample_rate;
    }

    int Media::GetAudioChannelCount() {
        assert(IsAudioOpened());

        return audio_channel_count;
    }

    bool Media::HasAlbumArt() {
        return attached_pic;
    }

    Media::Result Media::Open(const FileName& filename, bool open_video, bool open_audio, Settings* playback_settings) {
        Result result;

        if (playback_settings != nullptr)
            settings = *playback_settings;

        result = ApplySettings();
        if (result != Result::Success)
            return result;

        // If media is already open, close it first
        if (IsVideoOpened() || IsAudioOpened()) {
            Close();
        }

        result = OpenFile(filename);
        if (result != Result::Success)
            return result;

        // Opened video won't be paused, even if the previous video was paused, to avoid possible confusion
        is_paused = false;

        if (open_video) {
            result = InitVideo();
            if (result != Result::Success)
                return result;
        }

        if (open_audio) {
            result = InitAudio();
            if (result != Result::Success) {
                return result;
            }
        }

        StartDecodingThread();

        return Result::Success;
    }

    Media::Result Media::OpenFile(const FileName& filename) {
        int response;

        av_format_ctx = avformat_alloc_context();
        OLC_MEDIA_ASSERT(av_format_ctx != nullptr, "Couldn't allocate AVFormatContext");

        OLC_MEDIA_ASSERT(ioCtx.initAVFmtCtx(filename, av_format_ctx) == true, "Couldn't initialize AVFormatContext: most likely couldn't find/open file");

        response = avformat_open_input(&av_format_ctx, "", NULL, NULL);
        if (response < 0) {
            printf("avformat_open_input response: %s\n", GetError(response));
        }
        OLC_MEDIA_ASSERT(response == 0, "Couldn't open file: most likely format isn't supported");

        response = avformat_find_stream_info(av_format_ctx, nullptr);
        OLC_MEDIA_ASSERT(response >= 0, "Couldn't find stream info");

        return Result::Success;
    }

    void Media::CloseFile() {
        avformat_close_input(&av_format_ctx);

        // Don't think this function is really needed, but I put it here for sanity reasons
        avformat_free_context(av_format_ctx);

        ioCtx.closeIO();
    }

    void Media::StartDecodingThread() {
        printf("Starting thread\n");
        keep_loading = true;
        finished_reading = false;
        this->frame_loader = std::thread(&Media::DecodingThread, this);
    }

    void Media::StopDecodingThread() {
        keep_loading = false;
        finished_reading = true;
        conditional.notify_one();
        if (frame_loader.joinable()) {
            frame_loader.join();
        }
    }

    Media::Result Media::DecodingThread() {
        finished_reading = false;

        std::unique_lock<std::mutex> lock(mutex);

        // TODO: figure out if I should add limit for audio queue size

        // Maximum amount of frames that can be pre-decoded
        size_t max_video_queue_size;

        // Minimum amount of frames that should be pre-decoded
        size_t min_video_queue_size;
        size_t min_audio_queue_size;

        if (IsVideoOpened()) {
            if (HasAlbumArt()) {
                max_video_queue_size = 1;
                min_video_queue_size = 1;
            }
            else {
                // "-1" because resizing is disabled, and it allows to avoid overwriting a frame recevied from "GetVideoFrame()"
                max_video_queue_size = video_fifo.capacity() - 1;
                min_video_queue_size = std::max(max_video_queue_size / 2, size_t(1));
            }

            //printf("max_v: %llu\n", max_video_queue_size);
            //printf("min_v: %llu\n", min_video_queue_size);
        }

        if (IsAudioOpened()) {
            min_audio_queue_size = std::max(size_t(audio_fifo.capacity() / 2), size_t(1));

            //printf("min_a: %llu\n", min_audio_queue_size);
        }

        int response;

        AVFrame* av_audio_frame = av_frame_alloc();
        OLC_MEDIA_ASSERT(av_audio_frame != nullptr, "Couldn't allocate resampled AVFrame");

        // Used to store converted "av_audio_frame"
        AVFrame* resampled_audio_frame = av_frame_alloc();
        OLC_MEDIA_ASSERT(resampled_audio_frame != nullptr, "Couldn't allocate resampled AVFrame");

        AVPacket* av_packet = av_packet_alloc();
        OLC_MEDIA_ASSERT(av_packet != nullptr, "Couldn't allocate resampled AVFrame");

        while (true) {
            while (true) {
                if (!HasAlbumArt() && IsVideoOpened() && video_fifo.size() <= min_video_queue_size) {
                    //printf("vb\n");
                    break;
                }
                
                if (IsAudioOpened() && audio_fifo.size() <= min_audio_queue_size) {
                    //printf("ab\n");
                    break;
                }

                if (keep_loading == false)
                    break;

                conditional.wait(lock);
            }

            if (keep_loading == false)
                break;


            /*// Get remaining audio from previous conversion
            if (IsAudioOpened()) {
                if (swr_get_delay(swr_audio_resampler, std::max(resampled_audio_frame->sample_rate, av_audio_frame->sample_rate)) > 0) {
                    response = swr_convert_frame(swr_audio_resampler, resampled_audio_frame, nullptr);
                    OLC_MEDIA_ASSERT(response == 0, "Couldn't resample the frame");

                    int samples_written = audio_fifo.push((void**)resampled_audio_frame->data, resampled_audio_frame->nb_samples);

                    //printf("afd\n");

                    continue;
                }
            }*/

            // Try reading next packet
            response = av_read_frame(av_format_ctx, av_packet);

            // Return if error or end of file was encountered
            if (response < 0) {
                printf("Error or end of file happened\n");
                printf("Exit info: %s\n", GetError(response));

                // TODO: check if response is error or end of file
                break;
            }

            if (IsVideoOpened() && av_packet->stream_index == video_stream_index) {
                //printf("vp\n");
                PiraTimer::start("DecodeVideoFrame");

                // Drain a frame when max size is reached
                if (max_video_queue_size == video_fifo.size()) {
                    video_fifo.pop();
                }
                    

                AVFrame* av_video_frame = video_fifo.back();

                // Send packet to decode
                response = avcodec_send_packet(av_video_codec_ctx, av_packet);
                OLC_MEDIA_ASSERT(response == 0, "Couldn't decode packet");

                // Receive decoded frame
                response = avcodec_receive_frame(av_video_codec_ctx, av_video_frame);
                if (response < 0) {
                    OLC_MEDIA_ASSERT(response == AVERROR_EOF || response == AVERROR(EAGAIN), "Couldn't receive decoded frame");
                }
                
                // We don't want to store empty frame (it could belong to video delay)
                if (av_video_frame->pkt_size != -1) {
                    //printf("vpts: %lf\n", CalculateVideoPts(av_video_frame));

                    //AV_PICTURE_TYPE_B;

                    video_fifo.push();
                }

                PiraTimer::end("DecodeVideoFrame");
            }
            else if (IsAudioOpened() && av_packet->stream_index == audio_stream_index) {
                //printf("ap\n");
                PiraTimer::start("DecodeAudioFrame");

                // Send packet to decode
                response = avcodec_send_packet(av_audio_codec_ctx, av_packet);
                if (response < 0) {
                    OLC_MEDIA_ASSERT(response == AVERROR(EAGAIN), "Failed to decode packet");
                }

                // Single packet can contain multiple frames, so receive them in a loop
                while (true) {
                    response = avcodec_receive_frame(av_audio_codec_ctx, av_audio_frame);
                    if (response < 0) {
                        OLC_MEDIA_ASSERT(response == AVERROR_EOF || response == AVERROR(EAGAIN), "Something went wrong when trying to receive decoded frame");
                        break;
                    }

                    // We don't want to do anything with empty frame
                    if (av_audio_frame->pkt_size != -1) {
                        //printf("af\n");
                        
                        //printf("apts: %lf\n", CalculateAudioPts(av_audio_frame));

                        // We have to manually copy some frame data
                        resampled_audio_frame->sample_rate = av_audio_frame->sample_rate;
                        resampled_audio_frame->channel_layout = av_audio_frame->channel_layout;
                        resampled_audio_frame->channels = av_audio_frame->channels;
                        resampled_audio_frame->format = (int)audio_format;

                        response = swr_convert_frame(swr_audio_resampler, resampled_audio_frame, av_audio_frame);
                        OLC_MEDIA_ASSERT(response == 0, "Couldn't resample the frame");

                        av_frame_unref(av_audio_frame);

                        // Insert decoded audio samples
                        int samples_written = audio_fifo.push((void**)resampled_audio_frame->data, resampled_audio_frame->nb_samples);
                        static size_t total_written = 0;
                        total_written += samples_written;

                        // Get remaining audio from previous conversion
                        while (swr_get_delay(swr_audio_resampler, std::max(resampled_audio_frame->sample_rate, av_audio_frame->sample_rate)) > 0) {
                            response = swr_convert_frame(swr_audio_resampler, resampled_audio_frame, nullptr);
                            OLC_MEDIA_ASSERT(response == 0, "Couldn't resample the frame");

                            int samples_written = audio_fifo.push((void**)resampled_audio_frame->data, resampled_audio_frame->nb_samples);
                        }

                        //printf("total_written: %llu\n", total_written);
                        //printf("sw: %i\n", samples_written);
                    }
                }

                PiraTimer::end("DecodeAudioFrame");
            }
            //std::this_thread::sleep_for(std::chrono::milliseconds(10));

            av_packet_unref(av_packet);
        }

        finished_reading = true;

        // Free the resources
        av_frame_free(&av_audio_frame);
        av_frame_free(&resampled_audio_frame);
        av_packet_free(&av_packet);

        printf("Exiting thread\n");

        return Result::Success;
    }

    Media::Result Media::AdjustSeekedPosition(double wanted_timepoint) {
        // TODO: figure out if I should add limit for audio queue size

        // Maximum amount of frames that can be pre-decoded
        size_t max_video_queue_size;

        // Minimum amount of frames that should be pre-decoded
        size_t min_video_queue_size;
        size_t min_audio_queue_size;

        if (IsVideoOpened()) {
            // "-1" because resizing is disabled, and it allows to avoid overwriting a frame recevied from "GetVideoFrame()"
            max_video_queue_size = video_fifo.capacity() - 1;
            min_video_queue_size = std::max(max_video_queue_size / 2, size_t(1));

            //printf("max_v: %llu\n", max_video_queue_size);
            //printf("min_v: %llu\n", min_video_queue_size);
        }

        if (IsAudioOpened()) {
            min_audio_queue_size = std::max(size_t(audio_fifo.capacity() / 2), size_t(1));

            //printf("min_a: %llu\n", min_audio_queue_size);
        }

        int response;

        AVFrame* av_audio_frame = av_frame_alloc();
        OLC_MEDIA_ASSERT(av_audio_frame != nullptr, "Couldn't allocate resampled AVFrame");

        // Used to store converted "av_audio_frame"
        AVFrame* resampled_audio_frame = av_frame_alloc();
        OLC_MEDIA_ASSERT(resampled_audio_frame != nullptr, "Couldn't allocate resampled AVFrame");

        AVPacket* av_packet = av_packet_alloc();
        OLC_MEDIA_ASSERT(av_packet != nullptr, "Couldn't allocate resampled AVFrame");

        bool video_seeked = IsVideoOpened() ? false : true;
        bool audio_seeked = IsAudioOpened() ? false : true;

        while (true) {
            if (video_seeked && audio_seeked) {
                if (IsVideoOpened()) {
                    last_video_pts = CalculateVideoPts(video_fifo.front());
                    printf("last_video_pts: %lf\n", last_video_pts);
                    delta_time_accumulator = last_video_pts;
                }
                if (IsAudioOpened()) {
                    double at = audio_time;
                    printf("audio_time: %lf\n", at);
                }

                break;
            }

            // Try reading next packet
            response = av_read_frame(av_format_ctx, av_packet);

            // Return if error or end of file was encountered
            if (response < 0) {
                printf("Error or end of file happened\n");
                printf("Exit info: %s\n", GetError(response));

                // TODO: check if response is error or end of file
                break;
            }

            if (IsVideoOpened() && av_packet->stream_index == video_stream_index) {
                //printf("vp\n");
                PiraTimer::start("Skip_VideoFrame");

                // Drain a frame when max size is reached
                if (max_video_queue_size == video_fifo.size()) {
                    video_fifo.pop();
                }


                AVFrame* av_video_frame = video_fifo.back();

                // Send packet to decode
                response = avcodec_send_packet(av_video_codec_ctx, av_packet);
                OLC_MEDIA_ASSERT(response == 0, "Couldn't decode packet");

                // Receive decoded frame
                response = avcodec_receive_frame(av_video_codec_ctx, av_video_frame);
                if (response < 0) {
                    OLC_MEDIA_ASSERT(response == AVERROR_EOF || response == AVERROR(EAGAIN), "Couldn't receive decoded frame");
                }

                // We don't want to store empty frame (it could belong to video delay)
                if (av_video_frame->pkt_size != -1) {
                    //printf("v_pts: %lf\n", CalculateVideoPts(av_video_frame));

                    // Skip frames that appear before timepoint
                    if (CalculateVideoPts(av_video_frame) >= wanted_timepoint) {
                        video_seeked = true;

                        video_fifo.push();
                    }
                }

                PiraTimer::end("Skip_VideoFrame");
            }
            else if (IsAudioOpened() && av_packet->stream_index == audio_stream_index) {
                //printf("ap\n");
                //PiraTimer::start("DecodeAudioFrame");

                // Send packet to decode
                response = avcodec_send_packet(av_audio_codec_ctx, av_packet);
                if (response < 0) {
                    OLC_MEDIA_ASSERT(response == AVERROR(EAGAIN), "Failed to decode packet");
                }

                // Single packet can contain multiple frames, so receive them in a loop
                while (true) {
                    response = avcodec_receive_frame(av_audio_codec_ctx, av_audio_frame);
                    if (response < 0) {
                        OLC_MEDIA_ASSERT(response == AVERROR_EOF || response == AVERROR(EAGAIN), "Something went wrong when trying to receive decoded frame");
                        break;
                    }

                    // We don't want to do anything with empty frame
                    if (av_audio_frame->pkt_size != -1) {
                        //printf("af\n");

                        //printf("a_pts: %lf\n", CalculateAudioPts(av_audio_frame));

                        if (CalculateAudioPts(av_audio_frame) >= wanted_timepoint) {
                            // Keep track of, when the first sample starts
                            if (audio_seeked == false) {
                                // TODO: test out if this always works
                                
                                audio_time = CalculateAudioPts(av_audio_frame);
                                audio_frames_consumed = size_t(audio_time * audio_sample_rate);
                            }

                            audio_seeked = true;

                            // We have to manually copy some frame data
                            resampled_audio_frame->sample_rate = av_audio_frame->sample_rate;
                            resampled_audio_frame->channel_layout = av_audio_frame->channel_layout;
                            resampled_audio_frame->channels = av_audio_frame->channels;
                            resampled_audio_frame->format = (int)audio_format;

                            response = swr_convert_frame(swr_audio_resampler, resampled_audio_frame, av_audio_frame);
                            OLC_MEDIA_ASSERT(response == 0, "Couldn't resample the frame");

                            av_frame_unref(av_audio_frame);

                            // Insert decoded audio samples
                            int samples_written = audio_fifo.push((void**)resampled_audio_frame->data, resampled_audio_frame->nb_samples);
                            static size_t total_written = 0;
                            total_written += samples_written;

                            // Get remaining audio from previous conversion
                            while (swr_get_delay(swr_audio_resampler, std::max(resampled_audio_frame->sample_rate, av_audio_frame->sample_rate)) > 0) {
                                response = swr_convert_frame(swr_audio_resampler, resampled_audio_frame, nullptr);
                                OLC_MEDIA_ASSERT(response == 0, "Couldn't resample the frame");

                                int samples_written = audio_fifo.push((void**)resampled_audio_frame->data, resampled_audio_frame->nb_samples);
                            }
                        }

                        //printf("total_written: %llu\n", total_written);
                        //printf("sw: %i\n", samples_written);
                    }
                }

                //PiraTimer::end("DecodeAudioFrame");
            }
            //std::this_thread::sleep_for(std::chrono::milliseconds(10));

            av_packet_unref(av_packet);
        }

        // Free the resources
        av_frame_free(&av_audio_frame);
        av_frame_free(&resampled_audio_frame);
        av_packet_free(&av_packet);

        return Result::Success;
    }

    // av_err2str returns a temporary array. This doesn't work in gcc.
    // This function can be used as a replacement for av_err2str.
    const char* Media::GetError(int errnum) {
        static char str[AV_ERROR_MAX_STRING_SIZE];
        memset(str, 0, sizeof(str));
        return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
    }

    Media::Result Media::ApplySettings() {
        OLC_MEDIA_ASSERT(settings.preloaded_frames_scale > 0, "\"preloaded_frames_scale\" can't be 0");

        return Result::Success;
    }

    Media::Result Media::InitVideo() {
        int response;

        AVCodecParameters* av_video_codec_params = nullptr;

        video_stream_index = av_find_best_stream(av_format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &av_video_codec, 0);
        if (video_stream_index < 0) {
            if (video_stream_index == AVERROR_STREAM_NOT_FOUND) {
                // TODO: might change it later
                return Result::Success;
            }
            else if (video_stream_index == AVERROR_DECODER_NOT_FOUND) {
                OLC_MEDIA_ASSERT(false, "Couldn't find decoder for any of the video streams");
            }
            else {
                OLC_MEDIA_ASSERT(false, "Unknown error occured when trying to find video stream");
            }
        }

        av_video_codec_params = av_format_ctx->streams[video_stream_index]->codecpar;

        //av_dump_format(av_format_ctx, video_stream_index, "assets/DWIG - Orange Evening  Laut & Luise (LUL007).mp3", 0);
        av_dump_format(av_format_ctx, video_stream_index, "assets/face.jpg", 0);
        //printf("fps: %f\n", av_q2d(av_video_format_ctx->streams[video_stream_index]->avg_frame_rate));
        //av_video_format_ctx->streams[video_stream_index]->avg_frame_rate;

        // Set up a codec context for the decoder
        av_video_codec_ctx = avcodec_alloc_context3(av_video_codec);
        OLC_MEDIA_ASSERT(av_video_codec_ctx != nullptr, "Couldn't create AVCodecContext");

        response = avcodec_parameters_to_context(av_video_codec_ctx, av_video_codec_params);
        OLC_MEDIA_ASSERT(response >= 0, "Couldn't send parameters to AVCodecContext");

        response = avcodec_open2(av_video_codec_ctx, av_video_codec, NULL);
        OLC_MEDIA_ASSERT(response == 0, "Couldn't initialise AVCodecContext");

        AVPixelFormat source_pix_fmt = Media::CorrectDeprecatedPixelFormat(av_video_codec_ctx->pix_fmt);
        sws_video_scaler_ctx = sws_getContext(
            av_video_codec_params->width, av_video_codec_params->height, source_pix_fmt,
            av_video_codec_params->width, av_video_codec_params->height, AV_PIX_FMT_RGB0,
            SWS_BILINEAR, NULL, NULL, NULL
        );
        OLC_MEDIA_ASSERT(sws_video_scaler_ctx != nullptr, "Couldn't initialise SwsContext");

        // Minimum video fifo capacity must stay 2, regardless of video fps
        if (HasAlbumArt()) {
            video_fifo.init(2);
        }
        else {
            Result result = video_fifo.init(std::max(uint16_t(settings.preloaded_frames_scale * GetAverageVideoFPS()), uint16_t(2)));
            OLC_MEDIA_ASSERT(result == Result::Success, "Couldn't allocate video fifo");
        }
        

        // Not sure if this is needed for video streams, but I'll leave it anyway
        av_video_codec_ctx->pkt_timebase = av_format_ctx->streams[video_stream_index]->time_base;

        attached_pic = (av_format_ctx->streams[video_stream_index]->disposition & AV_DISPOSITION_ATTACHED_PIC) ? true : false;
        video_opened = true;
        video_width = av_video_codec_params->width;
        video_height = av_video_codec_params->height;
        video_frame.Create(video_width, video_height);
        video_time_base = av_format_ctx->streams[video_stream_index]->time_base;
        video_delay = av_video_codec_params->video_delay;

        temp_video_frame = av_frame_alloc();
        temp_video_frame->format = AV_PIX_FMT_RGB0;
        temp_video_frame->width = video_width;
        temp_video_frame->height = video_height;
        av_frame_get_buffer(temp_video_frame, 0);

        // Reset values if video was previously opened
        delta_time_accumulator = 0.0f;
        last_video_pts = 0.0;

        PrintVideoInfo();

        return Result::Success;
    }

    void Media::CloseVideo() {
        avcodec_free_context(&av_video_codec_ctx);
        sws_freeContext(sws_video_scaler_ctx);
        sws_video_scaler_ctx = nullptr;
        av_frame_free(&temp_video_frame);

        attached_pic = false;
        video_opened = false;
        video_fifo.clear();
        video_fifo.free();

        // Doesn't fully clear memory, but better than nothing
        video_frame.Create(0, 0);
    }

    void Media::ConvertFrameToRGBASprite(AVFrame* frame, olc::Sprite* target) {
        // TODO: implement some error checking

        PiraTimer::start("Convert");

        sws_scale(sws_video_scaler_ctx, 
            frame->data, frame->linesize, 0, frame->height, 
            temp_video_frame->data, temp_video_frame->linesize
        );

        // Manually copy every pixel row from source to the destination target ("linesize", can be longer,
        // than "width * 4", due to magic alignment, that's why we can't copy entire picture at once)
        uint8_t* src = temp_video_frame->data[0];
        uint8_t* dest = (uint8_t*)target->pColData.data();
        for (int y = 0; y < temp_video_frame->height; y++) {
            memcpy(dest, src, target->width * 4);

            src += temp_video_frame->linesize[0];
            dest += target->width * 4;
        }

        // Convert pixel format from (most likely) YUV representation to RGBA
        /*uint8_t* dest[4] = {(uint8_t*)(target->pColData), NULL, NULL, NULL};
        int dest_linesize[4] = { video_width * 4, 0, 0, 0 };
        sws_scale(sws_video_scaler_ctx, frame->data, frame->linesize, 0, frame->height, dest, dest_linesize);*/

        

        PiraTimer::end("Convert");
        PiraTimer::start("UpdateResultSprite");

        UpdateResultSprite();

        PiraTimer::end("UpdateResultSprite");
    }

    void Media::UpdateResultSprite() { 
        video_frame.Decal()->Update(); 
    }

    double Media::CalculateVideoPts(const AVFrame* frame) {
        return double(frame->best_effort_timestamp * video_time_base.num) / double(video_time_base.den);
    }

    const AVFrame* Media::PeekFrame() {
        if (video_fifo.size() > 0) {
            return video_fifo.front();
        }

        return nullptr;
    }

    AVPixelFormat Media::CorrectDeprecatedPixelFormat(AVPixelFormat pix_fmt) {
        // Fix swscaler deprecated pixel format warning
        // (YUVJ has been deprecated, change pixel format to regular YUV)
        switch (pix_fmt) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default:                  return pix_fmt;
        }
    }

    Media::Result Media::InitAudio() {
        int response;
        Result result;

        AVCodecParameters* av_audio_codec_params = nullptr;

        audio_stream_index = av_find_best_stream(av_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &av_audio_codec, 0);
        if (audio_stream_index < 0) {
            if (audio_stream_index == AVERROR_STREAM_NOT_FOUND) {
                // TODO: might change it later
                return Result::Success;
            }   
            else if (audio_stream_index == AVERROR_DECODER_NOT_FOUND) {
                OLC_MEDIA_ASSERT(false, "Couldn't find decoder for any of the audio streams");
            }   
            else {
                OLC_MEDIA_ASSERT(false, "Unknown error occured when trying to find audio stream");
            }
        }
        
        av_audio_codec_params = av_format_ctx->streams[audio_stream_index]->codecpar;

        // Set up a codec context for the decoder
        av_audio_codec_ctx = avcodec_alloc_context3(av_audio_codec);
        OLC_MEDIA_ASSERT(av_audio_codec_ctx != nullptr, "Couldn't create AVCodecContext");

        response = avcodec_parameters_to_context(av_audio_codec_ctx, av_audio_codec_params);
        OLC_MEDIA_ASSERT(response >= 0, "Couldn't send parameters to AVCodecContext");

        response = avcodec_open2(av_audio_codec_ctx, av_audio_codec, NULL);
        OLC_MEDIA_ASSERT(response == 0, "Couldn't initialise AVCodecContext");

        OLC_MEDIA_ASSERT(ChooseAudioFormat() == Result::Success, "Couldn't choose audio format");

        swr_audio_resampler = swr_alloc_set_opts(
            nullptr,
            av_audio_codec_params->channel_layout, audio_format, av_audio_codec_params->sample_rate,
            av_audio_codec_params->channel_layout, (AVSampleFormat)av_audio_codec_params->format, av_audio_codec_params->sample_rate,
            0, nullptr
        );
        OLC_MEDIA_ASSERT(swr_audio_resampler != nullptr, "Couldn't allocate SwrContext");

        // Should be set when decoding
        av_audio_codec_ctx->pkt_timebase = av_format_ctx->streams[audio_stream_index]->time_base;

        audio_time_base = av_format_ctx->streams[audio_stream_index]->time_base;
        audio_channel_count = av_audio_codec_params->channels;
        audio_sample_rate = av_audio_codec_params->sample_rate;

        // Reset values if audio was previously opened
        audio_frames_consumed = 0;
        audio_time = 0.0;

        result = audio_fifo.init(audio_format, av_audio_codec_params->channels, settings.preloaded_frames_scale * av_audio_codec_params->sample_rate);
        OLC_MEDIA_ASSERT(result == Result::Success, "Couldn't allocate audio fifo");

        result = InitialiseAndStartMiniaudio();
        OLC_MEDIA_ASSERT(result == Result::Success, "Couldn't start miniaud.io");
        
        audio_opened = true;

        PrintAudioInfo();

        return Result::Success;
    }

    void Media::CloseAudio() {
        avcodec_free_context(&av_audio_codec_ctx);
        swr_free(&swr_audio_resampler);
        ma_device_uninit(&audio_device);

        audio_opened = false;
        audio_fifo.clear();
        audio_fifo.free();
    }
    
    double Media::CalculateAudioPts(const AVFrame* frame) {
        return double(frame->best_effort_timestamp * audio_time_base.num) / double(audio_time_base.den);
    }

    Media::Result Media::ChooseAudioFormat() {
        switch (settings.audio_format) {
        case AudioFormat::Default:
            switch ((AVSampleFormat)av_format_ctx->streams[audio_stream_index]->codecpar->format) {
            case AV_SAMPLE_FMT_U8:
            case AV_SAMPLE_FMT_U8P:
                audio_format = AV_SAMPLE_FMT_U8;
                audio_sample_size = 1;
                break;

            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                audio_format = AV_SAMPLE_FMT_S16;
                audio_sample_size = 2;
                break;

            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                audio_format = AV_SAMPLE_FMT_S32;
                audio_sample_size = 4;
                break;

            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                audio_format = AV_SAMPLE_FMT_FLT;
                audio_sample_size = 4;
                break;

            default:
                audio_format = AV_SAMPLE_FMT_FLT;
                audio_sample_size = 4;
                break;
            }
            break;

        case AudioFormat::U8:
            audio_format = AV_SAMPLE_FMT_U8;
            audio_sample_size = 1;
            break;

        case AudioFormat::S16:
            audio_format = AV_SAMPLE_FMT_S16;
            audio_sample_size = 2;
            break;

        case AudioFormat::S32:
            audio_format = AV_SAMPLE_FMT_S32;
            audio_sample_size = 4;
            break;

        case AudioFormat::F32:
            audio_format = AV_SAMPLE_FMT_FLT;
            audio_sample_size = 4;
            break;

            // If default case happened, something wrong happened
        default:
            return Result::Error;
        }

        return Result::Success;
    }

    Media::Result Media::InitialiseAndStartMiniaudio() {
#ifndef OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK
        ma_device_config audio_device_config;

        audio_device_config = ma_device_config_init(ma_device_type_playback);

        switch (audio_format) {
        case AV_SAMPLE_FMT_U8:
            audio_device_config.playback.format = ma_format_u8;
            break;
        
        case AV_SAMPLE_FMT_S16:
            audio_device_config.playback.format = ma_format_s16;
            break;

        case AV_SAMPLE_FMT_S32:
            audio_device_config.playback.format = ma_format_s32;
            break;

        case AV_SAMPLE_FMT_FLT:
            audio_device_config.playback.format = ma_format_f32;
            break;

        default:
            return Result::Error;
            break;
        }
        
        audio_device_config.playback.channels = av_format_ctx->streams[audio_stream_index]->codecpar->channels;
        audio_device_config.sampleRate = av_format_ctx->streams[audio_stream_index]->codecpar->sample_rate;
        audio_device_config.pUserData = this;
        // Since the user can choose to play the audio himself, we will silence the buffer ourselves
        audio_device_config.noPreSilencedOutputBuffer = true;
        audio_device_config.dataCallback = [](ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
            Media* media = reinterpret_cast<Media*>(pDevice->pUserData);
            int frames_read = media->GetAudioFrame(&pOutput, frameCount);

            //std::this_thread::sleep_for(std::chrono::milliseconds(200));
            (void)pInput;
        };

        OLC_MEDIA_ASSERT(ma_device_init(NULL, &audio_device_config, &audio_device) == MA_SUCCESS, "Couldn't open playback device");

        OLC_MEDIA_ASSERT(ma_device_start(&audio_device) == MA_SUCCESS, "Couldn't start playback device");

        ma_device_set_master_volume(&audio_device, audio_volume);

#endif //OLC_MEDIA_CUSTOM_AUDIO_PLAYBACK
        return Result::Success;
    }
}

#endif // OLCPGEX_MEDIA_H