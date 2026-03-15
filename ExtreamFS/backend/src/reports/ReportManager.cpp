#include "reports/ReportManager.h"
#include "disk/MountManager.h"
#include "disk/Structs.h"
#include "fs/Ext2Structs.h"

#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <vector>
#include <algorithm>

#include <set>
#include <map>

static std::string escapeHtml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string sanitizeBlockContent(const char* data, size_t size) {
    std::string out;
    out.reserve(size);

    for (size_t i = 0; i < size; i++) {
        unsigned char c = static_cast<unsigned char>(data[i]);

        // detener en null para no ensuciar la salida
        if (c == '\0') break;

        // dejar solo caracteres imprimibles razonables
        if (c >= 32 && c <= 126) {
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back('.');
        }
    }

    return out;
}


static std::string sanitizeFixedString(const char* data, size_t size) {
    std::string out;
    out.reserve(size);

    for (size_t i = 0; i < size; i++) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == '\0') break;

        if (c >= 32 && c <= 126) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('.');
        }
    }

    return out;
}


static bool readInodeAt(std::fstream& file, const SuperBlock& sb, int inodeIndex, Inode& inode) {
    file.seekg(sb.s_inode_start + inodeIndex * (int)sizeof(Inode));
    file.read(reinterpret_cast<char*>(&inode), sizeof(Inode));
    return (bool)file;
}

static bool readFolderBlockAt(std::fstream& file, const SuperBlock& sb, int blockIndex, FolderBlock& block) {
    file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FolderBlock));
    file.read(reinterpret_cast<char*>(&block), sizeof(FolderBlock));
    return (bool)file;
}

static bool readFileBlockAt(std::fstream& file, const SuperBlock& sb, int blockIndex, FileBlock& block) {
    file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
    file.read(reinterpret_cast<char*>(&block), sizeof(FileBlock));
    return (bool)file;
}

static std::string getContentName(const Content& c) {
    return sanitizeFixedString(c.b_name, sizeof(c.b_name));
}


static std::string bitmapToHtmlRows(const std::vector<char>& bits, int perRow = 20) {
    std::stringstream ss;

    for (size_t i = 0; i < bits.size(); i++) {
        ss << bits[i];
        if ((i + 1) % perRow == 0) {
            ss << "<br/>";
        } else {
            ss << " ";
        }
    }

    return ss.str();
}

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;

    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }

    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

static int findChildInodeByName(std::fstream& file,
                                const SuperBlock& sb,
                                const Inode& folderInode,
                                const std::string& targetName) {
    for (int i = 0; i < 15; i++) {
        int blockIndex = folderInode.i_block[i];
        if (blockIndex < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlockAt(file, sb, blockIndex, fb)) {
            continue;
        }

        for (int j = 0; j < 4; j++) {
            std::string name = getContentName(fb.b_content[j]);
            int inodeIdx = fb.b_content[j].b_inodo;

            if (inodeIdx >= 0 && name == targetName) {
                return inodeIdx;
            }
        }
    }

    return -1;
}

static bool findInodeByPath(std::fstream& file,
                            const SuperBlock& sb,
                            const std::string& path,
                            int& outInodeIndex,
                            Inode& outInode) {
    if (path.empty() || path[0] != '/') {
        return false;
    }

    std::vector<std::string> parts = splitPath(path);

    int currentInodeIndex = 0;
    Inode currentInode{};
    if (!readInodeAt(file, sb, currentInodeIndex, currentInode)) {
        return false;
    }

    for (const std::string& part : parts) {
        int nextInodeIndex = findChildInodeByName(file, sb, currentInode, part);
        if (nextInodeIndex < 0) {
            return false;
        }

        if (!readInodeAt(file, sb, nextInodeIndex, currentInode)) {
            return false;
        }

        currentInodeIndex = nextInodeIndex;
    }

    outInodeIndex = currentInodeIndex;
    outInode = currentInode;
    return true;
}

static std::string readFileContentFromInode(std::fstream& file,
                                            const SuperBlock& sb,
                                            const Inode& inode) {
    std::string content;

    for (int i = 0; i < 15; i++) {
        int blockIndex = inode.i_block[i];
        if (blockIndex < 0) continue;

        FileBlock fb{};
        if (!readFileBlockAt(file, sb, blockIndex, fb)) {
            continue;
        }

        content += sanitizeBlockContent(fb.b_content, sizeof(fb.b_content));
    }

    return content;
}



