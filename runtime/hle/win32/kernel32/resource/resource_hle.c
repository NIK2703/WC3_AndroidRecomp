/*
 * resource_hle.c - target-side KERNEL32 PE resource lookup.
 *
 * FindResourceA walks the mapped guest image's resource directory.  The
 * returned HRSRC is a small guest-side record owned by this module; LoadResource,
 * LockResource and SizeofResource all validate that record, so the group is
 * internally consistent without a legacy handle table.
 *
 * Resource names follow the Win32 rules: a value whose high 16 bits are zero
 * is an integer ID, and an ANSI string beginning with '#' is parsed as a
 * decimal integer ID.  Directory strings are UTF-16; the game's resources use
 * ASCII names, which are compared case-insensitively.
 */
#include "resource_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "guest_layout.h"
#include "legacy_shim.h"

#define RESOURCE_RECORD_MAGIC 0x43525352u /* 'RSRC' */
#define RESOURCE_RECORD_SIZE 16u
#define RESOURCE_NAME_MAX 255u
#define RESOURCE_ENTRY_LIMIT 4096u

typedef struct ResourceName {
    int is_id;
    uint32_t id;
    char text[RESOURCE_NAME_MAX + 1u];
} ResourceName;

static int read_u16(uint32_t address, uint16_t *out)
{
    if (!out || !wc3_legacy_guest_ok(address, 2u)) return 0;
    *out = wc3_legacy_rd16g(address);
    return 1;
}

static int read_u32(uint32_t address, uint32_t *out)
{
    if (!out || !wc3_legacy_guest_ok(address, 4u)) return 0;
    *out = wc3_legacy_rd32g(address);
    return 1;
}

static char ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
}

