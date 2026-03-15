#include "disk/DiskManagement.h"
#include "disk/Structs.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>
#include <algorithm>
#include <climits>

static std::string nowString() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);

    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

static int bytesFromSize(int size, char unit) {
    // unit: 'k' or 'm'
    if (unit == 'k') return size * 1024;
    return size * 1024 * 1024; // default MB
}

static char normalizeFit(char fit) {
    // fit: 'b','f','w'
    fit = (char)std::tolower((unsigned char)fit);
    if (fit == 'b' || fit == 'f' || fit == 'w') return fit;
    return 'f'; // default First Fit
}

static char normalizeUnit(char unit) {
    unit = (char)std::tolower((unsigned char)unit);
    if (unit == 'k' || unit == 'm') return unit;
    return 'm';
}

bool DiskManagement::Mkdisk(int size, const std::string& path, char unit, char fit, std::string& outMsg) {
    outMsg.clear();

    if (size <= 0) {
        outMsg = "Error: -size debe ser mayor a 0.";
        return false;
    }
    if (path.empty()) {
        outMsg = "Error: -path es obligatorio.";
        return false;
    }

    unit = normalizeUnit(unit);
    fit = normalizeFit(fit);

    int totalBytes = bytesFromSize(size, unit);

    // Crear directorios si no existen
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            outMsg = "Error creando directorios: " + ec.message();
            return false;
        }
    }

    // Crear archivo y llenarlo con 0
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo crear el archivo en path: " + path;
        return false;
    }

    const size_t BUF_SIZE = 1024;
    char buffer[BUF_SIZE];
    std::memset(buffer, 0, BUF_SIZE);

    int remaining = totalBytes;
    while (remaining > 0) {
        int chunk = (remaining >= (int)BUF_SIZE) ? (int)BUF_SIZE : remaining;
        file.write(buffer, chunk);
        remaining -= chunk;
    }

    // Construir MBR
    MBR mbr{};
    mbr.mbr_tamano = totalBytes;
    std::string fecha = nowString();
    std::memset(mbr.mbr_fecha_creacion, 0, sizeof(mbr.mbr_fecha_creacion));
    std::strncpy(mbr.mbr_fecha_creacion, fecha.c_str(), sizeof(mbr.mbr_fecha_creacion) - 1);

    // signature random
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int32_t> dist(1, 2147483647);
    mbr.mbr_disk_signature = dist(gen);

    mbr.disk_fit = fit;

    // inicializar particiones en “vacío”
    for (int i = 0; i < 4; i++) {
        mbr.mbr_partitions[i].part_status = '0';
        mbr.mbr_partitions[i].part_type = 'p';
        mbr.mbr_partitions[i].part_fit = fit;
        mbr.mbr_partitions[i].part_start = -1;
        mbr.mbr_partitions[i].part_size = 0;
        std::memset(mbr.mbr_partitions[i].part_name, 0, sizeof(mbr.mbr_partitions[i].part_name));
    }

    // Escribir MBR al inicio
    file.seekp(0);
    file.write(reinterpret_cast<char*>(&mbr), sizeof(MBR));
    file.close();

    outMsg = "Disco creado OK: " + path + " (" + std::to_string(totalBytes) + " bytes)";
    return true;
}

bool DiskManagement::Rmdisk(const std::string& path, std::string& outMsg) {
    outMsg.clear();

    if (path.empty()) {
        outMsg = "Error: -path es obligatorio.";
        return false;
    }

    std::filesystem::path p(path);

    if (!std::filesystem::exists(p)) {
        outMsg = "Error: el disco no existe en la ruta indicada: " + path;
        return false;
    }

    std::error_code ec;
    bool removed = std::filesystem::remove(p, ec);

    if (ec) {
        outMsg = "Error al eliminar el disco: " + ec.message();
        return false;
    }

    if (!removed) {
        outMsg = "Error: no se pudo eliminar el disco.";
        return false;
    }

    outMsg = "Disco eliminado correctamente: " + path;
    return true;
}

bool DiskManagement::ReadMBR(const std::string& path, MBR& outMBR, std::string& outMsg) {
    outMsg.clear();

    if (path.empty()) {
        outMsg = "Error: path vacio.";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo abrir el disco: " + path;
        return false;
    }

    file.seekg(0);
    file.read(reinterpret_cast<char*>(&outMBR), sizeof(MBR));

    if (!file) {
        outMsg = "Error: no se pudo leer el MBR del disco.";
        return false;
    }

    file.close();
    return true;
}

