#include "Asset.hpp"
#include <stdexcept>
#include <utility>

Asset::Asset(std::string name, double price, double expected_return, double volatility)
    : name_(std::move(name)), price_(price), mu_(expected_return), sigma_(volatility) {
    if (name_.empty()) throw std::invalid_argument("Asset: name must be non-empty.");
    if (price_ < 0.0) throw std::invalid_argument("Asset: price must be >= 0.");
    if (sigma_ < 0.0) throw std::invalid_argument("Asset: volatility (sigma) must be >= 0.");
}

const std::string& Asset::name() const { return name_; }
double Asset::price() const { return price_; }
double Asset::expectedReturn() const { return mu_; }
double Asset::volatility() const { return sigma_; }

void Asset::setPrice(double price) {
    if (price < 0.0) throw std::invalid_argument("Asset::setPrice: price must be >= 0.");
    price_ = price;
}
