#ifndef ASSET_HPP
#define ASSET_HPP

#include <string>

class Asset {
private:
    std::string name_;
    double price_;
    double mu_;
    double sigma_;

public:
    Asset(std::string name, double price, double expected_return, double volatility);

    const std::string& name() const;
    double price() const;
    double expectedReturn() const;
    double volatility() const;

    void setPrice(double price);
};

#endif
