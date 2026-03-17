#pragma once
#include <string>
#include <map>

class Analyzer {
public:
    void RunInteractive();
    std::string ExecuteScript(const std::string& input);

private:
    struct ParsedLine {
        std::string command;
        std::map<std::string, std::string> params;
    };

    void ExecuteLine(const std::string& line);
    bool ParseLine(const std::string& line, ParsedLine& out, std::string& error);
};