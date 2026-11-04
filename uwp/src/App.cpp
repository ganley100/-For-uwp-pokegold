// Native UWP host for Pokemon Silver (Xbox Dev Mode / Windows 10+).
//
// The Silver ROM built from this repository is packaged inside the app and
// run on libgambatte (a cycle-accurate Game Boy Color core). Video goes out
// through Direct3D 11, audio through XAudio2, input comes from
// Windows.Gaming.Input gamepads (and the keyboard on PC). The cartridge SRAM
// and RTC are persisted to the app's LocalFolder so in-game saves survive.

#include <unknwn.h>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <xaudio2.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

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

// Compiled at build time by fxc (see CMakeLists.txt).
#include "FullscreenVS.h"
#include "SharpBilinearPS.h"

// ---- Symbols libgambatte expects from its (omitted) libretro frontend ----
extern "C" void gambatte_log(enum retro_log_level, const char *format, ...)
{
   char msg[512];
   va_list ap;
   va_start(ap, format);
   vsnprintf(msg, sizeof(msg), format, ap);
   va_end(ap);
   OutputDebugStringA("[Gambatte] ");
   OutputDebugStringA(msg);
}
extern "C" void gambatte_log_set_cb(retro_log_printf_t) {}
void cartridge_set_rumble(unsigned) {}

namespace wac  = winrt::Windows::ApplicationModel::Core;
namespace wa   = winrt::Windows::ApplicationModel;
namespace wf   = winrt::Windows::Foundation;
namespace wgi  = winrt::Windows::Gaming::Input;
namespace wgd  = winrt::Windows::Graphics::Display;
namespace ws   = winrt::Windows::Storage;
namespace wsys = winrt::Windows::System;
namespace wuc  = winrt::Windows::UI::Core;
namespace wuvm = winrt::Windows::UI::ViewManagement;

namespace {

constexpr int GB_W = 160;
constexpr int GB_H = 144;
constexpr unsigned SAMPLES_PER_FRAME = 35112;   // GB native rate: 2097152 Hz
constexpr unsigned DECIMATE = 64;               // 2097152 / 64 = 32768 Hz
constexpr unsigned OUT_RATE = 2097152 / DECIMATE;

// ------------------------------------------------------------------ files
bool ReadFile(const std::wstring &path, std::vector<uint8_t> &out)
{
   FILE *f = _wfopen(path.c_str(), L"rb");
   if (!f)
      return false;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n < 0) { fclose(f); return false; }
   out.resize(static_cast<size_t>(n));
   size_t got = n ? fread(out.data(), 1, out.size(), f) : 0;
   fclose(f);
   return got == out.size();
}

