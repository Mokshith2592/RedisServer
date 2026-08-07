#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <iostream>
#include <string>

class TestSuite {
    public:
        explicit TestSuite(const std::string& name) : name(name) {}

        void expect(bool condition, const std::string& message) {
            if (!condition) {
                std::cerr << name << ": FAIL: " << message << '\n';
                ++failures;
            }
        }

        int result() const {
            if (failures == 0) {
                std::cout << name << ": all tests passed\n";
            }
            return failures == 0 ? 0 : 1;
        }

    private:
        std::string name;
        int failures = 0;
};

#endif
