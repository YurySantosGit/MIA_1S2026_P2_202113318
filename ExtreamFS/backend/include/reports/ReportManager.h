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
    
    static bool RepTree(const std::string& id,
                        const std::string& outPath,
                        std::string& outMsg);
    
    static bool RepFile(const std::string& id,
                    const std::string& outPath,
                    const std::string& filePath,
                    std::string& outMsg);

    static bool RepLs(const std::string& id,
                  const std::string& outPath,
                  const std::string& dirPath,
                  std::string& outMsg);
    
    static bool RepBmInode(const std::string& id,
                       const std::string& outPath,
                       std::string& outMsg);

    static bool RepBmBlock(const std::string& id,
                       const std::string& outPath,
                       std::string& outMsg);
};