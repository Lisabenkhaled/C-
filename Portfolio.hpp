#ifndef PORTFOLIO_HPP
#define PORTFOLIO_HPP

#include "Asset.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

struct Position {
    Asset asset;
    double quantity;

    Position(const Asset& a, double q);
    double value() const;
};

class Portfolio {
private:
    // Premium: ordre stable (tri lexical) => corr matrix reproductible
    std::map<std::string, Position> positions_;

    static void validateCorrelationMatrix(const std::vector<std::vector<double>>& corr, std::size_t n);

public:
    void addPosition(const Asset& a, double quantity);
    void removePosition(const std::string& assetName, double quantity);
    void printWeights() const;

    Position& operator[](const std::string& assetName);
    const Position& operator[](const std::string& assetName) const;

    friend Portfolio operator+(const Portfolio& lhs, const Portfolio& rhs);

    std::size_t size() const;
    double totalValue() const;
    double expectedReturn() const;

    double varianceApprox(const std::vector<std::vector<double>>& corr) const;
    double volatilityApprox(const std::vector<std::vector<double>>& corr) const;
    std::vector<double> varianceContributionsApprox(const std::vector<std::vector<double>>& corr) const;
    
    // Utile pour construire la matrice dans le bon ordre
    std::set<std::string> assetNameSet() const;
    std::vector<std::string> assetOrder() const;
    void display() const;
};

#endif
