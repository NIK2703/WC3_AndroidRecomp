#ifndef PROFILE_BIND_H
#define PROFILE_BIND_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

const ProfileBind* profile_bind_from_name(const char* name);
void profile_bind_init_all(void);
const ProfileBind* profile_bind_parse(const char* name);
bool profile_bind_is_none(const ProfileBind* bind);
const char* profile_bind_display_name(const ProfileBind* bind);
int profile_bind_from_string(const char* name, ProfileBind* out_bind);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_BIND_H */
