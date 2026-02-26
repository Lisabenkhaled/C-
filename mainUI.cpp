#include "Asset.hpp"
#include "Portfolio.hpp"
#include "Yahoo.hpp"
#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <limits>
#include <cstdint>
#include <string>
#include <vector>

static Portfolio g_portfolio;
static std::mutex g_mutex;
static std::vector<std::vector<double>> g_last_corr;
static std::vector<std::string> g_last_corr_labels;
static std::string g_last_corr_source;
static bool g_has_last_corr = false;
static bool g_has_last_what_if = false;
static std::string g_last_what_if_html;
static bool g_has_last_optimization = false;
static std::string g_last_optimization_html;

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


static std::string trimCopy(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

static std::vector<std::string> splitCSVLine(const std::string& line, char delimiter) {
    std::vector<std::string> out;
    std::string cell;
    bool inQuotes = false;

    for (char c : line) {
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes && c == delimiter) {
            out.push_back(trimCopy(cell));
            cell.clear();
            continue;
        }
        cell.push_back(c);
    }
    out.push_back(trimCopy(cell));
    return out;
}

static std::string exportPortfolioCSV(const Portfolio& p,
                                      const std::vector<std::vector<double>>& corr,
                                      bool corrAvailable) {
    std::ostringstream os;
    os << "name,qty,price,mu,sigma,value\n";
    for (const auto& name : p.assetOrder()) {
        const auto& pos = p[name];
        os << name << ","
           << pos.quantity << ","
           << pos.asset.price() << ","
           << pos.asset.expectedReturn() << ","
           << pos.asset.volatility() << ","
           << pos.value() << "\n";
    }

    os << "\nmetric,value\n";
    os << "total_value," << p.totalValue() << "\n";
    os << "expected_return," << p.expectedReturn() << "\n";
    if (corrAvailable) {
        os << "volatility," << p.volatilityApprox(corr) << "\n";
        const auto ord = p.assetOrder();
        const auto contrib = p.varianceContributionsApprox(corr);
        const double totalVar = p.varianceApprox(corr);
        if (totalVar > 0.0 && contrib.size() == ord.size()) {
            for (std::size_t i = 0; i < ord.size(); ++i) {
                os << "risk_share_" << ord[i] << "," << (contrib[i] / totalVar) << "\n";
            }
        }
    }
    return os.str();
}

static Portfolio portfolioFromCSVText(const std::string& csvText) {
    std::istringstream iss(csvText);
    std::string line;
    std::vector<std::string> lines;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trimCopy(line).empty()) lines.push_back(line);
    }
    if (lines.empty()) throw std::invalid_argument("CSV empty.");

    char delimiter = ',';
    if (std::count(lines[0].begin(), lines[0].end(), ';') >
        std::count(lines[0].begin(), lines[0].end(), ',')) {
        delimiter = ';';
    }

    auto header = splitCSVLine(lines[0], delimiter);
    bool hasHeader = header.size() >= 5;
    if (hasHeader) {
        std::string h0 = header[0];
        for (char& c : h0) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        hasHeader = (h0 == "name" || h0 == "asset");
    }

    Portfolio imported;
    std::size_t start = hasHeader ? 1 : 0;
    std::size_t rowCount = 0;
    for (std::size_t i = start; i < lines.size(); ++i) {
        auto cols = splitCSVLine(lines[i], delimiter);
        if (cols.size() < 5) {
            throw std::invalid_argument("CSV line " + std::to_string(i + 1) + ": expected 5 columns (name,price,mu,sigma,qty).");
        }

        std::string name = cols[0];
        double price = 0.0, mu = 0.0, sigma = 0.0, qty = 0.0;
        if (!parseDouble(cols[1], price) || !parseDouble(cols[2], mu) ||
            !parseDouble(cols[3], sigma) || !parseDouble(cols[4], qty)) {
            throw std::invalid_argument("CSV line " + std::to_string(i + 1) + ": invalid numeric value.");
        }
        imported.addPosition(Asset(name, price, mu, sigma), qty);
        rowCount++;
    }

    if (rowCount == 0) throw std::invalid_argument("CSV contains no data row.");
    return imported;
}


