#include "../../include/api/HttpServer.h"
#include "../../include/core/Analyzer.h"
#include "../../include/third_party/httplib.h"

#include <iostream>
#include <string>

HttpServer::HttpServer(const std::string& host, int port)
    : host_(host), port_(port) {}

bool HttpServer::Start() {
    httplib::Server server;

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
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

    server.Post("/api/execute", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
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
                switch (c) {
                    case '\\': safeOutput += "\\\\"; break;
                    case '"':  safeOutput += "\\\""; break;
                    case '\n': safeOutput += "\\n";  break;
                    case '\r': safeOutput += "\\r";  break;
                    case '\t': safeOutput += "\\t";  break;
                    default:   safeOutput += c;      break;
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