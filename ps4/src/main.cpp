// PS4 homebrew host for Pokemon Silver (OpenOrbis toolchain, jailbroken PS4).
//
// The Silver ROM built from this repository is packaged inside the .pkg and
// run on libgambatte (Game Boy Color core). Video, audio and the controller
// go through the toolchain's SDL2 port. Cartridge SRAM and RTC are saved to
// /data/pokesilver so in-game saves survive closing the app.

#include <SDL2/SDL.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gambatte.h"
#include "gambatte_log.h"

// ---- Symbols libgambatte expects from its (omitted) libretro frontend ----
extern "C" void gambatte_log(enum retro_log_level, const char *format, ...)
{
   va_list ap;
   va_start(ap, format);
   std::vfprintf(stdout, format, ap);
   va_end(ap);
}
extern "C" void gambatte_log_set_cb(retro_log_printf_t) {}
void cartridge_set_rumble(unsigned) {}

namespace {

constexpr int SCREEN_W = 1920;
constexpr int SCREEN_H = 1080;
constexpr int GB_W = 160;
constexpr int GB_H = 144;
constexpr unsigned SAMPLES_PER_FRAME = 35112;   // GB native rate: 2097152 Hz
constexpr unsigned DECIMATE = 64;               // 2097152 / 64 = 32768 Hz
constexpr int OUT_RATE = 2097152 / DECIMATE;

const char *const ROM_PATH  = "/app0/assets/misc/pokesilver.gbc";
const char *const SAVE_DIR  = "/data/pokesilver";
const char *const SAVE_PATH = "/data/pokesilver/pokesilver.srm";
const char *const RTC_PATH  = "/data/pokesilver/pokesilver.rtc";
const char *const CFG_PATH  = "/data/pokesilver/view.cfg";

// PS4 controller button indices exposed by the OpenOrbis SDL2 joystick driver.
enum PadButton
{
   PAD_CROSS = 0, PAD_CIRCLE = 1, PAD_SQUARE = 2, PAD_TRIANGLE = 3,
   PAD_L1 = 4, PAD_R1 = 5, PAD_OPTIONS = 9, PAD_L3 = 11, PAD_R3 = 12,
   PAD_UP = 13, PAD_DOWN = 14, PAD_LEFT = 15, PAD_RIGHT = 16,
   PAD_TOUCHPAD = 17, PAD_L2 = 18, PAD_R2 = 19
};

// ------------------------------------------------------------------ files
bool ReadFile(const char *path, std::vector<uint8_t> &out)
{
   FILE *f = std::fopen(path, "rb");
   if (!f)
      return false;
   std::fseek(f, 0, SEEK_END);
   long n = std::ftell(f);
   std::fseek(f, 0, SEEK_SET);
   if (n < 0) { std::fclose(f); return false; }
   out.resize(static_cast<size_t>(n));
   size_t got = n ? std::fread(out.data(), 1, out.size(), f) : 0;
   std::fclose(f);
   return got == out.size();
}

bool WriteFileAtomic(const char *path, const void *data, size_t size)
{
   std::string tmp = std::string(path) + ".tmp";
   FILE *f = std::fopen(tmp.c_str(), "wb");
   if (!f)
      return false;
   bool ok = std::fwrite(data, 1, size, f) == size;
   ok = (std::fflush(f) == 0) && ok;
   std::fclose(f);
   if (!ok)
      return false;
   return std::rename(tmp.c_str(), path) == 0;
}

// ------------------------------------------------------------------ emulator
struct Joypad : gambatte::InputGetter
{
   unsigned state = 0;
   unsigned operator()() override { return state; }
};

class Emulator
{
public:
   bool Load()
   {
      if (!ReadFile(ROM_PATH, rom_) || rom_.empty())
         return false;
      gb_.setInputGetter(&joypad_);
      if (gb_.load(rom_.data(), static_cast<unsigned>(rom_.size()), 0) != 0)
         return false;
      // Real (bright) GBC colours rather than the dim LCD simulation.
      gb_.setColorCorrection(false);

      LoadBlob(SAVE_PATH, gb_.savedata_ptr(), gb_.savedata_size());
      LoadBlob(RTC_PATH, gb_.rtcdata_ptr(), gb_.rtcdata_size());
      Snapshot(lastSave_, gb_.savedata_ptr(), gb_.savedata_size());
      Snapshot(lastRtc_, gb_.rtcdata_ptr(), gb_.rtcdata_size());

      sound_.resize(SAMPLES_PER_FRAME + 2064 + 4096);
      std::memset(video_, 0, sizeof(video_));
      return true;
   }

