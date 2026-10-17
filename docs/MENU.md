# IPL e SnowFlame Dashboard (interface principal)

**IPL (firmware de boot).** `Soc::boot_dvd(path)` carrega um executável do SnowFS do DVD em `IPL_LOAD_ADDR` (0x1000) e inicia a CPU no endereço guardado no campo `flags` da entrada do diretório. Um programa pede novo boot gravando o caminho (NUL-terminado) em `SRAM_BASE` e `1` em `BR_SYS_CTRL`; em C: `os_reboot("games/x.bin")`. Antes de carregar, `Soc::quiesce()` silencia IRQs, timers, vídeo (camadas, Copper, cor de fundo), áudio, HID, DVD, cartão, MMU, filas das VPUs e semáforos, e esvazia as caches.

**SnowFlame Dashboard 2.0 (`os/apps/menu.c`, `boot/menu.bin`).** Interface do console no espírito da 6ª geração (Dreamcast/PS2/GameCube/Xbox, ~2002):
tema "gelo e fogo" — cristal de neve 3D com núcleo de chama e anéis de órbita, cápsulas "gel" com reflexo especular, painéis de vidro fosco, grade em
perspectiva rumo ao horizonte, aurora no céu, neve e brasas, sino de abertura e pad ambiente. 640×480 (480p), 60 quadros/s, buffer duplo.

Cada quadro segue o pipeline da especificação, com os chips em paralelo:

| Etapa | Chip | O que faz no Dashboard |
|---|---|---|
| 1 | SnowFire (ColdFire) | entrada, lógica e animação; matrizes em ponto fixo 16.16 (o snowcc não tem float) convertidas para blocos de constantes; publica os jobs e segue |
| 2 | SnowVector A | `sys/vx/geometry.vx`: 2340 vértices do emblema (chama, cristal, 2 anéis) em 6 jobs na fila; DMA local de entrada/saída; `SIGNAL 0` por job |
| 2 | SnowVector B | `sys/vx/dynamics.vx`: 152 partículas (neve na tela toda, brasas subindo do emblema) **residentes na DMEM**; só os quads saem por DMA; `SIGNAL 1` |
| 3 | FlameCore | anéis opacos com Z, chama e cristal aditivos, partículas texturizadas, interface 2D; alvo limpo com alfa 0 e mistura de alfa separada (`gx_blend_sep`) |
| 4 | SnowBlitter | HUD na camada 1 (buffer duplo): barras com `FILL` e textos/botões como **lista de sprites** (fonte convertida para ARGB no boot) |
| 4 | SnowCopper | céu: degradê, aurora e brilho do horizonte escritos em `VID_BG` a cada 2 linhas (lista dupla, recalculada por quadro) |
| 5 | SnowPaula + vídeo | pad de 4 acordes com coro e reverb tocado **só por eventos do sequenciador**; sino (amostra de 5 parciais) afinado por voz; sons de navegação; a camada 0 é `LF_PREMUL` |

Telas: **abertura** (o cristal nasce girando, os sinos tocam C-E-G-B-D, a chama acende, "AMIGA / SNOWFLAME", e tudo desliza para o menu; qualquer tecla pula),
**menu** (5 cápsulas: GAMES, MEMORY CARD, SYSTEM, DISC, SETTINGS; a selecionada fica em brasa com aura pulsante) e **painéis** de vidro que entram pela direita.
SYSTEM mostra a ficha técnica e um **monitor ao vivo** de carga de cada chip (CPU pelo tempo de quadro, VPUs por `VPU_TOTAL_CYCLES`, GPU por `GPU_STAT_CYCLES`,
DSP por `PA_DSP_LOAD`), tráfego de DMA das VPUs, sprites do Blitter e triângulos. SETTINGS: volume, música ambiente liga/desliga, teste de som.
Entrada: controle (D-pad, A/Start, B) ou teclado (setas, Enter/Espaço, Esc/Backspace). MEMORY CARD grava o contador de boots no setor 0 do cartão (apagar + programar), que sobrevive aos resets.
Jogos: todo `games/NOME.bin` do disco aparece no painel GAMES e é iniciado por `os_reboot`.

**Recursos do disco** (`src/sysassets.h`, gerados pelo `sf100_mkdvd` e usados também pelos testes): `sys/ui.raw` (cápsula, reflexo, halo, floco, painel 9-fatias e borda, ARGB8888),
`sys/hud.raw` (botões A/B/X/Y e emblema para o Blitter), `sys/emblem.msh` (malha do emblema, 64 B por vértice), `sys/snd.raw` (tique, abrir, voltar, sino, pad), `sys/vx/*.vx` (microprogramas) e
`sys/icons.raw`; fonte e logotipo continuam em `assets/` (`tools/mkassets.py`).

**Ferramentas.**
- `make dvd` / `sf100_mkdvd -o out/snowflame.dvd [--game nome=arquivo.c]... [--data caminho=arquivo]...` compila o menu e os jogos (snowcc), gera os ícones e junta `assets/` (fonte Lato/OFL; regenere com `python3 tools/mkassets.py`) num DVD SnowFS.
- `sf100_boot disco.dvd --frames N --shot F --key F:right|left|up|down|enter|esc|space --pad F:mascara --mc cartao.bin` liga o SF-100 sem janela, injeta entradas e grava PNG dos quadros.

## SnowRoids (`os/apps/games/snowroids.c`, `games/snowroids.bin`)
Jogo original, estilo Asteroids, para o SnowOS Native. Gráficos vetoriais neon (cada linha é um quad fino desenhado duas vezes com mistura aditiva: halo largo + núcleo), física em ponto fixo, objetos que dão a volta na tela (cópias nas bordas), ondas crescentes, vida extra a cada 10 000 pontos, partículas, estrelas, pausa, recorde gravado no **setor 8** do cartão de memória e som gerado em tempo de execução (tiro, explosões de 3 tamanhos, propulsor em laço, jingle) pela SnowPaula. Controles: girar = setas/A·D ou direcional; propulsor = cima/W ou botão A; tiro = espaço ou botão B; pausa = P/Start; menu = Esc/Select (volta pelo IPL).
Biblioteca compartilhada com o menu: `os/apps/ui.h` (vértices por tabelas de float, texto, segmentos de estado do FlameCore, cartão). Teste: `tests/test_apps.cpp`.
Para colocar o jogo no disco: `make dvd`; para jogar: `sf100_boot out/snowflame.dvd --frames 700 --shot 600 --key 80:space --key 100:enter --key 115:enter --key 235:enter --pad 300:0x24`.
