CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -MMD -MP -O2 -Iinclude

SRC_DIR = src
BUILD_DIR = build

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TARGET = my_redis_server
TEST_TARGETS = database_tests command_handler_tests

all : $(TARGET)

test: $(TEST_TARGETS)
	./database_tests
	./command_handler_tests

# End-to-end tests over real TCP sockets (pipelining, persistence across restarts, ...)
integration: $(TARGET)
	python3 tests/integration_test.py ./$(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

database_tests: tests/DatabaseTests.cpp tests/TestHelpers.h src/RedisDatabase.cpp include/RedisDatabase.h
	$(CXX) $(CXXFLAGS) tests/DatabaseTests.cpp src/RedisDatabase.cpp -o $@

command_handler_tests: tests/CommandHandlerTests.cpp tests/TestHelpers.h src/RedisCommandHandler.cpp src/RedisDatabase.cpp include/RedisCommandHandler.h include/RedisDatabase.h
	$(CXX) $(CXXFLAGS) tests/CommandHandlerTests.cpp src/RedisCommandHandler.cpp src/RedisDatabase.cpp -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TEST_TARGETS) *.d

rebuild: clean all

run: all
	./$(TARGET)

-include $(DEPS)

.PHONY: all test integration clean rebuild run