bool DiskManagement::WriteMBR(const std::string& path, const MBR& mbr, std::string& outMsg) {
    outMsg.clear();

    if (path.empty()) {
        outMsg = "Error: path vacio.";
        return false;
    }

    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo abrir el disco para escritura: " + path;
        return false;
    }

    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&mbr), sizeof(MBR));

    if (!file) {
        outMsg = "Error: no se pudo escribir el MBR en el disco.";
        return false;
    }

    file.close();
    return true;
}

static int bytesFromUnitFDisk(int size, char unit) {
    unit = (char)std::tolower((unsigned char)unit);

    if (unit == 'b') return size;
    if (unit == 'k') return size * 1024;
    if (unit == 'm') return size * 1024 * 1024;

    return size * 1024;
}


static char normalizeType(char type) {
    type = (char)std::tolower((unsigned char)type);
    if (type == 'p' || type == 'e' || type == 'l') return type;
    return 'p';
}

static int findFreePartitionSlot(const MBR& mbr) {
    for (int i = 0; i < 4; i++) {
        if (mbr.mbr_partitions[i].part_start == -1 || mbr.mbr_partitions[i].part_size == 0) {
            return i;
        }
    }
    return -1;
}

static bool partitionNameExists(const MBR& mbr, const std::string& name) {
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_start != -1 && p.part_size > 0) {
            std::string existingName(p.part_name);
            if (existingName == name) {
                return true;
            }
        }
    }
    return false;
}

static std::vector<Partition> getActivePartitions(const MBR& mbr) {
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
    return parts;
}

struct Gap {
    int start;
    int size;
};

static std::vector<Gap> getAvailableGaps(const MBR& mbr) {
    std::vector<Gap> gaps;
    std::vector<Partition> parts = getActivePartitions(mbr);

    int diskStart = (int)sizeof(MBR);
    int diskEnd = mbr.mbr_tamano;

    if (parts.empty()) {
        gaps.push_back({diskStart, diskEnd - diskStart});
        return gaps;
    }

    // Hueco entre MBR y primera partición
    if (parts[0].part_start > diskStart) {
        gaps.push_back({diskStart, parts[0].part_start - diskStart});
    }

    // Huecos intermedios
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        int gapStart = parts[i].part_start + parts[i].part_size;
        int gapSize = parts[i + 1].part_start - gapStart;
        if (gapSize > 0) {
            gaps.push_back({gapStart, gapSize});
        }
    }

    // Último hueco
    int lastEnd = parts.back().part_start + parts.back().part_size;
    if (diskEnd > lastEnd) {
        gaps.push_back({lastEnd, diskEnd - lastEnd});
    }

    return gaps;
}

static int chooseGapIndex(const std::vector<Gap>& gaps, int requiredSize, char fit) {
    fit = (char)std::tolower((unsigned char)fit);

    int selected = -1;

    if (fit == 'f') {
        for (size_t i = 0; i < gaps.size(); i++) {
            if (gaps[i].size >= requiredSize) {
                return (int)i;
            }
        }
        return -1;
    }

    if (fit == 'b') {
        int bestSize = INT_MAX;
        for (size_t i = 0; i < gaps.size(); i++) {
            if (gaps[i].size >= requiredSize && gaps[i].size < bestSize) {
                bestSize = gaps[i].size;
                selected = (int)i;
            }
        }
        return selected;
    }

    if (fit == 'w') {
        int worstSize = -1;
        for (size_t i = 0; i < gaps.size(); i++) {
            if (gaps[i].size >= requiredSize && gaps[i].size > worstSize) {
                worstSize = gaps[i].size;
                selected = (int)i;
            }
        }
        return selected;
    }

    return -1;
}

static int countPrimaryExtended(const MBR& mbr) {
    int count = 0;
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_start != -1 && p.part_size > 0) {
            count++;
        }
    }
    return count;
}

static int findExtendedIndex(const MBR& mbr) {
    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_start != -1 && p.part_size > 0 &&
            (p.part_type == 'e' || p.part_type == 'E')) {
            return i;
        }
    }
    return -1;
}

