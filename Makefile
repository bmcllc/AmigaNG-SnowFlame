# O CXX do ambiente é ignorado (a referência da emulação bit a bit é o g++; com clang as imagens da demo divergem, e objetos de
# compiladores diferentes não se misturam na libsnowflame.a). Para outro compilador: make CXX=clang++ (na linha de comando).
ifneq ($(filter default environment,$(origin CXX)),)
CXX := g++
endif
# -ffp-contract=off: sem FMA implícito (com -march=native o GCC fundiria a*b+c e mudaria o arredondamento: a emulação deixaria
# de ser bit a bit igual entre builds). -pthread: FlameCore e vídeo usam threads do host (HostPool).
CXXFLAGS ?= -std=c++17 -O3 -ffp-contract=off -pthread -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Isrc
# Núcleo (CPU, barramento/caches, ferramentas, compilador) e dispositivos do SoC. Os dispositivos têm duas realizações do mesmo
# comportamento (docs/HOST-FABRIC.md): BACKEND=ref usa os modelos em C++ de src/; BACKEND=sv usa o backend SystemVerilog de rtl/
# (compilado pelo Verilator) ligado pelo Host Fabric (fabric/). Saída em build/ (ref) ou build/sv/ (sv), com os mesmos alvos.
CORE_SRC := src/bus.cpp src/vpu_asm.cpp src/soc.cpp src/cfdis.cpp src/debugger.cpp src/profiler.cpp src/snowcc_pp.cpp src/snowcc_parse.cpp src/snowcc_gen.cpp src/cpu68k.cpp src/classic.cpp src/m68k2cf.cpp
DEV_SRC := src/cpu_cf.cpp src/bridge.cpp src/vpu.cpp src/flamecore.cpp src/chipset.cpp src/paula.cpp src/periph.cpp src/fabric_ref.cpp
SRC := $(CORE_SRC) $(DEV_SRC)
# Dispositivos que o backend SV ainda realiza pelo modelo de referência
SV_REF_SRC :=
SV_FAB_SRC := fabric/fabric.cpp fabric/sv_devices.cpp fabric/sv_cpu.cpp

BACKEND ?= ref
ifeq ($(BACKEND),sv)
B := build/sv
else
B := build
endif
CORE_OBJ := $(CORE_SRC:src/%.cpp=build/%.o)
OBJ := $(CORE_OBJ) $(DEV_SRC:src/%.cpp=build/%.o)
SV_OBJ := $(CORE_OBJ) $(SV_REF_SRC:src/%.cpp=build/%.o) $(SV_FAB_SRC:fabric/%.cpp=build/sv/fab_%.o)
LIB := $(B)/libsnowflame.a

all: $(LIB) $(B)/sf100_tests $(B)/sf100_demo $(B)/snowcc $(B)/sfdbg $(B)/sfprof $(B)/m68kxlat $(B)/sfclassic $(B)/sf100_techdemos $(B)/sf100_mkdvd $(B)/sf100_boot $(B)/sf100_run

# Carimbo do compilador+flags: trocar de compilador refaz tudo em vez de misturar objetos (erros de ligação com __isoc23_*)
CXXSTAMP := build/.cc-$(shell echo '$(CXX) $(CXXFLAGS)' | md5sum | cut -c1-8)
$(CXXSTAMP):
	@mkdir -p build && rm -f build/.cc-* && touch $@