static std::vector<EBR> readLogicalPartitions(const std::string& diskPath, const Partition& extendedPart) {
    std::vector<EBR> logicals;

    if (extendedPart.part_start == -1 || extendedPart.part_size <= 0) {
        return logicals;
    }

    std::ifstream file(diskPath, std::ios::binary);
    if (!file.is_open()) {
        return logicals;
    }

    int pos = extendedPart.part_start;

    while (pos != -1) {
        EBR ebr{};
        file.seekg(pos);
        file.read(reinterpret_cast<char*>(&ebr), sizeof(EBR));
        if (!file) {
            break;
        }

        if (ebr.part_size > 0 || std::string(ebr.part_name).size() > 0) {
            logicals.push_back(ebr);
        }

        if (ebr.part_next == -1) {
            break;
        }

        pos = ebr.part_next;
    }

    file.close();
    return logicals;
}

bool ReportManager::RepSb(const std::string& id,
                          const std::string& outPath,
                          std::string& outMsg) {
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
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }
    file.close();

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  sb [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='2'><b>SUPER BLOQUE</b></td></tr>\n";

    dotFile << "    <tr><td>s_filesystem_type</td><td>" << sb.s_filesystem_type << "</td></tr>\n";
    dotFile << "    <tr><td>s_inodes_count</td><td>" << sb.s_inodes_count << "</td></tr>\n";
    dotFile << "    <tr><td>s_blocks_count</td><td>" << sb.s_blocks_count << "</td></tr>\n";
    dotFile << "    <tr><td>s_free_blocks_count</td><td>" << sb.s_free_blocks_count << "</td></tr>\n";
    dotFile << "    <tr><td>s_free_inodes_count</td><td>" << sb.s_free_inodes_count << "</td></tr>\n";
    dotFile << "    <tr><td>s_mtime</td><td>" << escapeHtml(sb.s_mtime) << "</td></tr>\n";
    dotFile << "    <tr><td>s_umtime</td><td>" << escapeHtml(sb.s_umtime) << "</td></tr>\n";
    dotFile << "    <tr><td>s_mnt_count</td><td>" << sb.s_mnt_count << "</td></tr>\n";
    dotFile << "    <tr><td>s_magic</td><td>" << sb.s_magic << "</td></tr>\n";
    dotFile << "    <tr><td>s_inode_size</td><td>" << sb.s_inode_size << "</td></tr>\n";
    dotFile << "    <tr><td>s_block_size</td><td>" << sb.s_block_size << "</td></tr>\n";
    dotFile << "    <tr><td>s_first_ino</td><td>" << sb.s_first_ino << "</td></tr>\n";
    dotFile << "    <tr><td>s_first_blo</td><td>" << sb.s_first_blo << "</td></tr>\n";
    dotFile << "    <tr><td>s_bm_inode_start</td><td>" << sb.s_bm_inode_start << "</td></tr>\n";
    dotFile << "    <tr><td>s_bm_block_start</td><td>" << sb.s_bm_block_start << "</td></tr>\n";
    dotFile << "    <tr><td>s_inode_start</td><td>" << sb.s_inode_start << "</td></tr>\n";
    dotFile << "    <tr><td>s_block_start</td><td>" << sb.s_block_start << "</td></tr>\n";

    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte SB generado correctamente: " + outPath;
    return true;
}

