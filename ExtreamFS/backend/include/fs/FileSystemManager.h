#pragma once
#include <string>
#include <vector>
#include <fstream>
#include "fs/Ext2Structs.h"

class FileSystemManager {
public:
    static bool Mkfs(const std::string& id,
                     const std::string& type,
                     const std::string& fs,
                     std::string& outMsg);

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

    static bool Mkdir(const std::string& path,
                      bool recursiveP,
                      std::string& outMsg,
                      bool writeJournal = true);

    static bool Mkfile(const std::string& path,
                   int size,
                   const std::string& contPath,
                   bool recursive,
                   std::string& outMsg,
                   bool writeJournal = true);

    static bool Cat(const std::vector<std::string>& filePaths, std::string& outMsg);

    static bool ShowJournaling(const std::string& id, std::string& outMsg);

    static bool Loss(const std::string& id, std::string& outMsg);
    static bool Recovery(const std::string& id, std::string& outMsg);

    static bool Remove(const std::string& path, std::string& outMsg, bool registrarJournal = true);

    static bool Edit(const std::string& path,
                     const std::string& contentPath,
                     std::string& outMsg);

    static bool Rename(const std::string& path,
                       const std::string& newName,
                       std::string& outMsg);

    static bool Copy(const std::string& path,
                     const std::string& destino,
                     std::string& outMsg,
                     bool writeJournal = true);

    static bool Move(const std::string& path,
                     const std::string& destino,
                     std::string& outMsg);

    static bool Find(const std::string& path,
                     const std::string& name,
                     std::string& outMsg);

    static bool Chown(const std::string& path,
                      const std::string& newOwner,
                      bool recursive,
                      std::string& outMsg);

    static bool Chmod(const std::string& path,
                      const std::string& ugo,
                      bool recursive,
                      std::string& outMsg);

private:
    static bool CopyInternal(const std::string& path,
                             const std::string& destino,
                             std::string& outMsg,
                             bool writeJournal);

    static bool RemoveInternal(const std::string& path,
                               std::string& outMsg,
                               bool writeJournal);

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

    static bool AppendJournalEntry(std::fstream& file,
                                   int partitionStart,
                                   const SuperBlock& sb,
                                   const std::string& operation,
                                   const std::string& path,
                                   const std::string& content,
                                   std::string& outMsg);

    static bool RemoveEntryFromFolder(std::fstream& file,
                                      const SuperBlock& sb,
                                      Inode& parentInode,
                                      int parentInodeIndex,
                                      const std::string& name,
                                      std::string& outMsg);

    static bool DeleteInodeRecursive(std::fstream& file,
                                     SuperBlock& sb,
                                     int inodeIndex,
                                     const Inode& inode,
                                     std::string& outMsg);

    static bool FreeInodeBit(std::fstream& file, SuperBlock& sb, int inodeIndex);
    static bool FreeBlockBit(std::fstream& file, SuperBlock& sb, int blockIndex);
};