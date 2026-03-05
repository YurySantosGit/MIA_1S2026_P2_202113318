#include <iostream>
#include "core/Analyzer.h"

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::cout << "ExtreamFS (CLI) - Proyecto 1 MIA\n";
    std::cout << "Escribe 'exit' para salir.\n\n";

    Analyzer analyzer;
    analyzer.RunInteractive();

    return 0;
}