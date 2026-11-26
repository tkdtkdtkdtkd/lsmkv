#include <iostream>
#include <string>
#include <vector>
#include <functional>

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase>& GetTestCases() {
    static std::vector<TestCase> cases;
    return cases;
}

void RegisterTest(const std::string& name, std::function<void()> fn) {
    GetTestCases().push_back({name, std::move(fn)});
}

int main() {
    int passed = 0;
    int failed = 0;
    std::cout << "Running " << GetTestCases().size() << " test suites...\n";
    for (const auto& tc : GetTestCases()) {
        std::cout << "  [RUN] " << tc.name << "\n";
        try {
            tc.fn();
            std::cout << "  [PASS] " << tc.name << "\n";
            passed++;
        } catch (const std::exception& e) {
            std::cout << "  [FAIL] " << tc.name << ": " << e.what() << "\n";
            failed++;
        } catch (...) {
            std::cout << "  [FAIL] " << tc.name << ": Unknown exception\n";
            failed++;
        }
    }
    std::cout << "\nResults: " << passed << " passed, " << failed << " failed\n";
    return (failed == 0) ? 0 : 1;
}
