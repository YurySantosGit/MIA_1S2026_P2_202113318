#include "fs/FileSystemManager.h"
#include "fs/Ext2Structs.h"
#include "disk/MountManager.h"

#include <fstream>
#include <cstring>
#include <ctime>
#include <cmath>

static std::string nowStringFS() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);

    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

static int calcN(int partitionSize) {
    int sb = (int)sizeof(SuperBlock);
    int inode = (int)sizeof(Inode);
    int block = (int)sizeof(FileBlock); // todos valen 64 bytes
    double n = (double)(partitionSize - sb) / (1.0 + 3.0 + inode + 3.0 * block);
    return (int)std::floor(n);
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