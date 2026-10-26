CXX:= g++
BUILD = release

SANITIZE := -fsanitize=address -fno-omit-frame-pointer
CPPFLAGS := -Iinclude -MMD -MP
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic $(SANITIZE)
LDFLAGS := $(SANITIZE)
LDLIBS := -lrt

ifeq ($(BUILD),debug)
  CXXFLAGS += -g -O0
else
  CXXFLAGS += -O2
endif

BUILD_DIR := build/$(BUILD)

COMMON_SRCS := $(wildcard src/*.cpp)
SRCS_S := server.cpp $(COMMON_SRCS)
SRCS_C := client.cpp $(COMMON_SRCS)
OBJS_S := $(SRCS_S:%.cpp=$(BUILD_DIR)/%.o)
OBJS_C := $(SRCS_C:%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(sort $(OBJS_S:.o=.d) $(OBJS_C:.o=.d))

SERVER := $(BUILD_DIR)/server
CLIENT := $(BUILD_DIR)/client

.PHONY: all debug release clean
all: $(SERVER) $(CLIENT)

$(SERVER): $(OBJS_S)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(CLIENT): $(OBJS_C)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

debug:
	@$(MAKE) --no-print-directory BUILD=debug

release:
	@$(MAKE) --no-print-directory BUILD=release

clean:
	$(RM) -r build

-include $(DEPS)