build/%.o: src/%.cpp $(wildcard src/*.h) $(CXXSTAMP)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libsnowflame.a: $(OBJ)
	ar rcs $@ $^

# ---------------- Backend SystemVerilog (B.A.S.E. Host Fabric) ----------------
# rtl/*.sv -> Verilator (C++ gerado em build/sv/vl) -> libVsf100.a + libverilated.a; o pacote do ABI é gerado de src/abi.h.
VERILATOR ?= verilator
VL_ROOT := $(shell $(VERILATOR) --getenv VERILATOR_ROOT 2>/dev/null)
VLDIR := build/sv/vl
RTL_SRC := rtl/fabric_pkg.sv $(sort $(filter-out rtl/fabric_pkg.sv rtl/sf100.sv,$(wildcard rtl/*.sv))) rtl/sf100.sv
VLFLAGS ?= -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL
VL_LIBS := $(VLDIR)/libVsf100.a $(VLDIR)/libverilated.a
build/sv/sf100_abi_pkg.sv: src/abi.h tools/abi2sv.py
	@mkdir -p build/sv
	python3 tools/abi2sv.py $< $@
$(VLDIR)/Vsf100.mk: build/sv/sf100_abi_pkg.sv $(RTL_SRC)
	@rm -rf $(VLDIR) && mkdir -p $(VLDIR)
	$(VERILATOR) --cc --Mdir $(VLDIR) --top-module sf100 -O3 --x-assign fast --x-initial fast $(VLFLAGS) build/sv/sf100_abi_pkg.sv $(RTL_SRC)
$(VL_LIBS) &: $(VLDIR)/Vsf100.mk
	tools/vlbuild.sh $(VLDIR) "$(CXX)" "$(VL_ROOT)"
VL_DEFS := -DVM_COVERAGE=0 -DVM_SC=0 -DVM_TIMING=0 -DVM_TRACE=0 -DVM_TRACE_FST=0 -DVM_TRACE_VCD=0
build/sv/fab_%.o: fabric/%.cpp fabric/fabric.h $(wildcard src/*.h) $(VL_LIBS) $(CXXSTAMP)
	$(CXX) $(CXXFLAGS) $(VL_DEFS) -I$(VLDIR) -isystem $(VL_ROOT)/include -isystem $(VL_ROOT)/include/vltstd -c $< -o $@
build/sv/libsnowflame.a: $(SV_OBJ) $(VL_LIBS)
	rm -f $@ && ar rcs $@ $(SV_OBJ) && printf 'OPEN $@\nADDLIB $(VLDIR)/libVsf100.a\nADDLIB $(VLDIR)/libverilated.a\nSAVE\nEND\n' | ar -M

TESTSRC := $(wildcard tests/*.cpp)
$(B)/sf100_tests: $(TESTSRC) $(LIB) $(wildcard tests/*.h) $(wildcard demo/*.h)
	$(CXX) $(CXXFLAGS) -Itests -Idemo $(TESTSRC) $(LIB) -o $@

$(B)/sf100_demo: demo/demo_main.cpp $(LIB) $(wildcard demo/*.h)
	$(CXX) $(CXXFLAGS) -Idemo $< $(LIB) -o $@

$(B)/snowcc: tools/snowcc_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/sfdbg: tools/sfdbg_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/sfprof: tools/sfprof_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/m68kxlat: tools/m68kxlat_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/sfclassic: tools/sfclassic_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/sf100_techdemos: tools/techdemos_main.cpp $(LIB) $(wildcard src/*.h) $(wildcard demo/*.h)
	$(CXX) $(CXXFLAGS) -Idemo $< $(LIB) -o $@

$(B)/sf100_mkdvd: tools/mkdvd_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/sf100_run: tools/sfrun_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

$(B)/sf100_boot: tools/sfboot_main.cpp $(LIB) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@

# Frontend interativo (janela, controle, teclado, som): opcional, usa SDL2 (fora de 'all'; o núcleo segue sem dependências)
# compilado direto das fontes com otimização para a máquina local (e LTO): a emulação precisa de toda a velocidade possível
PLAYSRC := src/bus.cpp src/bridge.cpp src/cpu_cf.cpp src/vpu.cpp src/flamecore.cpp src/chipset.cpp src/paula.cpp src/periph.cpp src/soc.cpp src/classic.cpp src/cpu68k.cpp src/cfdis.cpp src/fabric_ref.cpp
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

dvd: $(B)/sf100_mkdvd os/apps/menu.c $(wildcard os/vx/*.vsm) $(wildcard os/apps/*.h) $(wildcard os/apps/games/*.c) $(wildcard os/*.c) $(wildcard os/include/*.h) $(wildcard assets/*)
	mkdir -p out && ./$(B)/sf100_mkdvd -o out/snowflame.dvd $(GAMES)

# OpenArena (ioq3): `make oa` compila o motor e monta o pk3 em out/oa; `make dvd-oa` põe os dois no DVD; `make play-oa` abre o jogo (ver docs/PORT.md)
OA_DIR ?= out/oa
oa:
	port/ioq3/make_oa.sh $(OA_DIR)

dvd-oa: oa
	$(MAKE) dvd GAMES="$(GAMES) --bin openarena=$(OA_DIR)/oa.bin --data baseoa/pak0.pk3=$(OA_DIR)/pak0.pk3"

play-oa: dvd-oa build/sf100_play
	./build/sf100_play out/snowflame.dvd

boot: dvd $(B)/sf100_boot
	./$(B)/sf100_boot out/snowflame.dvd --frames 400 --shot 100 --shot 250 --shot 399

techdemos: $(B)/sf100_techdemos
	mkdir -p out/techdemos && ./$(B)/sf100_techdemos out/techdemos

test: $(B)/sf100_tests
	./$(B)/sf100_tests

demo: $(B)/sf100_demo
	mkdir -p out && ./$(B)/sf100_demo out

# Verificação diferencial (docs/HOST-FABRIC.md): as duas realizações dos dispositivos (C++ de referência e SystemVerilog)
# passam pela mesma suíte e a demo precisa sair bit a bit igual (quadros PPM e áudio WAV).
VERIFY_FRAMES ?= 60
verify:
	$(MAKE) BACKEND=ref build/sf100_tests build/sf100_demo
	$(MAKE) BACKEND=sv build/sv/sf100_tests build/sv/sf100_demo
	./build/sf100_tests && ./build/sv/sf100_tests
	rm -rf out/verify && mkdir -p out/verify/ref out/verify/sv
	./build/sf100_demo out/verify/ref $(VERIFY_FRAMES) >/dev/null && ./build/sv/sf100_demo out/verify/sv $(VERIFY_FRAMES) >/dev/null
	diff -r out/verify/ref out/verify/sv && echo "verify: ref == sv ($(VERIFY_FRAMES) quadros + áudio)"

clean:
	rm -rf build out

.PHONY: all test demo verify techdemos dvd boot play oa dvd-oa play-oa clean
