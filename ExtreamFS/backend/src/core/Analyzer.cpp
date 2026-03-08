#include "core/Analyzer.h"
#include "disk/DiskManagement.h"

#include <iostream>
#include <algorithm>
#include <regex>

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

void Analyzer::RunInteractive() {
    std::string line;

    // IMPORTANTE: el banner debe estar SOLO en main.cpp (no aquí)
    while (true) {
        std::cout << ">> " << std::flush;

        if (!std::getline(std::cin, line)) {
            std::cout << "\nEOF detectado. Saliendo...\n";
            break;
        }

        std::string cleaned = trim(line);
        if (cleaned.empty()) continue;

        if (cleaned[0] == '#') {
            std::cout << cleaned << "\n";
            continue;
        }

        if (toLower(cleaned) == "exit") {
            std::cout << "Saliendo...\n";
            break;
        }

        ExecuteLine(cleaned);
    }
}

void Analyzer::ExecuteLine(const std::string& line) {
    ParsedLine parsed;
    std::string error;

    if (!ParseLine(line, parsed, error)) {
        std::cout << "[ERROR] " << error << "\n";
        return;
    }

    // Debug temporal
    std::cout << "[CMD] " << parsed.command << "\n";
    for (const auto& [k, v] : parsed.params) {
        std::cout << "  - " << k << " = " << v << "\n";
    }

    // ============ DISPATCH ============
    if (parsed.command == "mkdisk") {
        if (!parsed.params.count("size") || !parsed.params.count("path")) {
            std::cout << "[ERROR] mkdisk requiere -size y -path\n";
            return;
        }

        int size = 0;
        try {
            size = std::stoi(parsed.params["size"]);
        } catch (...) {
            std::cout << "[ERROR] -size debe ser un entero valido\n";
            return;
        }

        std::string path = parsed.params["path"];
        char unit = parsed.params.count("unit") ? parsed.params["unit"][0] : 'm';
        char fit  = parsed.params.count("fit")  ? parsed.params["fit"][0]  : 'f';

        std::string msg;
        if (!DiskManagement::Mkdisk(size, path, unit, fit, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

        if (parsed.command == "rmdisk") {
            if (!parsed.params.count("path")) {
                std::cout << "[ERROR] rmdisk requiere -path\n";
                return;
            }

            std::string path = parsed.params["path"];
            std::string msg;

            if (!DiskManagement::Rmdisk(path, msg)) {
                std::cout << "[ERROR] " << msg << "\n";
            } else {
                std::cout << "[OK] " << msg << "\n";
            }
            return;
    }

    std::cout << "[INFO] Comando reconocido pero no implementado: " << parsed.command << "\n";
}

bool Analyzer::ParseLine(const std::string& line, ParsedLine& out, std::string& error) {
    out = ParsedLine{};
    error.clear();

    // validar comillas parejas
    int quotes = 0;
    for (char c : line) if (c == '"') quotes++;
    if (quotes % 2 != 0) {
        error = "Comillas no cerradas en la linea: " + line;
        return false;
    }

    std::string cleaned = trim(line);

    // separar comando
    size_t sp = cleaned.find(' ');
    std::string cmd = (sp == std::string::npos) ? cleaned : cleaned.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? "" : cleaned.substr(sp + 1);

    out.command = toLower(cmd);
    rest = trim(rest);

    // -key=value donde value puede ser "..." o token sin espacios
    std::regex re(R"(-([A-Za-z_]\w*)=("[^"]*"|\S+))");

    std::sregex_iterator it(rest.begin(), rest.end(), re);
    std::sregex_iterator end;

    std::string consumed = rest;

    for (; it != end; ++it) {
        std::string key = toLower((*it)[1].str());
        std::string val = stripQuotes((*it)[2].str());

        if (out.params.count(key)) {
            error = "Parametro repetido: -" + key;
            return false;
        }

        out.params[key] = val;

        // marcar consumido
        size_t pos = consumed.find((*it)[0].str());
        if (pos != std::string::npos) {
            consumed.replace(pos, (*it)[0].str().size(),
                             std::string((*it)[0].str().size(), ' '));
        }
    }

    // si sobran cosas, hay parametros mal formados
    std::string leftover = trim(consumed);
    if (!leftover.empty()) {
        error = "Texto/parametros no reconocidos o sin '=': " + leftover;
        return false;
    }

    return true;
}