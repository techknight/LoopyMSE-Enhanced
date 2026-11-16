#pragma once
#include <cstdint>
#include <string>

// Recording of what the emulated Loopy shows and plays, in emulated time:
// every emulated frame becomes a video frame and the sound is rendered in
// step with emulation, so a run recorded at any speed (or hidden) gives the
// same, real-time video.
namespace Recorder
{

struct Settings
{
	std::string video;       // .mp4 / .mkv / ... through ffmpeg, with sound
	std::string audio;       // .wav
	std::string frames_dir;  // one PNG per frame, no ffmpeg needed
	int scale = 3;           // video only: integer pixel scale
	int quality = 16;        // video only: x264 CRF (lower is better)
	std::string ffmpeg = "ffmpeg";
};

bool active();

// Before the system is initialised: sets up the sound capture.
bool start(const Settings& settings, std::string& error);

// After every emulated frame; frame is the number of frames done.
void video_frame(uint64_t frame, const uint16_t* display, int scanlines);

// Finish the files (muxes video and sound).
void stop();

}  // namespace Recorder