static int parse_decimal_id(const char *text, uint32_t *out)
{
    uint32_t value = 0;
    if (!text || !*text || !out) return 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        const uint32_t digit = (uint32_t)(*p - '0');
        if (value > (0xFFFFu - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    *out = value;
    return 1;
}

static int read_guest_string(uint32_t address, char *out, uint32_t capacity)
{
    if (!address || !out || capacity < 2u) return 0;
    for (uint32_t i = 0; i + 1u < capacity; i++) {
        if (address > UINT32_MAX - i) return 0;
        if (!wc3_legacy_guest_ok(address + i, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(address + i);
        out[i] = ch;
        if (!ch) return 1;
    }
    return 0;
}

static int parse_resource_name(uint32_t value, ResourceName *out)
{
    if (!out || value == 0) return 0;
    if (value <= 0xFFFFu) {
        out->is_id = 1;
        out->id = value;
        out->text[0] = '\0';
        return 1;
    }

    if (!read_guest_string(value, out->text, sizeof out->text)) return 0;
    if (out->text[0] == '#') {
        uint32_t id = 0;
        if (!parse_decimal_id(out->text + 1, &id)) return 0;
        out->is_id = 1;
        out->id = id;
        out->text[0] = '\0';
        return 1;
    }
    out->is_id = 0;
    out->id = 0;
    return 1;
}

static int pe_resource_root(uint32_t module, uint32_t *root_out,
                            uint32_t *size_out)
{
    if (!module) module = GUEST_IMAGE_WAR3;
    if (!root_out || !size_out) return 0;

    uint16_t dos_magic = 0;
    if (!read_u16(module, &dos_magic) || dos_magic != 0x5A4Du) return 0;

    uint32_t nt_offset = 0;
    if (!read_u32(module + 0x3Cu, &nt_offset) ||
        nt_offset > UINT32_MAX - module)
        return 0;
    if (nt_offset > UINT32_MAX - module - 24u) return 0;
    const uint32_t nt = module + nt_offset;
    uint32_t signature = 0;
    if (!read_u32(nt, &signature) || signature != 0x00004550u) return 0;

    const uint32_t optional = nt + 24u;
    uint16_t magic = 0;
    if (!read_u16(optional, &magic)) return 0;

    uint32_t directory_offset;
    if (magic == 0x10Bu)
        directory_offset = 96u + 2u * 8u; /* PE32 data directory */
    else if (magic == 0x20Bu)
        directory_offset = 112u + 2u * 8u; /* PE32+ data directory */
    else
        return 0;

    if (optional > UINT32_MAX - directory_offset - 8u) return 0;
    uint32_t resource_rva = 0;
    uint32_t resource_size = 0;
    if (!read_u32(optional + directory_offset, &resource_rva) ||
        !read_u32(optional + directory_offset + 4u, &resource_size) ||
        !resource_rva || !resource_size)
        return 0;
    if (resource_rva > UINT32_MAX - module) return 0;
    const uint32_t root = module + resource_rva;
    if (resource_size > UINT32_MAX - root ||
        !wc3_legacy_guest_ok(root, resource_size))
        return 0;

    *root_out = root;
    *size_out = resource_size;
    return 1;
}

static int resource_directory_header(uint32_t root, uint32_t size,
                                     uint32_t directory_offset,
                                     uint32_t *entry_offset,
                                     uint32_t *entry_count)
{
    if (directory_offset > size || size - directory_offset < 16u)
        return 0;
    const uint32_t directory = root + directory_offset;
    uint16_t named = 0;
    uint16_t ids = 0;
    if (!read_u16(directory + 12u, &named) ||
        !read_u16(directory + 14u, &ids))
        return 0;

    const uint32_t count = (uint32_t)named + (uint32_t)ids;
    if (count > RESOURCE_ENTRY_LIMIT) return 0;
    const uint32_t available = (size - directory_offset - 16u) / 8u;
    if (count > available) return 0;

    *entry_offset = directory + 16u;
    *entry_count = count;
    return 1;
}

static int resource_string_matches(uint32_t root, uint32_t size,
                                   uint32_t name_offset,
                                   const ResourceName *wanted)
{
    if (wanted->is_id) return 0;
    if (name_offset > size || size - name_offset < 2u) return 0;

    uint16_t length = 0;
    if (!read_u16(root + name_offset, &length)) return 0;
    if ((uint32_t)length > RESOURCE_NAME_MAX) return 0;
    const uint32_t bytes = (uint32_t)length * 2u;
    if (bytes > size - name_offset - 2u) return 0;
    if (!wc3_legacy_guest_ok(root + name_offset + 2u, bytes)) return 0;

    for (uint32_t i = 0; i < length; i++) {
        const uint16_t unit = wc3_legacy_rd16g(root + name_offset + 2u +
                                               i * 2u);
        if (unit > 0xFFu || wanted->text[i] == '\0' ||
            ascii_lower((char)unit) != ascii_lower(wanted->text[i]))
            return 0;
    }
    return wanted->text[length] == '\0';
}

static int resource_find_entry(uint32_t root, uint32_t size,
                               uint32_t directory_offset,
                               const ResourceName *wanted,
                               uint32_t *entry_value_out)
{
    uint32_t entry_offset = 0;
    uint32_t entry_count = 0;
    if (!resource_directory_header(root, size, directory_offset, &entry_offset,
                                   &entry_count))
        return 0;

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint32_t entry = entry_offset + i * 8u;
        uint32_t name = 0;
        uint32_t value = 0;
        if (!read_u32(entry, &name) || !read_u32(entry + 4u, &value))
            return 0;

        int matches;
        if (name & 0x80000000u)
            matches = resource_string_matches(root, size, name & 0x7FFFFFFFu,
                                              wanted);
        else
            matches = wanted->is_id && wanted->id == name;
        if (matches) {
            *entry_value_out = value;
            return 1;
        }
    }
    return 0;
}

static int resource_find_language(uint32_t root, uint32_t size,
                                  uint32_t directory_offset,
                                  uint32_t *entry_value_out)
{
    uint32_t entry_offset = 0;
    uint32_t entry_count = 0;
    if (!resource_directory_header(root, size, directory_offset, &entry_offset,
                                   &entry_count))
        return 0;

    uint32_t first_value = 0;
    int have_first = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint32_t entry = entry_offset + i * 8u;
        uint32_t language = 0;
        uint32_t value = 0;
        if (!read_u32(entry, &language) || !read_u32(entry + 4u, &value))
            return 0;
        if (language & 0x80000000u) continue;
        if (!have_first) {
            first_value = value;
            have_first = 1;
        }
        if (language == 0u) {
            *entry_value_out = value;
            return 1;
        }
    }
    if (!have_first) return 0;
    *entry_value_out = first_value;
    return 1;
}

static int resource_record_read(uint32_t record, uint32_t *module,
                                uint32_t *data, uint32_t *size)
{
    if (!record || !wc3_legacy_guest_ok(record, RESOURCE_RECORD_SIZE))
        return 0;
    if (wc3_legacy_rd32g(record) != RESOURCE_RECORD_MAGIC) return 0;
    if (module) *module = wc3_legacy_rd32g(record + 4u);
    if (data) *data = wc3_legacy_rd32g(record + 8u);
    if (size) *size = wc3_legacy_rd32g(record + 12u);
    return 1;
}

static void h_find_resource_a(CPU *c)
{
    uint32_t module = wc3_legacy_arg32(c, 0u);
    ResourceName name;
    ResourceName type;
    c->eax = 0;

    if (!parse_resource_name(wc3_legacy_arg32(c, 1u), &name) ||
        !parse_resource_name(wc3_legacy_arg32(c, 2u), &type))
        return;

    uint32_t root = 0;
    uint32_t size = 0;
    if (!pe_resource_root(module, &root, &size)) return;
    if (!module) module = GUEST_IMAGE_WAR3;

    uint32_t type_value = 0;
    if (!resource_find_entry(root, size, 0u, &type, &type_value) ||
        !(type_value & 0x80000000u))
        return;

    uint32_t name_value = 0;
    if (!resource_find_entry(root, size, type_value & 0x7FFFFFFFu, &name,
                             &name_value) ||
        !(name_value & 0x80000000u))
        return;

    uint32_t language_value = 0;
    if (!resource_find_language(root, size, name_value & 0x7FFFFFFFu,
                                &language_value) ||
        (language_value & 0x80000000u))
        return;

    const uint32_t data_entry_offset = language_value & 0x7FFFFFFFu;
    if (data_entry_offset > size || size - data_entry_offset < 16u) return;
    const uint32_t data_entry = root + data_entry_offset;
    uint32_t data_rva = 0;
    uint32_t data_size = 0;
    if (!read_u32(data_entry, &data_rva) ||
        !read_u32(data_entry + 4u, &data_size))
        return;
    if (data_rva > UINT32_MAX - module) return;
    const uint32_t data = module + data_rva;
    if (!wc3_legacy_guest_ok(data, data_size)) return;

    const uint32_t record = wc3_legacy_guest_alloc0(RESOURCE_RECORD_SIZE);
    if (!record) return;
    wc3_legacy_wr32g(record, RESOURCE_RECORD_MAGIC);
    wc3_legacy_wr32g(record + 4u, module);
    wc3_legacy_wr32g(record + 8u, data);
    wc3_legacy_wr32g(record + 12u, data_size);
    c->eax = record;
}

static void h_load_resource(CPU *c)
{
    const uint32_t record = wc3_legacy_arg32(c, 1u);
    c->eax = resource_record_read(record, NULL, NULL, NULL) ? record : 0u;
}

static void h_lock_resource(CPU *c)
{
    uint32_t data = 0;
    const uint32_t record = wc3_legacy_arg32(c, 0u);
    c->eax = resource_record_read(record, NULL, &data, NULL) ? data : 0u;
}

static void h_sizeof_resource(CPU *c)
{
    uint32_t size = 0;
    const uint32_t record = wc3_legacy_arg32(c, 1u);
    c->eax = resource_record_read(record, NULL, NULL, &size) ? size : 0u;
}

static void h_free_resource(CPU *c)
{
    const uint32_t record = wc3_legacy_arg32(c, 0u);
    c->eax = resource_record_read(record, NULL, NULL, NULL) ? 0u : 1u;
}

typedef struct ResourceAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} ResourceAddition;

static const ResourceAddition g_additions[] = {
    { "FindResourceA", 3u, h_find_resource_a },
    { "LoadResource", 2u, h_load_resource },
    { "LockResource", 1u, h_lock_resource },
    { "SizeofResource", 2u, h_sizeof_resource },
    { "FreeResource", 1u, h_free_resource },
};

int wc3_hle_kernel32_resource_install(void)
{
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("KERNEL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
