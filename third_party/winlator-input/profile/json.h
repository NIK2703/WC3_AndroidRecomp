#ifndef TP_JSON_H
#define TP_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TP_JSON_NULL,
    TP_JSON_BOOL,
    TP_JSON_NUMBER,
    TP_JSON_STRING,
    TP_JSON_ARRAY,
    TP_JSON_OBJECT
} TpJsonType;

typedef struct TpJsonValue TpJsonValue;

TpJsonValue* tp_json_parse(const char* json);
void tp_json_free(TpJsonValue* val);

TpJsonType tp_json_get_type(const TpJsonValue* val);
bool tp_json_is_null(const TpJsonValue* val);
bool tp_json_is_bool(const TpJsonValue* val);
bool tp_json_is_number(const TpJsonValue* val);
bool tp_json_is_string(const TpJsonValue* val);
bool tp_json_is_array(const TpJsonValue* val);
bool tp_json_is_object(const TpJsonValue* val);

bool tp_json_get_bool(const TpJsonValue* val);
double tp_json_get_number(const TpJsonValue* val);
int tp_json_get_int(const TpJsonValue* val);
const char* tp_json_get_string(const TpJsonValue* val);

int tp_json_arr_len(const TpJsonValue* arr);
TpJsonValue* tp_json_arr_get(const TpJsonValue* arr, int index);
int tp_json_get_array_count(const TpJsonValue* arr);
TpJsonValue* tp_json_get_array_item(const TpJsonValue* arr, int index);

TpJsonValue* tp_json_obj_get(const TpJsonValue* obj, const char* key);
TpJsonValue* tp_json_get_object_field(const TpJsonValue* obj, const char* key);
TpJsonValue* tp_json_get_nested(const TpJsonValue* val, const char* path);

const char* tp_json_get_string_def(const TpJsonValue* obj, const char* key, const char* def);
bool tp_json_get_bool_def(const TpJsonValue* obj, const char* key, bool def);
int tp_json_get_int_def(const TpJsonValue* obj, const char* key, int def);
double tp_json_get_num_def(const TpJsonValue* obj, const char* key, double def);
double tp_json_get_double_def(const TpJsonValue* obj, const char* key, double def);

char* tp_json_serialize(const TpJsonValue* val, int indent);

#ifdef __cplusplus
}
#endif

#endif
