# Network Latency and Packet Loss Chaos Emulator
#   make          build (default flags)
#   make debug    build with -g -O0 and AddressSanitizer
#   make release  build optimised (-O2) and strip
#   make clean    remove build output

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CPPFLAGS += -Iinclude
TARGET   := chaos-emulator
SRCS     := $(wildcard src/*.cpp)
OBJS     := $(SRCS:src/%.cpp=build/%.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

build/%.o: src/%.cpp $(wildcard include/*.hpp) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

debug: CXXFLAGS := -std=c++17 -Wall -Wextra -g -O0 -fsanitize=address,undefined
debug: clean $(TARGET)

release: CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -DNDEBUG
release: clean $(TARGET)
	strip $(TARGET)

clean:
	rm -rf build $(TARGET)

.PHONY: all debug release clean