bool ReportManager::RepMbr(const std::string& id,
                           const std::string& outPath,
                           std::string& outMsg) {
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

    MBR mbr{};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    if (!file) {
        file.close();
        outMsg = "No se pudo leer el MBR del disco.";
        return false;
    }
    file.close();

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  mbr [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='2'><b>REPORTE DE MBR</b></td></tr>\n";

    dotFile << "    <tr><td><b>mbr_tamano</b></td><td>" << mbr.mbr_tamano << "</td></tr>\n";
    dotFile << "    <tr><td><b>mbr_fecha_creacion</b></td><td>" << escapeHtml(mbr.mbr_fecha_creacion) << "</td></tr>\n";
    dotFile << "    <tr><td><b>mbr_disk_signature</b></td><td>" << mbr.mbr_disk_signature << "</td></tr>\n";
    dotFile << "    <tr><td><b>disk_fit</b></td><td>" << mbr.disk_fit << "</td></tr>\n";

    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];

        dotFile << "    <tr><td colspan='2'><b>PARTICION " << (i + 1) << "</b></td></tr>\n";
        dotFile << "    <tr><td>part_status</td><td>" << p.part_status << "</td></tr>\n";
        dotFile << "    <tr><td>part_type</td><td>" << p.part_type << "</td></tr>\n";
        dotFile << "    <tr><td>part_fit</td><td>" << p.part_fit << "</td></tr>\n";
        dotFile << "    <tr><td>part_start</td><td>" << p.part_start << "</td></tr>\n";
        dotFile << "    <tr><td>part_size</td><td>" << p.part_size << "</td></tr>\n";
        dotFile << "    <tr><td>part_name</td><td>" << escapeHtml(std::string(p.part_name)) << "</td></tr>\n";

        // Si es extendida, leer y reportar EBR/logicas
        if ((p.part_type == 'e' || p.part_type == 'E') &&
            p.part_start != -1 && p.part_size > 0) {

            std::vector<EBR> logicals = readLogicalPartitions(mp.path, p);

            for (size_t j = 0; j < logicals.size(); j++) {
                const EBR& e = logicals[j];

                dotFile << "    <tr><td colspan='2'><b>EBR " << (j + 1) << "</b></td></tr>\n";
                dotFile << "    <tr><td>part_mount</td><td>" << e.part_mount << "</td></tr>\n";
                dotFile << "    <tr><td>part_fit</td><td>" << e.part_fit << "</td></tr>\n";
                dotFile << "    <tr><td>part_start</td><td>" << e.part_start << "</td></tr>\n";
                dotFile << "    <tr><td>part_size</td><td>" << e.part_size << "</td></tr>\n";
                dotFile << "    <tr><td>part_next</td><td>" << e.part_next << "</td></tr>\n";
                dotFile << "    <tr><td>part_name</td><td>" << escapeHtml(std::string(e.part_name)) << "</td></tr>\n";
            }
        }
    }

    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte MBR generado correctamente: " + outPath;
    return true;
}

bool ReportManager::RepDisk(const std::string& id,
                            const std::string& outPath,
                            std::string& outMsg) {
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

    MBR mbr{};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    if (!file) {
        file.close();
        outMsg = "No se pudo leer el MBR del disco.";
        return false;
    }
    file.close();

    std::vector<Partition> parts;
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_start != -1 && p.part_size > 0) {
            parts.push_back(p);
        }
    }

    std::sort(parts.begin(), parts.end(), [](const Partition& a, const Partition& b) {
        return a.part_start < b.part_start;
    });

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    auto percent = [&](int bytes) -> double {
        return (static_cast<double>(bytes) * 100.0) / static_cast<double>(mbr.mbr_tamano);
    };

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  disk [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='20'><b>REPORTE DISK</b></td></tr>\n";
    dotFile << "    <tr>\n";

    // MBR
    dotFile << "      <td><b>MBR</b><br/>" << percent((int)sizeof(MBR)) << "%</td>\n";

    int current = (int)sizeof(MBR);

    for (const auto& p : parts) {
        // Espacio libre antes de esta partición
        if (p.part_start > current) {
            int freeSize = p.part_start - current;
            dotFile << "      <td><b>LIBRE</b><br/>" << percent(freeSize) << "%</td>\n";
        }

        if (p.part_type == 'e' || p.part_type == 'E') {
            // Dibujar extendida con lógicas internas
            std::vector<EBR> logicals = readLogicalPartitions(mp.path, p);

            dotFile << "      <td>\n";
            dotFile << "        <table border='1' cellborder='1' cellspacing='0'>\n";
            dotFile << "          <tr><td colspan='" << std::max(1, (int)logicals.size() * 2)
                    << "'><b>EXTENDIDA</b><br/>"
                    << escapeHtml(std::string(p.part_name))
                    << "<br/>" << percent(p.part_size) << "%</td></tr>\n";
            dotFile << "          <tr>\n";

            if (logicals.empty()) {
                dotFile << "            <td><b>LIBRE</b></td>\n";
            } else {
                for (const auto& e : logicals) {
                    dotFile << "            <td><b>EBR</b></td>\n";
                    dotFile << "            <td><b>LOGICA</b><br/>"
                            << escapeHtml(std::string(e.part_name))
                            << "<br/>" << percent(e.part_size) << "%</td>\n";
                }
            }

            dotFile << "          </tr>\n";
            dotFile << "        </table>\n";
            dotFile << "      </td>\n";
        } else {
            // Primaria normal
            dotFile << "      <td><b>PRIMARIA</b><br/>"
                    << escapeHtml(std::string(p.part_name))
                    << "<br/>" << percent(p.part_size) << "%</td>\n";
        }

        current = p.part_start + p.part_size;
    }

    // Espacio libre final
    if (mbr.mbr_tamano > current) {
        int freeSize = mbr.mbr_tamano - current;
        dotFile << "      <td><b>LIBRE</b><br/>" << percent(freeSize) << "%</td>\n";
    }

    dotFile << "    </tr>\n";
    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte DISK generado correctamente: " + outPath;
    return true;
}


