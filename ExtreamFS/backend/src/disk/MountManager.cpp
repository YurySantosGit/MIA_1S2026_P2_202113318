#include "disk/MountManager.h"
#include <algorithm>

std::vector<MountedPartition> MountManager::mountedPartitions;

static std::string extractDiskKey(const std::string& path) {
    return path;
}

std::string MountManager::Mount(const std::string& path,
                                const std::string& name,
                                int start,
                                int size,
                                const std::string& carnetLastTwo,
                                std::string& outMsg) {
    outMsg.clear();

    // evitar montar duplicado la misma partición del mismo disco
    for (const auto& mp : mountedPartitions) {
        if (mp.path == path && mp.name == name) {
            outMsg = "Error: la particion ya esta montada con id " + mp.id;
            return "";
        }
    }

    std::string diskKey = extractDiskKey(path);

    // determinar letra del disco
    std::vector<std::string> uniqueDisks;
    for (const auto& mp : mountedPartitions) {
        if (std::find(uniqueDisks.begin(), uniqueDisks.end(), mp.path) == uniqueDisks.end()) {
            uniqueDisks.push_back(mp.path);
        }
    }

    char letter = 'A';
    auto itDisk = std::find(uniqueDisks.begin(), uniqueDisks.end(), diskKey);
    if (itDisk == uniqueDisks.end()) {
        letter = (char)('A' + uniqueDisks.size());
    } else {
        letter = (char)('A' + std::distance(uniqueDisks.begin(), itDisk));
    }

    // número de partición para ese disco
    int numberForDisk = 1;
    for (const auto& mp : mountedPartitions) {
        if (mp.path == path) {
            numberForDisk++;
        }
    }

    std::string id = carnetLastTwo + std::to_string(numberForDisk) + letter;

    MountedPartition mp;
    mp.id = id;
    mp.path = path;
    mp.name = name;
    mp.start = start;
    mp.size = size;

    mountedPartitions.push_back(mp);

    outMsg = "Particion montada correctamente con id: " + id;
    return id;
}

const std::vector<MountedPartition>& MountManager::GetMountedPartitions() {
    return mountedPartitions;
}

bool MountManager::FindById(const std::string& id, MountedPartition& outPartition) {
    for (const auto& mp : mountedPartitions) {
        if (mp.id == id) {
            outPartition = mp;
            return true;
        }
    }
    return false;
}