struct PortfolioPoint {
    std::vector<double> weights;
    double expectedReturn = 0.0;
    double volatility = 0.0;
    double score = 0.0;
};

static double expectedReturnFromWeights(const std::vector<double>& w, const std::vector<double>& mu) {
    double er = 0.0;
    for (std::size_t i = 0; i < w.size(); ++i) er += w[i] * mu[i];
    return er;
}

static double volatilityFromWeights(const std::vector<double>& w,
                                    const std::vector<double>& sigma,
                                    const std::vector<std::vector<double>>& corr) {
    double var = 0.0;
    for (std::size_t i = 0; i < w.size(); ++i) {
        for (std::size_t j = 0; j < w.size(); ++j) {
            var += w[i] * w[j] * sigma[i] * sigma[j] * corr[i][j];
        }
    }
    return std::sqrt(std::max(0.0, var));
}

static double random01(std::uint64_t& state) {
    state = state * 6364136223846793005ULL + 1ULL;
    return static_cast<double>((state >> 11) & ((1ULL << 53) - 1)) / static_cast<double>(1ULL << 53);
}

static std::vector<double> randomLongOnlyWeights(std::size_t n, std::uint64_t& state) {
    std::vector<double> w(n, 0.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = 1e-6 + random01(state);
        sum += w[i];
    }
    if (sum <= 0.0) return w;
    for (double& x : w) x /= sum;
    return w;
}

static std::string optimizationChartSVG(const std::vector<PortfolioPoint>& candidates,
                                        const PortfolioPoint& current,
                                        const PortfolioPoint& best,
                                        const std::string& objective) {
    if (candidates.empty()) return "";

    double minVol = std::numeric_limits<double>::max();
    double maxVol = 0.0;
    double minRet = std::numeric_limits<double>::max();
    double maxRet = -std::numeric_limits<double>::max();

    auto absorb = [&](double vol, double er) {
        minVol = std::min(minVol, vol);
        maxVol = std::max(maxVol, vol);
        minRet = std::min(minRet, er);
        maxRet = std::max(maxRet, er);
    };

    for (const auto& p : candidates) absorb(p.volatility, p.expectedReturn);
    absorb(current.volatility, current.expectedReturn);
    absorb(best.volatility, best.expectedReturn);

    const double w = 760.0;
    const double h = 320.0;
    const double pad = 35.0;
    const double xSpan = std::max(1e-9, maxVol - minVol);
    const double ySpan = std::max(1e-9, maxRet - minRet);

    auto px = [&](double vol) { return pad + (vol - minVol) / xSpan * (w - 2.0 * pad); };
    auto py = [&](double er) { return h - pad - (er - minRet) / ySpan * (h - 2.0 * pad); };

    std::ostringstream os;
    os << "<div><b>Efficient-like cloud (x=risk, y=return)</b><br/>";
    os << "<svg width='" << w << "' height='" << h << "' viewBox='0 0 " << w << " " << h << "' "
       << "style='border:1px solid #d1d5db;border-radius:10px;background:#fff'>";

    os << "<line x1='" << pad << "' y1='" << (h-pad) << "' x2='" << (w-pad)
       << "' y2='" << (h-pad) << "' stroke='#9ca3af'/>";
    os << "<line x1='" << pad << "' y1='" << pad << "' x2='" << pad
       << "' y2='" << (h-pad) << "' stroke='#9ca3af'/>";

    for (const auto& p : candidates) {
        os << "<circle cx='" << px(p.volatility) << "' cy='" << py(p.expectedReturn)
           << "' r='2.2' fill='#94a3b8' opacity='0.55'/>";
    }

    os << "<circle cx='" << px(current.volatility) << "' cy='" << py(current.expectedReturn)
       << "' r='5' fill='#2563eb'/>";
    os << "<circle cx='" << px(best.volatility) << "' cy='" << py(best.expectedReturn)
       << "' r='6' fill='#dc2626'/>";

    os << "<text x='" << (px(current.volatility)+8) << "' y='" << (py(current.expectedReturn)-6)
       << "' font-size='12' fill='#1d4ed8'>Current</text>";
    os << "<text x='" << (px(best.volatility)+8) << "' y='" << (py(best.expectedReturn)-6)
       << "' font-size='12' fill='#b91c1c'>Best (" << htmlEscape(objective) << ")</text>";

    os << "</svg></div>";
    return os.str();
}

