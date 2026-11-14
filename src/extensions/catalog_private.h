#ifndef AH_CATALOG_PRIVATE_H
#define AH_CATALOG_PRIVATE_H
#include "catalog.h"
#include "tomlc17.h"
#include <limits.h>

#define EXT_METADATA_MAX (256u * 1024u)
#define EXT_FRONTMATTER_MAX (16u * 1024u)
#define EXT_DEPTH_MAX 12u

typedef struct {
    char *id, *kind, *name, *description, *path, *plugin, *reason;
    char *config;
    bool enabled, available, expanded;
    ah_code expansion_failure;
} ext_entry;

struct ext_catalog {
    ah_extensions_config options;
    char *home, *workspace, *user_home;
    ext_entry *entries;
    size_t count, bytes, retained, steps;
    bool config_loaded, skills_loaded, servers_loaded, plugins_loaded;
    toml_result_t config[2];
    ah_error failure;
    char **visited;
    size_t visited_count;
};

static inline size_t ext_remaining(const ext_catalog *c) {
    return c->options.max_catalog_bytes - c->bytes - c->retained;
}

char *ext_join(const char *, const char *);
char *ext_id(const char *, const char *);
bool ext_inside(const char *, const char *);
ah_code ext_read(ext_catalog *, const char *, size_t, char **, ah_error *);
ah_code ext_json_file(ext_catalog *, const char *, yyjson_doc **, ah_error *);
ah_code ext_children(ext_catalog *, const char *, char ***, size_t *, ah_error *);
void ext_names_free(char **, size_t);
ah_code ext_add(ext_catalog *, const ext_entry *, ah_error *);
ext_entry *ext_find(ext_catalog *, const char *);
ah_code ext_config_load(ext_catalog *, ah_error *);
bool ext_disabled(ext_catalog *, const char *, const char *, const char *);
ah_code ext_skills_load(ext_catalog *, ah_error *);
ah_code ext_skills_walk(ext_catalog *, const char *, const char *, const char *, unsigned,
                        ah_error *);
ah_code ext_servers_load(ext_catalog *, ah_error *);
ah_code ext_server_add(ext_catalog *, const char *, const char *, yyjson_val *, const char *,
                       ah_error *);
ah_code ext_plugins_load(ext_catalog *, ah_error *);
ah_code ext_plugin_expand(ext_catalog *, const char *, ah_error *);
char *ext_expand(const char *, const char *);
yyjson_mut_val *ext_toml_json(yyjson_mut_doc *, toml_datum_t, unsigned);
#endif
