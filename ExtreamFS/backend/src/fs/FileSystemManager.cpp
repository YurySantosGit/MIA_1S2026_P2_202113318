#include "fs/FileSystemManager.h"
#include "fs/Ext2Structs.h"
#include "disk/MountManager.h"
#include "fs/SessionManager.h"

#include <sstream>
#include <vector>
#include <fstream>
#include <cstring>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <functional>

static std::string nowStringFS() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);

    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

static std::vector<std::string> splitFS(const std::string& text, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(text);
    std::string item;

    while (std::getline(ss, item, delim)) {
        parts.push_back(item);
    }

    return parts;
}

static std::string trimFS(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static int calcN(int partitionSize, bool isExt3) {
    int sb = (int)sizeof(SuperBlock);
    int journal = isExt3 ? (int)sizeof(Journal) : 0;
    int inode = (int)sizeof(Inode);
    int block = (int)sizeof(FileBlock); // todos valen 64 bytes
    double n = (double)(partitionSize - sb - journal) / (1.0 + 3.0 + inode + 3.0 * block);
    return (int)std::floor(n);
}

static int FsPermDigit(char c) {
    return (c >= '0' && c <= '7') ? (c - '0') : 0;
}

struct ChownCmdUserInfo {
    int uid = -1;
    int gid = -1;
    std::string group;
    bool active = false;
};

static int FsEffectivePerm(const Inode& inode,
                           const ChownCmdUserInfo& currentUser,
                           bool isRoot) {
    if (isRoot) return 7;

    if (inode.i_uid == currentUser.uid) {
        return FsPermDigit(inode.i_perm[0]);
    }

    if (inode.i_gid == currentUser.gid) {
        return FsPermDigit(inode.i_perm[1]);
    }

    return FsPermDigit(inode.i_perm[2]);
}

static bool FsCanRead(const Inode& inode,
                      const ChownCmdUserInfo& currentUser,
                      bool isRoot) {
    return (FsEffectivePerm(inode, currentUser, isRoot) & 4) != 0;
}

static bool FsCanWrite(const Inode& inode,
                       const ChownCmdUserInfo& currentUser,
                       bool isRoot) {
    return (FsEffectivePerm(inode, currentUser, isRoot) & 2) != 0;
}



static bool ChownCmdReadInode(std::fstream& file,
                              const SuperBlock& sb,
                              int inodeIndex,
                              Inode& outInode) {
    if (inodeIndex < 0) return false;
    file.clear();
    file.seekg(sb.s_inode_start + inodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&outInode), sizeof(Inode));
    return file.good();
}

static bool ChownCmdWriteInode(std::fstream& file,
                               const SuperBlock& sb,
                               int inodeIndex,
                               const Inode& inode) {
    if (inodeIndex < 0) return false;
    file.clear();
    file.seekp(sb.s_inode_start + inodeIndex * (int)sizeof(Inode));
    file.write(reinterpret_cast<const char*>(&inode), sizeof(Inode));
    file.flush();
    return file.good();
}

static bool ChownCmdReadFolderBlock(std::fstream& file,
                                    const SuperBlock& sb,
                                    int blockIndex,
                                    FolderBlock& outBlock) {
    if (blockIndex < 0) return false;
    file.clear();
    file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
    file.read(reinterpret_cast<char*>(&outBlock), sizeof(FolderBlock));
    return file.good();
}

static bool ChownCmdReadFileBlock(std::fstream& file,
                                  const SuperBlock& sb,
                                  int blockIndex,
                                  FileBlock& outBlock) {
    if (blockIndex < 0) return false;
    file.clear();
    file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
    file.read(reinterpret_cast<char*>(&outBlock), sizeof(FileBlock));
    return file.good();
}

static std::vector<std::string> ChownCmdSplitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;

    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            parts.push_back(item);
        }
    }
    return parts;
}

static int ChownCmdFindEntryInFolder(std::fstream& file,
                                     const SuperBlock& sb,
                                     const Inode& folderInode,
                                     const std::string& name) {
    if (folderInode.i_type != '0') return -1;

    for (int i = 0; i < 15; i++) {
        if (folderInode.i_block[i] < 0) continue;

        FolderBlock fb{};
        if (!ChownCmdReadFolderBlock(file, sb, folderInode.i_block[i], fb)) {
            continue;
        }

        for (int j = 0; j < 4; j++) {
            std::string entryName(fb.b_content[j].b_name);
            int inodeIdx = fb.b_content[j].b_inodo;

            if (inodeIdx >= 0 && entryName == name) {
                return inodeIdx;
            }
        }
    }

    return -1;
}

static bool ChownCmdResolvePath(std::fstream& file,
                                const SuperBlock& sb,
                                const std::string& path,
                                int& outInodeIndex,
                                Inode& outInode) {
    if (path == "/") {
        outInodeIndex = 0;
        return ChownCmdReadInode(file, sb, 0, outInode);
    }

    std::vector<std::string> parts = ChownCmdSplitPath(path);
    int currentInodeIndex = 0;
    Inode currentInode{};

    if (!ChownCmdReadInode(file, sb, currentInodeIndex, currentInode)) {
        return false;
    }

    for (const auto& part : parts) {
        int nextInodeIndex = ChownCmdFindEntryInFolder(file, sb, currentInode, part);
        if (nextInodeIndex < 0) {
            return false;
        }

        if (!ChownCmdReadInode(file, sb, nextInodeIndex, currentInode)) {
            return false;
        }

        currentInodeIndex = nextInodeIndex;
    }

    outInodeIndex = currentInodeIndex;
    outInode = currentInode;
    return true;
}

static bool ChownCmdReadFileContent(std::fstream& file,
                                    const SuperBlock& sb,
                                    const Inode& inode,
                                    std::string& outContent) {
    outContent.clear();

    if (inode.i_type != '1') return false;

    int remaining = inode.i_size;
    for (int i = 0; i < 15 && remaining > 0; i++) {
        if (inode.i_block[i] < 0) continue;

        FileBlock fb{};
        if (!ChownCmdReadFileBlock(file, sb, inode.i_block[i], fb)) {
            return false;
        }

        int take = std::min(remaining, (int)sizeof(fb.b_content));
        outContent.append(fb.b_content, take);
        remaining -= take;
    }

    return true;
}

static bool ChownCmdGetUserInfo(std::fstream& file,
                                const SuperBlock& sb,
                                const std::string& username,
                                ChownCmdUserInfo& outUser) {
    outUser = ChownCmdUserInfo{};

    Inode usersInode{};
    if (!ChownCmdReadInode(file, sb, 1, usersInode)) {
        return false;
    }

    std::string usersContent;
    if (!ChownCmdReadFileContent(file, sb, usersInode, usersContent)) {
        return false;
    }

    std::map<std::string, int> activeGroups;
    std::stringstream ss(usersContent);
    std::string line;

    while (std::getline(ss, line)) {
        line = trimFS(line);
        if (line.empty()) continue;

        std::vector<std::string> cols = splitFS(line, ',');
        if (cols.size() < 3) continue;

        if (cols[1] == "G") {
            if (cols[0] != "0") {
                activeGroups[cols[2]] = std::stoi(cols[0]);
            }
        }
    }

    std::stringstream ss2(usersContent);
    while (std::getline(ss2, line)) {
        line = trimFS(line);
        if (line.empty()) continue;

        std::vector<std::string> cols = splitFS(line, ',');
        if (cols.size() < 5) continue;

        if (cols[1] == "U" && cols[0] != "0" && cols[3] == username) {
            outUser.uid = std::stoi(cols[0]);
            outUser.group = cols[2];
            outUser.active = true;

            auto it = activeGroups.find(cols[2]);
            if (it != activeGroups.end()) {
                outUser.gid = it->second;
            } else {
                outUser.gid = 1;
            }

            return true;
        }
    }

    return false;
}

static bool ChownCmdApplyRecursive(std::fstream& file,
                                   const SuperBlock& sb,
                                   int inodeIndex,
                                   int newUid,
                                   int newGid,
                                   int currentUid,
                                   bool isRoot,
                                   bool recursive) {
    Inode inode{};
    if (!ChownCmdReadInode(file, sb, inodeIndex, inode)) {
        return false;
    }

    if (!isRoot && inode.i_uid != currentUid) {
        return true;
    }

    inode.i_uid = newUid;
    inode.i_gid = newGid;

    std::string now = nowStringFS();
    std::memset(inode.i_mtime, 0, sizeof(inode.i_mtime));
    std::strncpy(inode.i_mtime, now.c_str(), sizeof(inode.i_mtime) - 1);

    if (!ChownCmdWriteInode(file, sb, inodeIndex, inode)) {
        return false;
    }

    if (!recursive || inode.i_type != '0') {
        return true;
    }

    for (int i = 0; i < 15; i++) {
        if (inode.i_block[i] < 0) continue;

        FolderBlock fb{};
        if (!ChownCmdReadFolderBlock(file, sb, inode.i_block[i], fb)) {
            return false;
        }

        for (int j = 0; j < 4; j++) {
            int childInodeIndex = fb.b_content[j].b_inodo;
            std::string childName(fb.b_content[j].b_name);

            if (childInodeIndex < 0) continue;
            if (childName == "." || childName == ".." || childName.empty()) continue;

            if (!ChownCmdApplyRecursive(file,
                                        sb,
                                        childInodeIndex,
                                        newUid,
                                        newGid,
                                        currentUid,
                                        isRoot,
                                        recursive)) {
                return false;
            }
        }
    }

    return true;
}

static bool ChmodCmdIsValidUGO(const std::string& ugo) {
    if (ugo.size() != 3) return false;

    for (char c : ugo) {
        if (c < '0' || c > '7') {
            return false;
        }
    }
    return true;
}

static bool ChmodCmdApplyRecursive(std::fstream& file,
                                   const SuperBlock& sb,
                                   int inodeIndex,
                                   const std::string& ugo,
                                   int currentUid,
                                   bool isRoot,
                                   bool recursive) {
    Inode inode{};
    if (!ChownCmdReadInode(file, sb, inodeIndex, inode)) {
        return false;
    }

    if (!isRoot && inode.i_uid != currentUid) {
        return true;
    }

    std::memcpy(inode.i_perm, ugo.c_str(), 3);

    std::string now = nowStringFS();
    std::memset(inode.i_mtime, 0, sizeof(inode.i_mtime));
    std::strncpy(inode.i_mtime, now.c_str(), sizeof(inode.i_mtime) - 1);

    if (!ChownCmdWriteInode(file, sb, inodeIndex, inode)) {
        return false;
    }

    if (!recursive || inode.i_type != '0') {
        return true;
    }

    for (int i = 0; i < 15; i++) {
        if (inode.i_block[i] < 0) continue;

        FolderBlock fb{};
        if (!ChownCmdReadFolderBlock(file, sb, inode.i_block[i], fb)) {
            return false;
        }

        for (int j = 0; j < 4; j++) {
            int childInodeIndex = fb.b_content[j].b_inodo;
            std::string childName(fb.b_content[j].b_name);

            if (childInodeIndex < 0) continue;
            if (childName.empty() || childName == "." || childName == "..") continue;

            if (!ChmodCmdApplyRecursive(file,
                                        sb,
                                        childInodeIndex,
                                        ugo,
                                        currentUid,
                                        isRoot,
                                        recursive)) {
                return false;
            }
        }
    }

    return true;
}

static float nowJournalValue() {
    return static_cast<float>(std::time(nullptr));
}

static std::string sanitizeJournalText(const std::string& text) {
    std::string clean;
    clean.reserve(text.size());

    for (char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            clean.push_back(' ');
        } else {
            clean.push_back(c);
        }
    }

    return trimFS(clean);
}

static std::string journalDateToString(float rawDate) {
    std::time_t t = static_cast<std::time_t>(rawDate);
    if (t <= 0) return "-";

    std::tm tm{};
    localtime_r(&t, &tm);

    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

static std::vector<int> getUsersTxtUsedBlocks(const Inode& inode) {
    std::vector<int> blocks;
    for (int i = 0; i < 15; i++) {
        if (inode.i_block[i] != -1) {
            blocks.push_back(inode.i_block[i]);
        }
    }
    return blocks;
}

static std::vector<int> findFreeBlocks(std::fstream& file, const SuperBlock& sb, int countNeeded) {
    std::vector<int> freeBlocks;

    file.seekg(sb.s_bm_block_start);
    for (int i = 0; i < sb.s_blocks_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) break;

        if (bit == '0') {
            freeBlocks.push_back(i);
            if ((int)freeBlocks.size() == countNeeded) {
                break;
            }
        }
    }

    return freeBlocks;
}

