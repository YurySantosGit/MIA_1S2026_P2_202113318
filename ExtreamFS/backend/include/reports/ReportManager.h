#pragma once
#include <string>

class ReportManager {
public:
    static bool RepSb(const std::string& id,
                      const std::string& outPath,
                      std::string& outMsg);

    static bool RepMbr(const std::string& id,
                       const std::string& outPath,
                       std::string& outMsg);

    static bool RepDisk(const std::string& id,
                        const std::string& outPath,
                        std::string& outMsg);

    static bool RepInode(const std::string& id,
                         const std::string& outPath,
                         std::string& outMsg);

    static bool RepBlock(const std::string& id,
                         const std::string& outPath,
                         std::string& outMsg);
};