bool ReportManager::RepInode(const std::string& id,
                             const std::string& outPath,
                             std::string& outMsg) {
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
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    std::vector<int> usedInodes;
    file.seekg(sb.s_bm_inode_start);
    for (int i = 0; i < sb.s_inodes_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) {
            file.close();
            outMsg = "No se pudo leer el bitmap de inodos.";
            return false;
        }
        if (bit == '1') {
            usedInodes.push_back(i);
        }
    }

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        file.close();
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  rankdir=LR;\n";
    dotFile << "  node [shape=plaintext];\n";

    for (size_t idx = 0; idx < usedInodes.size(); idx++) {
        int inodeIndex = usedInodes[idx];

        Inode inode{};
        file.seekg(sb.s_inode_start + inodeIndex * (int)sizeof(Inode));
        file.read(reinterpret_cast<char*>(&inode), sizeof(Inode));
        if (!file) {
            file.close();
            dotFile.close();
            outMsg = "No se pudo leer el inodo " + std::to_string(inodeIndex);
            return false;
        }

        dotFile << "  inode" << inodeIndex << " [label=<\n";
        dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
        dotFile << "    <tr><td colspan='2'><b>INODO " << inodeIndex << "</b></td></tr>\n";
        dotFile << "    <tr><td>i_uid</td><td>" << inode.i_uid << "</td></tr>\n";
        dotFile << "    <tr><td>i_gid</td><td>" << inode.i_gid << "</td></tr>\n";
        dotFile << "    <tr><td>i_size</td><td>" << inode.i_size << "</td></tr>\n";
        dotFile << "    <tr><td>i_atime</td><td>" << escapeHtml(inode.i_atime) << "</td></tr>\n";
        dotFile << "    <tr><td>i_ctime</td><td>" << escapeHtml(inode.i_ctime) << "</td></tr>\n";
        dotFile << "    <tr><td>i_mtime</td><td>" << escapeHtml(inode.i_mtime) << "</td></tr>\n";

        for (int i = 0; i < 15; i++) {
            dotFile << "    <tr><td>i_block[" << i << "]</td><td>" << inode.i_block[i] << "</td></tr>\n";
        }

        dotFile << "    <tr><td>i_type</td><td>" << inode.i_type << "</td></tr>\n";
        dotFile << "    <tr><td>i_perm</td><td>"
                << std::string(inode.i_perm, inode.i_perm + 3) << "</td></tr>\n";
        dotFile << "  </table>\n";
        dotFile << "  >];\n";

        if (idx + 1 < usedInodes.size()) {
            dotFile << "  inode" << inodeIndex << " -> inode" << usedInodes[idx + 1] << ";\n";
        }
    }

    dotFile << "}\n";
    dotFile.close();
    file.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte INODE generado correctamente: " + outPath;
    return true;
}


bool ReportManager::RepBlock(const std::string& id,
                             const std::string& outPath,
                             std::string& outMsg) {
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
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    std::vector<int> usedBlocks;
    file.seekg(sb.s_bm_block_start);
    for (int i = 0; i < sb.s_blocks_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) {
            file.close();
            outMsg = "No se pudo leer el bitmap de bloques.";
            return false;
        }
        if (bit == '1') {
            usedBlocks.push_back(i);
        }
    }

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        file.close();
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  rankdir=LR;\n";
    dotFile << "  node [shape=plaintext];\n";

    for (size_t idx = 0; idx < usedBlocks.size(); idx++) {
        int blockIndex = usedBlocks[idx];

        FileBlock fb{};
        file.seekg(sb.s_block_start + blockIndex * (int)sizeof(FileBlock));
        file.read(reinterpret_cast<char*>(&fb), sizeof(FileBlock));
        if (!file) {
            file.close();
            dotFile.close();
            outMsg = "No se pudo leer el bloque " + std::to_string(blockIndex);
            return false;
        }

        std::string content = sanitizeBlockContent(fb.b_content, 64);

        dotFile << "  block" << blockIndex << " [label=<\n";
        dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
        dotFile << "    <tr><td colspan='2'><b>BLOQUE " << blockIndex << "</b></td></tr>\n";
        dotFile << "    <tr><td>b_content</td><td>" << escapeHtml(content) << "</td></tr>\n";
        dotFile << "  </table>\n";
        dotFile << "  >];\n";

        if (idx + 1 < usedBlocks.size()) {
            dotFile << "  block" << blockIndex << " -> block" << usedBlocks[idx + 1] << ";\n";
        }
    }

    dotFile << "}\n";
    dotFile.close();
    file.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte BLOCK generado correctamente: " + outPath;
    return true;
}



