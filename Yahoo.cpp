#include "Yahoo.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// #pragma comment(lib, "winhttp.lib") // inutile avec g++ + -lwinhttp

static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], size_needed);
    return w;
}

static std::string httpGetWinHTTP(const std::wstring& host, const std::wstring& path) {
    HINTERNET hSession = WinHttpOpen(L"PortfolioProject/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) throw std::runtime_error("WinHttpOpen failed");

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpConnect failed");
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    BOOL ok = WinHttpSendRequest(hRequest,
                                 WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0,
                                 0, 0);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpSendRequest failed");
    }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpReceiveResponse failed");
    }

    std::string response;
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;

        std::vector<char> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) break;

        response.append(buffer.data(), buffer.data() + dwDownloaded);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (response.empty()) throw std::runtime_error("Empty HTTP response");
    return response;
}

// parsing ciblé : extrait "close":[ ... ]
static std::vector<double> extractCloseArray(const std::string& json) {
    const std::string key = "\"close\":[";
    std::size_t pos = json.find(key);
    if (pos == std::string::npos) throw std::runtime_error("Yahoo JSON: could not find close array.");
    pos += key.size();

    std::size_t end = json.find(']', pos);
    if (end == std::string::npos) throw std::runtime_error("Yahoo JSON: malformed close array.");

    std::string arr = json.substr(pos, end - pos);

    std::vector<double> closes;
    std::stringstream ss(arr);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.find("null") != std::string::npos) continue;

        while (!token.empty() && (token.front() == ' ' || token.front() == '\n' || token.front() == '\r')) token.erase(token.begin());
        while (!token.empty() && (token.back() == ' ' || token.back() == '\n' || token.back() == '\r')) token.pop_back();

        if (token.empty()) continue;
        closes.push_back(std::stod(token));
    }

    if (closes.size() < 30) {
        throw std::runtime_error("Not enough close prices returned by Yahoo (need ~30+).");
    }
    return closes;
}

static std::vector<double> closesToDailyLogReturns(const std::vector<double>& closes) {
    std::vector<double> r;
    r.reserve(closes.size() - 1);
    for (std::size_t i = 1; i < closes.size(); ++i) {
        if (closes[i-1] <= 0.0 || closes[i] <= 0.0) continue;
        r.push_back(std::log(closes[i] / closes[i-1]));
    }
    if (r.size() < 20) throw std::runtime_error("Not enough valid returns to compute stats.");
    return r;
}

static std::pair<double,double> annualMuSigmaFromDailyLogReturns(const std::vector<double>& r) {
    double mean = 0.0;
    for (double x : r) mean += x;
    mean /= (double)r.size();

    double var = 0.0;
    for (double x : r) var += (x - mean) * (x - mean);
    var /= (double)(r.size() - 1);
    double stdev = std::sqrt(std::max(0.0, var));

    // annualize (252 trading days)
    return { mean * 252.0, stdev * std::sqrt(252.0) };
}

Asset fetchAssetFromYahoo(const std::string& ticker) {
    const std::wstring host = L"query1.finance.yahoo.com";
    std::wstring path = L"/v8/finance/chart/" + toWide(ticker) + L"?range=1y&interval=1d";

    std::string json = httpGetWinHTTP(host, path);
    auto closes = extractCloseArray(json);
    auto r = closesToDailyLogReturns(closes);

    double lastPrice = closes.back();
    auto [mu, sigma] = annualMuSigmaFromDailyLogReturns(r);

    return Asset(ticker, lastPrice, mu, sigma);
}

std::vector<double> fetchDailyLogReturns1y(const std::string& ticker) {
    const std::wstring host = L"query1.finance.yahoo.com";
    std::wstring path = L"/v8/finance/chart/" + toWide(ticker) + L"?range=1y&interval=1d";

    std::string json = httpGetWinHTTP(host, path);
    auto closes = extractCloseArray(json);
    return closesToDailyLogReturns(closes);
}

// corr(i,j) = cov(r_i, r_j) / (sd_i sd_j)
static double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.size() < 2) {
        throw std::invalid_argument("correlation: series size mismatch or too small.");
    }

    const std::size_t n = a.size();
    double meanA = 0.0, meanB = 0.0;
    for (std::size_t i = 0; i < n; ++i) { meanA += a[i]; meanB += b[i]; }
    meanA /= (double)n; meanB /= (double)n;

    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        sxx += da * da;
        syy += db * db;
        sxy += da * db;
    }

    if (sxx <= 0.0 || syy <= 0.0) return 0.0; // série quasi constante
    return sxy / std::sqrt(sxx * syy);
}

std::vector<std::vector<double>> correlationMatrixFromYahoo(const std::vector<std::string>& tickers) {
    const std::size_t n = tickers.size();
    if (n == 0) return {};

    // Fetch returns for each ticker
    std::vector<std::vector<double>> returns(n);
    for (std::size_t i = 0; i < n; ++i) {
        returns[i] = fetchDailyLogReturns1y(tickers[i]);
    }

    // Align lengths (simple approach): keep last minLen values
    std::size_t minLen = returns[0].size();
    for (std::size_t i = 1; i < n; ++i) minLen = std::min(minLen, returns[i].size());
    if (minLen < 20) throw std::runtime_error("Not enough aligned returns to compute correlation matrix.");

    for (std::size_t i = 0; i < n; ++i) {
        if (returns[i].size() > minLen) {
            returns[i].erase(returns[i].begin(), returns[i].end() - (long long)minLen);
        }
    }

    std::vector<std::vector<double>> corr(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        corr[i][i] = 1.0;
        for (std::size_t j = i + 1; j < n; ++j) {
            double c = correlation(returns[i], returns[j]);
            // clamp safety
            if (c < -1.0) c = -1.0;
            if (c > 1.0) c = 1.0;
            corr[i][j] = c;
            corr[j][i] = c;
        }
    }
    return corr;
}
