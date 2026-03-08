#include "disk/DiskManagement.h"
#include "disk/Structs.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <ctime>
#include <random>

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