static void renderInodeNode(std::ofstream& dotFile, int inodeIndex, const Inode& inode) {
    dotFile << "  inode" << inodeIndex << " [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='2'><b>INODO " << inodeIndex << "</b></td></tr>\n";
    dotFile << "    <tr><td>i_uid</td><td>" << inode.i_uid << "</td></tr>\n";
    dotFile << "    <tr><td>i_gid</td><td>" << inode.i_gid << "</td></tr>\n";
    dotFile << "    <tr><td>i_size</td><td>" << inode.i_size << "</td></tr>\n";
    dotFile << "    <tr><td>i_type</td><td>" << inode.i_type << "</td></tr>\n";
    dotFile << "    <tr><td>i_perm</td><td>"
            << std::string(inode.i_perm, inode.i_perm + 3) << "</td></tr>\n";

    for (int i = 0; i < 15; i++) {
        dotFile << "    <tr><td>i_block[" << i << "]</td><td>" << inode.i_block[i] << "</td></tr>\n";
    }

    dotFile << "  </table>\n";
    dotFile << "  >];\n";
}

static void renderFolderBlockNode(std::ofstream& dotFile, int blockIndex, const FolderBlock& fb) {
    dotFile << "  block" << blockIndex << " [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='2'><b>BLOQUE CARPETA " << blockIndex << "</b></td></tr>\n";

    for (int i = 0; i < 4; i++) {
        std::string name = escapeHtml(getContentName(fb.b_content[i]));
        dotFile << "    <tr><td>" << name << "</td><td>" << fb.b_content[i].b_inodo << "</td></tr>\n";
    }

    dotFile << "  </table>\n";
    dotFile << "  >];\n";
}

static void renderFileBlockNode(std::ofstream& dotFile, int blockIndex, const FileBlock& fb) {
    std::string content = sanitizeBlockContent(fb.b_content, sizeof(fb.b_content));

    dotFile << "  block" << blockIndex << " [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='2'><b>BLOQUE ARCHIVO " << blockIndex << "</b></td></tr>\n";
    dotFile << "    <tr><td>b_content</td><td>" << escapeHtml(content) << "</td></tr>\n";
    dotFile << "  </table>\n";
    dotFile << "  >];\n";
}

static void traverseTreeRecursive(std::fstream& file,
                                  const SuperBlock& sb,
                                  int inodeIndex,
                                  std::ofstream& dotFile,
                                  std::set<int>& visitedInodes,
                                  std::set<int>& visitedBlocks) {
    if (inodeIndex < 0) return;
    if (visitedInodes.count(inodeIndex)) return;

    Inode inode{};
    if (!readInodeAt(file, sb, inodeIndex, inode)) return;

    visitedInodes.insert(inodeIndex);
    renderInodeNode(dotFile, inodeIndex, inode);

    bool isFolder = (inode.i_type == '0' || inode.i_type == 0);

    for (int i = 0; i < 15; i++) {
        int blockIndex = inode.i_block[i];
        if (blockIndex < 0) continue;

        dotFile << "  inode" << inodeIndex << " -> block" << blockIndex << ";\n";

        if (visitedBlocks.count(blockIndex)) {
            continue;
        }

        visitedBlocks.insert(blockIndex);

        if (isFolder) {
            FolderBlock fb{};
            if (!readFolderBlockAt(file, sb, blockIndex, fb)) continue;

            renderFolderBlockNode(dotFile, blockIndex, fb);

            for (int j = 0; j < 4; j++) {
                int childInode = fb.b_content[j].b_inodo;
                std::string name = getContentName(fb.b_content[j]);

                if (childInode < 0) continue;
                if (name.empty()) continue;
                if (name == "." || name == "..") continue;

                dotFile << "  block" << blockIndex << " -> inode" << childInode << ";\n";
                traverseTreeRecursive(file, sb, childInode, dotFile, visitedInodes, visitedBlocks);
            }
        } else {
            FileBlock fb{};
            if (!readFileBlockAt(file, sb, blockIndex, fb)) continue;
            renderFileBlockNode(dotFile, blockIndex, fb);
        }
    }
}

