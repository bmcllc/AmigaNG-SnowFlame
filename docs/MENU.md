# IPL e SnowFlame Dashboard (interface principal)

**IPL (firmware de boot).** `Soc::boot_dvd(path)` carrega um executável do SnowFS do DVD em `IPL_LOAD_ADDR` (0x1000) e inicia a CPU no endereço guardado no campo `flags` da entrada do diretório. Um programa pede novo boot gravando o caminho (NUL-terminado) em `SRAM_BASE` e `1` em `BR_SYS_CTRL`; em C: `os_reboot("games/x.bin")`. Antes de carregar, `Soc::quiesce()` silencia IRQs, timers, vídeo (camadas, Copper, cor de fundo), áudio, HID, DVD, cartão, MMU, filas das VPUs e semáforos, e esvazia as caches.

**SnowFlame Dashboard 2.1 (`os/apps/menu.c`, `boot/menu.bin`).** Interface do console no espírito da 6ª geração (Dreamcast/PS2/GameCube/Xbox, ~2002):
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
**menu** (5 cápsulas: GAMES, MEMORY CARD, SYSTEM, DISC, SETTINGS; a selecionada fica em brasa com aura pulsante; sob o título dela, uma linha de estado ao vivo:
jogos no disco, blocos usados do cartão, uptime e carga, arquivos do disco, volume/música/efeitos) e **painéis** de vidro que entram pela direita.
Entrada: controle (D-pad, A/Start, B, L/R) ou teclado (setas, Enter/Espaço, Esc/Backspace, Q/E ou PgUp/PgDn como L/R). **Segurar** um direcional repete (25 quadros de espera, depois a cada 5);
o teclado também vale como "segurado" até soltar a tecla (a pressão nunca se perde, mesmo solta no mesmo quadro). B no menu abre **INFO** (controles e sobre).

| Painel | O que mostra / faz |
|---|---|
| **GAMES** | todo `games/NOME.bin` do disco (até 24, em ordem de disco, com rolagem; L/R andam uma página). Título e descrição vêm de `sys/games.inf` (`sf100_mkdvd --info`); sem entrada o título sai do nome do arquivo. Rodapé: descrição, tamanho do `.bin` e, no SnowRoids, o recorde lido do cartão. "LAST" marca o último jogado. A inicia o jogo (`os_reboot`). |
| **MEMORY CARD** | mapa de blocos 32×8 (uma célula por bloco de 4 KB: livre, sistema, recorde do SnowRoids, outro) que se acende aos poucos, "N of M blocks used", lista de saves por bloco e **apagar** (A, com confirmação; começa em Cancel). O bloco do sistema é protegido. |
| **SYSTEM** | 3 abas (esquerda/direita ou L/R): **HARDWARE** (ficha técnica, uptime/heap, disco, cartão, controles); **ACTIVITY** (monitor ao vivo de carga de cada chip: CPU pelo tempo de quadro, VPUs por `VPU_TOTAL_CYCLES`, GPU por `GPU_STAT_CYCLES`, DSP por `PA_DSP_LOAD`; DMA das VPUs, sprites do Blitter, triângulos; **histórico** de 4 s da carga de quadro); **PADS** (4 portas: botões acesos, analógicos e gatilhos ao vivo). |
| **DISC** | tamanho do disco e **todos** os arquivos do SnowFS com tamanho, com rolagem; o nome completo do selecionado fica no rodapé. |
| **SETTINGS** | Volume, Ambient music, Sound test, **Effects** (High/Low/Off: densidade de neve e brasas; "Off" deixa a SnowVector-B parada), **Boot intro** (On/Off), Restore defaults. Cada linha explica a si mesma no rodapé. |
| **INFO** | controles do console e sobre (versão, jogos, arquivos). |

**Cartão de memória (SnowCard).** O Dashboard é dono do **bloco 0** (4 KB, a unidade de apagamento): setor 0 = `'SFMC'` + contador de boots (como na 2.0); setor 1 = ajustes `'SFST'`, versão 1, volume, música, efeitos, abertura, "volume mexido" e o nome do último jogo (24 bytes
em +32 B). Qualquer alteração apaga e regrava o bloco inteiro (os ajustes são gravados 1,5 s depois da última mudança, ou ao iniciar um jogo). O volume padrão deixa o mestre da SnowPaula como está (o mix do menu foi calibrado assim); só um volume mexido pelo usuário é aplicado no boot.
O resto do cartão só é lido: o menu lê o 1º setor de cada bloco, 8 por quadro (a espera é por IRQ do cartão, sem girar a CPU), e classifica como livre, sistema, recorde do SnowRoids (setor 8) ou "outro". Quem grava no cartão deve começar a gravação no 1º setor de um bloco de 4 KB.

