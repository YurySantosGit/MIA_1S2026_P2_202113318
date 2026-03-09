#include "core/Analyzer.h"
#include "disk/DiskManagement.h"
#include "disk/MountManager.h"
#include "disk/Structs.h"

#include "fs/FileSystemManager.h"
#include "fs/SessionManager.h"

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

    if (parsed.command == "fdisk") {
        if (!parsed.params.count("size") ||
            !parsed.params.count("path") ||
            !parsed.params.count("name")) {
            std::cout << "[ERROR] fdisk requiere -size, -path y -name\n";
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
        std::string name = parsed.params["name"];

        char unit = parsed.params.count("unit") ? parsed.params["unit"][0] : 'k';
        char type = parsed.params.count("type") ? parsed.params["type"][0] : 'p';
        char fit  = parsed.params.count("fit")  ? parsed.params["fit"][0]  : 'w';

        std::string msg;
        if (!DiskManagement::Fdisk(size, path, name, unit, type, fit, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "mount") {
        if (!parsed.params.count("path") || !parsed.params.count("name")) {
            std::cout << "[ERROR] mount requiere -path y -name\n";
            return;
        }

        std::string path = parsed.params["path"];
        std::string name = parsed.params["name"];

        Partition part{};
        std::string msg;

        if (!DiskManagement::FindPartitionByName(path, name, part, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
            return;
        }

        //últimos dos dígitos de carnet
        std::string carnetLastTwo = "18";

        std::string id = MountManager::Mount(path,
                                             name,
                                             part.part_start,
                                             part.part_size,
                                             carnetLastTwo,
                                             msg);

        if (id.empty()) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "mounted") {
        const auto& mounted = MountManager::GetMountedPartitions();

        if (mounted.empty()) {
            std::cout << "[INFO] No hay particiones montadas.\n";
            return;
        }

        std::cout << "[MOUNTED]\n";
        for (const auto& mp : mounted) {
            std::cout << "  - id=" << mp.id
                      << " | name=" << mp.name
                      << " | path=" << mp.path
                      << " | start=" << mp.start
                      << " | size=" << mp.size
                      << "\n";
        }
        return;
    }

    if (parsed.command == "mkfs") {
        if (!parsed.params.count("id")) {
            std::cout << "[ERROR] mkfs requiere -id\n";
            return;
        }

        std::string id = parsed.params["id"];
        std::string msg;

        if (!FileSystemManager::Mkfs(id, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "login") {

        if (!parsed.params.count("user") ||
            !parsed.params.count("pass") ||
            !parsed.params.count("id")) {

            std::cout << "[ERROR] login requiere -user -pass -id\n";
            return;
        }

        std::string user = parsed.params["user"];
        std::string pass = parsed.params["pass"];
        std::string id   = parsed.params["id"];

        std::string msg;

        if (!SessionManager::Login(user, pass, id, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }

        return;
    }

    if (parsed.command == "logout") {
        if (!SessionManager::currentSession.active) {
            std::cout << "[ERROR] No hay una sesion activa.\n";
            return;
        }

        SessionManager::Logout();
        std::cout << "[OK] Sesion cerrada correctamente.\n";
        return;
    }

    if (parsed.command == "session") {
        if (!SessionManager::currentSession.active) {
            std::cout << "[INFO] No hay sesion activa.\n";
            return;
        }

        std::cout << "[SESSION]\n";
        std::cout << "  - user=" << SessionManager::currentSession.user << "\n";
        std::cout << "  - group=" << SessionManager::currentSession.group << "\n";
        std::cout << "  - id=" << SessionManager::currentSession.partitionId << "\n";
        return;
    }

    if (parsed.command == "mkgrp") {
        if (!parsed.params.count("name")) {
            std::cout << "[ERROR] mkgrp requiere -name\n";
            return;
        }

        std::string groupName = parsed.params["name"];
        std::string msg;

        if (!FileSystemManager::Mkgrp(groupName, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "rmgrp") {
        if (!parsed.params.count("name")) {
            std::cout << "[ERROR] rmgrp requiere -name\n";
            return;
        }

        std::string groupName = parsed.params["name"];
        std::string msg;

        if (!FileSystemManager::Rmgrp(groupName, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "mkusr") {
        if (!parsed.params.count("user") ||
            !parsed.params.count("pass") ||
            !parsed.params.count("grp")) {
            std::cout << "[ERROR] mkusr requiere -user, -pass y -grp\n";
            return;
        }

        std::string user = parsed.params["user"];
        std::string pass = parsed.params["pass"];
        std::string grp  = parsed.params["grp"];
        std::string msg;

        if (!FileSystemManager::Mkusr(user, pass, grp, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "rmusr") {
        if (!parsed.params.count("user")) {
            std::cout << "[ERROR] rmusr requiere -user\n";
            return;
        }

        std::string user = parsed.params["user"];
        std::string msg;

        if (!FileSystemManager::Rmusr(user, msg)) {
            std::cout << "[ERROR] " << msg << "\n";
        } else {
            std::cout << "[OK] " << msg << "\n";
        }
        return;
    }

    if (parsed.command == "chgrp") {
        if (!parsed.params.count("user") || !parsed.params.count("grp")) {
            std::cout << "[ERROR] chgrp requiere -user y -grp\n";
            return;
        }

        std::string user = parsed.params["user"];
        std::string grp  = parsed.params["grp"];
        std::string msg;

        if (!FileSystemManager::Chgrp(user, grp, msg)) {
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