// sf100_play: liga o SF-100 numa janela e deixa jogar com controle e teclado (frontend opcional; usa SDL2, que o núcleo não exige).
//   sf100_play [disco.dvd] [--mc cartao.bin] [--scale N] [--boot caminho] [--mute]
// Controle: direcional/analógico esquerdo, A B X Y, LB RB, Start, Back = Select (layout do SDL_GameController).
// Teclado: vai para o HID do console (setas, Enter, Esc...). F11 = tela cheia, F12 ou fechar a janela = sair.
#include "soc.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sf;
using namespace sf::abi;

static std::vector<u8> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary); if (!f) return {};
  return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  std::string dvd = "out/snowflame.dvd", mcfile = "out/card.bin", boot = "boot/menu.bin"; int scale = 2; bool mute = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i]; auto nx = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--mc") mcfile = nx(); else if (a == "--scale") scale = std::max(1, std::atoi(nx().c_str())); else if (a == "--boot") boot = nx();
    else if (a == "--mute") mute = true; else if (a[0] == '-') { std::fprintf(stderr, "uso: sf100_play [disco.dvd] [--mc cartao.bin] [--scale N] [--boot caminho] [--mute]\n"); return 2; }
    else dvd = a;
  }
  auto img = slurp(dvd);
  if (img.empty()) { std::fprintf(stderr, "não abri %s (gere com: make dvd)\n", dvd.c_str()); return 1; }

  Soc s; s.dvd.insert(img); s.pads.pad[0].connected = true;
  { auto m = slurp(mcfile); if (m.size() == s.mcard.data.size()) s.mcard.data = m; }
  s.console.on_char = [](char c) { std::putchar(c); std::fflush(stdout); };
  if (!s.boot_dvd(boot)) { std::fprintf(stderr, "o disco não tem %s\n", boot.c_str()); return 1; }
  s.video.start();

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | (mute ? 0 : SDL_INIT_AUDIO)) != 0) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  int W = s.video.width(), H = s.video.height();
  SDL_Window* win = SDL_CreateWindow("Amiga SnowFlame SF-100", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * scale, H * scale, SDL_WINDOW_RESIZABLE);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!win || !ren) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  SDL_RenderSetLogicalSize(ren, W, H);
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

  SDL_AudioDeviceID audio = 0;
  if (!mute) {
    SDL_AudioSpec want{}, have{}; want.freq = (int)PAULA_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0); else std::fprintf(stderr, "sem áudio: %s\n", SDL_GetError());
  }

  SDL_GameController* pad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks() && !pad; i++) if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
  if (pad) std::printf("[sf100_play] controle: %s\n", SDL_GameControllerName(pad));
  else std::printf("[sf100_play] nenhum controle reconhecido; use o teclado\n");

  const u64 freq = SDL_GetPerformanceFrequency(); u64 t_last = SDL_GetPerformanceCounter(), t_title = t_last; int frames_title = 0, title_n = 0;
  bool quit = false;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) quit = true;
      else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        SDL_Scancode sc = e.key.keysym.scancode;
        if (e.type == SDL_KEYDOWN && sc == SDL_SCANCODE_F12) quit = true;
        else if (e.type == SDL_KEYDOWN && sc == SDL_SCANCODE_F11) SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        else if (!e.key.repeat && sc < 256) s.hid.key_event((u8)sc, e.type == SDL_KEYDOWN);   // scancodes do SDL = usages USB HID
      } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) {
        pad = SDL_GameControllerOpen(e.cdevice.which); if (pad) std::printf("[sf100_play] controle: %s\n", SDL_GameControllerName(pad));
      } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
        SDL_GameControllerClose(pad); pad = nullptr;
      }
    }
    // controle -> porta 0 (o analógico esquerdo também aciona o direcional)
    PadState& p = s.pads.pad[0]; p.buttons = 0;
    if (pad) {
      auto b = [&](SDL_GameControllerButton k) { return SDL_GameControllerGetButton(pad, k) != 0; };
      auto ax = [&](SDL_GameControllerAxis k) { return (int)SDL_GameControllerGetAxis(pad, k); };
      int lx = ax(SDL_CONTROLLER_AXIS_LEFTX), ly = ax(SDL_CONTROLLER_AXIS_LEFTY);
      if (b(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -16000) p.buttons |= BTN_UP;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 16000) p.buttons |= BTN_DOWN;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -16000) p.buttons |= BTN_LEFT;
      if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 16000) p.buttons |= BTN_RIGHT;
      if (b(SDL_CONTROLLER_BUTTON_A)) p.buttons |= BTN_A; if (b(SDL_CONTROLLER_BUTTON_B)) p.buttons |= BTN_B;
      if (b(SDL_CONTROLLER_BUTTON_X)) p.buttons |= BTN_X; if (b(SDL_CONTROLLER_BUTTON_Y)) p.buttons |= BTN_Y;
      if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) p.buttons |= BTN_L; if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p.buttons |= BTN_R;
      if (b(SDL_CONTROLLER_BUTTON_START)) p.buttons |= BTN_START; if (b(SDL_CONTROLLER_BUTTON_BACK)) p.buttons |= BTN_SELECT;
      auto u8ax = [](int v) { return (u8)std::clamp((v + 32768) >> 8, 0, 255); };
      p.lx = u8ax(lx); p.ly = u8ax(ly); p.rx = u8ax(ax(SDL_CONTROLLER_AXIS_RIGHTX)); p.ry = u8ax(ax(SDL_CONTROLLER_AXIS_RIGHTY));
      p.lt = (u8)(ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7); p.rt = (u8)(ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
      if (p.rumble) SDL_GameControllerRumble(pad, 0x6000, 0x6000, 50);
    }

    s.run_frames(1);
    if (s.cpu.halted && s.sched.empty()) { std::printf("[sf100_play] a CPU parou\n"); break; }

    // áudio: entrega o que a SnowPaula gerou; descarta se a fila passar de ~0,25 s (emulação mais rápida que o tempo real)
    s.sync_audio();
    if (audio && !s.paula.pcm.empty()) {
      if (SDL_GetQueuedAudioSize(audio) < PAULA_RATE) SDL_QueueAudio(audio, s.paula.pcm.data(), (u32)(s.paula.pcm.size() * sizeof(i16)));
    }
    s.paula.pcm.clear();

    if (s.video.width() != W || s.video.height() != H) {        // troca de modo de vídeo (240p/480p/720p)
      W = s.video.width(); H = s.video.height(); SDL_DestroyTexture(tex);
      tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H); SDL_RenderSetLogicalSize(ren, W, H);
    }
    SDL_UpdateTexture(tex, nullptr, s.video.frame.data(), W * 4);   // 0x00RRGGBB: o byte alto é ignorado em ARGB opaco
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr); SDL_RenderPresent(ren);

    // ritmo de 60 Hz quando a emulação sobra; velocidade no título
    u64 now = SDL_GetPerformanceCounter(), target = t_last + freq / 60;
    if (now < target) { SDL_Delay((u32)((target - now) * 1000 / freq)); now = SDL_GetPerformanceCounter(); }
    t_last = now; frames_title++;
    if (now - t_title > freq) {
      char t[128]; double fps = frames_title * (double)freq / (double)(now - t_title);
      std::snprintf(t, sizeof t, "Amiga SnowFlame SF-100  -  %.0f fps (%.0f%% da velocidade real)", fps, fps / 60 * 100);
      SDL_SetWindowTitle(win, t); t_title = now; frames_title = 0;
      if (++title_n % 5 == 0) { std::printf("[sf100_play] %.0f fps (%.0f%% da velocidade real)\n", fps, fps / 60 * 100); std::fflush(stdout); }
    }
  }

  { std::ofstream m(mcfile, std::ios::binary); if (m) m.write((const char*)s.mcard.data.data(), (std::streamsize)s.mcard.data.size()); }
  if (pad) SDL_GameControllerClose(pad);
  if (audio) SDL_CloseAudioDevice(audio);
  SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