   void SetButtons(unsigned buttons) { joypad_.state = buttons; }

   void RunFrame(bool wantAudio)
   {
      for (int guard = 0; guard < 8; ++guard)
      {
         unsigned samples = SAMPLES_PER_FRAME;
         long drawn = gb_.runFor(video_, GB_W, sound_.data(), sound_.size(), samples);
         if (wantAudio)
            Resample(samples);
         if (drawn >= 0)
            break;
      }
   }

   const gambatte::video_pixel_t *Video() const { return video_; }
   std::vector<int16_t> &Audio() { return audio_; }

   void FlushSave()
   {
      FlushBlob(SAVE_PATH, lastSave_, gb_.savedata_ptr(), gb_.savedata_size());
      FlushBlob(RTC_PATH, lastRtc_, gb_.rtcdata_ptr(), gb_.rtcdata_size());
   }

private:
   static void LoadBlob(const char *path, void *dst, unsigned size)
   {
      std::vector<uint8_t> data;
      if (dst && size && ReadFile(path, data) && !data.empty())
         std::memcpy(dst, data.data(), std::min<size_t>(data.size(), size));
   }

   static void Snapshot(std::vector<uint8_t> &dst, const void *src, unsigned size)
   {
      if (src && size)
         dst.assign(static_cast<const uint8_t *>(src), static_cast<const uint8_t *>(src) + size);
      else
         dst.clear();
   }

   static void FlushBlob(const char *path, std::vector<uint8_t> &last, const void *src, unsigned size)
   {
      if (!src || !size)
         return;
      if (last.size() == size && std::memcmp(last.data(), src, size) == 0)
         return;
      if (WriteFileAtomic(path, src, size))
         Snapshot(last, src, size);
   }

   void Resample(unsigned samples)
   {
      for (unsigned i = 0; i < samples; ++i)
      {
         uint32_t s = sound_[i];
         accL_ += static_cast<int16_t>(s & 0xFFFF);
         accR_ += static_cast<int16_t>(s >> 16);
         if (++accN_ == DECIMATE)
         {
            audio_.push_back(static_cast<int16_t>(accL_ / static_cast<int32_t>(DECIMATE)));
            audio_.push_back(static_cast<int16_t>(accR_ / static_cast<int32_t>(DECIMATE)));
            accL_ = accR_ = 0;
            accN_ = 0;
         }
      }
   }

   gambatte::GB gb_;
   Joypad joypad_;
   std::vector<uint8_t> rom_;
   gambatte::video_pixel_t video_[GB_W * GB_H];
   std::vector<gambatte::uint_least32_t> sound_;
   std::vector<int16_t> audio_;
   int32_t accL_ = 0, accR_ = 0;
   unsigned accN_ = 0;
   std::vector<uint8_t> lastSave_, lastRtc_;
};

// ------------------------------------------------------------------ video
// Nearest-neighbour scale of the 160x144 frame into the window surface:
// full screen height at the Game Boy's aspect ratio, or stretched to fill.
class Scaler
{
public:
   Scaler()
   {
      // Brighten for TVs: lift midtones (gamma 0.75).
      for (int i = 0; i < 256; ++i)
      {
         double v = std::pow(i / 255.0, 0.75) * 255.0 + 0.5;
         lut_[i] = static_cast<uint8_t>(v > 255.0 ? 255.0 : v);
      }
   }

