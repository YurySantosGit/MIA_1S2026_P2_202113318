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