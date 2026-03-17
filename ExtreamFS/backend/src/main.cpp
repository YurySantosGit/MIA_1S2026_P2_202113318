#include "../include/core/Analyzer.h"
#include "../include/api/HttpServer.h"

#include <iostream>
#include "core/Analyzer.h"

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string mode = argv[1];

        if (mode == "--api") {
            HttpServer server("0.0.0.0", 8080);
            return server.Start() ? 0 : 1;
        }
    }

    std::cout << "ExtreamFS (CLI) - Proyecto 1 MIA\n";
    std::cout << "Modo interactivo iniciado\n";
    std::cout << "Escribe 'exit' para salir.\n\n";

    Analyzer analyzer;
    analyzer.RunInteractive();

    return 0;
}