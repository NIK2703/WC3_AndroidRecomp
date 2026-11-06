/*
 * pe_format.h - the 32-bit PE structures the WC3 host loader reads.
 * Packed little-endian definitions; no windows.h dependency.
 *
 * Derived from pcrecomp's runtime/win32hle/pe_format.h (same project family),
 * with the export directory added.
 */
#ifndef WC3_PE_FORMAT_H
#define WC3_PE_FORMAT_H
#include <stdint.h>

#pragma pack(push, 1)

typedef struct {
    uint16_t e_magic;
    uint16_t e_cblp[29];
    int32_t  e_lfanew;
} pe_dos_header;

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} pe_file_header;

typedef struct { uint32_t VirtualAddress, Size; } pe_data_dir;

typedef struct {
    uint16_t    Magic;
    uint8_t     MajorLinkerVersion, MinorLinkerVersion;
    uint32_t    SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    uint32_t    AddressOfEntryPoint, BaseOfCode, BaseOfData;
    uint32_t    ImageBase;
    uint32_t    SectionAlignment, FileAlignment;
    uint16_t    MajorOSVersion, MinorOSVersion, MajorImageVersion, MinorImageVersion;
    uint16_t    MajorSubsystemVersion, MinorSubsystemVersion;
    uint32_t    Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t    Subsystem, DllCharacteristics;
    uint32_t    SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t    LoaderFlags, NumberOfRvaAndSizes;
    pe_data_dir DataDirectory[16];
} pe_opt_header32;

typedef struct {
    uint32_t       Signature;
    pe_file_header FileHeader;
    pe_opt_header32 OptionalHeader;
} pe_nt_headers32;

typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations, PointerToLinenumbers;
    uint16_t NumberOfRelocations, NumberOfLinenumbers;
    uint32_t Characteristics;
} pe_section_header;

typedef struct {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp, ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} pe_import_descriptor;

typedef struct { uint32_t VirtualAddress, SizeOfBlock; } pe_base_reloc;

typedef struct {
    uint32_t Characteristics, TimeDateStamp;
    uint16_t MajorVersion, MinorVersion;
    uint32_t Name, Base, NumberOfFunctions, NumberOfNames;
    uint32_t AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} pe_export_dir;

#pragma pack(pop)

#define PE_DIR_EXPORT    0
#define PE_DIR_IMPORT    1
#define PE_DIR_RESOURCE  2
#define PE_DIR_BASERELOC 5
#define PE_REL_ABSOLUTE  0
#define PE_REL_HIGH      1
#define PE_REL_LOW       2
#define PE_REL_HIGHLOW   3
#define PE_REL_HIGHADJ   4
#define PE_ORDINAL_FLAG  0x80000000u

#endif
