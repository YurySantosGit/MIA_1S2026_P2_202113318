#pragma once
#include <string>
#include <vector>

struct MountedPartition {
    std::string id;
    std::string path;
    std::string name;
    int start;
    int size;
};

class MountManager {
public:
    static std::string Mount(const std::string& path,
                             const std::string& name,
                             int start,
                             int size,
                             const std::string& carnetLastTwo,
                             std::string& outMsg);

    static const std::vector<MountedPartition>& GetMountedPartitions();
    static bool FindById(const std::string& id, MountedPartition& outPartition);
    static bool Unmount(const std::string& id, std::string& outMsg);

private:
    static std::vector<MountedPartition> mountedPartitions;
};