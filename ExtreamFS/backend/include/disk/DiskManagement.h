#pragma once
#include <string>

class DiskManagement {
public:
    static bool Mkdisk(int size, const std::string& path, char unit, char fit, std::string& outMsg);
};