static std::string fmt(double x, int precision);
static std::string portfolioTableHTML(const Portfolio& p) {
    std::ostringstream os;
    os << "<table border='1' cellpadding='6' cellspacing='0'>"
       << "<tr><th>Asset</th><th>Qty</th><th>Price</th><th>Mu (%)</th><th>Sigma (%)</th><th>Value</th></tr>";

    for (const auto& name : p.assetOrder()) {
        const auto& pos = p[name];
        os << "<tr>"
           << "<td>" << htmlEscape(name) << "</td>"
           << "<td>" << pos.quantity << "</td>"
           << "<td>" << pos.asset.price() << "</td>"
           << "<td>" << fmt(pos.asset.expectedReturn() * 100.0, 2) << "%</td>"
           << "<td>" << fmt(pos.asset.volatility() * 100.0, 2) << "%</td>"
           << "<td>" << pos.value() << "</td>"
           << "</tr>";
    }
    os << "</table>";
    return os.str();
}
static std::string fmt(double x, int precision = 4) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << x;
    return os.str();
}

static std::string fmtPercent(double x, int precision = 2) {
    return fmt(x * 100.0, precision) + "%";
}

static bool hasCompatibleMatrixSize(const std::vector<std::vector<double>>& matrix, std::size_t n) {
    if (matrix.size() != n) return false;
    for (const auto& row : matrix) {
        if (row.size() != n) return false;
    }
    return true;
}

static std::string corrColor(double c) {
    c = std::max(-1.0, std::min(1.0, c));
    int r = 255;
    int g = 255;
    int b = 255;

    if (c < 0.0) {
        const double t = 1.0 + c; // [-1,0] -> [0,1]
        r = 255;
        g = static_cast<int>(235.0 * t + 20.0 * (1.0 - t));
        b = static_cast<int>(235.0 * t + 20.0 * (1.0 - t));
    } else {
        const double t = c; // [0,1]
        r = static_cast<int>(235.0 * (1.0 - t) + 20.0 * t);
        g = static_cast<int>(255.0);
        b = static_cast<int>(235.0 * (1.0 - t) + 20.0 * t);
    }

    std::ostringstream os;
    os << "rgb(" << r << "," << g << "," << b << ")";
    return os.str();
}