bool DiskManagement::Fdisk(int size,
                           const std::string& path,
                           const std::string& name,
                           char unit,
                           char type,
                           char fit,
                           std::string& outMsg) {
    outMsg.clear();

    if (size <= 0) {
        outMsg = "Error: -size debe ser mayor a 0.";
        return false;
    }

    if (path.empty()) {
        outMsg = "Error: -path es obligatorio.";
        return false;
    }

    if (name.empty()) {
        outMsg = "Error: -name es obligatorio.";
        return false;
    }

    unit = (char)std::tolower((unsigned char)unit);
    type = normalizeType(type);
    fit  = normalizeFit(fit);

    int partBytes = bytesFromUnitFDisk(size, unit);

    if (type == 'l') {
        return CreateLogical(partBytes, path, name, fit, outMsg);
    }

    return CreatePrimaryOrExtended(partBytes, path, name, type, fit, outMsg);
}

bool DiskManagement::CreatePrimaryOrExtended(int sizeBytes,
                                             const std::string& path,
                                             const std::string& name,
                                             char type,
                                             char fit,
                                             std::string& outMsg) {
    MBR mbr{};
    if (!ReadMBR(path, mbr, outMsg)) {
        return false;
    }

    if (partitionNameExists(mbr, name)) {
        outMsg = "Error: ya existe una particion con el nombre: " + name;
        return false;
    }

    if (countPrimaryExtended(mbr) >= 4) {
        outMsg = "Error: ya existen 4 particiones primarias/extendidas.";
        return false;
    }

    if (type == 'e' && findExtendedIndex(mbr) != -1) {
        outMsg = "Error: ya existe una particion extendida en el disco.";
        return false;
    }

    int slot = findFreePartitionSlot(mbr);
    if (slot == -1) {
        outMsg = "Error: no hay entradas libres en el MBR.";
        return false;
    }

    std::vector<Gap> gaps = getAvailableGaps(mbr);
    int gapIndex = chooseGapIndex(gaps, sizeBytes, fit);

    if (gapIndex == -1) {
        outMsg = "Error: no hay espacio suficiente para crear la particion.";
        return false;
    }

    Partition& p = mbr.mbr_partitions[slot];
    p.part_status = '1';
    p.part_type = type;
    p.part_fit = fit;
    p.part_start = gaps[gapIndex].start;
    p.part_size = sizeBytes;
    std::memset(p.part_name, 0, sizeof(p.part_name));
    std::strncpy(p.part_name, name.c_str(), sizeof(p.part_name) - 1);

    if (!WriteMBR(path, mbr, outMsg)) {
        return false;
    }

    if (type == 'e') {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            outMsg = "Error: no se pudo abrir el disco para inicializar el EBR.";
            return false;
        }

        EBR firstEbr{};
        firstEbr.part_mount = '0';
        firstEbr.part_fit   = fit;
        firstEbr.part_start = p.part_start;
        firstEbr.part_size  = 0;
        firstEbr.part_next  = -1;
        std::memset(firstEbr.part_name, 0, sizeof(firstEbr.part_name));

        file.seekp(p.part_start);
        file.write(reinterpret_cast<char*>(&firstEbr), sizeof(EBR));
        file.close();

        outMsg = "Particion extendida creada correctamente: " + name +
                 " | start=" + std::to_string(p.part_start) +
                 " | size=" + std::to_string(p.part_size) + " bytes";
        return true;
    }

    outMsg = "Particion primaria creada correctamente: " + name +
             " | start=" + std::to_string(p.part_start) +
             " | size=" + std::to_string(p.part_size) + " bytes";
    return true;
}