   void Draw(SDL_Surface *surface, const gambatte::video_pixel_t *frame, bool stretch)
   {
      // Apply brightness to the small source frame once, then scale.
      for (int i = 0; i < GB_W * GB_H; ++i)
      {
         uint32_t p = static_cast<uint32_t>(frame[i]);
         bright_[i] = (uint32_t(lut_[(p >> 16) & 0xFF]) << 16) |
                      (uint32_t(lut_[(p >> 8) & 0xFF]) << 8) | lut_[p & 0xFF];
      }
      frame = bright_;

      const int W = surface->w, H = surface->h;
      int dw = W, dh = H;
      if (!stretch)
      {
         // Fit: as large as possible while keeping 10:9.
         if (W * GB_H > H * GB_W)
            dw = (H * GB_W + GB_H / 2) / GB_H;
         else
            dh = (W * GB_H + GB_W / 2) / GB_W;
      }
      const int x0 = (W - dw) / 2, y0 = (H - dh) / 2;

      if (dw != mapW_)
      {
         xmap_.resize(dw);
         for (int x = 0; x < dw; ++x)
            xmap_[x] = x * GB_W / dw;
         mapW_ = dw;
      }

      // Pixel format: gambatte gives 0x00RRGGBB. Swap channels if needed.
      const SDL_PixelFormat *fmt = surface->format;
      const bool rgbOrder = fmt->Rmask == 0x00FF0000u;
      const uint32_t alpha = fmt->Amask ? fmt->Amask : 0;

      if (SDL_MUSTLOCK(surface))
         SDL_LockSurface(surface);

      if (stretch != lastStretch_ || W != lastW_ || H != lastH_ || clearFrames_ > 0)
      {
         // Clear borders (a couple of times, in case of multiple buffers).
         for (int y = 0; y < H; ++y)
            std::memset(static_cast<uint8_t *>(surface->pixels) + y * surface->pitch, 0, W * 4);
         if (stretch != lastStretch_ || W != lastW_ || H != lastH_)
            clearFrames_ = 3;
         else
            --clearFrames_;
         lastStretch_ = stretch;
         lastW_ = W;
         lastH_ = H;
      }

      int prevSrcY = -1;
      uint32_t *prevRow = nullptr;
      for (int y = 0; y < dh; ++y)
      {
         uint32_t *dst = reinterpret_cast<uint32_t *>(
            static_cast<uint8_t *>(surface->pixels) + (y0 + y) * surface->pitch) + x0;
         const int sy = y * GB_H / dh;
         if (sy == prevSrcY)
         {
            std::memcpy(dst, prevRow, dw * sizeof(uint32_t));
            continue;
         }
         const gambatte::video_pixel_t *src = frame + sy * GB_W;
         for (int x = 0; x < dw; ++x)
         {
            uint32_t p = static_cast<uint32_t>(src[xmap_[x]]);
            if (!rgbOrder)
               p = ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
            dst[x] = p | alpha;
         }
         prevSrcY = sy;
         prevRow = dst;
      }

      if (SDL_MUSTLOCK(surface))
         SDL_UnlockSurface(surface);
   }

private:
   uint8_t lut_[256];
   gambatte::video_pixel_t bright_[GB_W * GB_H];
   std::vector<int> xmap_;
   int mapW_ = -1;
   bool lastStretch_ = false;
   int lastW_ = 0, lastH_ = 0;
   int clearFrames_ = 3;
};

// ------------------------------------------------------------------ input
class Input
{
public:
   void Open()
   {
      if (!joy_ && SDL_NumJoysticks() > 0)
         joy_ = SDL_JoystickOpen(0);
   }

   void OnEvent(const SDL_Event &ev)
   {
      if (ev.type == SDL_JOYDEVICEADDED)
         Open();
      else if (ev.type == SDL_JOYDEVICEREMOVED && joy_ && !SDL_JoystickGetAttached(joy_))
      {
         SDL_JoystickClose(joy_);
         joy_ = nullptr;
      }
   }

