# O CXX do ambiente é ignorado (a referência da emulação bit a bit é o g++; com clang as imagens da demo divergem, e objetos de
# compiladores diferentes não se misturam na libsnowflame.a). Para outro compilador: make CXX=clang++ (na linha de comando).
ifneq ($(filter default environment,$(origin CXX)),)
CXX := g++
endif
# -ffp-contract=off: sem FMA implícito (com -march=native o GCC fundiria a*b+c e mudaria o arredondamento: a emulação deixaria
# de ser bit a bit igual entre builds). -pthread: FlameCore e vídeo usam threads do host (HostPool).
CXXFLAGS ?= -std=c++17 -O3 -ffp-contract=off -pthread -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Isrc
SRC := src/bus.cpp src/cpu_cf.cpp src/vpu.cpp src/flamecore.cpp src/chipset.cpp src/paula.cpp src/periph.cpp src/soc.cpp src/cfdis.cpp src/debugger.cpp src/profiler.cpp src/snowcc_pp.cpp src/snowcc_parse.cpp src/snowcc_gen.cpp src/cpu68k.cpp src/classic.cpp src/m68k2cf.cpp
OBJ := $(SRC:src/%.cpp=build/%.o)
LIB := build/libsnowflame.a

all: $(LIB) build/sf100_tests build/sf100_demo build/snowcc build/sfdbg build/sfprof build/m68kxlat build/sfclassic build/sf100_techdemos build/sf100_mkdvd build/sf100_boot build/sf100_run

# Carimbo do compilador+flags: trocar de compilador refaz tudo em vez de misturar objetos (erros de ligação com __isoc23_*)
CXXSTAMP := build/.cc-$(shell echo '$(CXX) $(CXXFLAGS)' | md5sum | cut -c1-8)
$(CXXSTAMP):
	@mkdir -p build && rm -f build/.cc-* && touch $@

build/%.o: src/%.cpp $(wildcard src/*.h) $(CXXSTAMP)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	ar rcs $@ $^

TESTSRC := $(wildcard tests/*.cpp)
build/sf100_tests: $(TESTSRC) $(LIB) $(wildcard tests/*.h) $(wildcard demo/*.h)
	$(CXX) $(CXXFLAGS) -Itests -Idemo $(TESTSRC) $(LIB) -o $@

build/sf100_demo: demo/demo_main.cpp $(LIB) $(wildcard demo/*.h)
	$(CXX) $(CXXFLAGS) -Idemo $< $(LIB) -o $@

build/snowcc: tools/snowcc_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/sfdbg: tools/sfdbg_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/sfprof: tools/sfprof_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/m68kxlat: tools/m68kxlat_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/sfclassic: tools/sfclassic_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/sf100_techdemos: tools/techdemos_main.cpp $(LIB) $(wildcard src/*.h) $(wildcard demo/*.h)
	$(CXX) $(CXXFLAGS) -Idemo $< $(LIB) -o $@

build/sf100_mkdvd: tools/mkdvd_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/sf100_run: tools/sfrun_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

build/sf100_boot: tools/sfboot_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

# Frontend interativo (janela, controle, teclado, som): opcional, usa SDL2 (fora de 'all'; o núcleo segue sem dependências)
# compilado direto das fontes com otimização para a máquina local (e LTO): a emulação precisa de toda a velocidade possível
PLAYSRC := src/bus.cpp src/cpu_cf.cpp src/vpu.cpp src/flamecore.cpp src/chipset.cpp src/paula.cpp src/periph.cpp src/soc.cpp src/classic.cpp src/cpu68k.cpp src/cfdis.cpp
# PLAY_CXX: g++ por padrão, mesmo com CXX=clang++ no ambiente (alguns clang não enxergam /usr/include/<arch>, onde o SDL2 instala o config;
# e o g++ é o compilador de referência da emulação bit a bit). Troque com `make play PLAY_CXX=clang++`.
PLAY_CXX ?= g++
build/sf100_play: tools/sfplay_main.cpp $(PLAYSRC) $(wildcard src/*.h)
	@mkdir -p build
	$(PLAY_CXX) $(CXXFLAGS) -O3 -march=native -flto=auto $$(pkg-config --cflags sdl2) $< $(PLAYSRC) $$(pkg-config --libs sdl2) -o $@

play: dvd build/sf100_play
	./build/sf100_play out/snowflame.dvd

# DVD do sistema (menu + recursos + jogos) e boot sem janela
GAMES ?= --game snowroids=os/apps/games/snowroids.c

dvd: build/sf100_mkdvd os/apps/menu.c $(wildcard os/vx/*.vsm) $(wildcard os/apps/*.h) $(wildcard os/apps/games/*.c) $(wildcard os/*.c) $(wildcard os/include/*.h) $(wildcard assets/*)
	mkdir -p out && ./build/sf100_mkdvd -o out/snowflame.dvd $(GAMES)

# OpenArena (ioq3): `make oa` compila o motor e monta o pk3 em out/oa; `make dvd-oa` põe os dois no DVD; `make play-oa` abre o jogo (ver docs/PORT.md)
OA_DIR ?= out/oa
oa:
	port/ioq3/make_oa.sh $(OA_DIR)

dvd-oa: oa
	$(MAKE) dvd GAMES="$(GAMES) --bin openarena=$(OA_DIR)/oa.bin --data baseoa/pak0.pk3=$(OA_DIR)/pak0.pk3"

play-oa: dvd-oa build/sf100_play
	./build/sf100_play out/snowflame.dvd

boot: dvd build/sf100_boot
	./build/sf100_boot out/snowflame.dvd --frames 400 --shot 100 --shot 250 --shot 399

techdemos: build/sf100_techdemos
	mkdir -p out/techdemos && ./build/sf100_techdemos out/techdemos

test: build/sf100_tests
	./build/sf100_tests

demo: build/sf100_demo
	mkdir -p out && ./build/sf100_demo out

clean:
	rm -rf build out

.PHONY: all test demo techdemos dvd boot play oa dvd-oa play-oa clean
