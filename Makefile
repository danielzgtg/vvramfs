CXX ?= g++
CXXFLAGS += $(shell pkg-config fuse3 --cflags) -flto -I include/ -march=x86-64-v3 -std=c++23 -Wall -Werror
LDFLAGS += $(shell pkg-config fuse3 vulkan --libs) -flto -latomic

.PHONY: all clean debug release

ifeq ($(BUILD),release)
    CXXFLAGS += -DNDEBUG -O3
    all: bin/vvramfs
else
    CXXFLAGS += -g
    all: bin/vvramfs_unstripped
endif

bin/vvramfs: bin/vvramfs_unstripped
	strip --strip-all -o $@ $^

bin/vvramfs_unstripped: build/command_pool.o build/dir.o build/entry.o build/file.o build/gpu_init.o build/memory.o build/symlink.o build/util.o build/vramfs.o | bin
	$(CXX) -o $@ $^ $(LDFLAGS)

build bin:
	@mkdir -p $@

build/%.o: src/%.cpp $(wildcard include/*.hpp) | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf build/ bin/
