#pragma once
#include <string>

class FileSystemManager {
public:
    static bool Mkfs(const std::string& id, std::string& outMsg);
    static bool Mkgrp(const std::string& groupName, std::string& outMsg);
    static bool Rmgrp(const std::string& groupName, std::string& outMsg);
    static bool Mkusr(const std::string& user,
                      const std::string& pass,
                      const std::string& group,
                      std::string& outMsg);
    static bool Rmusr(const std::string& user, std::string& outMsg);
    static bool Chgrp(const std::string& user,
                      const std::string& group,
                      std::string& outMsg);
};