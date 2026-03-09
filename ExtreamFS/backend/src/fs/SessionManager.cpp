#include "fs/SessionManager.h"
#include "disk/MountManager.h"
#include "fs/Ext2Structs.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>

Session SessionManager::currentSession;

static std::vector<std::string> split(const std::string& text, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(text);
    std::string item;

    while (std::getline(ss, item, delim)) {
        parts.push_back(item);
    }

    return parts;
}

static std::string trimUserField(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool SessionManager::Login(const std::string& user,
                           const std::string& pass,
                           const std::string& id,
                           std::string& outMsg) {
    outMsg.clear();

    if (currentSession.active) {
        outMsg = "Ya existe una sesion activa. Debe hacer logout primero.";
        return false;
    }

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

    // 1) Leer superbloque desde el inicio de la partición
    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        outMsg = "No se pudo leer el SuperBloque.";
        file.close();
        return false;
    }

    // 2) Leer el inodo 1 (users.txt)
    Inode usersInode{};
    file.seekg(sb.s_inode_start + sizeof(Inode)); // inodo 1
    file.read(reinterpret_cast<char*>(&usersInode), sizeof(Inode));
    if (!file) {
        outMsg = "No se pudo leer el inodo de users.txt.";
        file.close();
        return false;
    }

    // 3) Leer el primer bloque del archivo users.txt
    int blockIndex = usersInode.i_block[0];
    if (blockIndex < 0) {
        outMsg = "users.txt no tiene bloques asignados.";
        file.close();
        return false;
    }

    FileBlock usersBlock{};
    file.seekg(sb.s_block_start + blockIndex * sizeof(FileBlock));
    file.read(reinterpret_cast<char*>(&usersBlock), sizeof(FileBlock));
    if (!file) {
        outMsg = "No se pudo leer el bloque de users.txt.";
        file.close();
        return false;
    }

    file.close();

    std::string usersContent(usersBlock.b_content);

    // Estructura esperada:
    // 1,G,root
    // 1,U,root,root,123
    std::stringstream ss(usersContent);
    std::string line;

    while (std::getline(ss, line)) {
        line = trimUserField(line);
        if (line.empty()) continue;

        std::vector<std::string> cols = split(line, ',');
        if (cols.size() < 3) continue;

        for (std::string& c : cols) {
            c = trimUserField(c);
        }

        // Ignorar eliminados (id = 0)
        if (cols[0] == "0") continue;

        // Usuario: UID, U, grupo, usuario, password
        if (cols.size() == 5 && cols[1] == "U") {
            std::string fileGroup = cols[2];
            std::string fileUser  = cols[3];
            std::string filePass  = cols[4];

            if (fileUser == user) {
                if (filePass != pass) {
                    outMsg = "Autenticacion fallida: contraseña incorrecta.";
                    return false;
                }

                currentSession.active = true;
                currentSession.user = fileUser;
                currentSession.group = fileGroup;
                currentSession.partitionId = id;

                outMsg = "Sesion iniciada correctamente. Usuario: " + fileUser +
                         " | Grupo: " + fileGroup +
                         " | Particion: " + id;
                return true;
            }
        }
    }

    outMsg = "El usuario no existe en users.txt.";
    return false;
}

void SessionManager::Logout() {
    currentSession = Session{};
}