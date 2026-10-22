# Amiga SnowFlame SF-100 — emulador C++17

Implementação em C++ do console conceitual SF-100: CPU ColdFire V4e, 2× SnowVector-128, GPU FlameCore, Blitter, Copper,
vídeo (4 camadas), SnowPaula e SnowBridge — e o software em volta: compilador C (snowcc), SnowOS Native (kernel + SnowGX/SnowVX/SnowAudio),
SnowOS Classic (CPU 68000 + chipset Amiga OCS/ECS/AGA + tradutor 68k → ColdFire) e ferramentas (depurador, profiler). Sem dependências externas.

    make          # biblioteca, testes, demo e ferramentas em build/
    make test     # ~210 testes (hardware, ISA, texturas, chipset, compilador, SnowOS, Dashboard, Classic, tradutor)
    make demo     # roda a demo; escreve out/frame_*.ppm e out/audio.wav
    make boot     # monta o DVD do sistema e liga o SF-100 no SnowFlame Dashboard (PNGs em out/boot/)
    make play     # o mesmo numa janela, com controle, teclado e som (opcional: precisa de SDL2)
    ./build/sf100_demo out 60

    ./build/snowcc app.c -I os -I os/include --run         # compila e executa um programa C (ex.: apps do SnowOS)
    ./build/sfdbg app.c -I os -I os/include                 # depurador interativo
    ./build/sfprof app.c -I os -I os/include                # profiler
    ./build/m68kxlat prog68k.bin -o prog_cf.bin             # tradutor 68000 → ColdFire
    ./build/sfclassic prog68k.bin --frames 5 --out frames/  # SnowOS Classic: roda 68000 + chipset Amiga

Layout: `src/` (emulador, compilador, ferramentas em biblioteca), `os/` (SnowOS Native em C), `tools/` (programas de linha de comando), `demo/`, `tests/`.
O **SnowFlame Dashboard** (`os/apps/menu.c`) é a vitrine da arquitetura: a CPU só cuida da lógica e publica jobs; a SnowVector A transforma o emblema 3D e a B simula
neve e brasas na própria DMEM (DMA local + semáforos), o FlameCore rasteriza, o Blitter compõe o HUD, o Copper pinta o céu linha a linha e a SnowPaula toca o pad pelo sequenciador.

Documentação: `docs/SF100-ABI.md` (hardware), `docs/SNOWCC.md`, `docs/SNOWOS.md`, `docs/MENU.md` (IPL e Dashboard), `docs/TOOLS.md`.
