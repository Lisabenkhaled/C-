#include "Asset.hpp"
#include "Portfolio.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool cond, const std::string& msg) {
    if (!cond) throw std::runtime_error("Assertion failed: " + msg);
}

template <typename Ex, typename Fn>
void expectThrows(Fn&& fn, const std::string& msg) {
    bool thrown = false;
    try {
        fn();
    } catch (const Ex&) {
        thrown = true;
    }
    if (!thrown) throw std::runtime_error("Expected exception not thrown: " + msg);
}

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

void testAssetValidation() {
    expectThrows<std::invalid_argument>([] { Asset("", 10.0, 0.05, 0.1); }, "empty asset name");
    expectThrows<std::invalid_argument>([] { Asset("AAPL", -1.0, 0.05, 0.1); }, "negative price");
    expectThrows<std::invalid_argument>([] { Asset("AAPL", 1.0, 0.05, -0.1); }, "negative sigma");

    Asset a("MSFT", 100.0, 0.08, 0.2);
    expect(near(a.price(), 100.0), "asset price getter");
    expectThrows<std::invalid_argument>([&] { a.setPrice(-3.0); }, "setPrice negative");
    a.setPrice(120.0);
    expect(near(a.price(), 120.0), "setPrice update");
}

void testPositionAndAddRemoveValidation() {
    Asset a("AAPL", 200.0, 0.09, 0.25);
    expectThrows<std::invalid_argument>([&] { Position(a, 0.0); }, "position quantity zero");

    Portfolio p;
    expectThrows<std::invalid_argument>([&] { p.addPosition(a, 0.0); }, "addPosition quantity zero");
    p.addPosition(a, 10.0);

    expectThrows<std::invalid_argument>([&] { p.removePosition("AAPL", 0.0); }, "removePosition quantity zero");
    expectThrows<std::out_of_range>([&] { p.removePosition("MSFT", 1.0); }, "removePosition asset missing");
    expectThrows<std::invalid_argument>([&] { p.removePosition("AAPL", 100.0); }, "removePosition quantity exceeds");

    p.removePosition("AAPL", 10.0);
    expect(p.size() == 0, "removePosition erases empty position");
}

void testAssetParameterMismatch() {
    Portfolio p;
    p.addPosition(Asset("AAPL", 100.0, 0.10, 0.20), 5.0);

    // Same name, different mu -> must fail
    expectThrows<std::invalid_argument>(
        [&] { p.addPosition(Asset("AAPL", 101.0, 0.11, 0.20), 1.0); },
        "same-name asset with different mu");

    // Same name, different sigma -> must fail
    expectThrows<std::invalid_argument>(
        [&] { p.addPosition(Asset("AAPL", 102.0, 0.10, 0.25), 1.0); },
        "same-name asset with different sigma");
}

Portfolio samplePortfolio() {
    Portfolio p;
    p.addPosition(Asset("AAPL", 200.0, 0.10, 0.20), 10.0); // value=2000
    p.addPosition(Asset("BOND", 100.0, 0.02, 0.05), 20.0); // value=2000
    return p;
}

void testExpectedReturnAndVolatility() {
    Portfolio p = samplePortfolio();

    // Equal weights here: expected return = 0.5*0.10 + 0.5*0.02 = 0.06
    expect(near(p.expectedReturn(), 0.06), "expected return weighted average");

    const std::vector<std::vector<double>> corr = {
        {1.0, 0.0},
        {0.0, 1.0},
    };

    // variance = (0.5^2*0.2^2) + (0.5^2*0.05^2) = 0.010625
    // vol = sqrt(0.010625)
    const double expectedVar = 0.010625;
    expect(near(p.varianceApprox(corr), expectedVar), "variance approx numeric check");
    expect(near(p.volatilityApprox(corr), std::sqrt(expectedVar)), "volatility approx numeric check");
}

void testCorrelationMatrixErrors() {
    Portfolio p = samplePortfolio();

    expectThrows<std::invalid_argument>(
        [&] {
            p.varianceApprox({{1.0, 0.0}}); // wrong rows
        },
        "wrong number of rows");

    expectThrows<std::invalid_argument>(
        [&] {
            p.varianceApprox({{1.0, 0.0}, {0.0}}); // wrong cols
        },
        "wrong number of cols");

    expectThrows<std::invalid_argument>(
        [&] {
            p.varianceApprox({{0.9, 0.0}, {0.0, 1.0}}); // diagonal not 1
        },
        "diagonal must be 1");

    expectThrows<std::invalid_argument>(
        [&] {
            p.varianceApprox({{1.0, 1.2}, {1.2, 1.0}}); // out of bounds
        },
        "correlation out of bounds");

    expectThrows<std::invalid_argument>(
        [&] {
            p.varianceApprox({{1.0, 0.3}, {0.2, 1.0}}); // non-symmetric
        },
        "correlation matrix symmetry");
}

void testVarianceContributions() {
    Portfolio p = samplePortfolio();
    const std::vector<std::vector<double>> corr = {
        {1.0, 0.3},
        {0.3, 1.0},
    };

    const auto contributions = p.varianceContributionsApprox(corr);
    expect(contributions.size() == 2, "variance contributions size");

    const double totalVar = p.varianceApprox(corr);
    const double sumContrib = contributions[0] + contributions[1];
    expect(near(sumContrib, totalVar), "variance contributions sum to total variance");
}

void testOperatorAccessErrors() {
    Portfolio p;
    expectThrows<std::out_of_range>([&] { (void)p["MISSING"]; }, "operator[] missing asset");
    const Portfolio& cp = p;
    expectThrows<std::out_of_range>([&] { (void)cp["MISSING"]; }, "const operator[] missing asset");
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"Asset validation", testAssetValidation},
        {"Position/add/remove validation", testPositionAndAddRemoveValidation},
        {"Asset parameter mismatch", testAssetParameterMismatch},
        {"Expected return and volatility", testExpectedReturnAndVolatility},
        {"Correlation matrix errors", testCorrelationMatrixErrors},
        {"Variance contributions", testVarianceContributions},
        {"Operator[] errors", testOperatorAccessErrors},
    };

    for (const auto& [name, fn] : tests) {
        try {
            fn();
            ++passed;
            std::cout << "[PASS] " << name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[FAIL] " << name << " -> " << e.what() << "\n";
        }
    }

    std::cout << "\nSummary: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
