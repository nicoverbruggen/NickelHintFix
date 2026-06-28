#ifndef NHF_CONFIG_H
#define NHF_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#if !(defined(NHF_CONFIG_DIR) && defined(NHF_CONFIG_DIR_DISP))
#error "NHF_CONFIG_DIR not set (it should be done by the Makefile)"
#endif

typedef struct nhf_config_t nhf_config_t;

nhf_config_t *nhf_config_parse(void);
const char *nhf_config_get(nhf_config_t *cfg, const char *key);
bool nhf_config_bool(nhf_config_t *cfg, const char *key, bool default_value);
double nhf_config_double(nhf_config_t *cfg, const char *key, double default_value);
void nhf_config_free(nhf_config_t *cfg);
const char *nhf_global_config_get(const char *key);
bool nhf_global_config_bool(const char *key, bool default_value);
double nhf_global_config_double(const char *key, double default_value);

#ifdef __cplusplus
}
#endif
#endif
