CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Isrc
SRC := src/bus.cpp src/cpu_cf.cpp src/vpu.cpp src/flamecore.cpp src/chipset.cpp src/paula.cpp src/periph.cpp src/soc.cpp src/cfdis.cpp src/debugger.cpp src/profiler.cpp src/snowcc_pp.cpp src/snowcc_parse.cpp src/snowcc_gen.cpp src/cpu68k.cpp src/classic.cpp src/m68k2cf.cpp
OBJ := $(SRC:src/%.cpp=build/%.o)
LIB := build/libsnowflame.a

all: $(LIB) build/sf100_tests build/sf100_demo build/snowcc build/sfdbg build/sfprof build/m68kxlat build/sfclassic build/sf100_techdemos

build/%.o: src/%.cpp $(wildcard src/*.h)
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

techdemos: build/sf100_techdemos
	mkdir -p out/techdemos && ./build/sf100_techdemos out/techdemos

test: build/sf100_tests
	./build/sf100_tests

demo: build/sf100_demo
	mkdir -p out && ./build/sf100_demo out

clean:
	rm -rf build out

.PHONY: all test demo techdemos clean
