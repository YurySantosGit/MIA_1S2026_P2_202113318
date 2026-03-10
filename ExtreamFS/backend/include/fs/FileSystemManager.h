#pragma once
#include <string>
#include <vector>
#include <fstream>
#include "fs/Ext2Structs.h"


class FileSystemManager {
public:
    static bool Mkfs(const std::string& id, std::string& outMsg);
    static bool Mkgrp(const std::string& groupName, std::string& outMsg);
    static bool Rmgrp(const std::string& groupName, std::string& outMsg);
    static bool Mkusr(const std::string& user,
                      const std::string& pass,
                      const std::string& group,
                      std::string& outMsg);
    static bool Rmusr(const std::string& user, std::string& outMsg);
    static bool Chgrp(const std::string& user,
                      const std::string& group,
                      std::string& outMsg);

    static bool Mkdir(const std::string& path, bool recursiveP, std::string& outMsg);

    static bool Mkfile(const std::string& path,
                       int size,
                       const std::string& contPath,
                       std::string& outMsg);

    static bool Cat(const std::vector<std::string>& filePaths, std::string& outMsg);

private:
    static bool ReadUsersTxt(std::fstream& file,
                             const SuperBlock& sb,
                             const Inode& usersInode,
                             std::string& outContent,
                             std::string& outMsg);

    static bool WriteUsersTxt(std::fstream& file,
                              SuperBlock& sb,
                              Inode& usersInode,
                              const std::string& newContent,
                              std::string& outMsg);

    static int FindEntryInFolder(std::fstream& file,
                                 const SuperBlock& sb,
                                 const Inode& folderInode,
                                 const std::string& name);

    static bool AddEntryToFolder(std::fstream& file,
                                 SuperBlock& sb,
                                 Inode& parentInode,
                                 int parentInodeIndex,
                                 const std::string& name,
                                 int childInodeIndex,
                                 std::string& outMsg);

    static int AllocateFreeInode(std::fstream& file, SuperBlock& sb);
    static int AllocateFreeBlock(std::fstream& file, SuperBlock& sb);
};