bool ReportManager::RepTree(const std::string& id,
                            const std::string& outPath,
                            std::string& outMsg) {
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
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        file.close();
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  rankdir=LR;\n";
    dotFile << "  node [shape=plaintext];\n";

    std::set<int> visitedInodes;
    std::set<int> visitedBlocks;

    // raíz = inodo 0
    traverseTreeRecursive(file, sb, 0, dotFile, visitedInodes, visitedBlocks);

    dotFile << "}\n";
    dotFile.close();
    file.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte TREE generado correctamente: " + outPath;
    return true;
}


bool ReportManager::RepFile(const std::string& id,
                            const std::string& outPath,
                            const std::string& filePath,
                            std::string& outMsg) {
    outMsg.clear();

    if (filePath.empty()) {
        outMsg = "La ruta interna del archivo es obligatoria.";
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

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    int inodeIndex = -1;
    Inode inode{};
    if (!findInodeByPath(file, sb, filePath, inodeIndex, inode)) {
        file.close();
        outMsg = "No se encontro el archivo en la ruta: " + filePath;
        return false;
    }

    bool isFolder = (inode.i_type == '0' || inode.i_type == 0);
    if (isFolder) {
        file.close();
        outMsg = "La ruta indicada corresponde a una carpeta, no a un archivo.";
        return false;
    }

    std::string content = readFileContentFromInode(file, sb, inode);
    file.close();

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  fileRep [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td><b>REPORTE FILE</b></td></tr>\n";
    dotFile << "    <tr><td><b>Ruta:</b> " << escapeHtml(filePath) << "</td></tr>\n";
    dotFile << "    <tr><td>" << escapeHtml(content) << "</td></tr>\n";
    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte FILE generado correctamente: " + outPath;
    return true;
}

bool ReportManager::RepLs(const std::string& id,
                          const std::string& outPath,
                          const std::string& dirPath,
                          std::string& outMsg) {
    outMsg.clear();

    if (dirPath.empty()) {
        outMsg = "La ruta interna de la carpeta es obligatoria.";
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

    SuperBlock sb{};
    file.seekg(mp.start);
    file.read(reinterpret_cast<char*>(&sb), sizeof(SuperBlock));
    if (!file) {
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    int inodeIndex = -1;
    Inode inode{};
    if (!findInodeByPath(file, sb, dirPath, inodeIndex, inode)) {
        file.close();
        outMsg = "No se encontro la carpeta en la ruta: " + dirPath;
        return false;
    }

    bool isFolder = (inode.i_type == '0' || inode.i_type == 0);
    if (!isFolder) {
        file.close();
        outMsg = "La ruta indicada corresponde a un archivo, no a una carpeta.";
        return false;
    }

    struct LsEntry {
        std::string permisos;
        std::string owner;
        std::string grupo;
        int size;
        std::string fecha;
        std::string hora;
        std::string tipo;
        std::string nombre;
    };

    std::vector<LsEntry> entries;

    for (int i = 0; i < 15; i++) {
        int blockIndex = inode.i_block[i];
        if (blockIndex < 0) continue;

        FolderBlock fb{};
        if (!readFolderBlockAt(file, sb, blockIndex, fb)) {
            continue;
        }

        for (int j = 0; j < 4; j++) {
            std::string name = getContentName(fb.b_content[j]);
            int childInode = fb.b_content[j].b_inodo;

            if (childInode < 0 || name.empty()) continue;

            Inode child{};
            if (!readInodeAt(file, sb, childInode, child)) {
                continue;
            }

            std::string tipo = (child.i_type == '0' || child.i_type == 0) ? "Carpeta" : "Archivo";

            std::string owner = std::to_string(child.i_uid);
            std::string grupo = std::to_string(child.i_gid);

            std::string permisos(child.i_perm, child.i_perm + 3);

            std::string fechaHora = sanitizeFixedString(child.i_mtime, sizeof(child.i_mtime));
            std::string fecha = fechaHora;
            std::string hora = "";

            size_t spacePos = fechaHora.find(' ');
            if (spacePos != std::string::npos) {
                fecha = fechaHora.substr(0, spacePos);
                hora = fechaHora.substr(spacePos + 1);
            }

            entries.push_back({
                permisos,
                owner,
                grupo,
                child.i_size,
                fecha,
                hora,
                tipo,
                name
            });
        }
    }

    file.close();

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  lsrep [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td colspan='8'><b>REPORTE LS</b></td></tr>\n";
    dotFile << "    <tr><td colspan='8'><b>Ruta:</b> " << escapeHtml(dirPath) << "</td></tr>\n";
    dotFile << "    <tr>"
            << "<td><b>Permisos</b></td>"
            << "<td><b>Owner</b></td>"
            << "<td><b>Grupo</b></td>"
            << "<td><b>Size (bytes)</b></td>"
            << "<td><b>Fecha</b></td>"
            << "<td><b>Hora</b></td>"
            << "<td><b>Tipo</b></td>"
            << "<td><b>Name</b></td>"
            << "</tr>\n";

    for (const auto& e : entries) {
        dotFile << "    <tr>"
                << "<td>" << escapeHtml(e.permisos) << "</td>"
                << "<td>" << escapeHtml(e.owner) << "</td>"
                << "<td>" << escapeHtml(e.grupo) << "</td>"
                << "<td>" << e.size << "</td>"
                << "<td>" << escapeHtml(e.fecha) << "</td>"
                << "<td>" << escapeHtml(e.hora) << "</td>"
                << "<td>" << escapeHtml(e.tipo) << "</td>"
                << "<td>" << escapeHtml(e.nombre) << "</td>"
                << "</tr>\n";
    }

    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte LS generado correctamente: " + outPath;
    return true;
}

bool ReportManager::RepBmInode(const std::string& id,
                               const std::string& outPath,
                               std::string& outMsg) {
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
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    std::vector<char> bits;
    file.seekg(sb.s_bm_inode_start);

    for (int i = 0; i < sb.s_inodes_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) {
            file.close();
            outMsg = "No se pudo leer el bitmap de inodos.";
            return false;
        }
        bits.push_back(bit);
    }

    file.close();

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  bmi [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td><b>REPORTE BM_INODE</b></td></tr>\n";
    dotFile << "    <tr><td>" << bitmapToHtmlRows(bits, 20) << "</td></tr>\n";
    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte BM_INODE generado correctamente: " + outPath;
    return true;
}

bool ReportManager::RepBmBlock(const std::string& id,
                               const std::string& outPath,
                               std::string& outMsg) {
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
        file.close();
        outMsg = "No se pudo leer el SuperBloque.";
        return false;
    }

    std::vector<char> bits;
    file.seekg(sb.s_bm_block_start);

    for (int i = 0; i < sb.s_blocks_count; i++) {
        char bit = '0';
        file.read(&bit, 1);
        if (!file) {
            file.close();
            outMsg = "No se pudo leer el bitmap de bloques.";
            return false;
        }
        bits.push_back(bit);
    }

    file.close();

    std::filesystem::path output(outPath);
    std::filesystem::create_directories(output.parent_path());

    std::filesystem::path dotPath = output;
    dotPath.replace_extension(".dot");

    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outMsg = "No se pudo crear el archivo DOT del reporte.";
        return false;
    }

    dotFile << "digraph G {\n";
    dotFile << "  node [shape=plaintext];\n";
    dotFile << "  bmb [label=<\n";
    dotFile << "  <table border='1' cellborder='1' cellspacing='0'>\n";
    dotFile << "    <tr><td><b>REPORTE BM_BLOCK</b></td></tr>\n";
    dotFile << "    <tr><td>" << bitmapToHtmlRows(bits, 20) << "</td></tr>\n";
    dotFile << "  </table>\n";
    dotFile << "  >];\n";
    dotFile << "}\n";
    dotFile.close();

    std::stringstream cmd;
    cmd << "dot -Tpng \"" << dotPath.string() << "\" -o \"" << outPath << "\"";

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        outMsg = "Se genero el .dot pero fallo Graphviz al crear la imagen.";
        return false;
    }

    outMsg = "Reporte BM_BLOCK generado correctamente: " + outPath;
    return true;
}