#pragma once
#include <cstdint>

#pragma pack(push, 1)

struct Partition {
    char part_status;      // '0' libre / '1' ocupada (o segun tu logica)
    char part_type;        // 'p','e','l'
    char part_fit;         // 'b','f','w'
    int32_t part_start;    // byte inicio
    int32_t part_size;     // bytes
    char part_name[16];    // nombre
};

struct MBR {
    int32_t mbr_tamano;        // tamaño total del disco en bytes
    char    mbr_fecha_creacion[20]; // "YYYY-MM-DD HH:MM:SS"
    int32_t mbr_disk_signature; // random
    char    disk_fit;           // 'b','f','w'
    Partition mbr_partitions[4];
};

#pragma pack(pop)