bool DiskManagement::CreateLogical(int sizeBytes,
                                   const std::string& path,
                                   const std::string& name,
                                   char fit,
                                   std::string& outMsg) {
    MBR mbr{};
    if (!ReadMBR(path, mbr, outMsg)) {
        return false;
    }

    int extIndex = findExtendedIndex(mbr);
    if (extIndex == -1) {
        outMsg = "Error: no existe una particion extendida en el disco.";
        return false;
    }

    const Partition& ext = mbr.mbr_partitions[extIndex];

    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo abrir el disco para crear la particion logica.";
        return false;
    }

    int extStart = ext.part_start;
    int extEnd   = ext.part_start + ext.part_size;

    EBR current{};
    file.seekg(extStart);
    file.read(reinterpret_cast<char*>(&current), sizeof(EBR));
    if (!file) {
        file.close();
        outMsg = "Error: no se pudo leer el EBR inicial.";
        return false;
    }

    if (current.part_size == 0 && current.part_next == -1 && std::string(current.part_name).empty()) {
        int required = (int)sizeof(EBR) + sizeBytes;
        if (extStart + required > extEnd) {
            file.close();
            outMsg = "Error: no hay espacio suficiente en la particion extendida.";
            return false;
        }

        current.part_mount = '1';
        current.part_fit   = fit;
        current.part_start = extStart;
        current.part_size  = sizeBytes;
        current.part_next  = -1;
        std::memset(current.part_name, 0, sizeof(current.part_name));
        std::strncpy(current.part_name, name.c_str(), sizeof(current.part_name) - 1);

        file.seekp(extStart);
        file.write(reinterpret_cast<char*>(&current), sizeof(EBR));
        file.close();

        outMsg = "Particion logica creada correctamente: " + name +
                 " | start=" + std::to_string(extStart + (int)sizeof(EBR)) +
                 " | size=" + std::to_string(sizeBytes) + " bytes";
        return true;
    }

    EBR prev = current;

    while (true) {
        if (std::string(prev.part_name) == name) {
            file.close();
            outMsg = "Error: ya existe una particion con el nombre: " + name;
            return false;
        }

        if (prev.part_next == -1) break;

        file.seekg(prev.part_next);
        file.read(reinterpret_cast<char*>(&prev), sizeof(EBR));
        if (!file) {
            file.close();
            outMsg = "Error: no se pudo leer la lista de EBR.";
            return false;
        }
    }

    int lastDataEnd = prev.part_start + (int)sizeof(EBR) + prev.part_size;
    int newEbrPos   = lastDataEnd;
    int required    = (int)sizeof(EBR) + sizeBytes;

    if (newEbrPos + required > extEnd) {
        file.close();
        outMsg = "Error: no hay espacio suficiente en la particion extendida.";
        return false;
    }

    prev.part_next = newEbrPos;
    file.seekp(prev.part_start);
    file.write(reinterpret_cast<char*>(&prev), sizeof(EBR));

    EBR newEbr{};
    newEbr.part_mount = '1';
    newEbr.part_fit   = fit;
    newEbr.part_start = newEbrPos;
    newEbr.part_size  = sizeBytes;
    newEbr.part_next  = -1;
    std::memset(newEbr.part_name, 0, sizeof(newEbr.part_name));
    std::strncpy(newEbr.part_name, name.c_str(), sizeof(newEbr.part_name) - 1);

    file.seekp(newEbrPos);
    file.write(reinterpret_cast<char*>(&newEbr), sizeof(EBR));
    file.close();

    outMsg = "Particion logica creada correctamente: " + name +
             " | start=" + std::to_string(newEbrPos + (int)sizeof(EBR)) +
             " | size=" + std::to_string(sizeBytes) + " bytes";
    return true;
}

bool DiskManagement::FindPartitionByName(const std::string& path,
                                         const std::string& name,
                                         Partition& outPartition,
                                         std::string& outMsg) {
    outMsg.clear();

    MBR mbr{};
    if (!ReadMBR(path, mbr, outMsg)) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        const Partition& p = mbr.mbr_partitions[i];
        if (p.part_start != -1 && p.part_size > 0) {
            if (std::string(p.part_name) == name) {
                outPartition = p;
                return true;
            }
        }
    }

    int extIndex = findExtendedIndex(mbr);
    if (extIndex != -1) {
        const Partition& ext = mbr.mbr_partitions[extIndex];

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            outMsg = "Error: no se pudo abrir el disco para buscar particiones logicas.";
            return false;
        }

        EBR ebr{};
        int pos = ext.part_start;

        while (pos != -1) {
            file.seekg(pos);
            file.read(reinterpret_cast<char*>(&ebr), sizeof(EBR));
            if (!file) break;

            if (ebr.part_size > 0 && std::string(ebr.part_name) == name) {
                outPartition.part_status = '1';
                outPartition.part_type   = 'l';
                outPartition.part_fit    = ebr.part_fit;
                outPartition.part_start  = ebr.part_start + (int)sizeof(EBR);
                outPartition.part_size   = ebr.part_size;
                std::memset(outPartition.part_name, 0, sizeof(outPartition.part_name));
                std::strncpy(outPartition.part_name, ebr.part_name, sizeof(outPartition.part_name) - 1);
                file.close();
                return true;
            }

            pos = ebr.part_next;
        }

        file.close();
    }

    outMsg = "Error: no se encontro la particion con nombre: " + name;
    return false;
}