bool FileSystemManager::ReadUsersTxt(std::fstream& file,
                                     const SuperBlock& sb,
                                     const Inode& usersInode,
                                     std::string& outContent,
                                     std::string& outMsg) {
    outContent.clear();
    outMsg.clear();

    for (int i = 0; i < 15; i++) {
        int blockIndex = usersInode.i_block[i];
        if (blockIndex == -1) continue;

        FileBlock block{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
        file.read(reinterpret_cast<char*>(&block), sizeof(FileBlock));
        if (!file) {
            outMsg = "No se pudo leer un bloque de users.txt.";
            return false;
        }

        outContent.append(block.b_content, sizeof(block.b_content));
    }

    if ((int)outContent.size() > usersInode.i_size) {
        outContent = outContent.substr(0, usersInode.i_size);
    }

    return true;
}

bool FileSystemManager::WriteUsersTxt(std::fstream& file,
                                      SuperBlock& sb,
                                      Inode& usersInode,
                                      const std::string& newContent,
                                      std::string& outMsg) {
    outMsg.clear();

    int requiredBlocks = (int)((newContent.size() + sizeof(FileBlock) - 1) / sizeof(FileBlock));
    if (requiredBlocks <= 0) requiredBlocks = 1;

    if (requiredBlocks > 15) {
        outMsg = "users.txt requiere mas de 15 bloques directos. Aun no se soportan indirectos.";
        return false;
    }

    std::vector<int> currentBlocks = getUsersTxtUsedBlocks(usersInode);

    // Si faltan bloques, asignar nuevos
    if ((int)currentBlocks.size() < requiredBlocks) {
        int missing = requiredBlocks - (int)currentBlocks.size();

        std::vector<int> freeBlocks = findFreeBlocks(file, sb, missing);
        if ((int)freeBlocks.size() < missing) {
            outMsg = "No hay suficientes bloques libres para users.txt.";
            return false;
        }

        int pos = 0;
        for (int i = 0; i < 15 && pos < missing; i++) {
            if (usersInode.i_block[i] == -1) {
                usersInode.i_block[i] = freeBlocks[pos++];
            }
        }

        for (int idx : freeBlocks) {
            file.seekp(sb.s_bm_block_start + idx);
            char one = '1';
            file.write(&one, 1);
            if (!file) {
                outMsg = "No se pudo actualizar el bitmap de bloques.";
                return false;
            }
        }

        sb.s_free_blocks_count -= missing;
    }

    // Si sobran bloques, liberar
    currentBlocks = getUsersTxtUsedBlocks(usersInode);
    if ((int)currentBlocks.size() > requiredBlocks) {
        for (int i = requiredBlocks; i < (int)currentBlocks.size(); i++) {
            int idx = currentBlocks[i];

            file.seekp(sb.s_bm_block_start + idx);
            char zero = '0';
            file.write(&zero, 1);

            FileBlock emptyBlock{};
            std::memset(emptyBlock.b_content, 0, sizeof(emptyBlock.b_content));
            file.seekp(sb.s_block_start + idx * (int)sizeof(FileBlock));
            file.write(reinterpret_cast<char*>(&emptyBlock), sizeof(FileBlock));

            for (int j = 0; j < 15; j++) {
                if (usersInode.i_block[j] == idx) {
                    usersInode.i_block[j] = -1;
                    break;
                }
            }

            sb.s_free_blocks_count += 1;
        }
    }

    // Releer bloques finales
    std::vector<int> finalBlocks = getUsersTxtUsedBlocks(usersInode);
    if ((int)finalBlocks.size() < requiredBlocks) {
        outMsg = "No se pudieron asignar suficientes bloques a users.txt.";
        return false;
    }

    // Escribir contenido repartido
    size_t offset = 0;
    for (int i = 0; i < requiredBlocks; i++) {
        FileBlock block{};
        std::memset(block.b_content, 0, sizeof(block.b_content));

        size_t remaining = newContent.size() - offset;
        size_t chunk = std::min(remaining, sizeof(block.b_content));
        std::memcpy(block.b_content, newContent.data() + offset, chunk);

        int blockIndex = finalBlocks[i];
        file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
        file.write(reinterpret_cast<char*>(&block), sizeof(FileBlock));
        if (!file) {
            outMsg = "No se pudo escribir un bloque de users.txt.";
            return false;
        }

        offset += chunk;
    }

    usersInode.i_size = (int)newContent.size();
    return true;
}

int FileSystemManager::AllocateFreeInode(std::fstream& file, SuperBlock& sb) {
    file.seekg(sb.s_bm_inode_start);
    for (int i = 0; i < sb.s_inodes_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) return -1;

        if (bit == '0') {
            file.seekp(sb.s_bm_inode_start + i);
            char one = '1';
            file.write(&one, 1);

            sb.s_free_inodes_count--;
            sb.s_first_ino = i + 1;
            return i;
        }
    }
    return -1;
}

int FileSystemManager::AllocateFreeBlock(std::fstream& file, SuperBlock& sb) {
    file.seekg(sb.s_bm_block_start);
    for (int i = 0; i < sb.s_blocks_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) return -1;

        if (bit == '0') {
            file.seekp(sb.s_bm_block_start + i);
            char one = '1';
            file.write(&one, 1);

            sb.s_free_blocks_count--;
            sb.s_first_blo = i + 1;
            return i;
        }
    }
    return -1;
}

int FileSystemManager::FindEntryInFolder(std::fstream& file,
                                         const SuperBlock& sb,
                                         const Inode& folderInode,
                                         const std::string& name) {
    for (int i = 0; i < 12; i++) {
        if (folderInode.i_block[i] == -1) continue;

        FolderBlock folderBlock{};
        file.seekg(sb.s_block_start + folderInode.i_block[i] * (int)sizeof(FolderBlock));
        file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
        if (!file) return -1;

        for (int j = 0; j < 4; j++) {
            if (folderBlock.b_content[j].b_inodo == -1) continue;

            const char* raw = folderBlock.b_content[j].b_name;
            size_t len = 0;
            while (len < sizeof(folderBlock.b_content[j].b_name) && raw[len] != '\0') {
                ++len;
            }

            std::string currentName(raw, len);

            if (currentName == name) {
                return folderBlock.b_content[j].b_inodo;
            }
        }
    }
    return -1;
}

