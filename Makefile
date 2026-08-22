CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -march=native -Wall -Wextra -Wno-interference-size -pthread
CFLAGS   ?= -O3 -DNDEBUG -march=native -Wall -Wextra -pthread
LDFLAGS  ?= -pthread

CHESS_LIBRARY_DIR ?= /workspace/chess-library
CHESSTB_DIR       ?= /workspace/chesstb

INCLUDES := -I$(CHESS_LIBRARY_DIR)/include -I$(CHESSTB_DIR)/src -I$(CHESSTB_DIR)/lib

COMMON_CXX := \
  $(wildcard $(CHESSTB_DIR)/src/chess/*.cpp) \
  $(wildcard $(CHESSTB_DIR)/src/util/*.cpp) \
  $(wildcard $(CHESSTB_DIR)/src/probe/*.cpp)

COMMON_C := \
  $(wildcard $(CHESSTB_DIR)/lib/lz4/*.c) \
  $(wildcard $(CHESSTB_DIR)/lib/LZMA/*.c) \
  $(wildcard $(CHESSTB_DIR)/lib/zstd/common/*.c) \
  $(wildcard $(CHESSTB_DIR)/lib/zstd/compress/*.c) \
  $(wildcard $(CHESSTB_DIR)/lib/zstd/dictBuilder/*.c)

HEADERS := bridge.h material.h evaluator.h \
           options.h slot_writer.h slot_generator.h slot_state.h \
           material_key.h material_queues.h timers.h

BUILD_DIR := build

CXX_OBJS := $(BUILD_DIR)/generate.o $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(notdir $(COMMON_CXX)))
C_OBJS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(COMMON_C)))

.PHONY: all clean

all: generate

define COMPILE_CXX
$(BUILD_DIR)/$(notdir $(1:.cpp=.o)): $(1) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $$< -o $$@
endef
$(foreach src,generate.cpp $(COMMON_CXX),$(eval $(call COMPILE_CXX,$(src))))

define COMPILE_C
$(BUILD_DIR)/$(notdir $(1:.c=.o)): $(1)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $$< -o $$@
endef
$(foreach src,$(COMMON_C),$(eval $(call COMPILE_C,$(src))))

generate: $(CXX_OBJS) $(C_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR) generate
