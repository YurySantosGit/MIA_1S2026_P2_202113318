#pragma once
#include <string>
#include "disk/Structs.h"

class DiskManagement {
public:
    static bool Mkdisk(int size, const std::string& path, char unit, char fit, std::string& outMsg);
    static bool Rmdisk(const std::string& path, std::string& outMsg);

    static bool ReadMBR(const std::string& path, MBR& outMBR, std::string& outMsg);
    static bool WriteMBR(const std::string& path, const MBR& mbr, std::string& outMsg);

    static bool Fdisk(int size,
                      const std::string& path,
                      const std::string& name,
                      char unit,
                      char type,
                      char fit,
                      std::string& outMsg);
    
    static bool FindPartitionByName(const std::string& path,
                                    const std::string& name,
                                    Partition& outPartition,
                                    std::string& outMsg);
    
    static bool DeletePartition(const std::string& path,
                            const std::string& name,
                            const std::string& mode,
                            std::string& outMsg);

    static bool AddPartitionSpace(const std::string& path,
                              const std::string& name,
                              int add,
                              char unit,
                              std::string& outMsg);

private:
    static bool CreatePrimaryOrExtended(int sizeBytes,
                                        const std::string& path,
                                        const std::string& name,
                                        char type,
                                        char fit,
                                        std::string& outMsg);

    static bool CreateLogical(int sizeBytes,
                              const std::string& path,
                              const std::string& name,
                              char fit,
                              std::string& outMsg);
};