#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "util.h"

typedef struct nhf_config_entry_t {
    char *key;
    char *val;
    struct nhf_config_entry_t *next;
} nhf_config_entry_t;

struct nhf_config_t {
    nhf_config_entry_t *head;
    nhf_config_entry_t *tail;
};

static void nhf_config_append(nhf_config_t *cfg, const char *key, const char *val) {
    nhf_config_entry_t *e = (nhf_config_entry_t*)calloc(1, sizeof(nhf_config_entry_t));
    if (!e || !(e->key = strdup(key)) || !(e->val = strdup(val))) {
        NHF_LOG("warning: out of memory while parsing config, skipping '%s'", key);
        if (e) {
            free(e->key);
            free(e->val);
            free(e);
        }
        return;
    }

    if (cfg->tail)
        cfg->tail->next = e;
    else
        cfg->head = e;
    cfg->tail = e;
}

static void nhf_config_write_default(void) {
    mkdir(NHF_CONFIG_DIR, 0755);

    FILE *src = fopen(NHF_CONFIG_DIR "/default", "r");
    if (!src) {
        NHF_LOG("warning: no default config template at %s/default (%s); leaving config absent", NHF_CONFIG_DIR_DISP, strerror(errno));
        return;
    }

    FILE *dst = fopen(NHF_CONFIG_DIR "/config", "w");
    if (!dst) {
        NHF_LOG("warning: could not write default config to %s/config (%s)", NHF_CONFIG_DIR_DISP, strerror(errno));
        fclose(src);
        return;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            NHF_LOG("warning: could not fully write default config to %s/config", NHF_CONFIG_DIR_DISP);
            break;
        }
    }

    fclose(src);
    fclose(dst);
    NHF_LOG("wrote default config to %s/config from template", NHF_CONFIG_DIR_DISP);
}

nhf_config_t *nhf_config_parse(void) {
    nhf_config_t *cfg = (nhf_config_t*)calloc(1, sizeof(nhf_config_t));
    if (!cfg)
        return NULL;

    FILE *f = fopen(NHF_CONFIG_DIR "/config", "r");
    if (!f && errno == ENOENT) {
        NHF_LOG("no config file at %s/config; writing a default one", NHF_CONFIG_DIR_DISP);
        nhf_config_write_default();
        f = fopen(NHF_CONFIG_DIR "/config", "r");
    }
    if (!f) {
        NHF_LOG("could not open %s/config (%s); using built-in defaults", NHF_CONFIG_DIR_DISP, strerror(errno));
        return cfg;
    }

    char *buf = NULL;
    size_t bufsz = 0;
    ssize_t len;
    int lineno = 0;
    while ((len = getline(&buf, &bufsz, f)) != -1) {
        (void)len;
        lineno++;

        char *hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';

        char *line = strtrim(buf);
        if (!*line)
            continue;

        char *cur = line;
        char *key = strsep(&cur, ":");
        key = strtrim(key);
        if (!key || !*key) {
            NHF_LOG("warning: %s/config: line %d: expected key, ignoring line", NHF_CONFIG_DIR_DISP, lineno);
            continue;
        }
        if (!cur) {
            NHF_LOG("warning: %s/config: line %d: expected ':' after key '%s', ignoring line", NHF_CONFIG_DIR_DISP, lineno, key);
            continue;
        }

        char *val = strtrim(cur);
        nhf_config_append(cfg, key, val);
        NHF_LOG("config: %s = %s", key, val);
    }

    free(buf);
    fclose(f);
    return cfg;
}

const char *nhf_config_get(nhf_config_t *cfg, const char *key) {
    if (!cfg)
        return NULL;
    for (nhf_config_entry_t *e = cfg->head; e; e = e->next)
        if (!strcmp(e->key, key))
            return e->val;
    return NULL;
}

bool nhf_config_bool(nhf_config_t *cfg, const char *key, bool default_value) {
    const char *val = nhf_config_get(cfg, key);
    if (!val || !*val)
        return default_value;
    if (!strcmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
        return true;
    if (!strcmp(val, "0") || !strcasecmp(val, "false") || !strcasecmp(val, "no") || !strcasecmp(val, "off"))
        return false;

    NHF_LOG("warning: invalid boolean for '%s': '%s'; using default %d", key, val, default_value ? 1 : 0);
    return default_value;
}

double nhf_config_double(nhf_config_t *cfg, const char *key, double default_value) {
    const char *val = nhf_config_get(cfg, key);
    if (!val || !*val)
        return default_value;

    char *end = NULL;
    errno = 0;
    double parsed = strtod(val, &end);
    bool trailing = false;
    for (const char *p = end; p && *p; p++)
        if (!isspace((unsigned char)*p)) { trailing = true; break; }
    if (errno || end == val || trailing) {
        NHF_LOG("warning: invalid number for '%s': '%s'; using default %.4f", key, val, default_value);
        return default_value;
    }
    return parsed;
}

void nhf_config_free(nhf_config_t *cfg) {
    if (!cfg)
        return;

    nhf_config_entry_t *e = cfg->head;
    while (e) {
        nhf_config_entry_t *next = e->next;
        free(e->key);
        free(e->val);
        free(e);
        e = next;
    }
    free(cfg);
}

static nhf_config_t *nhf_global_config(void) {
    static nhf_config_t *global = NULL;
    if (!global)
        global = nhf_config_parse();
    return global;
}

const char *nhf_global_config_get(const char *key) {
    return nhf_config_get(nhf_global_config(), key);
}

bool nhf_global_config_bool(const char *key, bool default_value) {
    return nhf_config_bool(nhf_global_config(), key, default_value);
}

double nhf_global_config_double(const char *key, double default_value) {
    return nhf_config_double(nhf_global_config(), key, default_value);
}
