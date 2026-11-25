CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -march=native -Wall -Wextra -Wno-interference-size -pthread
LDFLAGS  ?= -pthread

CHESS_LIBRARY_DIR ?= /workspace/chess-library
CHESSTB_DIR       ?= /workspace/chesstb

# chesstb's `make lib` writes the prober as two drop-in files.
CHESSTB_LIB := $(CHESSTB_DIR)/obj/libchesstb

INCLUDES := -I$(CHESS_LIBRARY_DIR)/include -I$(CHESSTB_LIB)

HEADERS := bridge.h material.h evaluator.h \
           options.h slot_writer.h slot_generator.h slot_state.h \
           material_key.h material_queues.h timers.h

BUILD_DIR := build

.PHONY: all clean chesstb

all: generate

chesstb:
	$(MAKE) -C $(CHESSTB_DIR) lib

$(CHESSTB_LIB)/chesstb.h $(CHESSTB_LIB)/chesstb.cpp: chesstb

$(BUILD_DIR)/generate.o: generate.cpp $(HEADERS) $(CHESSTB_LIB)/chesstb.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/chesstb.o: $(CHESSTB_LIB)/chesstb.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

generate: $(BUILD_DIR)/generate.o $(BUILD_DIR)/chesstb.o
	$(CXX) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR) generate
