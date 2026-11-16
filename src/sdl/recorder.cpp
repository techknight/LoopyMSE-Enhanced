#include "recorder.h"

#include <log/log.h>
#include <sound/sound.h>
#include <video/video.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "imgwriter.h"

namespace fs = std::filesystem;
namespace imagew = SDL::ImageWriter;

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace Recorder
{

namespace
{

Settings settings;
bool recording = false;

FILE* wav = nullptr;
uint64_t wav_frames = 0;
std::string wav_path;
bool wav_is_temporary = false;

FILE* video_pipe = nullptr;
std::string video_temp;
std::vector<uint8_t> rgb;
uint64_t video_frames = 0;

// The shell runs these commands: on Windows, cmd /c strips one pair of
// outer quotes, so the whole line gets an extra pair.
std::string shell_command(const std::string& cmd)
{
#ifdef _WIN32
	return "\"" + cmd + "\"";
#else
	return cmd;
#endif
}

std::string quote(const std::string& s)
{
	return "\"" + s + "\"";
}

void put_u32(FILE* f, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	fwrite(b, 1, 4, f);
}

void put_u16(FILE* f, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	fwrite(b, 1, 2, f);
}

// 16-bit stereo PCM; the sizes are filled in when the file is closed.
void write_wav_header(FILE* f, uint32_t rate, uint32_t data_bytes)
{
	fwrite("RIFF", 1, 4, f);
	put_u32(f, 36 + data_bytes);
	fwrite("WAVEfmt ", 1, 8, f);
	put_u32(f, 16);
	put_u16(f, 1);
	put_u16(f, 2);
	put_u32(f, rate);
	put_u32(f, rate * 4);
	put_u16(f, 4);
	put_u16(f, 16);
	fwrite("data", 1, 4, f);
	put_u32(f, data_bytes);
}

void on_audio(const float* samples, int frames)
{
	if (!wav) return;
	std::vector<int16_t> pcm(frames * 2);
	for (int i = 0; i < frames * 2; i++)
	{
		float s = samples[i] * 32767.f;
		pcm[i] = (int16_t)(s > 32767.f ? 32767 : s < -32768.f ? -32768 : s);
	}
	fwrite(pcm.data(), sizeof(int16_t), pcm.size(), wav);
	wav_frames += frames;
}

bool ffmpeg_available()
{
	FILE* p = popen(shell_command(quote(settings.ffmpeg) + " -hide_banner -version").c_str(), "r");
	if (!p) return false;
	char buf[64];
	bool got = fgets(buf, sizeof buf, p) != nullptr && strstr(buf, "ffmpeg") != nullptr;
	while (fgets(buf, sizeof buf, p))
	{
	}
	pclose(p);
	return got;
}

}  // namespace

bool active()
{
	return recording;
}

bool start(const Settings& s, std::string& error)
{
	settings = s;
	if (settings.video.empty() && settings.audio.empty() && settings.frames_dir.empty()) return true;

	if (!settings.video.empty())
	{
		if (!ffmpeg_available())
		{
			error = "recording video needs ffmpeg; it was not found as '" + settings.ffmpeg +
					"' (set record.ffmpeg, or use record.frames and record.audio)";
			return false;
		}
		video_temp = settings.video + ".video.mkv";
		// The emulated frame rate as an exact fraction: 16 MHz over the cycles of a frame
		char rate[32];
		snprintf(rate, sizeof rate, "%.6f", Video::get_frame_rate());
		std::string cmd = quote(settings.ffmpeg) + " -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgb24 -s " +
						  std::to_string(Video::DISPLAY_WIDTH) + "x" + std::to_string(Video::DISPLAY_HEIGHT) +
						  " -framerate " + rate + " -i - -vf scale=iw*" + std::to_string(settings.scale) + ":ih*" +
						  std::to_string(settings.scale) + ":flags=neighbor -c:v libx264 -preset veryfast -crf " +
						  std::to_string(settings.quality) + " -pix_fmt yuv420p " + quote(video_temp);
		video_pipe = popen(shell_command(cmd).c_str(), "wb");
		if (!video_pipe)
		{
			error = "could not start ffmpeg";
			return false;
		}
		rgb.assign(Video::DISPLAY_WIDTH * Video::DISPLAY_HEIGHT * 3, 0);
	}

	if (!settings.audio.empty() || !settings.video.empty())
	{
		wav_is_temporary = settings.audio.empty();
		wav_path = wav_is_temporary ? settings.video + ".audio.wav" : settings.audio;
		wav = fopen(wav_path.c_str(), "wb");
		if (!wav)
		{
			error = "cannot write " + wav_path;
			return false;
		}
		write_wav_header(wav, Sound::get_capture_sample_rate(), 0);
		wav_frames = 0;
		Sound::set_capture(on_audio);
	}

	if (!settings.frames_dir.empty())
	{
		std::error_code ec;
		fs::create_directories(settings.frames_dir, ec);
	}

	recording = true;
	Log::info("[Record] recording%s%s%s", settings.video.empty() ? "" : (" video to " + settings.video).c_str(),
			  settings.audio.empty() ? "" : (" sound to " + settings.audio).c_str(),
			  settings.frames_dir.empty() ? "" : (" frames to " + settings.frames_dir).c_str());
	return true;
}

void video_frame(uint64_t frame, const uint16_t* display, int scanlines)
{
	if (!recording) return;

	if (!settings.frames_dir.empty())
	{
		char name[32];
		snprintf(name, sizeof name, "frame_%06llu.png", (unsigned long long)frame);
		imagew::save_image_16bpp(
			imagew::IMAGE_TYPE_PNG, fs::path(settings.frames_dir) / name, Video::DISPLAY_WIDTH, scanlines,
			(uint16_t*)display
		);
	}

	if (video_pipe)
	{
		uint8_t* out = rgb.data();
		for (int y = 0; y < Video::DISPLAY_HEIGHT; y++)
		{
			for (int x = 0; x < Video::DISPLAY_WIDTH; x++)
			{
				uint16_t c = y < scanlines ? display[y * Video::DISPLAY_WIDTH + x] : 0;
				*out++ = ((c >> 10) & 31) * 255 / 31;
				*out++ = ((c >> 5) & 31) * 255 / 31;
				*out++ = (c & 31) * 255 / 31;
			}
		}
		if (fwrite(rgb.data(), 1, rgb.size(), video_pipe) != rgb.size())
		{
			Log::error("[Record] ffmpeg stopped taking frames; the video ends at frame %llu", (unsigned long long)frame);
			pclose(video_pipe);
			video_pipe = nullptr;
		}
		video_frames++;
	}
}

void stop()
{
	if (!recording) return;
	recording = false;
	Sound::set_capture(nullptr);

	if (wav)
	{
		uint32_t bytes = (uint32_t)(wav_frames * 4);
		fseek(wav, 0, SEEK_SET);
		write_wav_header(wav, Sound::get_capture_sample_rate(), bytes);
		fclose(wav);
		wav = nullptr;
	}

	if (!settings.video.empty())
	{
		if (video_pipe)
		{
			pclose(video_pipe);
			video_pipe = nullptr;
		}
		std::string cmd = quote(settings.ffmpeg) + " -hide_banner -loglevel error -y -i " + quote(video_temp);
		if (wav_frames)
		{
			cmd += " -i " + quote(wav_path) + " -map 0:v -map 1:a -c:a aac -b:a 192k";
		}
		cmd += " -c:v copy -movflags +faststart " + quote(settings.video);
		int result = system(shell_command(cmd).c_str());
		std::error_code ec;
		if (result == 0)
		{
			fs::remove(video_temp, ec);
			if (wav_is_temporary) fs::remove(wav_path, ec);
			Log::info(
				"[Record] wrote %s (%llu frames, %.1f s)", settings.video.c_str(), (unsigned long long)video_frames,
				video_frames / Video::get_frame_rate()
			);
		}
		else
		{
			Log::error(
				"[Record] could not combine %s and %s into %s; both are kept", video_temp.c_str(), wav_path.c_str(),
				settings.video.c_str()
			);
		}
	}
	else if (!settings.audio.empty())
	{
		Log::info("[Record] wrote %s (%.1f s)", settings.audio.c_str(), wav_frames / (double)Sound::get_capture_sample_rate());
	}
}

}  // namespace Recorder
