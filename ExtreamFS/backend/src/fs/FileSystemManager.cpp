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

static int calcN(int partitionSize) {
    int sb = (int)sizeof(SuperBlock);
    int inode = (int)sizeof(Inode);
    int block = (int)sizeof(FileBlock); // todos valen 64 bytes
    double n = (double)(partitionSize - sb) / (1.0 + 3.0 + inode + 3.0 * block);
    return (int)std::floor(n);
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
    for (int i = 0; i < 12; i++) { // solo directos por ahora
        int blockIndex = folderInode.i_block[i];
        if (blockIndex == -1) continue;

        FolderBlock folderBlock{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
        file.read(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
        if (!file) return -1;

        for (int j = 0; j < 4; j++) {
            if (folderBlock.b_content[j].b_inodo == -1) continue;

            std::string entryName(folderBlock.b_content[j].b_name);
            entryName = entryName.c_str(); // limpia basura después del null
            if (entryName == name) {
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

    // 1) intentar meter la entrada en un bloque carpeta existente
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
                std::memset(folderBlock.b_content[j].b_name, 0, sizeof(folderBlock.b_content[j].b_name));
                std::strncpy(folderBlock.b_content[j].b_name, name.c_str(),
                             sizeof(folderBlock.b_content[j].b_name) - 1);
                folderBlock.b_content[j].b_inodo = childInodeIndex;

                file.seekp(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
                file.write(reinterpret_cast<char*>(&folderBlock), sizeof(FolderBlock));
                if (!file) {
                    outMsg = "No se pudo escribir el bloque carpeta del padre.";
                    return false;
                }

                // actualizar inodo padre
                std::string now = nowStringFS();
                std::memset(parentInode.i_mtime, 0, sizeof(parentInode.i_mtime));
                std::strncpy(parentInode.i_mtime, now.c_str(), sizeof(parentInode.i_mtime) - 1);

                file.seekp(sb.s_inode_start + parentInodeIndex * (int)sizeof(Inode));
                file.write(reinterpret_cast<char*>(&parentInode), sizeof(Inode));
                return (bool)file;
            }
        }
    }

    // 2) si no hay espacio, crear nuevo bloque carpeta
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
                std::memset(newFolderBlock.b_content[j].b_name, 0, sizeof(newFolderBlock.b_content[j].b_name));
            }

            std::strncpy(newFolderBlock.b_content[0].b_name, name.c_str(),
                         sizeof(newFolderBlock.b_content[0].b_name) - 1);
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

bool FileSystemManager::Mkfs(const std::string& id, std::string& outMsg) {
    outMsg.clear();

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

    int n = calcN(mp.size);
    if (n <= 0) {
        outMsg = "Error: la particion es demasiado pequena para formatearse.";
        file.close();
        return false;
    }

    SuperBlock sb{};
    sb.s_filesystem_type = 2;
    sb.s_inodes_count = n;
    sb.s_blocks_count = 3 * n;
    sb.s_free_inodes_count = n - 2; // root + users.txt
    sb.s_free_blocks_count = 3 * n - 2; // carpeta root + archivo users.txt
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

    sb.s_bm_inode_start = mp.start + sizeof(SuperBlock);
    sb.s_bm_block_start = sb.s_bm_inode_start + n;
    sb.s_inode_start = sb.s_bm_block_start + (3 * n);
    sb.s_block_start = sb.s_inode_start + (n * sizeof(Inode));

    // 1) escribir superbloque
    file.seekp(mp.start);
    file.write(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));

    // 2) escribir bitmap inodos
    file.seekp(sb.s_bm_inode_start);
    for (int i = 0; i < n; i++) {
        char value = '0';
        if (i == 0 || i == 1) value = '1';
        file.write(&value, 1);
    }

    // 3) escribir bitmap bloques
    file.seekp(sb.s_bm_block_start);
    for (int i = 0; i < 3 * n; i++) {
        char value = '0';
        if (i == 0 || i == 1) value = '1';
        file.write(&value, 1);
    }

    // 4) inicializar tabla de inodos vacía
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

    // 5) inicializar bloques vacíos
    FileBlock emptyBlock{};
    std::memset(emptyBlock.b_content, 0, sizeof(emptyBlock.b_content));

    file.seekp(sb.s_block_start);
    for (int i = 0; i < 3 * n; i++) {
        file.write(reinterpret_cast<char*>(&emptyBlock), sizeof(FileBlock));
    }

    // =========================
    // Crear inodo raíz
    // =========================
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

    // =========================
    // Crear bloque carpeta raíz
    // =========================
    FolderBlock rootBlock{};
    std::memset(&rootBlock, 0, sizeof(FolderBlock));

    std::strncpy(rootBlock.b_content[0].b_name, ".", sizeof(rootBlock.b_content[0].b_name) - 1);
    rootBlock.b_content[0].b_inodo = 0;

    std::strncpy(rootBlock.b_content[1].b_name, "..", sizeof(rootBlock.b_content[1].b_name) - 1);
    rootBlock.b_content[1].b_inodo = 0;

    std::strncpy(rootBlock.b_content[2].b_name, "users.txt", sizeof(rootBlock.b_content[2].b_name) - 1);
    rootBlock.b_content[2].b_inodo = 1;

    rootBlock.b_content[3].b_inodo = -1;

    // =========================
    // Crear inodo users.txt
    // =========================
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

    // =========================
    // Crear bloque users.txt
    // =========================
    FileBlock usersBlock{};
    std::memset(usersBlock.b_content, 0, sizeof(usersBlock.b_content));
    std::strncpy(usersBlock.b_content, usersContent.c_str(), sizeof(usersBlock.b_content) - 1);

    // escribir inodo root
    file.seekp(sb.s_inode_start + 0 * sizeof(Inode));
    file.write(reinterpret_cast<char*>(&root), sizeof(Inode));

    // escribir inodo users.txt
    file.seekp(sb.s_inode_start + 1 * sizeof(Inode));
    file.write(reinterpret_cast<char*>(&usersInode), sizeof(Inode));

    // escribir bloque root
    file.seekp(sb.s_block_start + 0 * sizeof(FolderBlock));
    file.write(reinterpret_cast<char*>(&rootBlock), sizeof(FolderBlock));

    // escribir bloque users.txt
    file.seekp(sb.s_block_start + 1 * sizeof(FileBlock));
    file.write(reinterpret_cast<char*>(&usersBlock), sizeof(FileBlock));

    file.close();

    outMsg = "Particion formateada correctamente en EXT2. id=" + id +
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

    file.close();
    outMsg = "Archivo creado correctamente: " + path;
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

        for (const auto& folderName : cleanParts) {
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