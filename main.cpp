#include "Asset.hpp"
#include "Portfolio.hpp"
#include "Yahoo.hpp"

#include <iostream>
#include <limits>
#include <vector>
#include <iomanip>

static void clearCin() {
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

static void printMenu() {
    std::cout << "\n========== PORTFOLIO MANAGER ==========\n"
              << "1) Add position (manual)\n"
              << "2) Add position (Yahoo Finance fetch)\n"
              << "3) Remove position\n"
              << "4) Show portfolio + order for corr matrix\n"
              << "5) Compute expected return + volatility (enter corr matrix)\n"
              << "6) Merge with demo portfolio (operator+)\n"
              << "7) Compute volatility with AUTO correlation from Yahoo\n"
              << "0) Quit\n";
}

static void showPortfolio(const Portfolio& p) {
    std::cout << "\n--- Portfolio ---\n";
    p.display();
    p.printWeights();

    std::cout << "Order for corr matrix (stable): ";
    auto ord = p.assetOrder();
    auto nameSet = p.assetNameSet();
    for (std::size_t i = 0; i < ord.size(); ++i) {
        std::cout << ord[i] << (i + 1 == ord.size() ? "" : ", ");
    }
    std::cout << "\nUnique assets (set): " << nameSet.size() << "\n";
}

static std::vector<std::vector<double>> readCorrMatrix(std::size_t n) {
    std::cout << "\nEnter correlation matrix " << n << "x" << n << " (row by row).\n"
              << "Example row: 1 0.2 0.6\n";
    std::vector<std::vector<double>> corr(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        std::cout << "Row " << i << ": ";
        for (std::size_t j = 0; j < n; ++j) {
            if (!(std::cin >> corr[i][j])) {
                clearCin();
                throw std::invalid_argument("Invalid input while reading correlation matrix.");
            }
        }
    }
    return corr;
}

int main() {
    try {
        Portfolio p;

        while (true) {
            printMenu();
            std::cout << "Choice: ";
            int choice;
            if (!(std::cin >> choice)) { clearCin(); continue; }

            if (choice == 0) break;

            if (choice == 1) {
                std::string name;
                double price, mu, sigma, qty;

                std::cout << "Name: "; std::cin >> name;
                std::cout << "Price: "; std::cin >> price;
                std::cout << "Expected return (mu, annual): "; std::cin >> mu;
                std::cout << "Stdev (sigma, annual): "; std::cin >> sigma;
                std::cout << "Quantity: "; std::cin >> qty;

                p.addPosition(Asset(name, price, mu, sigma), qty);
                std::cout << "OK.\n";
            }

            else if (choice == 2) {
                std::string ticker;
                double qty;

                std::cout << "Ticker (e.g. AAPL, MSFT, TSLA): ";
                std::cin >> ticker;
                std::cout << "Quantity: ";
                std::cin >> qty;

                std::cout << "Fetching " << ticker << " from Yahoo...\n";
                Asset a = fetchAssetFromYahoo(ticker);

                std::cout << "Fetched: price=" << a.price()
                          << " mu=" << a.expectedReturn()
                          << " sigma=" << a.volatility()
                          << "\n";

                p.addPosition(a, qty);
                std::cout << "Added.\n";
            }

            else if (choice == 3) {
                std::string name;
                double qty;

                std::cout << "Asset name: ";
                std::cin >> name;
                std::cout << "Quantity to remove: ";
                std::cin >> qty;

                p.removePosition(name, qty);
                std::cout << "OK.\n";
            }

            else if (choice == 4) {
                showPortfolio(p);
            }

            else if (choice == 5) {
                if (p.size() == 0) {
                    std::cout << "Portfolio empty.\n";
                    continue;
                }

                auto corr = readCorrMatrix(p.size());

                // Exceptions demandées :
                // - invalid_argument si matrice mal dimensionnée (ou incohérente)
                // - out_of_range si accès invalide (operator[])
                std::cout << "\nExpected return: " << p.expectedReturn() << "\n";
                std::cout << "Volatility approx: " << p.volatilityApprox(corr) << "\n";
            }

            else if (choice == 6) {
                Portfolio p2;
                p2.addPosition(Asset("DEMO_A", 100.0, 0.05, 0.10), 1);
                p2.addPosition(Asset("DEMO_B", 200.0, 0.07, 0.15), 2);

                Portfolio merged = p + p2; // operator+ obligatoire
                std::cout << "Merged portfolio:\n";
                showPortfolio(merged);
            }

            else if (choice == 7) {
                if (p.size() == 0) {
                    std::cout << "Portfolio empty.\n";
                    continue;
                }

                // Ordre stable du portfolio premium (tri lexical via map)
                auto tickers = p.assetOrder();

                std::cout << "Fetching returns and building correlation matrix from Yahoo...\n";
                for (const auto& t : tickers) std::cout << "  - " << t << "\n";

                auto corr = correlationMatrixFromYahoo(tickers);

                std::cout << "\nAuto correlation matrix (order = assetOrder):\n";
                for (std::size_t i = 0; i < corr.size(); ++i) {
                    for (std::size_t j = 0; j < corr.size(); ++j) {
                        std::cout << std::fixed << std::setprecision(3) << corr[i][j] << " ";
                    }
                    std::cout << "\n";
                }

                std::cout << "\nExpected return: " << p.expectedReturn() << "\n";
                std::cout << "Volatility approx (auto corr): " << p.volatilityApprox(corr) << "\n";
            }

            else {
                std::cout << "Unknown choice.\n";
            }
        }

        std::cout << "Bye.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
