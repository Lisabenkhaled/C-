#include "Portfolio.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <limits>

Position::Position(const Asset& a, double q) : asset(a), quantity(q) {
    if (q <= 0.0) throw std::invalid_argument("Position: quantity must be > 0.");
}

double Position::value() const { return asset.price() * quantity; }

std::size_t Portfolio::size() const { return positions_.size(); }

void Portfolio::addPosition(const Asset& a, double quantity) {
    if (quantity <= 0.0) throw std::invalid_argument("addPosition: quantity must be > 0.");

    auto it = positions_.find(a.name());
    if (it == positions_.end()) {
        positions_.emplace(a.name(), Position(a, quantity));
        return;
    }

    // Premium: vérification de cohérence des paramètres d’actif
    const Asset& existing = it->second.asset;
    const double eps = 1e-12;
    if (std::fabs(existing.expectedReturn() - a.expectedReturn()) > eps ||
        std::fabs(existing.volatility() - a.volatility()) > eps) {
        throw std::invalid_argument("addPosition: asset parameters mismatch for same name (mu/sigma).");
    }

    // Le prix peut varier dans le temps, on accepte le prix du "existing" ici.
    it->second.quantity += quantity;
}

void Portfolio::removePosition(const std::string& assetName, double quantity) {
    if (quantity <= 0.0) throw std::invalid_argument("removePosition: quantity must be > 0.");

    auto it = positions_.find(assetName);
    if (it == positions_.end()) throw std::out_of_range("removePosition: asset not found: " + assetName);

    if (quantity > it->second.quantity) {
        throw std::invalid_argument("removePosition: quantity exceeds current position.");
    }

    it->second.quantity -= quantity;
    if (it->second.quantity <= 0.0) positions_.erase(it);
}

Position& Portfolio::operator[](const std::string& assetName) {
    auto it = positions_.find(assetName);
    if (it == positions_.end()) throw std::out_of_range("operator[]: asset not found: " + assetName);
    return it->second;
}

const Position& Portfolio::operator[](const std::string& assetName) const {
    auto it = positions_.find(assetName);
    if (it == positions_.end()) throw std::out_of_range("operator[] const: asset not found: " + assetName);
    return it->second;
}

Portfolio operator+(const Portfolio& lhs, const Portfolio& rhs) {
    Portfolio out = lhs;
    for (const auto& [name, pos] : rhs.positions_) {
        out.addPosition(pos.asset, pos.quantity);
    }
    return out;
}

double Portfolio::totalValue() const {
    double total = 0.0;
    for (const auto& [_, pos] : positions_) total += pos.value();
    return total;
}

double Portfolio::expectedReturn() const {
    const double total = totalValue();
    if (total <= 0.0) return 0.0;

    double er = 0.0;
    for (const auto& [_, pos] : positions_) {
        const double w = pos.value() / total;
        er += w * pos.asset.expectedReturn();
    }
    return er;
}

std::vector<std::string> Portfolio::assetOrder() const {
    std::vector<std::string> order;
    order.reserve(positions_.size());
    for (const auto& [name, _] : positions_) order.push_back(name);
    return order;
}

void Portfolio::validateCorrelationMatrix(const std::vector<std::vector<double>>& corr, std::size_t n) {
    if (corr.size() != n) {
        throw std::invalid_argument("varianceApprox: correlation matrix wrong number of rows.");
    }
    for (const auto& row : corr) {
        if (row.size() != n) {
            throw std::invalid_argument("varianceApprox: correlation matrix wrong number of columns.");
        }
    }

    const double epsSym = 1e-10;
    const double epsDiag = 1e-10;

    for (std::size_t i = 0; i < n; ++i) {
        // diag ~ 1
        if (std::fabs(corr[i][i] - 1.0) > epsDiag) {
            throw std::invalid_argument("varianceApprox: correlation matrix diagonal must be 1.");
        }
        for (std::size_t j = i + 1; j < n; ++j) {
            const double a = corr[i][j];
            const double b = corr[j][i];

            // bounds
            if (a < -1.0 || a > 1.0 || b < -1.0 || b > 1.0) {
                throw std::invalid_argument("varianceApprox: correlation must be in [-1, 1].");
            }
            // symmetry
            if (std::fabs(a - b) > epsSym) {
                throw std::invalid_argument("varianceApprox: correlation matrix must be symmetric.");
            }
        }
    }
}

double Portfolio::varianceApprox(const std::vector<std::vector<double>>& corr) const {
    const std::size_t n = positions_.size();
    if (n == 0) return 0.0;

    validateCorrelationMatrix(corr, n);

    const double total = totalValue();
    if (total <= 0.0) return 0.0;

    // map order => stable
    std::vector<const Position*> pos;
    pos.reserve(n);
    for (const auto& [_, p] : positions_) pos.push_back(&p);

    std::vector<double> w(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) w[i] = pos[i]->value() / total;

    double var = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double si = pos[i]->asset.volatility();
        for (std::size_t j = 0; j < n; ++j) {
            const double sj = pos[j]->asset.volatility();
            var += w[i] * w[j] * corr[i][j] * si * sj;
        }
    }
    return var;
}

double Portfolio::volatilityApprox(const std::vector<std::vector<double>>& corr) const {
    const double v = varianceApprox(corr);
    return std::sqrt(std::max(0.0, v));
}

std::vector<double> Portfolio::varianceContributionsApprox(const std::vector<std::vector<double>>& corr) const {
    const std::size_t n = positions_.size();
    std::vector<double> contributions(n, 0.0);
    if (n == 0) return contributions;

    validateCorrelationMatrix(corr, n);

    const double total = totalValue();
    if (total <= 0.0) return contributions;

    std::vector<const Position*> pos;
    pos.reserve(n);
    for (const auto& [_, p] : positions_) pos.push_back(&p);

    std::vector<double> w(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) w[i] = pos[i]->value() / total;

    for (std::size_t i = 0; i < n; ++i) {
        const double si = pos[i]->asset.volatility();
        double covRowDotW = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double sj = pos[j]->asset.volatility();
            covRowDotW += corr[i][j] * si * sj * w[j];
        }
        contributions[i] = w[i] * covRowDotW;
    }

    return contributions;
}

void Portfolio::display() const {
    std::cout << "Asset order for corr matrix (stable):\n";
    std::size_t k = 0;
    for (const auto& [name, pos] : positions_) {
        std::cout << "  [" << k++ << "] " << name
                  << " | qty=" << pos.quantity
                  << " | price=" << pos.asset.price()
                  << " | mu=" << pos.asset.expectedReturn()
                  << " | sigma=" << pos.asset.volatility()
                  << " | value=" << pos.value()
                  << "\n";
    }
    std::cout << "Total value:     " << totalValue() << "\n";
    std::cout << "Expected return: " << expectedReturn() << "\n";
}

void Portfolio::printWeights() const {
    const double total = totalValue();
    if (total <= 0.0) {
        std::cout << "Weights: portfolio is empty.\n";
        return;
    }
    std::cout << "Weights:\n";
    for (const auto& [name, pos] : positions_) {
        const double w = pos.value() / total;
        std::cout << "  " << name << " : " << w << "\n";
    }
}