static std::string correlationMatrixHTML(const std::vector<std::vector<double>>& corr,
                                         const std::vector<std::string>& labels,
                                         const std::string& source) {
    if (corr.empty() || labels.empty()) return "";

    std::ostringstream os;
    os << "<div class='card'>"
       << "<h3>Correlation matrix";
    if (!source.empty()) os << " <span class='muted'>[" << htmlEscape(source) << "]</span>";
    os << "</h3>"
       << "<p class='muted'>Code couleur: rouge = corrélation négative, blanc = neutre, vert = corrélation positive.</p>"
       << "<table class='corr-table'><tr><th></th>";

    for (const auto& label : labels) {
        os << "<th>" << htmlEscape(label) << "</th>";
    }
    os << "</tr>";

    for (std::size_t i = 0; i < corr.size(); ++i) {
        os << "<tr><th>" << htmlEscape(labels[i]) << "</th>";
        for (std::size_t j = 0; j < corr[i].size(); ++j) {
            os << "<td style='background:" << corrColor(corr[i][j]) << "'>"
               << fmt(corr[i][j], 3) << "</td>";
        }
        os << "</tr>";
    }

    os << "</table>"
       << "<div class='legend'>"
       << "<span>-1.0</span><div class='legend-bar'></div><span>+1.0</span>"
       << "</div></div>";
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

static std::string whatIfResultHTML() {
    if (!g_has_last_what_if) return "";
    std::ostringstream os;
    os << "<div class='card'><h3>What-if simulation result</h3>" << g_last_what_if_html << "</div>";
    return os.str();
}

static std::string optimizationResultHTML() {
    if (!g_has_last_optimization) return "";
    std::ostringstream os;
    os << "<div class='card'><h3>Optimization result</h3>" << g_last_optimization_html << "</div>";
    return os.str();
}

static std::string riskBreakdownHTML(const Portfolio& p,
                                     const std::vector<std::vector<double>>& corr,
                                     bool corrAvailable) {
    std::ostringstream os;
    os << "<p><b>Risk contribution (variance decomposition):</b><br/>";

    const auto names = p.assetOrder();
    if (names.empty()) {
        os << "Portfolio empty.</p>";
        return os.str();
    }

    if (!corrAvailable) {
        os << "N/A (compute metrics first).</p>";
        return os.str();
    }

    const auto contrib = p.varianceContributionsApprox(corr);
    const double totalVariance = p.varianceApprox(corr);
    if (totalVariance <= 0.0 || contrib.size() != names.size()) {
        os << "N/A (variance is zero).</p>";
        return os.str();
    }

    for (std::size_t i = 0; i < names.size(); ++i) {
        const double share = contrib[i] / totalVariance;
        os << htmlEscape(names[i]) << " : " << fmtPercent(share, 2)
           << " of total risk" << "<br/>";
    }
    os << "</p>";
    return os.str();
}

static std::string pageHTML(const std::string& message = "") {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream os;

    os << "<!doctype html><html><head><meta charset='utf-8'/>"
    << "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
       << "<title>Portfolio Manager</title>"
       << "<style>"
       << "body{font-family:Inter,Segoe UI,Arial,sans-serif;margin:0;background:#f4f7fb;color:#1f2937;}"
       << ".container{max-width:1100px;margin:32px auto;padding:0 16px;}"
       << "h1{margin:0 0 18px;font-size:34px;}h3{margin:0 0 12px;}"
       << ".card{background:#fff;border:1px solid #e5e7eb;border-radius:14px;padding:18px;margin-bottom:16px;box-shadow:0 8px 24px rgba(0,0,0,.05);}"
       << ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px;}"
       << ".metrics{display:flex;gap:12px;flex-wrap:wrap;margin:12px 0;}"
       << ".metric{background:#f8fafc;border:1px solid #e2e8f0;border-radius:10px;padding:10px 14px;min-width:190px;}"
       << "table{width:100%;border-collapse:collapse;}th,td{padding:10px;border-bottom:1px solid #e5e7eb;text-align:left;}"
       << "th{background:#f8fafc;font-weight:700;}"
       << ".corr-table th,.corr-table td{text-align:center;border:1px solid #d1d5db;}"
       << ".legend{display:flex;align-items:center;gap:8px;margin-top:10px;color:#6b7280;font-size:13px;}"
       << ".legend-bar{height:10px;flex:1;border-radius:999px;background:linear-gradient(90deg,rgb(255,20,20),rgb(245,245,245),rgb(20,255,20));border:1px solid #d1d5db;}"
       << "form{display:flex;flex-wrap:wrap;gap:8px;align-items:center;}"
       << "input,textarea{border:1px solid #cbd5e1;border-radius:8px;padding:8px 10px;font:inherit;}"
       << "input{min-width:130px;}textarea{width:100%;resize:vertical;}"
       << "button{background:#2563eb;color:white;border:none;border-radius:8px;padding:9px 14px;font-weight:600;cursor:pointer;}"
       << "button:hover{background:#1d4ed8;}"
       << ".banner{padding:12px 14px;border-radius:10px;margin-bottom:14px;font-weight:600;}"
       << ".ok{background:#ecfdf3;border:1px solid #86efac;color:#14532d;}"
       << ".err{background:#fef2f2;border:1px solid #fca5a5;color:#7f1d1d;}"
       << ".muted{color:#6b7280;font-size:14px;}"
       << "</style></head><body><div class='container'>"
       << "<h1>Portfolio Manager </h1>";
    if (!message.empty()) {
        const bool isError = message.rfind("Error:", 0) == 0;
        os << "<div class='banner " << (isError ? "err" : "ok") << "'>" << htmlEscape(message) << "</div>";
    }

    os << "<div class='card'><h3>Current portfolio</h3>";
    os << portfolioTableHTML(g_portfolio);
    
    const double expectedReturn = g_portfolio.expectedReturn();
    bool hasVolatility = false;
    double volatility = 0.0;
    const auto assetCount = g_portfolio.assetOrder().size();
    if (g_has_last_corr && hasCompatibleMatrixSize(g_last_corr, assetCount)) {
        volatility = g_portfolio.volatilityApprox(g_last_corr);
        hasVolatility = true;
    }

    os << "<div class='metrics'>"
       << "<div class='metric'><b>Total value</b><br/>" << fmt(g_portfolio.totalValue(), 2) << "</div>"
       << "<div class='metric'><b>Expected return</b><br/>" << fmtPercent(expectedReturn, 2) << "</div>"
       << "<div class='metric'><b>Volatility</b><br/>"
       << (hasVolatility ? fmtPercent(volatility, 2) : std::string("N/A (compute metrics first)"))
       << "</div>"
       << "</div>";
    os << orderHTML(g_portfolio);
    os << weightsHTML(g_portfolio);
    os << riskBreakdownHTML(g_portfolio, g_last_corr, g_has_last_corr && hasCompatibleMatrixSize(g_last_corr, assetCount));
    os << "</div>";

    if (g_has_last_corr) {
        os << correlationMatrixHTML(g_last_corr, g_last_corr_labels, g_last_corr_source);
    }
    os << whatIfResultHTML();
    os << optimizationResultHTML();

    os << "<div class='grid'>";
    os << "<div class='card'><h3>Add position (Yahoo Finance)</h3>"
       << "<form action='/add_yahoo' method='get'>"
       << "Ticker: <input name='ticker' placeholder='AAPL'/> "
       << "Qty: <input name='qty' placeholder='10'/> "
       << "<button type='submit'>Add</button>"
       << "</form></div>";

    os << "<div class='card'><h3>Add position (Manual)</h3>"
       << "<form action='/add_manual' method='get'>"
       << "Name: <input name='name' placeholder='BOND'/> "
       << "Price: <input name='price' placeholder='100'/> "
       << "Mu: <input name='mu' placeholder='0.03'/> "
       << "Sigma: <input name='sigma' placeholder='0.05'/> "
       << "Qty: <input name='qty' placeholder='50'/> "
       << "<button type='submit'>Add</button>"
       << "</form></div>";

    os << "<div class='card'><h3>Remove position</h3>"
       << "<form action='/remove' method='get'>"
       << "Name: <input name='name' placeholder='AAPL'/> "
       << "Qty: <input name='qty' placeholder='5'/> "
       << "<button type='submit'>Remove</button>"
       << "</form></div>";

    // Metrics auto
    os << "<div class='card'><h3>Metrics (AUTO correlation from Yahoo)</h3>"
       << "<form action='/metrics_auto' method='get'>"
       << "<button type='submit'>Compute auto corr + volatility</button>"
       << "</form></div>";

    // Metrics manual corr (to satisfy requirement "corr fourni")
    os << "<div class='card'><h3>Metrics (MANUAL correlation matrix)</h3>"
       << "<form action='/metrics_manual' method='post'>"
       << "<p>Paste matrix (rows separated by newline, values separated by spaces or commas). "
       << "Order = " << htmlEscape([&](){
            std::ostringstream tmp;
            auto ord = g_portfolio.assetOrder();
            for (std::size_t i=0;i<ord.size();++i) tmp<<ord[i]<<(i+1==ord.size()?"":", ");
            return tmp.str();
          }()) << "</p>"
       << "<textarea name='matrix' rows='6' placeholder='1 0.2\n0.2 1'></textarea><br/>"
       << "<button type='submit'>Compute volatility (manual corr)</button>"
       << "</form></div>";
    
    
    os << "<div class='card'><h3>What-if simulation</h3>"
       << "<form action='/what_if' method='get'>"
       << "Name: <input name='name' placeholder='AAPL'/> "
       << "Qty delta (+/-): <input name='qty_delta' placeholder='10 or -5'/> "
       << "<button type='submit'>Simulate</button>"
       << "</form>"
       << "<p class='muted'>Positive delta = add position, negative delta = remove quantity.</p>"
       << "</div>";

    os << "<div class='card'><h3>Import portfolio (Excel CSV)</h3>"
       << "<form action='/import_csv' method='post'>"
       << "<textarea name='csv_text' rows='8' placeholder='name,price,mu,sigma,qty&#10;AAPL,200,0.08,0.20,10'></textarea><br/>"
       << "<button type='submit'>Import CSV</button>"
       << "</form>"
       << "<p class='muted'>From Excel: Save as CSV, open file, copy-paste content here. Columns: name,price,mu,sigma,qty.</p>"
       << "</div>";

    os << "<div class='card'><h3>Export portfolio</h3>"
       << "<form action='/export_csv' method='get'>"
       << "<button type='submit'>Download CSV</button>"
       << "</form></div>";


    os << "<div class='card'><h3>Optimize allocation (bonus)</h3>"
       << "<form action='/optimize' method='get'>"
       << "Objective: <select name='objective'>"
       << "<option value='min_variance'>Min variance</option>"
       << "<option value='max_return'>Max return</option>"
       << "<option value='max_score'>Max (return - lambda*risk)</option>"
       << "</select> "
       << "Target return (optional): <input name='target_return' placeholder='0.07'/> "
       << "Max volatility (optional): <input name='max_vol' placeholder='0.20'/> "
       << "Lambda (for max_score): <input name='lambda' placeholder='0.5'/> "
       << "<button type='submit'>Run optimization</button>"
       << "</form>"
       << "<p class='muted'>Uses Monte-Carlo long-only weights; also plots current portfolio and best candidate.</p>"
       << "</div>";

    os << "</div></div></body></html>";
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
                g_has_last_optimization = false;
                g_last_optimization_html.clear();
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
                g_has_last_optimization = false;
                g_last_optimization_html.clear();
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
                g_has_last_optimization = false;
                g_last_optimization_html.clear();
            }

            res.set_content(pageHTML("Removed " + std::to_string(qty) + " of " + name + "."), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // What-if simulation
    svr.Get("/what_if", [](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("name") || !req.has_param("qty_delta")) {
                res.set_content(pageHTML("Missing name/qty_delta."), "text/html; charset=utf-8");
                return;
            }

            std::string name = req.get_param_value("name");
            double qtyDelta = 0.0;
            if (!parseDouble(req.get_param_value("qty_delta"), qtyDelta) || qtyDelta == 0.0) {
                res.set_content(pageHTML("Invalid qty_delta (must be non-zero)."), "text/html; charset=utf-8");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                Portfolio simulated = g_portfolio;
                if (qtyDelta > 0.0) {
                    const auto& pos = simulated[name];
                    simulated.addPosition(pos.asset, qtyDelta);
                } else {
                    simulated.removePosition(name, std::fabs(qtyDelta));
                }

                const double simER = simulated.expectedReturn();
                std::ostringstream block;
                block << "<p><b>Scenario:</b> " << htmlEscape(name) << " qty delta = " << qtyDelta << "</p>"
                      << "<p><b>Expected return:</b> " << fmtPercent(simER, 2) << "</p>";

                const auto n = simulated.assetOrder().size();
                if (g_has_last_corr && hasCompatibleMatrixSize(g_last_corr, n)) {
                    const double simVol = simulated.volatilityApprox(g_last_corr);
                    block << "<p><b>Volatility:</b> " << fmtPercent(simVol, 2) << "</p>";
                    block << riskBreakdownHTML(simulated, g_last_corr, true);
                } else {
                    block << "<p><b>Volatility:</b> N/A (compute metrics first).</p>";
                }

                g_last_what_if_html = block.str();
                g_has_last_what_if = true;
            }

            res.set_content(pageHTML("What-if simulation computed."), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // CSV import from Excel
    svr.Post("/import_csv", [](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("csv_text")) {
                res.set_content(pageHTML("Missing csv_text."), "text/html; charset=utf-8");
                return;
            }

            Portfolio imported = portfolioFromCSVText(req.get_param_value("csv_text"));

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_portfolio = imported;
                g_has_last_corr = false;
                g_last_corr.clear();
                g_last_corr_labels.clear();
                g_last_corr_source.clear();
                g_has_last_what_if = false;
                g_last_what_if_html.clear();
                g_has_last_optimization = false;
                g_last_optimization_html.clear();
            }

            res.set_content(pageHTML("Portfolio imported from CSV (Excel-compatible)."), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // CSV export
    svr.Get("/export_csv", [](const httplib::Request&, httplib::Response& res) {
        try {
            std::string csv;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const bool corrOk = g_has_last_corr && hasCompatibleMatrixSize(g_last_corr, g_portfolio.assetOrder().size());
                csv = exportPortfolioCSV(g_portfolio, g_last_corr, corrOk);
            }
            res.set_header("Content-Disposition", "attachment; filename=portfolio_export.csv");
            res.set_content(csv, "text/csv; charset=utf-8");
        } catch (const std::exception& e) {
            res.set_content(pageHTML(std::string("Error: ") + e.what()), "text/html; charset=utf-8");
        }
    });

    // Portfolio optimization (bonus)
    svr.Get("/optimize", [](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string objective = req.has_param("objective") ? req.get_param_value("objective") : "min_variance";

            double targetReturn = -1.0;
            if (req.has_param("target_return") && !req.get_param_value("target_return").empty()) {
                if (!parseDouble(req.get_param_value("target_return"), targetReturn)) {
                    res.set_content(pageHTML("Invalid target_return."), "text/html; charset=utf-8");
                    return;
                }
            }

            double maxVol = -1.0;
            if (req.has_param("max_vol") && !req.get_param_value("max_vol").empty()) {
                if (!parseDouble(req.get_param_value("max_vol"), maxVol) || maxVol <= 0.0) {
                    res.set_content(pageHTML("Invalid max_vol."), "text/html; charset=utf-8");
                    return;
                }
            }

            double lambda = 0.5;
            if (req.has_param("lambda") && !req.get_param_value("lambda").empty()) {
                if (!parseDouble(req.get_param_value("lambda"), lambda) || lambda < 0.0) {
                    res.set_content(pageHTML("Invalid lambda."), "text/html; charset=utf-8");
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto names = g_portfolio.assetOrder();
                const std::size_t n = names.size();
                if (n < 2) throw std::invalid_argument("Need at least 2 assets to optimize.");
                if (!(g_has_last_corr && hasCompatibleMatrixSize(g_last_corr, n))) {
                    throw std::invalid_argument("Compute correlation matrix first (auto or manual) before optimization.");
                }

                std::vector<double> mu(n, 0.0), sigma(n, 0.0), currentW(n, 0.0);
                const double total = g_portfolio.totalValue();
                for (std::size_t i = 0; i < n; ++i) {
                    const auto& pos = g_portfolio[names[i]];
                    mu[i] = pos.asset.expectedReturn();
                    sigma[i] = pos.asset.volatility();
                    currentW[i] = total > 0.0 ? pos.value() / total : 0.0;
                }

                PortfolioPoint current;
                current.weights = currentW;
                current.expectedReturn = expectedReturnFromWeights(currentW, mu);
                current.volatility = volatilityFromWeights(currentW, sigma, g_last_corr);
                current.score = current.expectedReturn - lambda * current.volatility;

                std::vector<PortfolioPoint> candidates;
                candidates.reserve(3500);
                candidates.push_back(current);

                std::uint64_t rng = 0xC0FFEE1234ULL;
                for (int k = 0; k < 3500; ++k) {
                    auto w = randomLongOnlyWeights(n, rng);
                    PortfolioPoint p;
                    p.weights = w;
                    p.expectedReturn = expectedReturnFromWeights(w, mu);
                    p.volatility = volatilityFromWeights(w, sigma, g_last_corr);
                    p.score = p.expectedReturn - lambda * p.volatility;
                    candidates.push_back(p);
                }

                auto isEligible = [&](const PortfolioPoint& p) {
                    if (targetReturn >= 0.0 && p.expectedReturn < targetReturn) return false;
                    if (maxVol > 0.0 && p.volatility > maxVol) return false;
                    return true;
                };

                PortfolioPoint best;
                bool bestSet = false;

                for (const auto& p : candidates) {
                    if (!isEligible(p)) continue;
                    if (!bestSet) {
                        best = p;
                        bestSet = true;
                        continue;
                    }

                    if (objective == "max_return") {
                        if (p.expectedReturn > best.expectedReturn) best = p;
                    } else if (objective == "max_score") {
                        if (p.score > best.score) best = p;
                    } else { // min_variance
                        if (p.volatility < best.volatility) best = p;
                    }
                }

                if (!bestSet) {
                    throw std::invalid_argument("No candidate portfolio satisfies selected constraints.");
                }

                std::ostringstream html;
                html << "<p><b>Objective:</b> " << htmlEscape(objective) << "</p>";
                html << "<p><b>Current:</b> return=" << fmtPercent(current.expectedReturn, 2)
                     << " | vol=" << fmtPercent(current.volatility, 2) << "</p>";
                html << "<p><b>Best candidate:</b> return=" << fmtPercent(best.expectedReturn, 2)
                     << " | vol=" << fmtPercent(best.volatility, 2)
                     << " | score=" << fmt(best.score, 4) << "</p>";
                html << "<p><b>Weights:</b><br/>";
                for (std::size_t i = 0; i < n; ++i) {
                    html << htmlEscape(names[i]) << " : " << fmtPercent(best.weights[i], 2) << "<br/>";
                }
                html << "</p>";
                html << optimizationChartSVG(candidates, current, best, objective);

                g_last_optimization_html = html.str();
                g_has_last_optimization = true;
            }

            res.set_content(pageHTML("Optimization computed."), "text/html; charset=utf-8");
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
                g_last_corr = corr;
                g_last_corr_labels = tickers;
                g_last_corr_source = "AUTO / Yahoo";
                g_has_last_corr = true;
            }

            std::ostringstream msg;
            msg << "AUTO corr computed. Expected return=" << fmtPercent(er, 2)
                << " | Volatility=" << fmtPercent(vol, 2);
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
                auto ord = g_portfolio.assetOrder();
                er = g_portfolio.expectedReturn();
                vol = g_portfolio.volatilityApprox(M); // => invalid_argument si dimension incorrecte (exigence)
                g_last_corr = M;
                g_last_corr_labels = ord;
                g_last_corr_source = "MANUAL";
                g_has_last_corr = true;
            }

            std::ostringstream msg;
            msg << "MANUAL corr used. Expected return=" << fmtPercent(er, 2)
                << " | Volatility=" << fmtPercent(vol, 2);
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

