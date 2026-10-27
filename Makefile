CXX ?= g++
CXXFLAGS += -std=c++23 -march=native -O0 $(shell pkg-config fuse3 --cflags) -I include/
LDFLAGS += $(shell pkg-config fuse3 vulkan --libs)

ifeq ($(DEBUG), 1)
	CXXFLAGS += -g -DDEBUG -Wall -Werror
endif
CXXFLAGS += -g

.PHONY: all
all: bin/vramfs

bin/vramfs: build/util.o build/memory.o build/entry.o build/file.o build/dir.o build/symlink.o build/vramfs.o | bin
	$(CXX) -o $@ $^ $(LDFLAGS)

build bin:
	@mkdir -p $@

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf build/ bin/
