# Tech demos do SF-100

`make techdemos` (ou `./build/sf100_techdemos [dir] [flamecore|layers|classic|snowos|audio|all]`) roda cinco demos e grava PNG/WAV/texto em `out/techdemos/`.
Código: `tools/techdemos_main.cpp`. Todas usam só a ABI (registradores MMIO, listas de comandos, microcódigo das VPUs), sem atalhos internos do emulador.

| Demo | Subsistemas | Saída |
|---|---|---|
| `flamecore` | VPU-A (geometria/iluminação em microcódigo SnowVector), FlameCore em 6 passes: **stencil** (piso marca o stencil, toro refletido recortado por ele), **blend alfa** (piso translúcido com a "poça de luz" como alfa; esfera de vidro), **blend aditivo** (halo `BF_ONE,BF_ONE`), **multitextura** (TEX0 xadrez DXT1 mipmapped × TEX1 mármore, escala de UV por unidade; TEX1 do piso = poça de luz com `CLAMP`), fog, bilinear, HSR | `flamecore_0..3.png` — 480×360, 4 quadros animados |
| `layers` | Vídeo de 4 camadas com alfa/wrap, Copper reescrevendo scroll e alfa por scanline | `layers_000/040/080/119.png` — paralaxe, calor sobre as colinas, ondulação do lago |
| `classic` | SnowOS Classic: 5 bitplanes lores, barras de raster do Copper em COLOR00, color cycling; HAM6 (4096 cores) | `classic_bars_0..2.png`, `classic_ham6.png` |
| `snowos` | Kernel preemptivo + MMU (tarefa que escreve em NULL é encerrada), SnowFS no DVD, dos/exec (portas de mensagem), SnowGX, SnowIn (teclado) | `snowos_console.txt`, `snowos_quad.png` (textura lida do DVD e desenhada pela GPU) |
| `audio` | SnowPaula: 16 vozes com posição 3D, filtro, reverb, sequenciador de eventos (progressão Am–F–C–G, baixo, arpejo e chimbal) | `snowpaula_demo.wav` — 13,6 s, normalizado no arquivo |