bool FileSystemManager::AddEntryToFolder(std::fstream& file,
                                         SuperBlock& sb,
                                         Inode& parentInode,
                                         int parentInodeIndex,
                                         const std::string& name,
                                         int childInodeIndex,
                                         std::string& outMsg) {
    outMsg.clear();

    FolderBlock refBlock{};
    const size_t NAME_LEN = sizeof(refBlock.b_content[0].b_name);

    if (name.empty()) {
        outMsg = "Nombre vacio para entrada de carpeta.";
        return false;
    }

    if (name.size() > NAME_LEN) {
        outMsg = "El nombre excede el limite soportado por el sistema de archivos.";
        return false;
    }

    // 1) Intentar meter la entrada en un bloque carpeta existente
    for (int i = 0; i < 12; i++) {
        int blockIndex = parentInode.i_block[i];
        if (blockIndex == -1) continue;

        FolderBlock folderBlock{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
        file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
        if (!file) {
            outMsg = "No se pudo leer un bloque carpeta del padre.";
            return false;
        }

        for (int j = 0; j < 4; j++) {
            if (folderBlock.b_content[j].b_inodo == -1) {
                std::memset(folderBlock.b_content[j].b_name, 0, NAME_LEN);
                std::memcpy(folderBlock.b_content[j].b_name, name.c_str(), name.size());
                folderBlock.b_content[j].b_inodo = childInodeIndex;

                file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
                file.write(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
                if (!file) {
                    outMsg = "No se pudo escribir el bloque carpeta del padre.";
                    return false;
                }

                std::string now = nowStringFS();
                std::memset(parentInode.i_mtime, 0, sizeof(parentInode.i_mtime));
                std::strncpy(parentInode.i_mtime, now.c_str(), sizeof(parentInode.i_mtime) - 1);

                file.seekp(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
                file.write(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
                return (bool)file;
            }
        }
    }

    // 2) Si no hay espacio, crear nuevo bloque carpeta
    for (int i = 0; i < 12; i++) {
        if (parentInode.i_block[i] == -1) {
            int newBlockIndex = AllocateFreeBlock(file, sb);
            if (newBlockIndex == -1) {
                outMsg = "No hay bloques libres para expandir la carpeta padre.";
                return false;
            }

            FolderBlock newFolderBlock{};
            for (int j = 0; j < 4; j++) {
                newFolderBlock.b_content[j].b_inodo = -1;
                std::memset(newFolderBlock.b_content[j].b_name, 0, NAME_LEN);
            }

            std::memcpy(newFolderBlock.b_content[0].b_name, name.c_str(), name.size());
            newFolderBlock.b_content[0].b_inodo = childInodeIndex;

            file.seekp(sb.s_block_start + newBlockIndex * (int)sizeof(FolderBlock));
            file.write(reinterpret_cast<char*>(&newFolderBlock), sizeof(FolderBlock));
            if (!file) {
                outMsg = "No se pudo escribir el nuevo bloque carpeta del padre.";
                return false;
            }

            parentInode.i_block[i] = newBlockIndex;

            std::string now = nowStringFS();
            std::memset(parentInode.i_mtime, 0, sizeof(parentInode.i_mtime));
            std::strncpy(parentInode.i_mtime, now.c_str(), sizeof(parentInode.i_mtime) - 1);

            file.seekp(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
            file.write(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
            return (bool)file;
        }
    }

    outMsg = "La carpeta padre ya no tiene apuntadores directos disponibles.";
    return false;
}

bool FileSystemManager::AppendJournalEntry(std::fstream& file,
                                           int partitionStart,
                                           const SuperBlock& sb,
                                           const std::string& operation,
                                           const std::string& path,
                                           const std::string& content,
                                           std::string& outMsg) {
    outMsg.clear();

    if (sb.s_filesystem_type != 3) {
        return true; // solo EXT3 lleva journaling
    }

    int journalStart = partitionStart + (int)sizeof(SuperBlock);

    Journal journal{};
    file.clear();
    file.seekg(journalStart);
    file.read(reinterpret_cast<char*>(&journal), sizeof(Journal));
    if (!file) {
        outMsg = "No se pudo leer el Journal de la particion.";
        return false;
    }

    Information entry{};
    std::string op = sanitizeJournalText(operation);
    std::string p = sanitizeJournalText(path);
    std::string c = sanitizeJournalText(content);

    std::strncpy(entry.i_operation, op.c_str(), sizeof(entry.i_operation) - 1);
    std::strncpy(entry.i_path, p.c_str(), sizeof(entry.i_path) - 1);
    std::strncpy(entry.i_content, c.c_str(), sizeof(entry.i_content) - 1);
    entry.i_date = nowJournalValue();

    const int maxEntries = (int)(sizeof(journal.j_content) / sizeof(journal.j_content[0]));
    if (journal.j_count < 0) journal.j_count = 0;

    int writeIndex = 0;
    if (journal.j_count < maxEntries) {
        writeIndex = journal.j_count;
        journal.j_count++;
    } else {
        for (int i = 1; i < maxEntries; i++) {
            journal.j_content[i - 1] = journal.j_content[i];
        }
        writeIndex = maxEntries - 1;
    }

    journal.j_content[writeIndex] = entry;

    file.clear();
    file.seekp(journalStart);
    file.write(reinterpret_cast<char*>(&journal), sizeof(Journal));
    if (!file) {
        outMsg = "No se pudo escribir el Journal de la particion.";
        return false;
    }

    return true;
}

bool FileSystemManager::ShowJournaling(const std::string& id, std::string& outMsg) {
    outMsg.clear();

    MountedPartition mp{};
    if (!MountManager::FindById(id, mp)) {
        outMsg = "No existe una particion montada con id: " + id;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la particion montada.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    if (sb.s_magic != 0xEF53) {
        outMsg = "La particion no contiene un sistema de archivos valido.";
        file.close();
        return false;
    }

    if (sb.s_filesystem_type != 3) {
        outMsg = "La particion no esta formateada en EXT3, por lo tanto no tiene journaling.";
        file.close();
        return false;
    }

    Journal journal{};
    int journalStart = mp.start + (int)sizeof(SuperBlock);

    file.seekg(journalStart);
    file.read(reinterpret_cast<char*>(&journal), sizeof(Journal));
    if (!file) {
        outMsg = "No se pudo leer el Journal.";
        file.close();
        return false;
    }

    file.close();

    const int maxEntries = (int)(sizeof(journal.j_content) / sizeof(journal.j_content[0]));
    int total = journal.j_count;
    if (total < 0) total = 0;
    if (total > maxEntries) total = maxEntries;

    std::ostringstream oss;
    oss << "[JOURNALING]\n";
    oss << "  - id=" << mp.id
        << " | name=" << mp.name
        << " | total=" << total << "\n";

    if (total == 0) {
        oss << "  [INFO] No hay entradas de journaling.\n";
        outMsg = oss.str();
        return true;
    }

    for (int i = 0; i < total; i++) {
        const Information& info = journal.j_content[i];

        std::string op = sanitizeJournalText(info.i_operation);
        std::string path = sanitizeJournalText(info.i_path);
        std::string content = sanitizeJournalText(info.i_content);
        std::string date = journalDateToString(info.i_date);

        if (op.empty()) op = "-";
        if (path.empty()) path = "-";
        if (content.empty()) content = "-";

        oss << "  [" << (i + 1) << "] "
            << "op=" << op
            << " | path=" << path
            << " | content=" << content
            << " | date=" << date
            << "\n";
    }

    outMsg = oss.str();
    return true;
}

bool FileSystemManager::Mkfs(const std::string& id,
                             const std::string& type,
                             const std::string& fs,
                             std::string& outMsg) {
    outMsg.clear();

    std::string typeL = trimFS(type);
    std::transform(typeL.begin(), typeL.end(), typeL.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (typeL.empty()) typeL = "full";
    if (typeL != "full") {
        outMsg = "mkfs solo permite -type=full";
        return false;
    }

    std::string fsL = trimFS(fs);
    std::transform(fsL.begin(), fsL.end(), fsL.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (fsL.empty()) fsL = "2fs";
    if (fsL != "2fs" && fsL != "3fs") {
        outMsg = "Valor invalido en -fs. Use 2fs o 3fs";
        return false;
    }

    const bool isExt3 = (fsL == "3fs");

    MountedPartition mp{};
    if (!MountManager::FindById(id, mp)) {
        outMsg = "Error: no existe una particion montada con id: " + id;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo abrir el disco de la particion montada.";
        return false;
    }

    std::vector<char> zeros(mp.size, 0);
    file.seekp(mp.start);
    file.write(zeros.data(), (std::streamsize)zeros.size());

    int n = calcN(mp.size, isExt3);
    if (n <= 0) {
        outMsg = "Error: la particion es demasiado pequena para formatearse.";
        file.close();
        return false;
    }

    SuperBlock sb{};
    sb.s_filesystem_type = isExt3 ? 3 : 2;
    sb.s_inodes_count = n;
    sb.s_blocks_count = 3 * n;
    sb.s_free_inodes_count = n - 2;
    sb.s_free_blocks_count = 3 * n - 2;
    std::memset(sb.s_mtime, 0, sizeof(sb.s_mtime));
    std::memset(sb.s_umtime, 0, sizeof(sb.s_umtime));
    std::string now = nowStringFS();
    std::strncpy(sb.s_mtime, now.c_str(), sizeof(sb.s_mtime) - 1);
    std::strncpy(sb.s_umtime, now.c_str(), sizeof(sb.s_umtime) - 1);
    sb.s_mnt_count = 1;
    sb.s_magic = 0xEF53;
    sb.s_inode_size = sizeof(Inode);
    sb.s_block_size = sizeof(FileBlock);
    sb.s_first_ino = 2;
    sb.s_first_blo = 2;

    int journalStart = mp.start + (int)sizeof(SuperBlock);
    sb.s_bm_inode_start = journalStart + (isExt3 ? (int)sizeof(Journal) : 0);
    sb.s_bm_block_start = sb.s_bm_inode_start + n;
    sb.s_inode_start = sb.s_bm_block_start + (3 * n);
    sb.s_block_start = sb.s_inode_start + (n * (int)sizeof(Inode));

    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));

    if (isExt3) {
        Journal journal{};
        journal.j_count = 2;

        std::strncpy(journal.j_content[0].i_operation, "mkdir", sizeof(journal.j_content[0].i_operation) - 1);
        std::strncpy(journal.j_content[0].i_path, "/", sizeof(journal.j_content[0].i_path) - 1);
        std::strncpy(journal.j_content[0].i_content, "root", sizeof(journal.j_content[0].i_content) - 1);
        journal.j_content[0].i_date = nowJournalValue();

        std::strncpy(journal.j_content[1].i_operation, "mkfile", sizeof(journal.j_content[1].i_operation) - 1);
        std::strncpy(journal.j_content[1].i_path, "/users.txt", sizeof(journal.j_content[1].i_path) - 1);
        std::strncpy(journal.j_content[1].i_content, "1,G,root\n1,U,root,root,123\n", sizeof(journal.j_content[1].i_content) - 1);
        journal.j_content[1].i_date = nowJournalValue();

        file.seekp(journalStart);
        file.write(reinterpret_cast<char*>(&journal), sizeof(Journal));
    }

    file.seekp(sb.s_bm_inode_start);
    for (int i = 0; i < n; i++) {
        char value = '0';
        if (i == 0 || i == 1) value = '1';
        file.write(&value, 1);
    }

    file.seekp(sb.s_bm_block_start);
    for (int i = 0; i < 3 * n; i++) {
        char value = '0';
        if (i == 0 || i == 1) value = '1';
        file.write(&value, 1);
    }

    Inode emptyInode{};
    emptyInode.i_uid = -1;
    emptyInode.i_gid = -1;
    emptyInode.i_size = 0;
    std::memset(emptyInode.i_atime, 0, sizeof(emptyInode.i_atime));
    std::memset(emptyInode.i_ctime, 0, sizeof(emptyInode.i_ctime));
    std::memset(emptyInode.i_mtime, 0, sizeof(emptyInode.i_mtime));
    for (int& ptr : emptyInode.i_block) ptr = -1;
    emptyInode.i_type = '0';
    std::memset(emptyInode.i_perm, 0, sizeof(emptyInode.i_perm));

    file.seekp(sb.s_inode_start);
    for (int i = 0; i < n; i++) {
        file.write(reinterpret_cast<char*>(&emptyInode), sizeof(Inode));
    }

    FileBlock emptyBlock{};
    std::memset(emptyBlock.b_content, 0, sizeof(emptyBlock.b_content));

    file.seekp(sb.s_block_start);
    for (int i = 0; i < 3 * n; i++) {
        file.write(reinterpret_cast<char*>(&emptyBlock), sizeof(FileBlock));
    }

    Inode root{};
    root.i_uid = 1;
    root.i_gid = 1;
    root.i_size = 0;
    std::strncpy(root.i_atime, now.c_str(), sizeof(root.i_atime) - 1);
    std::strncpy(root.i_ctime, now.c_str(), sizeof(root.i_ctime) - 1);
    std::strncpy(root.i_mtime, now.c_str(), sizeof(root.i_mtime) - 1);
    for (int& ptr : root.i_block) ptr = -1;
    root.i_block[0] = 0;
    root.i_type = '0';
    std::memcpy(root.i_perm, "777", 3);

    FolderBlock rootBlock{};
    std::memset(&rootBlock, 0, sizeof(FolderBlock));

    std::strncpy(rootBlock.b_content[0].b_name, ".", sizeof(rootBlock.b_content[0].b_name) - 1);
    rootBlock.b_content[0].b_inodo = 0;

    std::strncpy(rootBlock.b_content[1].b_name, "..", sizeof(rootBlock.b_content[1].b_name) - 1);
    rootBlock.b_content[1].b_inodo = 0;

    std::strncpy(rootBlock.b_content[2].b_name, "users.txt", sizeof(rootBlock.b_content[2].b_name) - 1);
    rootBlock.b_content[2].b_inodo = 1;

    rootBlock.b_content[3].b_inodo = -1;

    std::string usersContent = "1,G,root\n1,U,root,root,123\n";

    Inode usersInode{};
    usersInode.i_uid = 1;
    usersInode.i_gid = 1;
    usersInode.i_size = (int)usersContent.size();
    std::strncpy(usersInode.i_atime, now.c_str(), sizeof(usersInode.i_atime) - 1);
    std::strncpy(usersInode.i_ctime, now.c_str(), sizeof(usersInode.i_ctime) - 1);
    std::strncpy(usersInode.i_mtime, now.c_str(), sizeof(usersInode.i_mtime) - 1);
    for (int& ptr : usersInode.i_block) ptr = -1;
    usersInode.i_block[0] = 1;
    usersInode.i_type = '1';
    std::memcpy(usersInode.i_perm, "664", 3);

    FileBlock usersBlock{};
    std::memset(usersBlock.b_content, 0, sizeof(usersBlock.b_content));
    std::strncpy(usersBlock.b_content, usersContent.c_str(), sizeof(usersBlock.b_content) - 1);

    file.seekp(sb.s_inode_start + 0 * (int)sizeof(Inode));
    file.write(reinterpret_cast<char*>(&root), sizeof(Inode));

    file.seekp(sb.s_inode_start + 1 * (int)sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));

    file.seekp(sb.s_block_start + 0 * (int)sizeof(FolderBlock));
    file.write(reinterpret_cast<char*>(&rootBlock), sizeof(FolderBlock));

    file.seekp(sb.s_block_start + 1 * (int)sizeof(FileBlock));
    file.write(reinterpret_cast<char*>(&usersBlock), sizeof(FileBlock));

    file.close();

    outMsg = "Particion formateada correctamente en " + std::string(isExt3 ? "EXT3" : "EXT2") +
             ". id=" + id +
             " | inodos=" + std::to_string(n) +
             " | bloques=" + std::to_string(3 * n);
    return true;
}

bool FileSystemManager::Mkgrp(const std::string& groupName, std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (SessionManager::currentSession.user != "root") {
        outMsg = "Solo el usuario root puede ejecutar mkgrp.";
        return false;
    }

    if (groupName.empty()) {
        outMsg = "El parametro -name es obligatorio.";
        return false;
    }

    if (groupName.size() > 10) {
        outMsg = "El nombre del grupo no puede exceder 10 caracteres.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    // 1) Leer superbloque
    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    // 2) Leer inodo de users.txt (inodo 1)
    Inode usersInode{};
    file.seekg(sb.s_inode_start + sizeof(Inode));
    file.read(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo de users.txt.";
        file.close();
        return false;
    }

    // 3) Leer contenido completo de users.txt (multibloque)
    std::string content;
    if (!ReadUsersTxt(file, sb, usersInode, content, outMsg)) {
        file.close();
        return false;
    }

    std::stringstream ss(content);
    std::string line;

    int maxGroupId = 0;
    bool exists = false;

    while (std::getline(ss, line)) {
        line = trimFS(line);
        if (line.empty()) continue;

        std::vector<std::string> cols = splitFS(line, ',');
        if (cols.size() < 3) continue;

        for (std::string& c : cols) {
            c = trimFS(c);
        }

        // ignorar eliminados
        if (cols[0] == "0") continue;

        if (cols[1] == "G") {
            int gid = 0;
            try {
                gid = std::stoi(cols[0]);
            } catch (...) {
                gid = 0;
            }

            if (gid > maxGroupId) {
                maxGroupId = gid;
            }

            if (cols[2] == groupName) {
                exists = true;
            }
        }
    }

    if (exists) {
        outMsg = "El grupo ya existe: " + groupName;
        file.close();
        return false;
    }

    int newId = maxGroupId + 1;
    std::string newLine = std::to_string(newId) + ",G," + groupName + "\n";
    std::string newContent = content + newLine;

    if (!WriteUsersTxt(file, sb, usersInode, newContent, outMsg)) {
        file.close();
        return false;
    }

    std::string now = nowStringFS();
    std::memset(usersInode.i_mtime, 0, sizeof(usersInode.i_mtime));
    std::strncpy(usersInode.i_mtime, now.c_str(), sizeof(usersInode.i_mtime) - 1);

    // escribir inodo actualizado
    file.seekp(sb.s_inode_start + sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo actualizar el inodo de users.txt.";
        file.close();
        return false;
    }

    // escribir superbloque actualizado
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "mkgrp", "/users.txt",
                            "name=" + groupName, journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();
    outMsg = "Grupo creado correctamente: " + groupName;
    return true;
}

bool FileSystemManager::Rmgrp(const std::string& groupName, std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (SessionManager::currentSession.user != "root") {
        outMsg = "Solo el usuario root puede ejecutar rmgrp.";
        return false;
    }

    if (groupName.empty()) {
        outMsg = "El parametro -name es obligatorio.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    // 1) Leer superbloque
    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    // 2) Leer inodo de users.txt
    Inode usersInode{};
    file.seekg(sb.s_inode_start + sizeof(Inode)); // inodo 1
    file.read(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo de users.txt.";
        file.close();
        return false;
    }

    // 3) Leer contenido completo de users.txt (multibloque)
    std::string content;
    if (!ReadUsersTxt(file, sb, usersInode, content, outMsg)) {
        file.close();
        return false;
    }

    std::stringstream ss(content);
    std::string line;

    std::vector<std::string> newLines;
    bool found = false;

    while (std::getline(ss, line)) {
        std::string originalLine = trimFS(line);
        if (originalLine.empty()) continue;

        std::vector<std::string> cols = splitFS(originalLine, ',');
        if (cols.size() < 3) {
            newLines.push_back(originalLine);
            continue;
        }

        for (std::string& c : cols) {
            c = trimFS(c);
        }

        // Si es grupo y coincide el nombre
        if (cols[1] == "G" && cols[2] == groupName) {
            // Si ya estaba eliminado
            if (cols[0] == "0") {
                outMsg = "El grupo ya estaba eliminado: " + groupName;
                file.close();
                return false;
            }

            cols[0] = "0";
            found = true;
        }

        // reconstruir línea
        std::string rebuilt;
        for (size_t i = 0; i < cols.size(); i++) {
            rebuilt += cols[i];
            if (i + 1 < cols.size()) rebuilt += ",";
        }
        newLines.push_back(rebuilt);
    }

    if (!found) {
        outMsg = "El grupo no existe: " + groupName;
        file.close();
        return false;
    }

    // reconstruir contenido completo
    std::string newContent;
    for (const auto& l : newLines) {
        newContent += l + "\n";
    }

    if (!WriteUsersTxt(file, sb, usersInode, newContent, outMsg)) {
        file.close();
        return false;
    }

    std::string now = nowStringFS();
    std::memset(usersInode.i_mtime, 0, sizeof(usersInode.i_mtime));
    std::strncpy(usersInode.i_mtime, now.c_str(), sizeof(usersInode.i_mtime) - 1);

    // escribir inodo actualizado
    file.seekp(sb.s_inode_start + sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo actualizar el inodo de users.txt.";
        file.close();
        return false;
    }

    // escribir superbloque actualizado
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "rmgrp", "/users.txt", "group=" + groupName, journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Grupo eliminado correctamente: " + groupName;
    return true;
}

bool FileSystemManager::Mkusr(const std::string& user,
                              const std::string& pass,
                              const std::string& group,
                              std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (SessionManager::currentSession.user != "root") {
        outMsg = "Solo el usuario root puede ejecutar mkusr.";
        return false;
    }

    if (user.empty() || pass.empty() || group.empty()) {
        outMsg = "Los parametros -user, -pass y -grp son obligatorios.";
        return false;
    }

    if (user.size() > 10 || pass.size() > 10 || group.size() > 10) {
        outMsg = "user, pass y grp no pueden exceder 10 caracteres.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    // 1) Leer superbloque
    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    // 2) Leer inodo de users.txt
    Inode usersInode{};
    file.seekg(sb.s_inode_start + sizeof(Inode)); // inodo 1
    file.read(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo de users.txt.";
        file.close();
        return false;
    }

    // 3) Leer bloque de users.txt
    std::string content;
    if (!ReadUsersTxt(file, sb, usersInode, content, outMsg)) {
        file.close();
        return false;
    }

    std::stringstream ss(content);
    std::string line;

    int maxUserId = 0;
    bool groupExists = false;
    bool userExists = false;

    while (std::getline(ss, line)) {
        line = trimFS(line);
        if (line.empty()) continue;

        std::vector<std::string> cols = splitFS(line, ',');
        if (cols.size() < 3) continue;

        for (std::string& c : cols) {
            c = trimFS(c);
        }

        // ignorar eliminados
        if (cols[0] == "0") continue;

        if (cols[1] == "G") {
            if (cols[2] == group) {
                groupExists = true;
            }
        }

        if (cols.size() == 5 && cols[1] == "U") {
            int uid = 0;
            try {
                uid = std::stoi(cols[0]);
            } catch (...) {
                uid = 0;
            }

            if (uid > maxUserId) {
                maxUserId = uid;
            }

            if (cols[3] == user) {
                userExists = true;
            }
        }
    }

    if (!groupExists) {
        outMsg = "El grupo no existe o esta eliminado: " + group;
        file.close();
        return false;
    }

    if (userExists) {
        outMsg = "El usuario ya existe: " + user;
        file.close();
        return false;
    }

    int newUserId = maxUserId + 1;
    std::string newLine = std::to_string(newUserId) + ",U," + group + "," + user + "," + pass + "\n";
    std::string newContent = content + newLine;

    // 4) Reescribir bloque
    if (!WriteUsersTxt(file, sb, usersInode, newContent, outMsg)) {
        file.close();
        return false;
    }

    std::string now = nowStringFS();
    std::memset(usersInode.i_mtime, 0, sizeof(usersInode.i_mtime));
    std::strncpy(usersInode.i_mtime, now.c_str(), sizeof(usersInode.i_mtime) - 1);

    // escribir inodo actualizado
    file.seekp(sb.s_inode_start + sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo actualizar el inodo de users.txt.";
        file.close();
        return false;
    }

    // escribir superbloque actualizado
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "mkusr", "/users.txt",
                            "user=" + user + ",pass=" + pass + ",grp=" + group, journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Usuario creado correctamente: " + user + " | grupo=" + group;
    return true;
}

bool FileSystemManager::Rmusr(const std::string& user, std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (SessionManager::currentSession.user != "root") {
        outMsg = "Solo el usuario root puede ejecutar rmusr.";
        return false;
    }

    if (user.empty()) {
        outMsg = "El parametro -user es obligatorio.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    Inode usersInode{};
    file.seekg(sb.s_inode_start + sizeof(Inode)); // inodo 1
    file.read(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo de users.txt.";
        file.close();
        return false;
    }

    std::string content;
    if (!ReadUsersTxt(file, sb, usersInode, content, outMsg)) {
        file.close();
        return false;
    }

    std::stringstream ss(content);
    std::string line;

    std::vector<std::string> newLines;
    bool found = false;

    while (std::getline(ss, line)) {
        std::string originalLine = trimFS(line);
        if (originalLine.empty()) continue;

        std::vector<std::string> cols = splitFS(originalLine, ',');
        if (cols.size() < 3) {
            newLines.push_back(originalLine);
            continue;
        }

        for (std::string& c : cols) {
            c = trimFS(c);
        }

        if (cols.size() == 5 && cols[1] == "U" && cols[3] == user) {
            if (cols[0] == "0") {
                outMsg = "El usuario ya estaba eliminado: " + user;
                file.close();
                return false;
            }

            cols[0] = "0";
            found = true;
        }

        std::string rebuilt;
        for (size_t i = 0; i < cols.size(); i++) {
            rebuilt += cols[i];
            if (i + 1 < cols.size()) rebuilt += ",";
        }
        newLines.push_back(rebuilt);
    }

    if (!found) {
        outMsg = "El usuario no existe: " + user;
        file.close();
        return false;
    }

    std::string newContent;
    for (const auto& l : newLines) {
        newContent += l + "\n";
    }

    if (!WriteUsersTxt(file, sb, usersInode, newContent, outMsg)) {
        file.close();
        return false;
    }

    std::string now = nowStringFS();
    std::memset(usersInode.i_mtime, 0, sizeof(usersInode.i_mtime));
    std::strncpy(usersInode.i_mtime, now.c_str(), sizeof(usersInode.i_mtime) - 1);

    // escribir inodo actualizado
    file.seekp(sb.s_inode_start + sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo actualizar el inodo de users.txt.";
        file.close();
        return false;
    }

    // escribir superbloque actualizado
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "rmusr", "/users.txt",
                            "user=" + user, journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Usuario eliminado correctamente: " + user;
    return true;
}

bool FileSystemManager::Chgrp(const std::string& user,
                              const std::string& group,
                              std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (SessionManager::currentSession.user != "root") {
        outMsg = "Solo el usuario root puede ejecutar chgrp.";
        return false;
    }

    if (user.empty() || group.empty()) {
        outMsg = "Los parametros -user y -grp son obligatorios.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    Inode usersInode{};
    file.seekg(sb.s_inode_start + sizeof(Inode)); // inodo 1
    file.read(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo de users.txt.";
        file.close();
        return false;
    }

    std::string content;
    if (!ReadUsersTxt(file, sb, usersInode, content, outMsg)) {
        file.close();
        return false;
    }

    std::stringstream ss(content);
    std::string line;

    std::vector<std::string> newLines;
    bool groupExists = false;
    bool userExists = false;

    while (std::getline(ss, line)) {
        std::string originalLine = trimFS(line);
        if (originalLine.empty()) continue;

        std::vector<std::string> cols = splitFS(originalLine, ',');
        if (cols.size() < 3) {
            newLines.push_back(originalLine);
            continue;
        }

        for (std::string& c : cols) {
            c = trimFS(c);
        }

        // Verificar grupo activo
        if (cols[0] != "0" && cols[1] == "G" && cols[2] == group) {
            groupExists = true;
        }

        // Cambiar grupo del usuario activo
        if (cols.size() == 5 && cols[1] == "U" && cols[3] == user) {
            if (cols[0] == "0") {
                outMsg = "El usuario esta eliminado: " + user;
                file.close();
                return false;
            }

            userExists = true;
            cols[2] = group;
        }

        std::string rebuilt;
        for (size_t i = 0; i < cols.size(); i++) {
            rebuilt += cols[i];
            if (i + 1 < cols.size()) rebuilt += ",";
        }
        newLines.push_back(rebuilt);
    }

    if (!groupExists) {
        outMsg = "El grupo no existe o esta eliminado: " + group;
        file.close();
        return false;
    }

    if (!userExists) {
        outMsg = "El usuario no existe: " + user;
        file.close();
        return false;
    }

    std::string newContent;
    for (const auto& l : newLines) {
        newContent += l + "\n";
    }

        if (!WriteUsersTxt(file, sb, usersInode, newContent, outMsg)) {
        file.close();
        return false;
    }

    std::string now = nowStringFS();
    std::memset(usersInode.i_mtime, 0, sizeof(usersInode.i_mtime));
    std::strncpy(usersInode.i_mtime, now.c_str(), sizeof(usersInode.i_mtime) - 1);

    // escribir inodo actualizado
    file.seekp(sb.s_inode_start + sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo actualizar el inodo de users.txt.";
        file.close();
        return false;
    }

    // escribir superbloque actualizado
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "chgrp", "/users.txt",
                            "user=" + user + ",grp=" + group, journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Grupo del usuario actualizado correctamente: " + user + " -> " + group;
    return true;
}

static std::string buildFileContent(int size) {
    if (size <= 0) return "";

    std::string content;
    content.reserve((size_t)size);

    for (int i = 0; i < size; i++) {
        content.push_back((char)('0' + (i % 10)));
    }

    return content;
}

static bool readFileContent(std::fstream& file,
                            const SuperBlock& sb,
                            const Inode& inode,
                            std::string& outContent,
                            std::string& outMsg) {
    outContent.clear();
    outMsg.clear();

    for (int i = 0; i < 12; i++) { // solo directos por ahora
        int blockIndex = inode.i_block[i];
        if (blockIndex == -1) continue;

        FileBlock fb{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
        file.read(reinterpret_cast<char*>(&fb), sizeof(FileBlock));
        if (!file) {
            outMsg = "No se pudo leer un bloque del archivo.";
            return false;
        }

        outContent.append(fb.b_content, sizeof(fb.b_content));
    }

    if ((int)outContent.size() > inode.i_size) {
        outContent = outContent.substr(0, inode.i_size);
    }

    return true;
}

static std::vector<int> getRegularFileUsedBlocks(const Inode& inode) {
    std::vector<int> blocks;
    for (int i = 0; i < 12; i++) {
        if (inode.i_block[i] != -1) {
            blocks.push_back(inode.i_block[i]);
        }
    }
    return blocks;
}

static bool WriteRegularFileContent(std::fstream& file,
                                    SuperBlock& sb,
                                    Inode& fileInode,
                                    const std::string& newContent,
                                    std::string& outMsg) {
    outMsg.clear();

    int requiredBlocks = (int)((newContent.size() + sizeof(FileBlock) - 1) / sizeof(FileBlock));
    if (requiredBlocks <= 0) requiredBlocks = 1;

    if (requiredBlocks > 12) {
        outMsg = "El archivo requiere mas de 12 bloques directos. Aun no se soportan indirectos.";
        return false;
    }

    std::vector<int> currentBlocks = getRegularFileUsedBlocks(fileInode);

    // Asignar bloques faltantes
    if ((int)currentBlocks.size() < requiredBlocks) {
        int missing = requiredBlocks - (int)currentBlocks.size();

        std::vector<int> freeBlocks = findFreeBlocks(file, sb, missing);
        if ((int)freeBlocks.size() < missing) {
            outMsg = "No hay suficientes bloques libres para editar el archivo.";
            return false;
        }

        int pos = 0;
        for (int i = 0; i < 12 && pos < missing; i++) {
            if (fileInode.i_block[i] == -1) {
                fileInode.i_block[i] = freeBlocks[pos++];
            }
        }

        for (int idx : freeBlocks) {
            file.seekp(sb.s_bm_block_start + idx);
            char one = '1';
            file.write(&one, 1);
            if (!file) {
                outMsg = "No se pudo actualizar el bitmap de bloques.";
                return false;
            }
        }

        sb.s_free_blocks_count -= missing;
    }

    // Liberar bloques sobrantes
    currentBlocks = getRegularFileUsedBlocks(fileInode);
    if ((int)currentBlocks.size() > requiredBlocks) {
        for (int i = requiredBlocks; i < (int)currentBlocks.size(); i++) {
            int idx = currentBlocks[i];

            file.seekp(sb.s_bm_block_start + idx);
            char zero = '0';
            file.write(&zero, 1);
            if (!file) {
                outMsg = "No se pudo actualizar el bitmap al liberar bloques.";
                return false;
            }

            FileBlock emptyBlock{};
            std::memset(emptyBlock.b_content, 0, sizeof(emptyBlock.b_content));
            file.seekp(sb.s_block_start + idx * (int)sizeof(FileBlock));
            file.write(reinterpret_cast<char*>(&emptyBlock), sizeof(FileBlock));
            if (!file) {
                outMsg = "No se pudo limpiar un bloque liberado.";
                return false;
            }

            for (int j = 0; j < 12; j++) {
                if (fileInode.i_block[j] == idx) {
                    fileInode.i_block[j] = -1;
                    break;
                }
            }

            sb.s_free_blocks_count += 1;
        }
    }

    std::vector<int> finalBlocks = getRegularFileUsedBlocks(fileInode);
    if ((int)finalBlocks.size() < requiredBlocks) {
        outMsg = "No se pudieron asignar suficientes bloques al archivo.";
        return false;
    }

    // Escribir contenido
    size_t offset = 0;
    for (int i = 0; i < requiredBlocks; i++) {
        FileBlock block{};
        std::memset(block.b_content, 0, sizeof(block.b_content));

        size_t remaining = (offset < newContent.size()) ? (newContent.size() - offset) : 0;
        size_t chunk = std::min(remaining, sizeof(block.b_content));

        if (chunk > 0) {
            std::memcpy(block.b_content, newContent.data() + offset, chunk);
        }

        int blockIndex = finalBlocks[i];
        file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
        file.write(reinterpret_cast<char*>(&block), sizeof(FileBlock));
        if (!file) {
            outMsg = "No se pudo escribir un bloque del archivo.";
            return false;
        }

        offset += chunk;
    }

    // Caso archivo vacío
    if (newContent.empty() && !finalBlocks.empty()) {
        FileBlock emptyBlock{};
        std::memset(emptyBlock.b_content, 0, sizeof(emptyBlock.b_content));
        file.seekp(sb.s_block_start + finalBlocks[0] * (int)sizeof(FileBlock));
        file.write(reinterpret_cast<char*>(&emptyBlock), sizeof(FileBlock));
        if (!file) {
            outMsg = "No se pudo escribir el bloque vacio del archivo.";
            return false;
        }
    }

    fileInode.i_size = (int)newContent.size();

    std::string now = nowStringFS();
    std::strncpy(fileInode.i_mtime, now.c_str(), sizeof(fileInode.i_mtime) - 1);

    return true;
}

static std::string getContentNameFS(const Content& content) {
    size_t len = 0;
    while (len < sizeof(content.b_name) && content.b_name[len] != '\0') {
        len++;
    }
    return std::string(content.b_name, len);
}

bool FileSystemManager::FreeInodeBit(std::fstream& file, SuperBlock& sb, int inodeIndex) {
    if (inodeIndex < 0 || inodeIndex >= sb.s_inodes_count) {
        return false;
    }

    file.seekg(sb.s_bm_inode_start + inodeIndex);
    char bit = '0';
    file.read(&bit, 1);
    if (!file) return false;

    if (bit != '0') {
        file.seekp(sb.s_bm_inode_start + inodeIndex);
        char zero = '0';
        file.write(&zero, 1);
        if (!file) return false;

        sb.s_free_inodes_count++;

        if (sb.s_first_ino <= 0 || inodeIndex < sb.s_first_ino) {
            sb.s_first_ino = inodeIndex;
        }
    }

    return true;
}

bool FileSystemManager::FreeBlockBit(std::fstream& file, SuperBlock& sb, int blockIndex) {
    if (blockIndex < 0 || blockIndex >= sb.s_blocks_count) {
        return false;
    }

    file.seekg(sb.s_bm_block_start + blockIndex);
    char bit = '0';
    file.read(&bit, 1);
    if (!file) return false;

    if (bit != '0') {
        file.seekp(sb.s_bm_block_start + blockIndex);
        char zero = '0';
        file.write(&zero, 1);
        if (!file) return false;

        sb.s_free_blocks_count++;

        if (sb.s_first_blo <= 0 || blockIndex < sb.s_first_blo) {
            sb.s_first_blo = blockIndex;
        }
    }

    return true;
}

bool FileSystemManager::DeleteInodeRecursive(std::fstream& file,
                                             SuperBlock& sb,
                                             int inodeIndex,
                                             const Inode& inode,
                                             std::string& outMsg) {
    outMsg.clear();

    if (inode.i_type == '0') {
        // Carpeta
        for (int i = 0; i < 12; i++) {
            int blockIndex = inode.i_block[i];
            if (blockIndex == -1) continue;

            FolderBlock folderBlock{};
            file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
            file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
            if (!file) {
                outMsg = "No se pudo leer un bloque de carpeta durante remove.";
                return false;
            }

            for (int j = 0; j < 4; j++) {
                int childInodeIndex = folderBlock.b_content[j].b_inodo;
                if (childInodeIndex == -1) continue;

                std::string childName = getContentNameFS(folderBlock.b_content[j]);
                if (childName == "." || childName == "..") continue;

                Inode childInode{};
                file.seekg(sb.s_inode_start + childInodeIndex * (int)sizeof(Inode));
                file.read(reinterpret_cast<char*>(&childInode), sizeof(Inode));
                if (!file) {
                    outMsg = "No se pudo leer un inodo hijo durante remove.";
                    return false;
                }

                if (!DeleteInodeRecursive(file, sb, childInodeIndex, childInode, outMsg)) {
                    return false;
                }
            }

            FolderBlock emptyFolder{};
            for (int k = 0; k < 4; k++) {
                emptyFolder.b_content[k].b_inodo = -1;
                std::memset(emptyFolder.b_content[k].b_name, 0, sizeof(emptyFolder.b_content[k].b_name));
            }

            file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
            file.write(reinterpret_cast<char*>(&emptyFolder), sizeof(FolderBlock));
            if (!file) {
                outMsg = "No se pudo limpiar un bloque de carpeta durante remove.";
                return false;
            }

            if (!FreeBlockBit(file, sb, blockIndex)) {
                outMsg = "No se pudo liberar el bitmap de bloque de carpeta.";
                return false;
            }
        }
    } else {
        // Archivo
        for (int i = 0; i < 12; i++) {
            int blockIndex = inode.i_block[i];
            if (blockIndex == -1) continue;

            FileBlock emptyFile{};
            std::memset(emptyFile.b_content, 0, sizeof(emptyFile.b_content));

            file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
            file.write(reinterpret_cast<char*>(&emptyFile), sizeof(FileBlock));
            if (!file) {
                outMsg = "No se pudo limpiar un bloque de archivo durante remove.";
                return false;
            }

            if (!FreeBlockBit(file, sb, blockIndex)) {
                outMsg = "No se pudo liberar el bitmap de bloque de archivo.";
                return false;
            }
        }
    }

    Inode emptyInode{};
    for (int& ptr : emptyInode.i_block) ptr = -1;

    file.seekp(sb.s_inode_start + inodeIndex * (int)sizeof(Inode));
    file.write(reinterpret_cast<char*>(&emptyInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo limpiar el inodo durante remove.";
        return false;
    }

    if (!FreeInodeBit(file, sb, inodeIndex)) {
        outMsg = "No se pudo liberar el bitmap de inodo.";
        return false;
    }

    return true;
}

bool FileSystemManager::RemoveEntryFromFolder(std::fstream& file,
                                              const SuperBlock& sb,
                                              Inode& parentInode,
                                              int parentInodeIndex,
                                              const std::string& name,
                                              std::string& outMsg) {
    outMsg.clear();

    for (int i = 0; i < 12; i++) {
        int blockIndex = parentInode.i_block[i];
        if (blockIndex == -1) continue;

        FolderBlock folderBlock{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
        file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
        if (!file) {
            outMsg = "No se pudo leer un bloque de la carpeta padre.";
            return false;
        }

        for (int j = 0; j < 4; j++) {
            if (folderBlock.b_content[j].b_inodo == -1) continue;

            std::string entryName = getContentNameFS(folderBlock.b_content[j]);
            if (entryName == name) {
                folderBlock.b_content[j].b_inodo = -1;
                std::memset(folderBlock.b_content[j].b_name, 0, sizeof(folderBlock.b_content[j].b_name));

                file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
                file.write(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
                if (!file) {
                    outMsg = "No se pudo actualizar el bloque de la carpeta padre.";
                    return false;
                }

                std::string now = nowStringFS();
                std::strncpy(parentInode.i_mtime, now.c_str(), sizeof(parentInode.i_mtime) - 1);

                file.seekp(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
                file.write(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
                if (!file) {
                    outMsg = "No se pudo actualizar el inodo de la carpeta padre.";
                    return false;
                }

                return true;
            }
        }
    }

    outMsg = "No se encontro la entrada a eliminar en la carpeta padre.";
    return false;
}



bool FileSystemManager::Mkdir(const std::string& path, bool recursiveP, std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta debe ser absoluta y comenzar con '/'.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    std::vector<std::string> parts = splitFS(path, '/');
    std::vector<std::string> cleanParts;
    for (const auto& p : parts) {
        std::string t = trimFS(p);
        if (!t.empty()) cleanParts.push_back(t);
    }

    if (cleanParts.empty()) {
        outMsg = "La ruta no contiene nombres de carpetas validos.";
        file.close();
        return false;
    }

    int currentInodeIndex = 0; // root
    Inode currentInode{};
    file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));

    for (size_t i = 0; i < cleanParts.size(); i++) {
        const std::string& folderName = cleanParts[i];

        int foundInode = FindEntryInFolder(file, sb, currentInode, folderName);

        if (foundInode != -1) {
            // ya existe, avanzar
            currentInodeIndex = foundInode;
            file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
            continue;
        }

        // no existe
        bool isLast = (i == cleanParts.size() - 1);
        if (!recursiveP && !isLast) {
            outMsg = "No existe una carpeta intermedia y no se uso -p: " + folderName;
            file.close();
            return false;
        }

        int newInodeIndex = AllocateFreeInode(file, sb);
        if (newInodeIndex == -1) {
            outMsg = "No hay inodos libres para crear la carpeta: " + folderName;
            file.close();
            return false;
        }

        int newBlockIndex = AllocateFreeBlock(file, sb);
        if (newBlockIndex == -1) {
            outMsg = "No hay bloques libres para crear la carpeta: " + folderName;
            file.close();
            return false;
        }

        // Crear inodo de nueva carpeta
        Inode newFolderInode{};
        newFolderInode.i_uid = 1;
        newFolderInode.i_gid = 1;
        newFolderInode.i_size = 0;
        std::string now = nowStringFS();
        std::strncpy(newFolderInode.i_atime, now.c_str(), sizeof(newFolderInode.i_atime) - 1);
        std::strncpy(newFolderInode.i_ctime, now.c_str(), sizeof(newFolderInode.i_ctime) - 1);
        std::strncpy(newFolderInode.i_mtime, now.c_str(), sizeof(newFolderInode.i_mtime) - 1);
        for (int& ptr : newFolderInode.i_block) ptr = -1;
        newFolderInode.i_block[0] = newBlockIndex;
        newFolderInode.i_type = '0';
        std::memcpy(newFolderInode.i_perm, "664", 3);

        // Crear bloque carpeta nuevo con . y ..
        FolderBlock newFolderBlock{};
        for (int j = 0; j < 4; j++) {
            newFolderBlock.b_content[j].b_inodo = -1;
            std::memset(newFolderBlock.b_content[j].b_name, 0, sizeof(newFolderBlock.b_content[j].b_name));
        }

        std::strncpy(newFolderBlock.b_content[0].b_name, ".", sizeof(newFolderBlock.b_content[0].b_name) - 1);
        newFolderBlock.b_content[0].b_inodo = newInodeIndex;

        std::strncpy(newFolderBlock.b_content[1].b_name, "..", sizeof(newFolderBlock.b_content[1].b_name) - 1);
        newFolderBlock.b_content[1].b_inodo = currentInodeIndex;

        // Escribir inodo nuevo
        file.seekp(sb.s_inode_start + newInodeIndex * (int)sizeof(Inode));
        file.write(reinterpret_cast<char*>(&newFolderInode), sizeof(Inode));
        if (!file) {
            outMsg = "No se pudo escribir el nuevo inodo de carpeta.";
            file.close();
            return false;
        }

        // Escribir bloque nuevo
        file.seekp(sb.s_block_start + newBlockIndex * (int)sizeof(FolderBlock));
        file.write(reinterpret_cast<char*>(&newFolderBlock), sizeof(FolderBlock));
        if (!file) {
            outMsg = "No se pudo escribir el nuevo bloque de carpeta.";
            file.close();
            return false;
        }

        // Agregar entrada al padre
        if (!AddEntryToFolder(file, sb, currentInode, currentInodeIndex, folderName, newInodeIndex, outMsg)) {
            file.close();
            return false;
        }

        // escribir superbloque actualizado
        file.seekp(mp.start);
        file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
        if (!file) {
            outMsg = "No se pudo actualizar el SuperBloque.";
            file.close();
            return false;
        }

        // avanzar al nuevo directorio
        currentInodeIndex = newInodeIndex;
        currentInode = newFolderInode;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "mkdir", path,
                            recursiveP ? "p=1" : "p=0", journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Carpeta creada correctamente: " + path;
    return true;
}

bool FileSystemManager::Mkfile(const std::string& path,
                               int size,
                               const std::string& contPath,
                               bool recursive,
                               std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta debe ser absoluta y comenzar con '/'.";
        return false;
    }

    if (size < 0) {
        outMsg = "El parametro -size no puede ser negativo.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    // Separar ruta padre y nombre del archivo
    size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos) {
        outMsg = "Ruta invalida.";
        file.close();
        return false;
    }

    std::string parentPath = path.substr(0, lastSlash);
    std::string fileName = path.substr(lastSlash + 1);

    if (fileName.empty()) {
        outMsg = "El nombre del archivo es invalido.";
        file.close();
        return false;
    }

    if (parentPath.empty()) {
        parentPath = "/";
    }

    // Navegar hasta carpeta padre
    std::vector<std::string> parts = splitFS(parentPath, '/');
    std::vector<std::string> cleanParts;
    for (const auto& p : parts) {
        std::string t = trimFS(p);
        if (!t.empty()) cleanParts.push_back(t);
    }

    int currentInodeIndex = 0; // root
    Inode currentInode{};
    file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo raiz.";
        file.close();
        return false;
    }

    for (const auto& folderName : cleanParts) {
        int foundInode = FindEntryInFolder(file, sb, currentInode, folderName);
        if (foundInode == -1) {
            if (!recursive) {
                outMsg = "No existe la carpeta padre: " + folderName;
                file.close();
                return false;
            }

            // crear toda la ruta padre con mkdir -p
            std::string mkdirMsg;
            if (!Mkdir(parentPath, true, mkdirMsg)) {
                outMsg = "No se pudo crear la ruta padre con -r: " + parentPath;
                file.close();
                return false;
            }

            // volver a empezar la navegación desde root
            currentInodeIndex = 0;
            file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
            if (!file) {
                outMsg = "No se pudo releer el inodo raiz tras mkdir -r.";
                file.close();
                return false;
            }

            bool pathOk = true;
            for (const auto& retryFolder : cleanParts) {
                int retryInode = FindEntryInFolder(file, sb, currentInode, retryFolder);
                if (retryInode == -1) {
                    pathOk = false;
                    break;
                }

                currentInodeIndex = retryInode;
                file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
                file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
                if (!file) {
                    outMsg = "No se pudo releer un inodo de carpeta padre.";
                    file.close();
                    return false;
                }
            }

            if (!pathOk) {
                outMsg = "No se pudo reconstruir la ruta padre tras usar -r.";
                file.close();
                return false;
            }

            break;
        }

        currentInodeIndex = foundInode;
        file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
        file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
        if (!file) {
            outMsg = "No se pudo leer un inodo de carpeta padre.";
            file.close();
            return false;
        }
    }

    // Verificar que no exista ya el archivo
    int existing = FindEntryInFolder(file, sb, currentInode, fileName);
    if (existing != -1) {
        outMsg = "Ya existe un archivo o carpeta con ese nombre: " + fileName;
        file.close();
        return false;
    }

    // Generar contenido
    std::string content;
    if (!contPath.empty()) {
        std::ifstream external(contPath, std::ios::binary);
        if (!external.is_open()) {
            outMsg = "No se pudo abrir el archivo indicado en -cont: " + contPath;
            file.close();
            return false;
        }

        std::stringstream buffer;
        buffer << external.rdbuf();
        content = buffer.str();
        external.close();
    } else {
        content = buildFileContent(size);
    }

    int newInodeIndex = AllocateFreeInode(file, sb);
    if (newInodeIndex == -1) {
        outMsg = "No hay inodos libres para crear el archivo.";
        file.close();
        return false;
    }

    int requiredBlocks = (int)((content.size() + sizeof(FileBlock) - 1) / sizeof(FileBlock));
    if (requiredBlocks <= 0) requiredBlocks = 1;

    if (requiredBlocks > 12) {
        outMsg = "El archivo requiere mas de 12 bloques directos. Aun no se soportan indirectos.";
        file.close();
        return false;
    }

    std::vector<int> assignedBlocks;
    for (int i = 0; i < requiredBlocks; i++) {
        int blockIndex = AllocateFreeBlock(file, sb);
        if (blockIndex == -1) {
            outMsg = "No hay bloques libres para crear el archivo.";
            file.close();
            return false;
        }
        assignedBlocks.push_back(blockIndex);
    }

    // Crear inodo de archivo
    Inode newFileInode{};
    newFileInode.i_uid = 1;
    newFileInode.i_gid = 1;
    newFileInode.i_size = (int)content.size();

    std::string now = nowStringFS();
    std::strncpy(newFileInode.i_atime, now.c_str(), sizeof(newFileInode.i_atime) - 1);
    std::strncpy(newFileInode.i_ctime, now.c_str(), sizeof(newFileInode.i_ctime) - 1);
    std::strncpy(newFileInode.i_mtime, now.c_str(), sizeof(newFileInode.i_mtime) - 1);

    for (int& ptr : newFileInode.i_block) ptr = -1;
    for (size_t i = 0; i < assignedBlocks.size(); i++) {
        newFileInode.i_block[i] = assignedBlocks[i];
    }

    newFileInode.i_type = '1';
    std::memcpy(newFileInode.i_perm, "664", 3);

    // Escribir bloques del archivo
    size_t offset = 0;
    for (size_t i = 0; i < assignedBlocks.size(); i++) {
        FileBlock fb{};
        std::memset(fb.b_content, 0, sizeof(fb.b_content));

        size_t remaining = content.size() - offset;
        size_t chunk = std::min(remaining, sizeof(fb.b_content));
        if (chunk > 0) {
            std::memcpy(fb.b_content, content.data() + offset, chunk);
        }

        file.seekp(sb.s_block_start + assignedBlocks[i] * (int)sizeof(FileBlock));
        file.write(reinterpret_cast<char*>(&fb), sizeof(FileBlock));
        if (!file) {
            outMsg = "No se pudo escribir un bloque del archivo.";
            file.close();
            return false;
        }

        offset += chunk;
    }

    // Si size=0 y cont vacío, igual dejamos el archivo con 1 bloque vacío
    if (content.empty() && !assignedBlocks.empty()) {
        FileBlock fb{};
        std::memset(fb.b_content, 0, sizeof(fb.b_content));
        file.seekp(sb.s_block_start + assignedBlocks[0] * (int)sizeof(FileBlock));
        file.write(reinterpret_cast<char*>(&fb), sizeof(FileBlock));
    }

    // Escribir inodo de archivo
    file.seekp(sb.s_inode_start + newInodeIndex * (int)sizeof(Inode));
    file.write(reinterpret_cast<char*>(&newFileInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo escribir el inodo del archivo.";
        file.close();
        return false;
    }

    // Enlazar en carpeta padre
    if (!AddEntryToFolder(file, sb, currentInode, currentInodeIndex, fileName, newInodeIndex, outMsg)) {
        file.close();
        return false;
    }

    // Escribir superbloque actualizado
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalContent = contPath.empty()
        ? ("size=" + std::to_string(size))
        : ("cont=" + contPath);

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "mkfile", path,
                            "size=" + std::to_string(size) +
                            ",cont=" + contPath +
                            ",p=" + std::string(recursive ? "1" : "0"),
                            journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Archivo creado correctamente: " + path;
    return true;
}

static std::string journalFieldToString(const char* data, size_t size) {
    size_t len = 0;
    while (len < size && data[len] != '\0') len++;
    return std::string(data, len);
}

static int journalToInt(const std::string& text, int defaultValue = 0) {
    try {
        if (trimFS(text).empty()) return defaultValue;
        return std::stoi(text);
    } catch (...) {
        return defaultValue;
    }
}

static std::string getJournalKV(const std::string& text, const std::string& key) {
    std::vector<std::string> parts = splitFS(text, ',');

    for (std::string part : parts) {
        part = trimFS(part);
        std::string prefix = key + "=";

        if (part.rfind(prefix, 0) == 0) {
            return part.substr(prefix.size());
        }
    }

    return "";
}

static std::vector<std::string> FindCmdSplitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;

    while (std::getline(ss, part, '/')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

static bool FindCmdReadInode(std::fstream& file, const SuperBlock& sb, int inodeIndex, Inode& inode) {
    if (inodeIndex < 0) return false;
    file.seekg(sb.s_inode_start + inodeIndex * (int)sizeof(Inode), std::ios::beg);
    file.read(reinterpret_cast<char*>(&inode), sizeof(Inode));
    return (bool)file;
}

static bool FindCmdReadFolderBlock(std::fstream& file, const SuperBlock& sb, int blockIndex, FolderBlock& block) {
    if (blockIndex < 0) return false;
    file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock), std::ios::beg);
    file.read(reinterpret_cast<char*>(&block), sizeof(FolderBlock));
    return (bool)file;
}

static int FindCmdFindEntryInFolder(std::fstream& file, const SuperBlock& sb, const Inode& folderInode, const std::string& name) {
    for (int i = 0; i < 15; i++) {
        if (folderInode.i_block[i] == -1) continue;

        FolderBlock fb{};
        if (!FindCmdReadFolderBlock(file, sb, folderInode.i_block[i], fb)) continue;

        for (int j = 0; j < 4; j++) {
            if (fb.b_content[j].b_inodo == -1) continue;

            std::string entryName(fb.b_content[j].b_name);
            size_t pos = entryName.find('\0');
            if (pos != std::string::npos) entryName = entryName.substr(0, pos);

            if (entryName == name) {
                return fb.b_content[j].b_inodo;
            }
        }
    }
    return -1;
}

static int FindCmdResolvePath(std::fstream& file, const SuperBlock& sb, const std::string& path) {
    if (path == "/") return 0;

    std::vector<std::string> parts = FindCmdSplitPath(path);
    int current = 0;

    for (const std::string& part : parts) {
        Inode inode{};
        if (!FindCmdReadInode(file, sb, current, inode)) return -1;
        if (inode.i_type != '0') return -1;

        current = FindCmdFindEntryInFolder(file, sb, inode, part);
        if (current == -1) return -1;
    }

    return current;
}

// Soporta:
// ?  -> un solo caracter
// *  -> uno o mas caracteres
static bool FindCmdWildcardMatch(const std::string& text, const std::string& pattern) {
    int n = (int)text.size();
    int m = (int)pattern.size();

    std::vector<std::vector<bool>> dp(n + 1, std::vector<bool>(m + 1, false));
    dp[0][0] = true;

    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= m; j++) {
            if (pattern[j - 1] == '?') {
                dp[i][j] = dp[i - 1][j - 1];
            } else if (pattern[j - 1] == '*') {
                // '*' = uno o mas caracteres
                dp[i][j] = dp[i - 1][j] || dp[i - 1][j - 1];
            } else {
                dp[i][j] = (text[i - 1] == pattern[j - 1]) && dp[i - 1][j - 1];
            }
        }
    }

    return dp[n][m];
}

static bool FindCmdCollectMatches(std::fstream& file,
                                  const SuperBlock& sb,
                                  int inodeIndex,
                                  const std::string& currentPath,
                                  const std::string& pattern,
                                  std::vector<std::string>& matches) {
    Inode currentInode{};
    if (!FindCmdReadInode(file, sb, inodeIndex, currentInode)) return false;

    if (currentInode.i_type != '0') return true;

    for (int i = 0; i < 15; i++) {
        if (currentInode.i_block[i] == -1) continue;

        FolderBlock fb{};
        if (!FindCmdReadFolderBlock(file, sb, currentInode.i_block[i], fb)) return false;

        for (int j = 0; j < 4; j++) {
            if (fb.b_content[j].b_inodo == -1) continue;

            std::string name(fb.b_content[j].b_name);
            size_t pos = name.find('\0');
            if (pos != std::string::npos) name = name.substr(0, pos);

            if (name.empty() || name == "." || name == "..") continue;

            std::string childPath = (currentPath == "/") ? ("/" + name) : (currentPath + "/" + name);

            if (FindCmdWildcardMatch(name, pattern)) {
                matches.push_back(childPath);
            }

            Inode childInode{};
            if (!FindCmdReadInode(file, sb, fb.b_content[j].b_inodo, childInode)) return false;

            if (childInode.i_type == '0') {
                if (!FindCmdCollectMatches(file, sb, fb.b_content[j].b_inodo, childPath, pattern, matches)) {
                    return false;
                }
            }
        }
    }

    return true;
}

static std::vector<std::string> FindCmdRelativeParts(const std::string& basePath, const std::string& fullPath) {
    if (basePath == "/") {
        return FindCmdSplitPath(fullPath);
    }

    std::string prefix = basePath + "/";
    if (fullPath.rfind(prefix, 0) == 0) {
        return FindCmdSplitPath(fullPath.substr(basePath.size()));
    }

    return {};
}

struct FindCmdTreeNode {
    std::string name;
    std::map<std::string, int> children;
};

static void FindCmdInsertPath(std::vector<FindCmdTreeNode>& tree, const std::vector<std::string>& parts) {
    int current = 0;

    for (const std::string& part : parts) {
        auto it = tree[current].children.find(part);
        if (it == tree[current].children.end()) {
            int newIndex = (int)tree.size();
            tree.push_back({part, {}});
            tree[current].children[part] = newIndex;
            current = newIndex;
        } else {
            current = it->second;
        }
    }
}

static void FindCmdRenderTree(const std::vector<FindCmdTreeNode>& tree,
                              int nodeIndex,
                              const std::string& prefix,
                              std::ostringstream& out,
                              bool isRoot) {
    if (isRoot) {
        out << tree[nodeIndex].name << "\n";
    }

    for (const auto& childPair : tree[nodeIndex].children) {
        int childIndex = childPair.second;
        out << prefix << "|_ " << tree[childIndex].name << "\n";
        FindCmdRenderTree(tree, childIndex, prefix + "   ", out, false);
    }
}

static bool journalFlagTrue(const std::string& v) {
    std::string x = v;
    std::transform(x.begin(), x.end(), x.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    return x == "1" || x == "true" || x == "yes" || x == "p" || x == "-p";
}

static void writeZerosFS(std::fstream& file, int start, int size) {
    if (size <= 0) return;

    std::vector<char> zeros(1024, 0);
    file.seekp(start);

    int pending = size;
    while (pending > 0) {
        int chunk = std::min(pending, (int)zeros.size());
        file.write(zeros.data(), chunk);
        pending -= chunk;
    }
}

bool FileSystemManager::Remove(const std::string& path, std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta debe ser absoluta y comenzar con '/'.";
        return false;
    }

    if (path == "/") {
        outMsg = "No se permite eliminar la raiz '/'.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    ChownCmdUserInfo currentUser;
    if (!ChownCmdGetUserInfo(file, sb, SessionManager::currentSession.user, currentUser) ||
        !currentUser.active) {
        outMsg = "No se pudo obtener la informacion del usuario actual.";
        file.close();
        return false;
    }

bool isRoot = (SessionManager::currentSession.user == "root");

    size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos) {
        outMsg = "Ruta invalida.";
        file.close();
        return false;
    }

    std::string parentPath = path.substr(0, lastSlash);
    std::string targetName = path.substr(lastSlash + 1);

    if (targetName.empty()) {
        outMsg = "El nombre objetivo es invalido.";
        file.close();
        return false;
    }

    if (parentPath.empty()) {
        parentPath = "/";
    }

    int parentInodeIndex = 0;
    Inode parentInode{};
    file.seekg(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo raiz.";
        file.close();
        return false;
    }

    if (parentPath != "/") {
        std::vector<std::string> parts = splitFS(parentPath, '/');
        std::vector<std::string> cleanParts;

        for (const auto& p : parts) {
            std::string t = trimFS(p);
            if (!t.empty()) cleanParts.push_back(t);
        }

        for (const auto& part : cleanParts) {
            int nextInodeIndex = FindEntryInFolder(file, sb, parentInode, part);
            if (nextInodeIndex == -1) {
                outMsg = "No existe la carpeta padre: " + part;
                file.close();
                return false;
            }

            Inode nextInode{};
            file.seekg(sb.s_inode_start + nextInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&nextInode), sizeof(Inode));
            if (!file) {
                outMsg = "No se pudo leer un inodo intermedio.";
                file.close();
                return false;
            }

            if (nextInode.i_type != '0') {
                outMsg = "Una ruta intermedia no es carpeta: " + part;
                file.close();
                return false;
            }

            parentInodeIndex = nextInodeIndex;
            parentInode = nextInode;
        }
    }

    if (!FsCanWrite(parentInode, currentUser, isRoot)) {
        outMsg = "Permiso denegado para eliminar en la carpeta: " + parentPath;
        file.close();
        return false;
    }

    int targetInodeIndex = FindEntryInFolder(file, sb, parentInode, targetName);
    if (targetInodeIndex == -1) {
        outMsg = "No existe la ruta a eliminar: " + path;
        file.close();
        return false;
    }

    Inode targetInode{};
    file.seekg(sb.s_inode_start + targetInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&targetInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo objetivo.";
        file.close();
        return false;
    }

    if (!DeleteInodeRecursive(file, sb, targetInodeIndex, targetInode, outMsg)) {
        file.close();
        return false;
    }

    if (!RemoveEntryFromFolder(file, sb, parentInode, parentInodeIndex, targetName, outMsg)) {
        file.close();
        return false;
    }

    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    std::string journalError;
    AppendJournalEntry(file, mp.start, sb, "remove", path, "", journalError);

    file.close();
    outMsg = "Ruta eliminada correctamente: " + path;
    return true;
}

bool FileSystemManager::Edit(const std::string& path,
                             const std::string& contentPath,
                             std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (contentPath.empty()) {
        outMsg = "El parametro -contenido es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta debe ser absoluta y comenzar con '/'.";
        return false;
    }

    if (path == "/" || path == "/users.txt") {
        outMsg = "No se permite editar esa ruta.";
        return false;
    }

    std::ifstream external(contentPath, std::ios::binary);
    if (!external.is_open()) {
        outMsg = "No se pudo abrir el archivo indicado en -contenido: " + contentPath;
        return false;
    }

    std::stringstream buffer;
    buffer << external.rdbuf();
    std::string newContent = buffer.str();
    external.close();

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    ChownCmdUserInfo currentUser;
    if (!ChownCmdGetUserInfo(file, sb, SessionManager::currentSession.user, currentUser) ||
        !currentUser.active) {
        outMsg = "No se pudo obtener la informacion del usuario actual.";
        file.close();
        return false;
    }

    bool isRoot = (SessionManager::currentSession.user == "root");

    auto hasWritePermission = [&](const Inode& inode) -> bool {
        if (isRoot) return true;

        int ownerPerm = (inode.i_perm[0] >= '0' && inode.i_perm[0] <= '7') ? (inode.i_perm[0] - '0') : 0;
        int groupPerm = (inode.i_perm[1] >= '0' && inode.i_perm[1] <= '7') ? (inode.i_perm[1] - '0') : 0;
        int otherPerm = (inode.i_perm[2] >= '0' && inode.i_perm[2] <= '7') ? (inode.i_perm[2] - '0') : 0;

        int appliedPerm = otherPerm;

        if (inode.i_uid == currentUser.uid) {
            appliedPerm = ownerPerm;
        } else if (inode.i_gid == currentUser.gid) {
            appliedPerm = groupPerm;
        }

        // bit de escritura
        return (appliedPerm & 2) != 0;
    };

    size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos) {
        outMsg = "Ruta invalida.";
        file.close();
        return false;
    }

    std::string parentPath = path.substr(0, lastSlash);
    std::string fileName = path.substr(lastSlash + 1);

    if (fileName.empty()) {
        outMsg = "El nombre del archivo es invalido.";
        file.close();
        return false;
    }

    if (parentPath.empty()) {
        parentPath = "/";
    }

    int currentInodeIndex = 0;
    Inode currentInode{};
    file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo raiz.";
        file.close();
        return false;
    }

    if (parentPath != "/") {
        std::vector<std::string> parts = splitFS(parentPath, '/');
        std::vector<std::string> cleanParts;

        for (const auto& p : parts) {
            std::string t = trimFS(p);
            if (!t.empty()) cleanParts.push_back(t);
        }

        for (const auto& part : cleanParts) {
            int nextInodeIndex = FindEntryInFolder(file, sb, currentInode, part);
            if (nextInodeIndex == -1) {
                outMsg = "No existe la carpeta: " + part;
                file.close();
                return false;
            }

            Inode nextInode{};
            file.seekg(sb.s_inode_start + nextInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&nextInode), sizeof(Inode));
            if (!file) {
                outMsg = "No se pudo leer un inodo intermedio.";
                file.close();
                return false;
            }

            if (nextInode.i_type != '0') {
                outMsg = "Una ruta intermedia no es carpeta: " + part;
                file.close();
                return false;
            }

            currentInodeIndex = nextInodeIndex;
            currentInode = nextInode;
        }
    }

    int fileInodeIndex = FindEntryInFolder(file, sb, currentInode, fileName);
    if (fileInodeIndex == -1) {
        outMsg = "No existe el archivo a editar: " + path;
        file.close();
        return false;
    }

    Inode fileInode{};
    file.seekg(sb.s_inode_start + fileInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&fileInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo del archivo.";
        file.close();
        return false;
    }

    if (fileInode.i_type != '1') {
        outMsg = "La ruta indicada no es un archivo: " + path;
        file.close();
        return false;
    }

    if (!hasWritePermission(fileInode)) {
        outMsg = "Permiso denegado para editar el archivo: " + path;
        file.close();
        return false;
    }

    if (!WriteRegularFileContent(file, sb, fileInode, newContent, outMsg)) {
        file.close();
        return false;
    }

    file.seekp(sb.s_inode_start + fileInodeIndex * (int)sizeof(Inode));
    file.write(reinterpret_cast<char*>(&fileInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo actualizar el inodo del archivo.";
        file.close();
        return false;
    }

    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo actualizar el SuperBloque.";
        file.close();
        return false;
    }

    file.close();
    outMsg = "Archivo editado correctamente: " + path;
    return true;
}

bool FileSystemManager::Rename(const std::string& path,
                               const std::string& newName,
                               std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (newName.empty()) {
        outMsg = "El parametro -name es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta debe ser absoluta y comenzar con '/'.";
        return false;
    }

    if (path == "/" || path == "/users.txt") {
        outMsg = "No se permite renombrar esa ruta.";
        return false;
    }

    if (newName == "." || newName == "..") {
        outMsg = "El nuevo nombre no es valido.";
        return false;
    }

    if (newName.find('/') != std::string::npos) {
        outMsg = "El parametro -name no debe contener '/'.";
        return false;
    }

    if (newName.size() > 12) {
        outMsg = "El nuevo nombre excede el limite actual de 12 caracteres.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    ChownCmdUserInfo currentUser;
    if (!ChownCmdGetUserInfo(file, sb, SessionManager::currentSession.user, currentUser) ||
        !currentUser.active) {
        outMsg = "No se pudo obtener la informacion del usuario actual.";
        file.close();
        return false;
    }

    bool isRoot = (SessionManager::currentSession.user == "root");

    size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos) {
        outMsg = "Ruta invalida.";
        file.close();
        return false;
    }

    std::string parentPath = path.substr(0, lastSlash);
    std::string oldName = path.substr(lastSlash + 1);

    if (oldName.empty()) {
        outMsg = "El nombre actual es invalido.";
        file.close();
        return false;
    }

    if (parentPath.empty()) {
        parentPath = "/";
    }

    int parentInodeIndex = 0;
    Inode parentInode{};
    file.seekg(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo raiz.";
        file.close();
        return false;
    }

    if (parentPath != "/") {
        std::vector<std::string> parts = splitFS(parentPath, '/');
        std::vector<std::string> cleanParts;

        for (const auto& p : parts) {
            std::string t = trimFS(p);
            if (!t.empty()) cleanParts.push_back(t);
        }

        

        for (const auto& part : cleanParts) {
            int nextInodeIndex = FindEntryInFolder(file, sb, parentInode, part);
            if (nextInodeIndex == -1) {
                outMsg = "No existe la carpeta: " + part;
                file.close();
                return false;
            }

            Inode nextInode{};
            file.seekg(sb.s_inode_start + nextInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&nextInode), sizeof(Inode));
            if (!file) {
                outMsg = "No se pudo leer un inodo intermedio.";
                file.close();
                return false;
            }

            if (nextInode.i_type != '0') {
                outMsg = "Una ruta intermedia no es carpeta: " + part;
                file.close();
                return false;
            }

            parentInodeIndex = nextInodeIndex;
            parentInode = nextInode;
        }
    }

    if (!FsCanWrite(parentInode, currentUser, isRoot)) {
        outMsg = "Permiso denegado para renombrar en la carpeta: " + parentPath;
        file.close();
        return false;
    }

    int oldInodeIndex = FindEntryInFolder(file, sb, parentInode, oldName);
    if (oldInodeIndex == -1) {
        outMsg = "No existe la ruta a renombrar: " + path;
        file.close();
        return false;
    }

    int duplicateIndex = FindEntryInFolder(file, sb, parentInode, newName);
    if (duplicateIndex != -1) {
        outMsg = "Ya existe una entrada con el nuevo nombre en la misma carpeta: " + newName;
        file.close();
        return false;
    }

    bool renamed = false;

    for (int i = 0; i < 12; i++) {
        int blockIndex = parentInode.i_block[i];
        if (blockIndex == -1) continue;

        FolderBlock folderBlock{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
        file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
        if (!file) {
            outMsg = "No se pudo leer un bloque de la carpeta padre.";
            file.close();
            return false;
        }

        for (int j = 0; j < 4; j++) {
            if (folderBlock.b_content[j].b_inodo == -1) continue;

            std::string entryName = getContentNameFS(folderBlock.b_content[j]);
            if (entryName == oldName) {
                std::memset(folderBlock.b_content[j].b_name, 0, sizeof(folderBlock.b_content[j].b_name));
                std::strncpy(folderBlock.b_content[j].b_name, newName.c_str(), sizeof(folderBlock.b_content[j].b_name));

                file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
                file.write(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
                if (!file) {
                    outMsg = "No se pudo actualizar el bloque de carpeta.";
                    file.close();
                    return false;
                }

                std::string now = nowStringFS();
                std::strncpy(parentInode.i_mtime, now.c_str(), sizeof(parentInode.i_mtime) - 1);

                file.seekp(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
                file.write(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
                if (!file) {
                    outMsg = "No se pudo actualizar el inodo de la carpeta padre.";
                    file.close();
                    return false;
                }

                renamed = true;
                break;
            }
        }

        if (renamed) break;
    }

    if (!renamed) {
    file.close();
    outMsg = "No se encontro la entrada a renombrar.";
    return false;
    }

    std::string journalError;
    if (!AppendJournalEntry(file, mp.start, sb, "rename", path,
                            "name=" + newName, journalError)) {
        file.close();
        outMsg = journalError;
        return false;
    }

    file.close();

    outMsg = "Ruta renombrada correctamente: " + path + " -> " + newName;
    return true;

    
    }

bool FileSystemManager::Copy(const std::string& path,
                             const std::string& destino,
                             std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (destino.empty()) {
        outMsg = "El parametro -destino es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta origen debe ser absoluta y comenzar con '/'.";
        return false;
    }

    if (destino[0] != '/') {
        outMsg = "La ruta destino debe ser absoluta y comenzar con '/'.";
        return false;
    }

    auto normalizePath = [](std::string p) -> std::string {
        while (p.size() > 1 && p.back() == '/') {
            p.pop_back();
        }
        return p;
    };

    std::string srcPath = normalizePath(path);
    std::string dstPath = normalizePath(destino);

    if (srcPath == "/") {
        outMsg = "No se permite copiar la raiz '/'.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    auto readEntryName = [](const char rawName[12]) -> std::string {
        size_t len = 0;
        while (len < 12 && rawName[len] != '\0') len++;
        return std::string(rawName, len);
    };

    auto resolvePathToInode = [&](const std::string& absolutePath,
                                  int& outInodeIndex,
                                  Inode& outInode,
                                  std::string& errMsg) -> bool {
        errMsg.clear();

        outInodeIndex = 0;
        file.clear();
        file.seekg(sb.s_inode_start + outInodeIndex * (int)sizeof(Inode));
        file.read(reinterpret_cast<char*>(&outInode), sizeof(Inode));
        if (!file) {
            errMsg = "No se pudo leer el inodo raiz.";
            return false;
        }

        if (absolutePath == "/") {
            return true;
        }

        std::vector<std::string> parts = splitFS(absolutePath, '/');
        std::vector<std::string> cleanParts;
        for (const auto& p : parts) {
            std::string t = trimFS(p);
            if (!t.empty()) cleanParts.push_back(t);
        }

        for (const auto& name : cleanParts) {
            int nextInode = FindEntryInFolder(file, sb, outInode, name);
            if (nextInode == -1) {
                errMsg = "No existe la ruta: " + absolutePath;
                return false;
            }

            outInodeIndex = nextInode;
            file.clear();
            file.seekg(sb.s_inode_start + outInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&outInode), sizeof(Inode));
            if (!file) {
                errMsg = "No se pudo leer un inodo al resolver la ruta: " + absolutePath;
                return false;
            }
        }

        return true;
    };

    int srcInodeIndex = -1;
    Inode srcInode{};
    std::string errMsg;

    if (!resolvePathToInode(srcPath, srcInodeIndex, srcInode, errMsg)) {
        outMsg = errMsg;
        file.close();
        return false;
    }

    int dstInodeIndex = -1;
    Inode dstInode{};
    if (!resolvePathToInode(dstPath, dstInodeIndex, dstInode, errMsg)) {
        outMsg = errMsg;
        file.close();
        return false;
    }

    if (dstInode.i_type != '0') {
        outMsg = "La ruta destino no corresponde a una carpeta.";
        file.close();
        return false;
    }

    if (srcInode.i_type == '0') {
        std::string prefix = srcPath + "/";
        if (dstPath == srcPath || dstPath.rfind(prefix, 0) == 0) {
            outMsg = "No se puede copiar una carpeta dentro de si misma.";
            file.close();
            return false;
        }
    }

    size_t lastSlash = srcPath.find_last_of('/');
    std::string sourceName = srcPath.substr(lastSlash + 1);

    struct CopyNode {
        bool isDirectory = false;
        std::string name;
        std::string content;
        std::vector<CopyNode> children;
    };

    auto buildCopyTree = [&](auto&& self,
                             const Inode& currentInode,
                             const std::string& currentName,
                             CopyNode& outNode,
                             std::string& buildErr) -> bool {
        outNode.name = currentName;
        outNode.isDirectory = (currentInode.i_type == '0');
        outNode.content.clear();
        outNode.children.clear();

        if (currentInode.i_type == '1') {
            return readFileContent(file, sb, currentInode, outNode.content, buildErr);
        }

        if (currentInode.i_type != '0') {
            buildErr = "Tipo de inodo desconocido durante copy.";
            return false;
        }

        for (int i = 0; i < 12; i++) {
            int blockIndex = currentInode.i_block[i];
            if (blockIndex == -1) continue;

            FolderBlock folderBlock{};
            file.clear();
            file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
            file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
            if (!file) {
                buildErr = "No se pudo leer un bloque de carpeta durante copy.";
                return false;
            }

            for (int j = 0; j < 4; j++) {
                int childInodeIndex = folderBlock.b_content[j].b_inodo;
                if (childInodeIndex == -1) continue;

                std::string childName = readEntryName(folderBlock.b_content[j].b_name);
                if (childName.empty() || childName == "." || childName == "..") continue;

                Inode childInode{};
                file.clear();
                file.seekg(sb.s_inode_start + childInodeIndex * (int)sizeof(Inode));
                file.read(reinterpret_cast<char*>(&childInode), sizeof(Inode));
                if (!file) {
                    buildErr = "No se pudo leer un inodo hijo durante copy.";
                    return false;
                }

                CopyNode childNode;
                if (!self(self, childInode, childName, childNode, buildErr)) {
                    return false;
                }

                outNode.children.push_back(childNode);
            }
        }

        return true;
    };

    CopyNode sourceTree;
    if (!buildCopyTree(buildCopyTree, srcInode, sourceName, sourceTree, errMsg)) {
        outMsg = errMsg;
        file.close();
        return false;
    }

    file.close();

    auto joinPath = [](const std::string& parent, const std::string& name) -> std::string {
        if (parent == "/") return "/" + name;
        return parent + "/" + name;
    };

    static int tempCopyCounter = 0;

    auto createCopyTree = [&](auto&& self,
                              const CopyNode& node,
                              const std::string& parentPath,
                              std::string& createErr) -> bool {
        std::string newPath = joinPath(parentPath, node.name);

        if (node.isDirectory) {
            std::string mkdirMsg;
            if (!FileSystemManager::Mkdir(newPath, false, mkdirMsg)) {
                createErr = mkdirMsg;
                return false;
            }

            for (const auto& child : node.children) {
                if (!self(self, child, newPath, createErr)) {
                    return false;
                }
            }
            return true;
        }

        if (node.content.empty()) {
            std::string mkfileMsg;
            if (!FileSystemManager::Mkfile(newPath, 0, "", false, mkfileMsg)) {
                createErr = mkfileMsg;
                return false;
            }
            return true;
        }

        std::string tempPath = "/tmp/extreamfs_copy_" +
                               std::to_string((long long)std::time(nullptr)) +
                               "_" +
                               std::to_string(++tempCopyCounter) +
                               ".tmp";

        {
            std::ofstream tmp(tempPath, std::ios::binary);
            if (!tmp.is_open()) {
                createErr = "No se pudo crear un archivo temporal para copy.";
                return false;
            }

            tmp.write(node.content.data(), (std::streamsize)node.content.size());
            if (!tmp) {
                tmp.close();
                std::remove(tempPath.c_str());
                createErr = "No se pudo escribir el archivo temporal para copy.";
                return false;
            }
        }

        std::string mkfileMsg;
        bool ok = FileSystemManager::Mkfile(newPath, 0, tempPath, false, mkfileMsg);
        std::remove(tempPath.c_str());

        if (!ok) {
            createErr = mkfileMsg;
            return false;
        }

        return true;
    };

    if (!createCopyTree(createCopyTree, sourceTree, dstPath, errMsg)) {
        outMsg = errMsg;
        return false;
    }

    outMsg = "Ruta copiada correctamente: " + srcPath + " -> " + dstPath;
    return true;
}

bool FileSystemManager::Move(const std::string& path,
                             const std::string& destino,
                             std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "El parametro -path es obligatorio.";
        return false;
    }

    if (destino.empty()) {
        outMsg = "El parametro -destino es obligatorio.";
        return false;
    }

    if (path[0] != '/') {
        outMsg = "La ruta origen debe ser absoluta y comenzar con '/'.";
        return false;
    }

    if (destino[0] != '/') {
        outMsg = "La ruta destino debe ser absoluta y comenzar con '/'.";
        return false;
    }

    auto normalizePath = [](std::string p) -> std::string {
        while (p.size() > 1 && p.back() == '/') {
            p.pop_back();
        }
        return p;
    };

    std::string srcPath = normalizePath(path);
    std::string dstPath = normalizePath(destino);

    if (srcPath == "/") {
        outMsg = "No se permite mover la raiz '/'.";
        return false;
    }

    size_t lastSlash = srcPath.find_last_of('/');
    std::string sourceName = srcPath.substr(lastSlash + 1);

    if (sourceName.empty()) {
        outMsg = "No se pudo determinar el nombre de la ruta origen.";
        return false;
    }

    std::string finalMovedPath = (dstPath == "/")
        ? "/" + sourceName
        : dstPath + "/" + sourceName;

    if (finalMovedPath == srcPath) {
        outMsg = "La ruta origen y la ruta destino final son la misma.";
        return false;
    }

    std::string copyMsg;
    if (!FileSystemManager::Copy(srcPath, dstPath, copyMsg)) {
        outMsg = copyMsg;
        return false;
    }

    std::string removeMsg;
    if (!FileSystemManager::Remove(srcPath, removeMsg)) {
        std::string rollbackMsg;
        if (!FileSystemManager::Remove(finalMovedPath, rollbackMsg)) {
            outMsg = "Move fallo al eliminar la ruta original despues de copiarla. "
                     "Ademas no se pudo revertir la copia creada en: " + finalMovedPath +
                     ". Error original: " + removeMsg +
                     " | Error rollback: " + rollbackMsg;
            return false;
        }

        outMsg = "Move fallo al eliminar la ruta original despues de copiarla, "
                 "pero la copia creada fue revertida. Detalle: " + removeMsg;
        return false;
    }

    outMsg = "Ruta movida correctamente: " + srcPath + " -> " + dstPath;
    return true;
}

bool FileSystemManager::Find(const std::string& path, const std::string& name, std::string& outMsg) {
    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No existe una particion montada con id: " + SessionManager::currentSession.partitionId;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la particion montada.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start, std::ios::beg);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));

    if (!file) {
        file.close();
        outMsg = "No se pudo leer el superbloque.";
        return false;
    }

    std::string startPath = path.empty() ? "/" : path;
    int startInodeIndex = FindCmdResolvePath(file, sb, startPath);
    if (startInodeIndex == -1) {
        file.close();
        outMsg = "No existe la carpeta inicial de busqueda: " + startPath;
        return false;
    }

    Inode startInode{};
    if (!FindCmdReadInode(file, sb, startInodeIndex, startInode)) {
        file.close();
        outMsg = "No se pudo leer el inodo de la ruta inicial.";
        return false;
    }

    if (startInode.i_type != '0') {
        file.close();
        outMsg = "La ruta indicada en -path no es una carpeta: " + startPath;
        return false;
    }

    std::vector<std::string> matches;
    if (!FindCmdCollectMatches(file, sb, startInodeIndex, startPath, name, matches)) {
        file.close();
        outMsg = "Error al recorrer el sistema de archivos para la busqueda.";
        return false;
    }

    file.close();

    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

    if (matches.empty()) {
        outMsg = "[INFO] No se encontraron coincidencias para: " + name;
        return true;
    }

    std::vector<FindCmdTreeNode> tree;
    tree.push_back({startPath == "/" ? "/" : startPath, {}});

    for (const std::string& match : matches) {
        std::vector<std::string> relativeParts = FindCmdRelativeParts(startPath, match);
        if (!relativeParts.empty()) {
            FindCmdInsertPath(tree, relativeParts);
        }
    }

    std::ostringstream oss;
    oss << "[FIND]\n";
    FindCmdRenderTree(tree, 0, "", oss, true);

    outMsg = oss.str();
    return true;
}

bool FileSystemManager::Chown(const std::string& path,
                              const std::string& newOwner,
                              bool recursive,
                              std::string& outMsg) {
    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "La ruta no puede venir vacia.";
        return false;
    }

    if (newOwner.empty()) {
        outMsg = "El parametro -usuario no puede venir vacio.";
        return false;
    }

    MountedPartition mp;
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No existe una particion montada con id: " +
                 SessionManager::currentSession.partitionId;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la particion montada.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file.good()) {
        outMsg = "No se pudo leer el superbloque.";
        file.close();
        return false;
    }

    ChownCmdUserInfo targetUser;
    if (!ChownCmdGetUserInfo(file, sb, newOwner, targetUser) || !targetUser.active) {
        outMsg = "El usuario destino no existe: " + newOwner;
        file.close();
        return false;
    }

    ChownCmdUserInfo currentUser;
    if (!ChownCmdGetUserInfo(file, sb, SessionManager::currentSession.user, currentUser) ||
        !currentUser.active) {
        outMsg = "No se pudo obtener la informacion del usuario actual.";
        file.close();
        return false;
    }

    int inodeIndex = -1;
    Inode targetInode{};
    if (!ChownCmdResolvePath(file, sb, path, inodeIndex, targetInode)) {
        outMsg = "No existe la ruta: " + path;
        file.close();
        return false;
    }

    bool isRoot = (SessionManager::currentSession.user == "root");

    if (!isRoot && targetInode.i_uid != currentUser.uid) {
        outMsg = "Solo puede cambiar propietario sobre sus propios archivos o carpetas.";
        file.close();
        return false;
    }

    if (!ChownCmdApplyRecursive(file,
                                sb,
                                inodeIndex,
                                targetUser.uid,
                                targetUser.gid,
                                currentUser.uid,
                                isRoot,
                                recursive)) {
        outMsg = "No se pudo aplicar chown sobre la ruta indicada.";
        file.close();
        return false;
    }

    file.close();

    // Si tu proyecto ya tiene journaling centralizado, deja esta linea.
    // Si compila con error porque aun no existe AppendJournalEntry con esa firma,
    // comentala temporalmente.
    /*
    AppendJournalEntry(SessionManager::currentSession.partitionId,
                       "chown",
                       path,
                       "usuario=" + newOwner + ",r=" + std::string(recursive ? "1" : "0"));
    */
    outMsg = "Propietario actualizado correctamente: " + path +
             " -> usuario=" + newOwner;
    return true;
    
}

bool FileSystemManager::Chmod(const std::string& path,
                              const std::string& ugo,
                              bool recursive,
                              std::string& outMsg) {
    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (path.empty()) {
        outMsg = "La ruta no puede venir vacia.";
        return false;
    }

    if (!ChmodCmdIsValidUGO(ugo)) {
        outMsg = "El parametro -ugo debe tener exactamente 3 digitos entre 0 y 7. Ejemplo: 664, 755, 777.";
        return false;
    }

    MountedPartition mp;
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No existe una particion montada con id: " +
                 SessionManager::currentSession.partitionId;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la particion montada.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file.good()) {
        outMsg = "No se pudo leer el superbloque.";
        file.close();
        return false;
    }

    ChownCmdUserInfo currentUser;
    if (!ChownCmdGetUserInfo(file, sb, SessionManager::currentSession.user, currentUser) ||
        !currentUser.active) {
        outMsg = "No se pudo obtener la informacion del usuario actual.";
        file.close();
        return false;
    }

    int inodeIndex = -1;
    Inode targetInode{};
    if (!ChownCmdResolvePath(file, sb, path, inodeIndex, targetInode)) {
        outMsg = "No existe la ruta: " + path;
        file.close();
        return false;
    }

    bool isRoot = (SessionManager::currentSession.user == "root");

    if (!isRoot && targetInode.i_uid != currentUser.uid) {
        outMsg = "Solo puede cambiar permisos sobre sus propios archivos o carpetas.";
        file.close();
        return false;
    }

    if (!ChmodCmdApplyRecursive(file,
                                sb,
                                inodeIndex,
                                ugo,
                                currentUser.uid,
                                isRoot,
                                recursive)) {
        outMsg = "No se pudieron actualizar los permisos sobre la ruta indicada.";
        file.close();
        return false;
    }

    file.close();

    /*
    AppendJournalEntry(SessionManager::currentSession.partitionId,
                       "chmod",
                       path,
                       "ugo=" + ugo + ",r=" + std::string(recursive ? "1" : "0"));
    */

    outMsg = "Permisos actualizados correctamente: " + path +
             " -> ugo=" + ugo;
    return true;
}

bool FileSystemManager::Cat(const std::vector<std::string>& filePaths, std::string& outMsg) {
    outMsg.clear();

    if (!SessionManager::currentSession.active) {
        outMsg = "No hay una sesion activa.";
        return false;
    }

    if (filePaths.empty()) {
        outMsg = "No se enviaron rutas para cat.";
        return false;
    }

    MountedPartition mp{};
    if (!MountManager::FindById(SessionManager::currentSession.partitionId, mp)) {
        outMsg = "No se encontro la particion de la sesion activa.";
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la sesion activa.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    ChownCmdUserInfo currentUser;
    if (!ChownCmdGetUserInfo(file, sb, SessionManager::currentSession.user, currentUser) ||
        !currentUser.active) {
        outMsg = "No se pudo obtener la informacion del usuario actual.";
        file.close();
        return false;
    }

    bool isRoot = (SessionManager::currentSession.user == "root");

    std::string finalOutput;

    for (size_t fileIdx = 0; fileIdx < filePaths.size(); fileIdx++) {
        const std::string& path = filePaths[fileIdx];

        if (path.empty() || path[0] != '/') {
            outMsg = "Ruta invalida en cat: " + path;
            file.close();
            return false;
        }

        size_t lastSlash = path.find_last_of('/');
        if (lastSlash == std::string::npos) {
            outMsg = "Ruta invalida en cat: " + path;
            file.close();
            return false;
        }

        std::string parentPath = path.substr(0, lastSlash);
        std::string fileName = path.substr(lastSlash + 1);

        if (fileName.empty()) {
            outMsg = "Nombre de archivo invalido en cat: " + path;
            file.close();
            return false;
        }

        if (parentPath.empty()) {
            parentPath = "/";
        }

        // navegar hasta carpeta padre
        std::vector<std::string> parts = splitFS(parentPath, '/');
        std::vector<std::string> cleanParts;
        for (const auto& p : parts) {
            std::string t = trimFS(p);
            if (!t.empty()) cleanParts.push_back(t);
        }

        int currentInodeIndex = 0; // root
        Inode currentInode{};
        file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
        file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
        if (!file) {
            outMsg = "No se pudo leer el inodo raiz.";
            file.close();
            return false;
        }

        std::string traversedPath = "/";

        for (const auto& folderName : cleanParts) {
            if (!FsCanRead(currentInode, currentUser, isRoot)) {
                outMsg = "Permiso denegado para acceder a carpeta: " + traversedPath;
                file.close();
                return false;
            }

            int foundInode = FindEntryInFolder(file, sb, currentInode, folderName);
            if (foundInode == -1) {
                outMsg = "No existe carpeta en la ruta: " + folderName;
                file.close();
                return false;
            }

            currentInodeIndex = foundInode;
            file.seekg(sb.s_inode_start + currentInodeIndex * (int)sizeof(Inode));
            file.read(reinterpret_cast<char*>(&currentInode), sizeof(Inode));
            if (!file) {
                outMsg = "No se pudo leer un inodo al navegar cat.";
                file.close();
                return false;
            }

            if (traversedPath != "/") traversedPath += "/";
            traversedPath += folderName;
        }

        if (!FsCanRead(currentInode, currentUser, isRoot)) {
            outMsg = "Permiso denegado para acceder a carpeta: " + parentPath;
            file.close();
            return false;
        }

        // buscar archivo dentro de carpeta padre
        int fileInodeIndex = FindEntryInFolder(file, sb, currentInode, fileName);
        if (fileInodeIndex == -1) {
            outMsg = "No existe el archivo: " + path;
            file.close();
            return false;
        }

        Inode fileInode{};
        file.seekg(sb.s_inode_start + fileInodeIndex * (int)sizeof(Inode));
        file.read(reinterpret_cast<char*>(&fileInode), sizeof(Inode));
        if (!file) {
            outMsg = "No se pudo leer el inodo del archivo: " + path;
            file.close();
            return false;
        }

        if (fileInode.i_type != '1') {
            outMsg = "La ruta no corresponde a un archivo: " + path;
            file.close();
            return false;
        }

        if (!FsCanRead(fileInode, currentUser, isRoot)) {
            outMsg = "Permiso denegado para leer el archivo: " + path;
            file.close();
            return false;
        }

        std::string fileContent;
        if (!readFileContent(file, sb, fileInode, fileContent, outMsg)) {
            file.close();
            return false;
        }

        finalOutput += fileContent;
        if (fileIdx + 1 < filePaths.size()) {
            finalOutput += "\n";
        }
    }

    file.close();
    outMsg = finalOutput;
    return true;
}

bool FileSystemManager::Loss(const std::string& id, std::string& outMsg) {
    outMsg.clear();

    MountedPartition mp{};
    if (!MountManager::FindById(id, mp)) {
        outMsg = "No existe una particion montada con id: " + id;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la particion montada.";
        return false;
    }

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));

    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    if (sb.s_magic != 0xEF53 || sb.s_filesystem_type != 3) {
        outMsg = "LOSS solo se puede ejecutar sobre una particion EXT3 valida.";
        file.close();
        return false;
    }

    int bmStart = sb.s_bm_inode_start;
    int partitionEnd = mp.start + mp.size;

    // Borra el superbloque
    writeZerosFS(file, mp.start, (int)sizeof(SuperBlock));

    // Borra bitmaps, inodos y bloques.
    // El journaling NO se toca porque está entre SuperBlock y bm_inode_start.
    writeZerosFS(file, bmStart, partitionEnd - bmStart);

    if (!file) {
        outMsg = "No se pudo simular la perdida del sistema.";
        file.close();
        return false;
    }

    file.close();

    // Evitar que quede una sesión activa apuntando a un FS destruido
    if (SessionManager::currentSession.active &&
        SessionManager::currentSession.partitionId == id) {
        SessionManager::Logout();
    }

    outMsg = "Se simulo la perdida del sistema correctamente en la particion: " + id;
    return true;
}

bool FileSystemManager::Recovery(const std::string& id, std::string& outMsg) {
    outMsg.clear();

    MountedPartition mp{};
    if (!MountManager::FindById(id, mp)) {
        outMsg = "No existe una particion montada con id: " + id;
        return false;
    }

    std::fstream file(mp.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "No se pudo abrir el disco de la particion montada.";
        return false;
    }

    // Verificar que realmente haya "loss"
    SuperBlock sbCheck{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sbCheck), sizeof(SuperBlock));
    file.clear();

    bool looksLost = (sbCheck.s_magic != 0xEF53);
    if (!looksLost) {
        outMsg = "La particion no presenta perdida del sistema. Ejecute loss antes de recovery.";
        file.close();
        return false;
    }

    // Leer journal guardado
    Journal oldJournal{};
    int journalStart = mp.start + (int)sizeof(SuperBlock);

    file.seekg(journalStart);
    file.read(reinterpret_cast<char*>(&oldJournal), sizeof(Journal));

    if (!file) {
        outMsg = "No se pudo leer el journaling de la particion.";
        file.close();
        return false;
    }

    file.close();

    if (oldJournal.j_count <= 0 || oldJournal.j_count > 50) {
        outMsg = "No existe journaling valido para recuperar la particion.";
        return false;
    }

    // Guardar sesión actual
    Session oldSession = SessionManager::currentSession;
    SessionManager::currentSession = Session{};

    // Recrear base EXT3
    std::string mkfsMsg;
    if (!Mkfs(id, "full", "3fs", mkfsMsg)) {
        SessionManager::currentSession = oldSession;
        outMsg = "No se pudo rehacer la particion EXT3 base para recovery. " + mkfsMsg;
        return false;
    }

    // Simular sesión root temporal para reprocesar operaciones
    SessionManager::currentSession.active = true;
    SessionManager::currentSession.user = "root";
    SessionManager::currentSession.group = "root";
    SessionManager::currentSession.partitionId = id;

    // Saltamos las dos primeras entradas, porque mkfs ya recrea:
    // 1) /
    // 2) /users.txt
    for (int i = 2; i < oldJournal.j_count; i++) {
        std::string op = trimFS(journalFieldToString(
            oldJournal.j_content[i].i_operation,
            sizeof(oldJournal.j_content[i].i_operation)
        ));

        std::string path = trimFS(journalFieldToString(
            oldJournal.j_content[i].i_path,
            sizeof(oldJournal.j_content[i].i_path)
        ));

        std::string content = trimFS(journalFieldToString(
            oldJournal.j_content[i].i_content,
            sizeof(oldJournal.j_content[i].i_content)
        ));

        if (op.empty()) continue;

        std::string stepMsg;
        bool ok = false;

        if (op == "mkgrp") {
            std::string name = getJournalKV(content, "name");
            if (name.empty()) name = getJournalKV(content, "group");
            ok = !name.empty() && Mkgrp(name, stepMsg);
        }
        else if (op == "rmgrp") {
            std::string name = getJournalKV(content, "name");
            if (name.empty()) name = getJournalKV(content, "group");
            ok = !name.empty() && Rmgrp(name, stepMsg);
        }
        else if (op == "mkusr") {
            std::string user = getJournalKV(content, "user");
            std::string pass = getJournalKV(content, "pass");
            std::string grp  = getJournalKV(content, "grp");

            ok = !user.empty() && !pass.empty() && !grp.empty() &&
                 Mkusr(user, pass, grp, stepMsg);
        }
        else if (op == "rmusr") {
            std::string user = getJournalKV(content, "user");
            ok = !user.empty() && Rmusr(user, stepMsg);
        }
        else if (op == "chgrp") {
            std::string user = getJournalKV(content, "user");
            std::string grp  = getJournalKV(content, "grp");

            ok = !user.empty() && !grp.empty() &&
                 Chgrp(user, grp, stepMsg);
        }
        else if (op == "mkdir") {
            bool p = (content == "-p") || journalFlagTrue(getJournalKV(content, "p"));
            ok = !path.empty() && Mkdir(path, p, stepMsg);
        }
        else if (op == "mkfile") {
            int size = journalToInt(getJournalKV(content, "size"), 0);
            std::string cont = getJournalKV(content, "cont");
            bool p = journalFlagTrue(getJournalKV(content, "p"));

            ok = !path.empty() && Mkfile(path, size, cont, p, stepMsg);
        }
        else {
            // Si aparece alguna operación que todavía no manejas, la ignoramos
            ok = true;
        }

        if (!ok) {
            SessionManager::currentSession = oldSession;
            outMsg = "Recovery fallo al reprocesar la operacion [" + op +
                     "] sobre [" + path + "]. " + stepMsg;
            return false;
        }
    }

    // Restaurar sesión previa
    SessionManager::currentSession = oldSession;

    outMsg = "Sistema recuperado correctamente desde journaling en la particion: " + id;
    return true;
}