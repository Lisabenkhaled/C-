#ifndef YAHOO_HPP
#define YAHOO_HPP

#include "Asset.hpp"
#include <string>
#include <vector>

// Asset depuis Yahoo : price = dernier close, mu/sigma annualisés depuis 1 an (log-returns)
Asset fetchAssetFromYahoo(const std::string& ticker);

// Renvoie les log-returns journaliers (1 an) pour un ticker
std::vector<double> fetchDailyLogReturns1y(const std::string& ticker);

// Calcule la matrice de corrélation à partir des log-returns Yahoo,
// dans l'ordre exact des tickers fournis
std::vector<std::vector<double>> correlationMatrixFromYahoo(const std::vector<std::string>& tickers);

#endif