**Retorno de um jogo.** `os_reboot("boot/menu.bin")` deixa o caminho em `SRAM_BASE`, que o IPL não apaga: o menu o reconhece (e o zera), pula o logotipo e o resto da abertura (o emblema só desliza para o menu), e abre o GAMES no último jogo, pronto para A. "Boot intro: Off" faz o mesmo a frio, sem abrir GAMES.

**Recursos do disco** (`src/sysassets.h`, gerados pelo `sf100_mkdvd` e usados também pelos testes): `sys/ui.raw` (cápsula, reflexo, halo, floco, painel 9-fatias e borda, ARGB8888),
`sys/hud.raw` (botões A/B/X/Y e emblema para o Blitter), `sys/emblem.msh` (malha do emblema, 64 B por vértice), `sys/snd.raw` (tique, abrir, voltar, sino, pad), `sys/vx/*.vx` (microprogramas) e
`sys/icons.raw` (6 ícones: jogos, cartão, sistema, disco, opções, informações); fonte e logotipo continuam em `assets/` (`tools/mkassets.py`). `sys/games.inf` (opcional) tem uma linha `nome|Título|Descrição` por jogo.

**Desempenho.** O limite no `sf100_play` é o interpretador (instruções por quadro). O emissor de vértices de `os/apps/ui.h` (`vtx`/`quad4`/`text`/`qpoly`) copia moldes de vértice prontos e calcula cada canto do quad uma vez (a saída é idêntica pixel a pixel
à da 2.0), e as esperas do cartão dormem no IRQ em vez de girar: menu 365 → ~275 mil instruções/quadro, SYSTEM 774 → ~400 mil, DISC 592 → ~365 mil. Os painéis novos custam menos que o SYSTEM antigo.

**Ferramentas.**
- `make dvd` / `sf100_mkdvd -o out/snowflame.dvd [--game nome=arquivo.c]... [--bin nome=imagem.bin]... [--data caminho=arquivo]... [--info "nome=Título|Descrição"]...` compila o menu e os jogos (snowcc), gera os ícones e junta `assets/` (fonte Lato/OFL; regenere com `python3 tools/mkassets.py`) num DVD SnowFS. `--info` escreve `sys/games.inf` (título e descrição que o painel GAMES mostra; `make dvd` já passa o do SnowRoids).
- `sf100_boot disco.dvd --frames N --shot F --key F:right|left|up|down|enter|esc|space --pad F:mascara --mc cartao.bin` liga o SF-100 sem janela, injeta entradas e grava PNG dos quadros.

## SnowRoids (`os/apps/games/snowroids.c`, `games/snowroids.bin`)
Jogo original, estilo Asteroids, para o SnowOS Native. Gráficos vetoriais neon (cada linha é um quad fino desenhado duas vezes com mistura aditiva: halo largo + núcleo), física em ponto fixo, objetos que dão a volta na tela (cópias nas bordas), ondas crescentes, vida extra a cada 10 000 pontos, partículas, estrelas, pausa, recorde gravado no **setor 8** do cartão de memória e som gerado em tempo de execução (tiro, explosões de 3 tamanhos, propulsor em laço, jingle) pela SnowPaula. Controles: girar = setas/A·D ou direcional; propulsor = cima/W ou botão A; tiro = espaço ou botão B; pausa = P/Start; menu = Esc/Select (volta pelo IPL).
Biblioteca compartilhada com o menu: `os/apps/ui.h` (vértices por tabelas de float, texto, segmentos de estado do FlameCore, cartão). Teste: `tests/test_apps.cpp`.
Para colocar o jogo no disco: `make dvd`; para jogar: `sf100_boot out/snowflame.dvd --frames 700 --shot 600 --key 80:space --key 100:enter --key 115:enter --key 235:enter --pad 300:0x24`.