bool WriteFileAtomic(const std::wstring &path, const void *data, size_t size)
{
   std::wstring tmp = path + L".tmp";
   FILE *f = _wfopen(tmp.c_str(), L"wb");
   if (!f)
      return false;
   bool ok = fwrite(data, 1, size, f) == size;
   ok = (fflush(f) == 0) && ok;
   fclose(f);
   if (!ok)
      return false;
   return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
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
   bool Load(const std::wstring &romPath, const std::wstring &saveDir)
   {
      if (!ReadFile(romPath, rom_) || rom_.empty())
         return false;
      gb_.setInputGetter(&joypad_);
      if (gb_.load(rom_.data(), static_cast<unsigned>(rom_.size()), 0) != 0)
         return false;
      // The core defaults to simulating the GBC's dim, washed-out LCD.
      // Output the game's real colours instead for a bright picture on a TV.
      gb_.setColorCorrection(false);

      savePath_ = saveDir + L"\\pokesilver.srm";
      rtcPath_  = saveDir + L"\\pokesilver.rtc";
      LoadBlob(savePath_, gb_.savedata_ptr(), gb_.savedata_size());
      LoadBlob(rtcPath_, gb_.rtcdata_ptr(), gb_.rtcdata_size());
      Snapshot(lastSave_, gb_.savedata_ptr(), gb_.savedata_size());
      Snapshot(lastRtc_, gb_.rtcdata_ptr(), gb_.rtcdata_size());

      sound_.resize(SAMPLES_PER_FRAME + 2064 + 4096);
      audio_.reserve(2 * (OUT_RATE / 50 + 64));
      std::memset(video_, 0, sizeof(video_));
      return true;
   }

   void SetButtons(unsigned buttons) { joypad_.state = buttons; }

   // Emulates one video frame. Resampled audio is appended to Audio().
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

   // Writes SRAM/RTC to disk if they changed since the last flush.
   void FlushSave(bool force)
   {
      FlushBlob(savePath_, lastSave_, gb_.savedata_ptr(), gb_.savedata_size(), force);
      FlushBlob(rtcPath_, lastRtc_, gb_.rtcdata_ptr(), gb_.rtcdata_size(), force);
   }

private:
   static void LoadBlob(const std::wstring &path, void *dst, unsigned size)
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

   static void FlushBlob(const std::wstring &path, std::vector<uint8_t> &last,
                         const void *src, unsigned size, bool force)
   {
      if (!src || !size)
         return;
      bool changed = last.size() != size || std::memcmp(last.data(), src, size) != 0;
      if (!changed && !force)
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
   std::wstring savePath_, rtcPath_;
   std::vector<uint8_t> lastSave_, lastRtc_;
};

// ------------------------------------------------------------------ audio
class Audio
{
public:
   bool Init()
   {
      if (FAILED(XAudio2Create(xaudio_.put(), 0, XAUDIO2_DEFAULT_PROCESSOR)))
         return false;
      if (FAILED(xaudio_->CreateMasteringVoice(&master_)))
         return false;
      WAVEFORMATEX wf{};
      wf.wFormatTag      = WAVE_FORMAT_PCM;
      wf.nChannels       = 2;
      wf.nSamplesPerSec  = OUT_RATE;
      wf.wBitsPerSample  = 16;
      wf.nBlockAlign     = 4;
      wf.nAvgBytesPerSec = OUT_RATE * 4;
      if (FAILED(xaudio_->CreateSourceVoice(&voice_, &wf)))
         return false;
      voice_->Start();
      return true;
   }

   ~Audio()
   {
      if (voice_) voice_->DestroyVoice();
      if (master_) master_->DestroyVoice();
   }

   bool Ok() const { return voice_ != nullptr; }

   unsigned Queued() const
   {
      XAUDIO2_VOICE_STATE st{};
      voice_->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
      return st.BuffersQueued;
   }

   void Submit(std::vector<int16_t> &pcm)
   {
      if (pcm.empty())
         return;
      std::vector<int16_t> &slot = ring_[next_];
      next_ = (next_ + 1) % kRing;
      slot.swap(pcm);
      pcm.clear();
      XAUDIO2_BUFFER buf{};
      buf.AudioBytes = static_cast<UINT32>(slot.size() * sizeof(int16_t));
      buf.pAudioData = reinterpret_cast<const BYTE *>(slot.data());
      voice_->SubmitSourceBuffer(&buf);
   }

   void Pause(bool paused)
   {
      if (!xaudio_) return;
      if (paused) xaudio_->StopEngine(); else xaudio_->StartEngine();
   }

private:
   static constexpr unsigned kRing = 16;   // far more than ever queued (<= 4)
   winrt::com_ptr<IXAudio2> xaudio_;
   IXAudio2MasteringVoice *master_ = nullptr;
   IXAudio2SourceVoice *voice_ = nullptr;
   std::vector<int16_t> ring_[kRing];
   unsigned next_ = 0;
};

// ------------------------------------------------------------------ video
class Renderer
{
public:
   enum class Mode { Fit, Stretch };

   void Init(wuc::CoreWindow const &window)
   {
      static const D3D_FEATURE_LEVEL levels[] = {
         D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
         D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
      winrt::check_hresult(D3D11CreateDevice(
         nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
         levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), nullptr, context_.put()));

      auto dxgiDevice = device_.as<IDXGIDevice>();
      winrt::com_ptr<IDXGIAdapter> adapter;
      winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));
      winrt::com_ptr<IDXGIFactory2> factory;
      winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

      UpdateSize(window);
      DXGI_SWAP_CHAIN_DESC1 sd{};
      sd.Width            = width_;
      sd.Height           = height_;
      sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
      sd.SampleDesc.Count = 1;
      sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount      = 2;
      sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      sd.Scaling          = DXGI_SCALING_STRETCH;
      sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
      winrt::check_hresult(factory->CreateSwapChainForCoreWindow(
         device_.get(), winrt::get_unknown(window), &sd, nullptr, swapChain_.put()));
      CreateTargets();
      CreatePipeline();
   }

   void Resize(wuc::CoreWindow const &window)
   {
      UINT oldW = width_, oldH = height_;
      UpdateSize(window);
      if (!swapChain_ || (oldW == width_ && oldH == height_))
         return;
      rtv_ = nullptr;
      context_->ClearState();
      context_->Flush();
      winrt::check_hresult(swapChain_->ResizeBuffers(2, width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
      CreateTargets();
   }

   void SetMode(Mode mode) { mode_ = mode; }
   Mode GetMode() const { return mode_; }

   void Draw(const gambatte::video_pixel_t *frame)
   {
      context_->UpdateSubresource(frameTex_.get(), 0, nullptr, frame, GB_W * sizeof(uint32_t), 0);

      // Destination rectangle: full height at the Game Boy's aspect ratio,
      // or the whole screen when stretching.
      float w = static_cast<float>(width_), h = static_cast<float>(height_);
      float dw = w, dh = h;
      if (mode_ == Mode::Fit)
      {
         float s = std::min(w / GB_W, h / GB_H);
         dw = std::floor(GB_W * s + 0.5f);
         dh = std::floor(GB_H * s + 0.5f);
      }
      D3D11_VIEWPORT vp{};
      vp.TopLeftX = std::floor((w - dw) * 0.5f);
      vp.TopLeftY = std::floor((h - dh) * 0.5f);
      vp.Width    = dw;
      vp.Height   = dh;
      vp.MaxDepth = 1.0f;

      Params p{ { float(GB_W), float(GB_H) }, { dw / GB_W, dh / GB_H } };
      context_->UpdateSubresource(params_.get(), 0, nullptr, &p, 0, 0);

      const float black[4] = { 0.f, 0.f, 0.f, 1.f };
      ID3D11RenderTargetView *rtv = rtv_.get();
      context_->OMSetRenderTargets(1, &rtv, nullptr);
      context_->ClearRenderTargetView(rtv, black);
      context_->RSSetViewports(1, &vp);
      context_->IASetInputLayout(nullptr);
      context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context_->VSSetShader(vs_.get(), nullptr, 0);
      context_->PSSetShader(ps_.get(), nullptr, 0);
      ID3D11ShaderResourceView *srv = frameSrv_.get();
      context_->PSSetShaderResources(0, 1, &srv);
      ID3D11SamplerState *smp = sampler_.get();
      context_->PSSetSamplers(0, 1, &smp);
      ID3D11Buffer *cb = params_.get();
      context_->PSSetConstantBuffers(0, 1, &cb);
      context_->Draw(3, 0);
   }

   void Present()
   {
      HRESULT hr = swapChain_->Present(1, 0);
      if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
         lost_ = true;
   }

   bool Lost() const { return lost_; }

   void Trim()
   {
      if (auto dxgi = device_.try_as<IDXGIDevice3>())
      {
         context_->ClearState();
         dxgi->Trim();
      }
   }

private:
   struct Params { float texSize[2]; float scale[2]; };

   void UpdateSize(wuc::CoreWindow const &window)
   {
      auto bounds = window.Bounds();
      float dpi = wgd::DisplayInformation::GetForCurrentView().LogicalDpi();
      width_  = std::max<UINT>(GB_W, static_cast<UINT>(bounds.Width * dpi / 96.0f + 0.5f));
      height_ = std::max<UINT>(GB_H, static_cast<UINT>(bounds.Height * dpi / 96.0f + 0.5f));
   }

   void CreateTargets()
   {
      winrt::com_ptr<ID3D11Texture2D> backBuffer;
      winrt::check_hresult(swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
      winrt::check_hresult(device_->CreateRenderTargetView(backBuffer.get(), nullptr, rtv_.put()));
   }

   void CreatePipeline()
   {
      winrt::check_hresult(device_->CreateVertexShader(g_FullscreenVS, sizeof(g_FullscreenVS), nullptr, vs_.put()));
      winrt::check_hresult(device_->CreatePixelShader(g_SharpBilinearPS, sizeof(g_SharpBilinearPS), nullptr, ps_.put()));

      D3D11_TEXTURE2D_DESC td{};
      td.Width            = GB_W;
      td.Height           = GB_H;
      td.MipLevels        = 1;
      td.ArraySize        = 1;
      td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage            = D3D11_USAGE_DEFAULT;
      td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
      winrt::check_hresult(device_->CreateTexture2D(&td, nullptr, frameTex_.put()));
      winrt::check_hresult(device_->CreateShaderResourceView(frameTex_.get(), nullptr, frameSrv_.put()));

      D3D11_SAMPLER_DESC sd{};
      sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sd.MaxLOD         = D3D11_FLOAT32_MAX;
      winrt::check_hresult(device_->CreateSamplerState(&sd, sampler_.put()));

      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = sizeof(Params);
      bd.Usage     = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      winrt::check_hresult(device_->CreateBuffer(&bd, nullptr, params_.put()));
   }

   winrt::com_ptr<ID3D11Device> device_;
   winrt::com_ptr<ID3D11DeviceContext> context_;
   winrt::com_ptr<IDXGISwapChain1> swapChain_;
   winrt::com_ptr<ID3D11RenderTargetView> rtv_;
   winrt::com_ptr<ID3D11VertexShader> vs_;
   winrt::com_ptr<ID3D11PixelShader> ps_;
   winrt::com_ptr<ID3D11Texture2D> frameTex_;
   winrt::com_ptr<ID3D11ShaderResourceView> frameSrv_;
   winrt::com_ptr<ID3D11SamplerState> sampler_;
   winrt::com_ptr<ID3D11Buffer> params_;
   UINT width_ = 0, height_ = 0;
   Mode mode_ = Mode::Fit;
   bool lost_ = false;
};

// ------------------------------------------------------------------ input
bool Has(wgi::GamepadButtons buttons, wgi::GamepadButtons flag)
{
   return (static_cast<uint32_t>(buttons) & static_cast<uint32_t>(flag)) != 0;
}

struct Keyboard
{
   unsigned buttons = 0;
   bool fast = false;

   void Set(wsys::VirtualKey key, bool down)
   {
      using VK = wsys::VirtualKey;
      using IG = gambatte::InputGetter;
      unsigned bit = 0;
      switch (key)
      {
      case VK::Up:    bit = IG::UP; break;
      case VK::Down:  bit = IG::DOWN; break;
      case VK::Left:  bit = IG::LEFT; break;
      case VK::Right: bit = IG::RIGHT; break;
      case VK::X:     bit = IG::A; break;
      case VK::Z:     bit = IG::B; break;
      case VK::Enter: bit = IG::START; break;
      case VK::Back:
      case VK::Shift:
      case VK::RightShift: bit = IG::SELECT; break;
      case VK::Space: fast = down; return;
      default: return;
      }
      buttons = down ? (buttons | bit) : (buttons & ~bit);
   }
};

// ------------------------------------------------------------------ app
struct App : winrt::implements<App, wac::IFrameworkViewSource, wac::IFrameworkView>
{
   wac::IFrameworkView CreateView() { return *this; }

   void Initialize(wac::CoreApplicationView const &view)
   {
      view.Activated({ this, &App::OnActivated });
      wac::CoreApplication::Suspending({ this, &App::OnSuspending });
      wac::CoreApplication::Resuming({ this, &App::OnResuming });
      // Touching the Gamepads collection early makes it start tracking pads.
      gamepadAdded_ = wgi::Gamepad::GamepadAdded([](auto &&, auto &&) {});
      // On PC, open full screen (Xbox apps are always full screen).
      wuvm::ApplicationView::PreferredLaunchWindowingMode(
         wuvm::ApplicationViewWindowingMode::FullScreen);
   }

   void Load(winrt::hstring const &) {}
   void Uninitialize() {}

   void SetWindow(wuc::CoreWindow const &window)
   {
      window_ = window;
      window.Closed([this](auto &&, auto &&) { closed_ = true; });
      window.VisibilityChanged([this](auto &&, wuc::VisibilityChangedEventArgs const &args) {
         visible_ = args.Visible();
      });
      window.SizeChanged([this](auto &&, auto &&) { if (ready_) renderer_.Resize(window_); });
      window.KeyDown([this](auto &&, wuc::KeyEventArgs const &args) {
         auto key = args.VirtualKey();
         bool repeat = args.KeyStatus().WasKeyDown;
         if (key == wsys::VirtualKey::F && !repeat)
            ToggleViewMode();
         else if (key == wsys::VirtualKey::F11 && !repeat)
            ToggleFullScreen();
         else
            keyboard_.Set(key, true);
      });
      window.KeyUp([this](auto &&, wuc::KeyEventArgs const &args) { keyboard_.Set(args.VirtualKey(), false); });

      // On Xbox the B button raises "back"; swallow it so the game gets it.
      wuc::SystemNavigationManager::GetForCurrentView().BackRequested(
         [](auto &&, wuc::BackRequestedEventArgs const &args) { args.Handled(true); });

      try
      {
         wuvm::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
            wuvm::ApplicationViewBoundsMode::UseCoreWindow);
      }
      catch (...) {}
   }

   void Run()
   {
      window_.Activate();
      auto dispatcher = window_.Dispatcher();

      try { wuvm::ApplicationView::GetForCurrentView().TryEnterFullScreenMode(); }
      catch (...) {}

      renderer_.Init(window_);
      renderer_.SetMode(LoadStretchSetting() ? Renderer::Mode::Stretch : Renderer::Mode::Fit);
      audio_.Init();

      std::wstring romPath = std::wstring(wa::Package::Current().InstalledLocation().Path()) + L"\\Rom\\pokesilver.gbc";
      std::wstring saveDir = std::wstring(ws::ApplicationData::Current().LocalFolder().Path());
      bool loaded = emulator_.Load(romPath, saveDir);
      ready_ = true;

      unsigned framesSinceFlush = 0;
      while (!closed_)
      {
         if (!visible_ || suspended_)
         {
            dispatcher.ProcessEvents(wuc::CoreProcessEventsOption::ProcessOneAndAllPending);
            continue;
         }
         dispatcher.ProcessEvents(wuc::CoreProcessEventsOption::ProcessAllIfPresent);

         if (loaded)
         {
            bool fast = false;
            emulator_.SetButtons(PollInput(fast));

            if (fast)
            {
               for (int i = 0; i < 4; ++i)
                  emulator_.RunFrame(false);
            }
            else if (audio_.Ok())
            {
               // Audio is the master clock: keep ~3 frames of sound queued.
               int frames = 0;
               while (audio_.Queued() < 3 && frames < 3)
               {
                  emulator_.RunFrame(true);
                  audio_.Submit(emulator_.Audio());
                  ++frames;
               }
            }
            else
            {
               emulator_.RunFrame(false);
            }

            if (++framesSinceFlush >= 300)   // ~5 s
            {
               emulator_.FlushSave(false);
               framesSinceFlush = 0;
            }
         }

         renderer_.Draw(emulator_.Video());
         renderer_.Present();
         if (renderer_.Lost())
            break;
      }

      if (loaded)
         emulator_.FlushSave(false);
   }

   unsigned PollInput(bool &fast)
   {
      using IG = gambatte::InputGetter;
      using GB = wgi::GamepadButtons;
      unsigned s = keyboard_.buttons;
      fast = keyboard_.fast;
      bool toggle = false;

      for (auto const &pad : wgi::Gamepad::Gamepads())
      {
         auto r = pad.GetCurrentReading();
         auto b = r.Buttons;
         if (Has(b, GB::A))         s |= IG::A;
         if (Has(b, GB::B))         s |= IG::B;
         if (Has(b, GB::X))         s |= IG::B;
         if (Has(b, GB::Y))         s |= IG::A;
         if (Has(b, GB::Menu))      s |= IG::START;
         if (Has(b, GB::View))      s |= IG::SELECT;
         if (Has(b, GB::DPadUp))    s |= IG::UP;
         if (Has(b, GB::DPadDown))  s |= IG::DOWN;
         if (Has(b, GB::DPadLeft))  s |= IG::LEFT;
         if (Has(b, GB::DPadRight)) s |= IG::RIGHT;
         if (r.LeftThumbstickY >  0.5) s |= IG::UP;
         if (r.LeftThumbstickY < -0.5) s |= IG::DOWN;
         if (r.LeftThumbstickX < -0.5) s |= IG::LEFT;
         if (r.LeftThumbstickX >  0.5) s |= IG::RIGHT;
         if (r.RightTrigger > 0.5) fast = true;
         if (Has(b, GB::LeftThumbstick)) toggle = true;
      }

      // Left stick click switches between fit-to-height and stretch.
      if (toggle && !toggleHeld_)
         ToggleViewMode();
      toggleHeld_ = toggle;

      // The Game Boy can't press opposite directions at once.
      if ((s & IG::UP) && (s & IG::DOWN))    s &= ~(IG::UP | IG::DOWN);
      if ((s & IG::LEFT) && (s & IG::RIGHT)) s &= ~(IG::LEFT | IG::RIGHT);
      return s;
   }

   void ToggleViewMode()
   {
      bool stretch = renderer_.GetMode() != Renderer::Mode::Stretch;
      renderer_.SetMode(stretch ? Renderer::Mode::Stretch : Renderer::Mode::Fit);
      try
      {
         ws::ApplicationData::Current().LocalSettings().Values().Insert(
            L"stretch", winrt::box_value(stretch));
      }
      catch (...) {}
   }

   static bool LoadStretchSetting()
   {
      try
      {
         auto v = ws::ApplicationData::Current().LocalSettings().Values().TryLookup(L"stretch");
         return v ? winrt::unbox_value_or<bool>(v, false) : false;
      }
      catch (...) { return false; }
   }

   static void ToggleFullScreen()
   {
      try
      {
         auto view = wuvm::ApplicationView::GetForCurrentView();
         if (view.IsFullScreenMode())
            view.ExitFullScreenMode();
         else
            view.TryEnterFullScreenMode();
      }
      catch (...) {}
   }

   void OnActivated(wac::CoreApplicationView const &, wa::Activation::IActivatedEventArgs const &)
   {
      wuc::CoreWindow::GetForCurrentThread().Activate();
   }

   void OnSuspending(wf::IInspectable const &, wa::SuspendingEventArgs const &)
   {
      // Raised on the UI thread (ours), between frames.
      if (ready_)
      {
         emulator_.FlushSave(false);
         audio_.Pause(true);
         renderer_.Trim();
      }
      suspended_ = true;
   }

   void OnResuming(wf::IInspectable const &, wf::IInspectable const &)
   {
      suspended_ = false;
      audio_.Pause(false);
   }

   wuc::CoreWindow window_{ nullptr };
   Renderer renderer_;
   Audio audio_;
   Emulator emulator_;
   Keyboard keyboard_;
   winrt::event_token gamepadAdded_{};
   bool closed_ = false;
   bool visible_ = true;
   bool suspended_ = false;
   bool ready_ = false;
   bool toggleHeld_ = false;
};

} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
   winrt::init_apartment();
   wac::CoreApplication::Run(winrt::make<App>());
   return 0;
}
