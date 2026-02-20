#include "Asset.hpp"
#include "Portfolio.hpp"
#include "Yahoo.hpp"
#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

static Portfolio g_portfolio;
static std::mutex g_mutex;

// ---------- helpers ----------
static std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static bool parseDouble(const std::string& s, double& out) {
    try {
        size_t idx = 0;
        out = std::stod(s, &idx);
        return idx == s.size();
    } catch (...) { return false; }
}

static bool parseSizeT(const std::string& s, std::size_t& out) {
    try {
        size_t idx = 0;
        unsigned long long v = std::stoull(s, &idx);
        if (idx != s.size()) return false;
        out = static_cast<std::size_t>(v);
        return true;
    } catch (...) { return false; }
}

// Parse matrix from textarea:
// rows separated by newline, values separated by spaces or commas.
// Example (3x3):
// 1 0.2 0.6
// 0.2 1 0.1
// 0.6 0.1 1
static std::vector<std::vector<double>> parseMatrixText(const std::string& text) {
    std::vector<std::vector<double>> M;
    std::istringstream iss(text);
    std::string line;

    while (std::getline(iss, line)) {
        // trim
        while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && std::isspace((unsigned char)line[start])) start++;
        line = line.substr(start);

        if (line.empty()) continue;

        // replace commas by spaces
        for (char& c : line) if (c == ',') c = ' ';

        std::istringstream ls(line);
        std::vector<double> row;
        double x;
        while (ls >> x) row.push_back(x);

        if (!row.empty()) M.push_back(row);
    }

    return M;
}

static std::string portfolioTableHTML(const Portfolio& p) {
    std::ostringstream os;
    os << "<table border='1' cellpadding='6' cellspacing='0'>"
       << "<tr><th>Asset</th><th>Qty</th><th>Price</th><th>Mu</th><th>Sigma</th><th>Value</th></tr>";

    for (const auto& name : p.assetOrder()) {
        const auto& pos = p[name];
        os << "<tr>"
           << "<td>" << htmlEscape(name) << "</td>"
           << "<td>" << pos.quantity << "</td>"
           << "<td>" << pos.asset.price() << "</td>"
           << "<td>" << pos.asset.expectedReturn() << "</td>"
           << "<td>" << pos.asset.volatility() << "</td>"
           << "<td>" << pos.value() << "</td>"
           << "</tr>";
    }
    os << "</table>";
    return os.str();
}

static std::string orderHTML(const Portfolio& p) {
    std::ostringstream os;
    os << "<p><b>Order for correlation matrix (stable):</b> ";
    auto ord = p.assetOrder();
    for (std::size_t i = 0; i < ord.size(); ++i) {
        os << htmlEscape(ord[i]) << (i + 1 == ord.size() ? "" : ", ");
    }
    os << "</p>";
    return os.str();
}

static std::string weightsHTML(const Portfolio& p) {
    std::ostringstream os;
    const double total = p.totalValue();
    os << "<p><b>Weights:</b><br/>";
    if (total <= 0.0) {
        os << "Portfolio empty.</p>";
        return os.str();
    }
    for (const auto& name : p.assetOrder()) {
        const auto& pos = p[name];
        const double w = pos.value() / total;
        os << htmlEscape(name) << " : " << w << "<br/>";
    }
    os << "</p>";
    return os.str();
}

static std::string pageHTML(const std::string& message = "") {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::ostringstream os;
    os << "<!doctype html><html><head><meta charset='utf-8'/>"
       << "<title>Portfolio Manager</title></head><body>"
       << "<h2>Portfolio Manager (Local)</h2>";

    if (!message.empty()) {
        os << "<p style='color:#0a0'><b>" << htmlEscape(message) << "</b></p>";
    }

    os << "<h3>Current portfolio</h3>";
    os << portfolioTableHTML(g_portfolio);
    os << "<p><b>Total value:</b> " << g_portfolio.totalValue() << "</p>";
    os << "<p><b>Expected return:</b> " << g_portfolio.expectedReturn() << "</p>";
    os << orderHTML(g_portfolio);
    os << weightsHTML(g_portfolio);

    os << "<hr/>";

    // Add Yahoo
    os << "<h3>Add position (Yahoo Finance)</h3>"
       << "<form action='/add_yahoo' method='get'>"
       << "Ticker: <input name='ticker' placeholder='AAPL'/> "
       << "Qty: <input name='qty' placeholder='10'/> "
       << "<button type='submit'>Add</button>"
       << "</form>";

    // Add manual
    os << "<h3>Add position (Manual)</h3>"
       << "<form action='/add_manual' method='get'>"
       << "Name: <input name='name' placeholder='BOND'/> "
       << "Price: <input name='price' placeholder='100'/> "
       << "Mu: <input name='mu' placeholder='0.03'/> "
       << "Sigma: <input name='sigma' placeholder='0.05'/> "
       << "Qty: <input name='qty' placeholder='50'/> "
       << "<button type='submit'>Add</button>"
       << "</form>";

    // Remove
    os << "<h3>Remove position</h3>"
       << "<form action='/remove' method='get'>"
       << "Name: <input name='name' placeholder='AAPL'/> "
       << "Qty: <input name='qty' placeholder='5'/> "
       << "<button type='submit'>Remove</button>"
       << "</form>";

    os << "<hr/>";

    // Metrics auto
    os << "<h3>Metrics (AUTO correlation from Yahoo)</h3>"
       << "<form action='/metrics_auto' method='get'>"
       << "<button type='submit'>Compute auto corr + volatility</button>"
       << "</form>";

    // Metrics manual corr (to satisfy requirement "corr fourni")
    os << "<h3>Metrics (MANUAL correlation matrix)</h3>"
       << "<form action='/metrics_manual' method='post'>"
       << "<p>Paste matrix (rows separated by newline, values separated by spaces or commas). "
       << "Order = " << htmlEscape([&](){
            std::ostringstream tmp;
            auto ord = g_portfolio.assetOrder();
            for (std::size_t i=0;i<ord.size();++i) tmp<<ord[i]<<(i+1==ord.size()?"":", ");
            return tmp.str();
          }()) << "</p>"
       << "<textarea name='matrix' rows='6' cols='60' placeholder='1 0.2\n0.2 1'></textarea><br/>"
       << "<button type='submit'>Compute volatility (manual corr)</button>"
       << "</form>";

    os << "</body></html>";
    return os.str();
}

