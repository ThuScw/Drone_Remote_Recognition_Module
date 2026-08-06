# MAVLink / GB 46750-2025 Host-Side Test Suite
# Build: make
# Clean: make clean
# Run:   make run  (or ./tests after make)

CXX      ?= g++
CXXFLAGS  = -std=c++14 -Wall -Wextra -g -D_USE_MATH_DEFINES

# Include paths: production code + stub replacements
INCLUDES  = -I./stubs -I./stubs/driver -I../main -I../main/data -I../main/protocol \
            -I../main/telemetry -I../main/broadcast -I../main/logging

# Test sources + production modules under test
SRCS      = test_main.cpp test_crc.cpp test_parser.cpp test_rid_messages.cpp \
            test_fault_log.cpp test_status_machine.cpp test_flight_log.cpp \
            ../main/data/mavlink_crc.cpp \
            ../main/data/mavlink_parser.cpp \
            ../main/protocol/rid_messages.cpp \
            ../main/telemetry/fault_log.cpp \
            ../main/logging/flight_log.cpp

BUILD_DIR = build
OBJS      = $(addprefix $(BUILD_DIR)/, $(notdir $(SRCS:.cpp=.o)))
TARGET    = tests

vpath %.cpp $(sort $(dir $(SRCS)))

.PHONY: all clean run

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