   // Returns Game Boy buttons; sets fast-forward and view-toggle (edge) flags.
   unsigned Poll(bool &fast, bool &toggleView)
   {
      using IG = gambatte::InputGetter;
      unsigned s = 0;
      fast = false;
      bool toggle = false;
      if (joy_)
      {
         auto btn = [this](int i) { return i < SDL_JoystickNumButtons(joy_) && SDL_JoystickGetButton(joy_, i); };
         if (btn(PAD_CROSS) || btn(PAD_TRIANGLE)) s |= IG::A;
         if (btn(PAD_CIRCLE) || btn(PAD_SQUARE))  s |= IG::B;
         if (btn(PAD_OPTIONS))                    s |= IG::START;
         if (btn(PAD_TOUCHPAD) || btn(PAD_L1))    s |= IG::SELECT;
         if (btn(PAD_UP))    s |= IG::UP;
         if (btn(PAD_DOWN))  s |= IG::DOWN;
         if (btn(PAD_LEFT))  s |= IG::LEFT;
         if (btn(PAD_RIGHT)) s |= IG::RIGHT;
         if (btn(PAD_R2) || btn(PAD_R1)) fast = true;
         toggle = btn(PAD_L3);

         if (SDL_JoystickNumHats(joy_) > 0)
         {
            Uint8 h = SDL_JoystickGetHat(joy_, 0);
            if (h & SDL_HAT_UP)    s |= IG::UP;
            if (h & SDL_HAT_DOWN)  s |= IG::DOWN;
            if (h & SDL_HAT_LEFT)  s |= IG::LEFT;
            if (h & SDL_HAT_RIGHT) s |= IG::RIGHT;
         }
         if (SDL_JoystickNumAxes(joy_) >= 2)
         {
            const int dead = 16000;
            int ax = SDL_JoystickGetAxis(joy_, 0), ay = SDL_JoystickGetAxis(joy_, 1);
            if (ax < -dead) s |= IG::LEFT;
            if (ax >  dead) s |= IG::RIGHT;
            if (ay < -dead) s |= IG::UP;
            if (ay >  dead) s |= IG::DOWN;
         }
      }
      if ((s & IG::UP) && (s & IG::DOWN))    s &= ~(IG::UP | IG::DOWN);
      if ((s & IG::LEFT) && (s & IG::RIGHT)) s &= ~(IG::LEFT | IG::RIGHT);

      toggleView = toggle && !toggleHeld_;
      toggleHeld_ = toggle;
      return s;
   }

private:
   SDL_Joystick *joy_ = nullptr;
   bool toggleHeld_ = false;
};

bool LoadStretch()
{
   std::vector<uint8_t> d;
   // Default is widescreen fill; '0' is stored only when the user picks fit.
   return !(ReadFile(CFG_PATH, d) && !d.empty() && d[0] == '0');
}

void SaveStretch(bool stretch)
{
   const char c = stretch ? '1' : '0';
   WriteFileAtomic(CFG_PATH, &c, 1);
}

void Halt(const char *what)
{
   std::printf("[pokesilver] fatal: %s (%s)\n", what, SDL_GetError());
   for (;;)
      SDL_Delay(1000);
}

} // namespace

int main(int, char *[])
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   mkdir(SAVE_DIR, 0777);

   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0)
      Halt("SDL_Init");

   SDL_Window *window = SDL_CreateWindow("Silver", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                         SCREEN_W, SCREEN_H, 0);
   if (!window)
      Halt("SDL_CreateWindow");
   SDL_Surface *surface = SDL_GetWindowSurface(window);
   if (!surface)
      Halt("SDL_GetWindowSurface");

   static Emulator emulator;   // large; keep off the stack
   if (!emulator.Load())
      Halt("loading " "/app0/assets/misc/pokesilver.gbc");

   SDL_AudioSpec want{}, have{};
   want.freq     = OUT_RATE;
   want.format   = AUDIO_S16SYS;
   want.channels = 2;
   want.samples  = 1024;
   SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
   if (audio)
      SDL_PauseAudioDevice(audio, 0);
   // ~4 frames of audio queued keeps sound smooth and acts as the clock.
   const Uint32 targetQueued = (OUT_RATE / 60) * 4 * 4;

   Input input;
   input.Open();
   Scaler scaler;
   bool stretch = LoadStretch();
   Uint32 lastFlush = SDL_GetTicks();

   for (;;)
   {
      SDL_Event ev;
      while (SDL_PollEvent(&ev))
         input.OnEvent(ev);

      bool fast = false, toggleView = false;
      emulator.SetButtons(input.Poll(fast, toggleView));
      if (toggleView)
      {
         stretch = !stretch;
         SaveStretch(stretch);
      }

      int frames = 0;
      if (fast)
      {
         for (; frames < 4; ++frames)
            emulator.RunFrame(false);
         if (audio)
            SDL_ClearQueuedAudio(audio);
      }
      else if (audio)
      {
         while (SDL_GetQueuedAudioSize(audio) < targetQueued && frames < 3)
         {
            emulator.RunFrame(true);
            std::vector<int16_t> &pcm = emulator.Audio();
            SDL_QueueAudio(audio, pcm.data(), static_cast<Uint32>(pcm.size() * sizeof(int16_t)));
            pcm.clear();
            ++frames;
         }
         if (frames == 0)
         {
            SDL_Delay(1);
            continue;
         }
      }
      else
      {
         emulator.RunFrame(false);
         frames = 1;
         SDL_Delay(15);
      }

      surface = SDL_GetWindowSurface(window);
      scaler.Draw(surface, emulator.Video(), stretch);
      SDL_UpdateWindowSurface(window);

      Uint32 now = SDL_GetTicks();
      if (now - lastFlush >= 5000)
      {
         emulator.FlushSave();
         lastFlush = now;
      }
   }
}