// ---------- main ----------
int main() {
    httplib::Server svr;

    // Home
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(pageHTML(), "text/html; charset=utf-8");
    });

    // Add Yahoo
    svr.Get("/add_yahoo", [](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("ticker") || !req.has_param("qty")) {
                res.set_content(pageHTML("Missing ticker/qty."), "text/html; charset=utf-8");
                return;
            }
            std::string ticker = req.get_param_value("ticker");
            std::string qty_s  = req.get_param_value("qty");
            double qty = 0.0;
            if (!parseDouble(qty_s, qty) || qty <= 0.0) {
                res.set_content(pageHTML("Invalid qty."), "text/html; charset=utf-8");
                return;
            }

            Asset a = fetchAssetFromYahoo(ticker);

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_portfolio.addPosition(a, qty);
            }

            res.set_content(pageHTML("Added " + ticker + " from Yahoo."), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // Add manual
    svr.Get("/add_manual", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const char* keys[] = {"name","price","mu","sigma","qty"};
            for (auto k : keys) {
                if (!req.has_param(k)) {
                    res.set_content(pageHTML("Missing parameter(s)."), "text/html; charset=utf-8");
                    return;
                }
            }
            std::string name = req.get_param_value("name");
            double price=0, mu=0, sigma=0, qty=0;
            if (!parseDouble(req.get_param_value("price"), price) ||
                !parseDouble(req.get_param_value("mu"), mu) ||
                !parseDouble(req.get_param_value("sigma"), sigma) ||
                !parseDouble(req.get_param_value("qty"), qty) ||
                qty <= 0.0) {
                res.set_content(pageHTML("Invalid numeric input."), "text/html; charset=utf-8");
                return;
            }

            Asset a(name, price, mu, sigma);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_portfolio.addPosition(a, qty);
            }

            res.set_content(pageHTML("Added " + name + " manually."), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // Remove
    svr.Get("/remove", [](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("name") || !req.has_param("qty")) {
                res.set_content(pageHTML("Missing name/qty."), "text/html; charset=utf-8");
                return;
            }
            std::string name = req.get_param_value("name");
            double qty = 0.0;
            if (!parseDouble(req.get_param_value("qty"), qty) || qty <= 0.0) {
                res.set_content(pageHTML("Invalid qty."), "text/html; charset=utf-8");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_portfolio.removePosition(name, qty);
            }

            res.set_content(pageHTML("Removed " + std::to_string(qty) + " of " + name + "."), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // Metrics auto correlation
    svr.Get("/metrics_auto", [](const httplib::Request&, httplib::Response& res) {
        try {
            std::vector<std::string> tickers;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                tickers = g_portfolio.assetOrder();
            }
            if (tickers.empty()) {
                res.set_content(pageHTML("Portfolio empty."), "text/html; charset=utf-8");
                return;
            }

            auto corr = correlationMatrixFromYahoo(tickers);

            double er=0, vol=0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                er = g_portfolio.expectedReturn();
                vol = g_portfolio.volatilityApprox(corr);
            }

            std::ostringstream msg;
            msg << "AUTO corr computed. Expected return=" << er << " | Volatility=" << vol;
            res.set_content(pageHTML(msg.str()), "text/html; charset=utf-8");

        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // Metrics manual correlation matrix (POST)
    svr.Post("/metrics_manual", [](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("matrix")) {
                res.set_content(pageHTML("Missing matrix."), "text/html; charset=utf-8");
                return;
            }
            std::string text = req.get_param_value("matrix");
            auto M = parseMatrixText(text);

            double er=0, vol=0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                er = g_portfolio.expectedReturn();
                vol = g_portfolio.volatilityApprox(M); // => invalid_argument si dimension incorrecte (exigence)
            }

            std::ostringstream msg;
            msg << "MANUAL corr used. Expected return=" << er << " | Volatility=" << vol;
            res.set_content(pageHTML(msg.str()), "text/html; charset=utf-8");

        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // Run server
    const char* host = "127.0.0.1";
    const int port = 8080;
    std::cout << "Open your browser at: http://" << host << ":" << port << "/\n";
    svr.listen(host, port);
    return 0;
}

