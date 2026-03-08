#pragma once
#include <string>

class FileSystemManager {
public:
    static bool Mkfs(const std::string& id, std::string& outMsg);
};