#include "../../include/api/HttpServer.h"
#include "../../include/core/Analyzer.h"
#include "../../include/third_party/httplib.h"
#include "../../include/disk/Structs.h"

#include <fstream>
#include <cstring>

#include <iostream>
#include <string>

#include <filesystem>
#include <vector>

HttpServer::HttpServer(const std::string& host, int port)
    : host_(host), port_(port) {}

bool HttpServer::Start() {
    httplib::Server server;

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Content-Type", "application/json");
        res.status = 200;
        res.set_content(R"({"status":"Conectado"})", "application/json");
    });

    server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 200;
    });

    server.Get("/api/disks", [](const httplib::Request&, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.set_header("Content-Type", "application/json");

    std::string basePath = "/home/ubuntu/Calificacion_MIA/Discos";
    std::string json = "{\"disks\":[";

    bool first = true;

        try {
            for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
                if (!entry.is_regular_file()) continue;

                std::string path = entry.path().string();
                std::string name = entry.path().filename().string();

                if (entry.path().extension() != ".mia") continue;

                if (!first) json += ",";
                first = false;

                json += "{";
                json += "\"name\":\"" + name + "\",";
                json += "\"path\":\"" + path + "\",";
                json += "\"size\":" + std::to_string(std::filesystem::file_size(entry.path()));
                json += "}";
            }

            json += "]}";

            res.status = 200;
            res.set_content(json, "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(
                R"({"disks":[],"error":"No se pudieron leer los discos."})",
                "application/json"
            );
        }
    });

    server.Get("/api/partitions", [](const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.set_header("Content-Type", "application/json");

    if (!req.has_param("path")) {
        res.status = 400;
        res.set_content(R"({"partitions":[],"error":"Falta parametro path."})", "application/json");
        return;
    }

    std::string diskPath = req.get_param_value("path");

    std::fstream file(diskPath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        res.status = 500;
        res.set_content(R"({"partitions":[],"error":"No se pudo abrir el disco."})", "application/json");
        return;
    }

    MBR mbr{};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    file.close();

    if (!file.good() && file.gcount() != sizeof(MBR)) {
        res.status = 500;
        res.set_content(R"({"partitions":[],"error":"No se pudo leer el MBR."})", "application/json");
        return;
    }

    std::string json = "{\"partitions\":[";
    bool first = true;

    for (int i = 0; i < 4; i++) {
        Partition part = mbr.mbr_partitions[i];

        if (part.part_status != '1' || part.part_size <= 0) {
            continue;
        }

        char nameBuffer[17];
        std::memset(nameBuffer, 0, sizeof(nameBuffer));
        std::memcpy(nameBuffer, part.part_name, 16);

        if (!first) json += ",";
        first = false;

        json += "{";
        json += "\"name\":\"" + std::string(nameBuffer) + "\",";
        json += "\"type\":\"" + std::string(1, part.part_type) + "\",";
        json += "\"fit\":\"" + std::string(1, part.part_fit) + "\",";
        json += "\"start\":" + std::to_string(part.part_start) + ",";
        json += "\"size\":" + std::to_string(part.part_size);
        json += "}";
    }

    json += "]}";

    res.status = 200;
    res.set_content(json, "application/json");
    });

    server.Get("/api/fs", [](const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.set_header("Content-Type", "application/json");

    std::string path = "/";

    if (req.has_param("path")) {
        path = req.get_param_value("path");
    }

    try {
        Analyzer analyzer;

        std::string command = "find -path=" + path + " -name=*";
        std::string output = analyzer.ExecuteScript(command);

        std::string safeOutput;
        for (char c : output) {
            unsigned char uc = static_cast<unsigned char>(c);

            switch (c) {
                case '\\': safeOutput += "\\\\"; break;
                case '"':  safeOutput += "\\\""; break;
                case '\n': safeOutput += "\\n";  break;
                case '\r': safeOutput += "\\r";  break;
                case '\t': safeOutput += "\\t";  break;
                default:
                    if (uc >= 32) {
                        safeOutput += c;
                    }
                    break;
            }
        }

        std::string jsonResponse =
            std::string("{\"success\":true,\"path\":\"") +
            path +
            "\",\"output\":\"" +
            safeOutput +
            "\"}";

        res.status = 200;
        res.set_content(jsonResponse, "application/json");
    } catch (...) {
        res.status = 500;
        res.set_content(
            R"({"success":false,"path":"/","output":"[ERROR] No se pudo leer el sistema de archivos."})",
            "application/json"
        );
    }
    });

    server.Post("/api/execute", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Content-Type", "application/json");

        try {
            std::string body = req.body;
            std::string commands;

            const std::string key = "\"commands\"";
            size_t keyPos = body.find(key);

            if (keyPos == std::string::npos) {
                res.status = 400;
                res.set_content(
                    R"({"success":false,"output":"[ERROR] No se encontro el campo commands.","reports":[]})",
                    "application/json"
                );
                return;
            }

            size_t colonPos = body.find(':', keyPos);
            size_t firstQuote = body.find('"', colonPos + 1);

            if (colonPos == std::string::npos || firstQuote == std::string::npos) {
                res.status = 400;
                res.set_content(
                    R"({"success":false,"output":"[ERROR] JSON invalido en campo commands.","reports":[]})",
                    "application/json"
                );
                return;
            }

            size_t current = firstQuote + 1;

            while (current < body.size()) {
                if (body[current] == '"' && body[current - 1] != '\\') {
                    break;
                }

                if (body[current] == '\\' && current + 1 < body.size()) {
                    char next = body[current + 1];

                    switch (next) {
                        case 'n': commands += '\n'; break;
                        case 't': commands += '\t'; break;
                        case 'r': commands += '\r'; break;
                        case '"': commands += '"'; break;
                        case '\\': commands += '\\'; break;
                        default: commands += next; break;
                    }

                    current += 2;
                } else {
                    commands += body[current];
                    current++;
                }
            }

            Analyzer analyzer;
            std::string output = analyzer.ExecuteScript(commands);

            std::string safeOutput;
            for (char c : output) {
                unsigned char uc = static_cast<unsigned char>(c);

                switch (c) {
                    case '\\':
                        safeOutput += "\\\\";
                        break;
                    case '"':
                        safeOutput += "\\\"";
                        break;
                    case '\n':
                        safeOutput += "\\n";
                        break;
                    case '\r':
                        safeOutput += "\\r";
                        break;
                    case '\t':
                        safeOutput += "\\t";
                        break;
                    default:
                        if (uc >= 32) {
                            safeOutput += c;
                        }
                        break;
                }
            }

            std::string jsonResponse =
                std::string("{\"success\":true,\"output\":\"") +
                safeOutput +
                "\",\"reports\":[]}";

            res.status = 200;
            res.set_content(jsonResponse, "application/json");
        } catch (...) {
            res.status = 500;
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(
                R"({"success":false,"output":"[ERROR] Error interno del servidor.","reports":[]})",
                "application/json"
            );
        }
    });

    std::cout << "[INFO] Backend HTTP escuchando en http://" << host_ << ":" << port_ << std::endl;

    if (!server.listen(host_.c_str(), port_)) {
        std::cerr << "[ERROR] No se pudo iniciar el servidor HTTP en el puerto " << port_ << std::endl;
        return false;
    }

    return true;
}

void HttpServer::Stop() {
}