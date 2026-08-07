CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread -MMD -MP -O2

SRC_DIR = src
BUILD_DIR = build

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp ,$(BUILD_DIR)/%.o ,$(SRCS))

TARGET = my_redis_server
TEST_TARGETS = database_tests command_handler_tests

all : $(TARGET)

test: $(TEST_TARGETS)
	./database_tests
	./command_handler_tests

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

database_tests: tests/DatabaseTests.cpp tests/TestHelpers.h src/RedisDatabase.cpp
	$(CXX) $(CXXFLAGS) -Iinclude tests/DatabaseTests.cpp src/RedisDatabase.cpp -o $@

command_handler_tests: tests/CommandHandlerTests.cpp tests/TestHelpers.h src/RedisCommandHandler.cpp src/RedisDatabase.cpp
	$(CXX) $(CXXFLAGS) -Iinclude tests/CommandHandlerTests.cpp src/RedisCommandHandler.cpp src/RedisDatabase.cpp -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TEST_TARGETS)

rebuild: clean all

run: all
	./$(TARGET)
