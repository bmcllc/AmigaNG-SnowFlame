# IPL e SnowFlame Dashboard (menu principal)

**IPL (firmware de boot).** `Soc::boot_dvd(path)` carrega um executável do SnowFS do DVD em `IPL_LOAD_ADDR` (0x1000) e inicia a CPU no endereço guardado no campo `flags` da entrada do diretório. Um programa pede novo boot gravando o caminho (NUL-terminado) em `SRAM_BASE` e `1` em `BR_SYS_CTRL`; em C: `os_reboot("games/x.bin")`. Antes de carregar, `Soc::quiesce()` silencia IRQs, timers, vídeo, áudio, HID, DVD, cartão e MMU e esvazia as caches.

**Menu (`os/apps/menu.c`, `boot/menu.bin`).** 640×480 (480p) com buffer duplo, tudo desenhado pelo FlameCore: ondas animadas, neve, cubo translúcido, carrossel de 5 ícones (GAMES, MEMORY CARD, SYSTEM, DISC, SETTINGS), painéis de informação, texto por atlas A8 com larguras proporcionais e sons de navegação/abertura pela SnowPaula. Entrada: controle (D-pad, A, B, Start) ou teclado (setas, Enter/Espaço, Esc/Backspace). O painel do cartão de memória grava um contador de boots no setor 0 do cartão (apagar + programar), que sobrevive aos resets. Jogos: todo `games/NOME.bin` do disco aparece no painel GAMES e é iniciado por `os_reboot`.

**Ferramentas.**
- `make dvd` / `sf100_mkdvd -o out/snowflame.dvd [--game nome=arquivo.c]... [--data caminho=arquivo]...` compila o menu e os jogos (snowcc), gera os ícones e junta `assets/` (fonte Lato/OFL; regenere com `python3 tools/mkassets.py`) num DVD SnowFS.
- `sf100_boot disco.dvd --frames N --shot F --key F:right|left|up|down|enter|esc|space --pad F:mascara --mc cartao.bin` liga o SF-100 sem janela, injeta entradas e grava PNG dos quadros.

## SnowRoids (`os/apps/games/snowroids.c`, `games/snowroids.bin`)
Jogo original, estilo Asteroids, para o SnowOS Native. Gráficos vetoriais neon (cada linha é um quad fino desenhado duas vezes com mistura aditiva: halo largo + núcleo), física em ponto fixo, objetos que dão a volta na tela (cópias nas bordas), ondas crescentes, vida extra a cada 10 000 pontos, partículas, estrelas, pausa, recorde gravado no **setor 8** do cartão de memória e som gerado em tempo de execução (tiro, explosões de 3 tamanhos, propulsor em laço, jingle) pela SnowPaula. Controles: girar = setas/A·D ou direcional; propulsor = cima/W ou botão A; tiro = espaço ou botão B; pausa = P/Start; menu = Esc/Select (volta pelo IPL).
Biblioteca compartilhada com o menu: `os/apps/ui.h` (vértices por tabelas de float, texto, segmentos de estado do FlameCore, cartão). Teste: `tests/test_apps.cpp`.
Para colocar o jogo no disco: `make dvd`; para jogar: `sf100_boot out/snowflame.dvd --frames 700 --shot 600 --key 80:space --key 100:enter --key 115:enter --key 235:enter --pad 300:0x24`.
