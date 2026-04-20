#include "disk/MountManager.h"
#include <algorithm>
#include <cctype>

std::vector<MountedPartition> MountManager::mountedPartitions;

static std::string extractDiskKey(const std::string& path) {
    return path;
}

static std::string normalizeId(std::string id) {
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return id;
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
        letter = static_cast<char>('A' + uniqueDisks.size());
    } else {
        letter = static_cast<char>('A' + std::distance(uniqueDisks.begin(), itDisk));
    }

    // número de partición para ese disco
    int numberForDisk = 1;
    for (const auto& mp : mountedPartitions) {
        if (mp.path == path) {
            numberForDisk++;
        }
    }

    std::string id = normalizeId(carnetLastTwo + std::to_string(numberForDisk) + letter);

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
    std::string normalizedId = normalizeId(id);

    for (const auto& mp : mountedPartitions) {
        if (normalizeId(mp.id) == normalizedId) {
            outPartition = mp;
            return true;
        }
    }
    return false;
}

bool MountManager::Unmount(const std::string& id, std::string& outMsg) {
    outMsg.clear();

    std::string normalizedId = normalizeId(id);

    auto it = std::find_if(mountedPartitions.begin(), mountedPartitions.end(),
                           [&](const MountedPartition& mp) {
                               return normalizeId(mp.id) == normalizedId;
                           });

    if (it == mountedPartitions.end()) {
        outMsg = "No existe una particion montada con id: " + id;
        return false;
    }

    std::string removedId = it->id;
    std::string removedName = it->name;

    mountedPartitions.erase(it);

    outMsg = "Particion desmontada correctamente. id=" + removedId +
             " | name=" + removedName;
    return true;
}