#pragma once
#include <cstdint>

#pragma pack(push, 1)

struct SuperBlock {
    int32_t s_filesystem_type;   // 2 = EXT2
    int32_t s_inodes_count;
    int32_t s_blocks_count;
    int32_t s_free_blocks_count;
    int32_t s_free_inodes_count;
    char    s_mtime[20];
    char    s_umtime[20];
    int32_t s_mnt_count;
    int32_t s_magic;
    int32_t s_inode_size;
    int32_t s_block_size;
    int32_t s_first_ino;
    int32_t s_first_blo;
    int32_t s_bm_inode_start;
    int32_t s_bm_block_start;
    int32_t s_inode_start;
    int32_t s_block_start;
};

struct Inode {
    int32_t i_uid;
    int32_t i_gid;
    int32_t i_size;
    char    i_atime[20];
    char    i_ctime[20];
    char    i_mtime[20];
    int32_t i_block[15];
    char    i_type;       // '0' carpeta, '1' archivo
    char    i_perm[3];    // por simplicidad: "664", "777", etc.
};

struct Content {
    char b_name[12];
    int32_t b_inodo;
};

struct FolderBlock {
    Content b_content[4];
};

struct FileBlock {
    char b_content[64];
};

struct PointerBlock {
    int32_t b_pointers[16];
};

#pragma pack(pop)