#include "policies.h"
#include "ah/workspace.h"
#include "internal.h"
#include <limits.h>
#include <stdio.h>

void cli_policy_init(cli_policy_options *options) {
    memset(options, 0, sizeof(*options));
    ah_context_config_init(&options->context);
}
cli_option_status cli_policy_option(cli_policy_options *options, int argc, char **argv, int *at) {
    const char *arg = argv[*at];
    if (!strcmp(arg, "--auto-context")) {
        options->automatic_context = true;
        return CLI_OPTION_HANDLED;
    }
    if (!strcmp(arg, "--coordinate-writes")) {
        options->coordinate_writes = true;
        return CLI_OPTION_HANDLED;
    }
    size_t *bytes = !strcmp(arg, "--context-bytes")     ? &options->context_bytes
                    : !strcmp(arg, "--context-trigger") ? &options->context.trigger_bytes
                    : !strcmp(arg, "--context-target")  ? &options->context.target_bytes
                    : !strcmp(arg, "--context-reserve") ? &options->context.reserve_bytes
                                                        : NULL;
    bool keep = !strcmp(arg, "--keep-exchanges"), tokens = !strcmp(arg, "--compact-tokens");
    if (!bytes && !keep && !tokens)
        return CLI_OPTION_UNHANDLED;
    if (*at + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", arg);
        return CLI_OPTION_INVALID;
    }
    uintmax_t value = 0, maximum = bytes ? 64u * 1024u * 1024u : keep ? 64u : UINT_MAX;
    uintmax_t minimum = bytes == &options->context_bytes ? 4096 : tokens ? 1 : 0;
    if (!cli_option_number(arg, argv[++*at], minimum, maximum, &value))
        return CLI_OPTION_INVALID;
    if (bytes)
        *bytes = (size_t)value;
    else if (keep)
        options->context.keep_recent_exchanges = (unsigned)value;
    else
        options->context.compact_threshold_tokens = (unsigned)value;
    if (bytes != &options->context_bytes)
        options->automatic_context = true;
    return CLI_OPTION_HANDLED;
}
ah_code cli_policy_enable(ah_engine *e, const cli_policy_options *options, const char *workspace,
                          ah_error *err) {
    ah_code rc = options->automatic_context ? ah_context_enable(e, &options->context, err) : AH_OK;
    if (rc == AH_OK && options->coordinate_writes)
        rc = ah_workspace_enable(e, workspace, err);
    return rc;
}
bool cli_policy_command(const char *command) {
    return !strcmp(command, "context") || !strcmp(command, "workspace");
}
ah_code cli_policy_execute(ah_engine *e, const char *command, const char *sid, char **json,
                           ah_error *err) {
    return !strcmp(command, "context")     ? ah_context_json(e, sid, json, err)
           : !strcmp(command, "workspace") ? ah_workspace_json(e, json, err)
                                           : AH_INVALID;
}
static ah_code assign(ah_engine *e, yyjson_val *obj, ah_error *err) {
    yyjson_val *values = yyjson_obj_get(obj, "paths"), *broad = yyjson_obj_get(obj, "broad");
    if (!yyjson_is_arr(values) || yyjson_arr_size(values) > 64 || (broad && !yyjson_is_bool(broad)))
        return ah_fail(err, AH_INVALID,
                       "paths must be an array of at most 64 strings; broad must be boolean");
    const char *paths[64];
    size_t i, count;
    yyjson_val *value;
    yyjson_arr_foreach(values, i, count, value) {
        paths[i] = yyjson_get_str(value);
        if (!paths[i])
            return ah_fail(err, AH_INVALID, "write paths must be strings");
    }
    const char *sid = yyjson_get_str(yyjson_obj_get(obj, "session"));
    return ah_workspace_assign(e, sid, paths, yyjson_arr_size(values), yyjson_get_bool(broad), err);
}
bool cli_policy_rpc(ah_engine *e, const char *method, yyjson_val *obj, ah_code *rc, char **json,
                    ah_error *err) {
    const char *sid = yyjson_get_str(yyjson_obj_get(obj, "session"));
    if (cli_policy_command(method))
        *rc = cli_policy_execute(e, method, sid, json, err);
    else if (!strcmp(method, "workspace_assign"))
        *rc = assign(e, obj, err);
    else if (!strcmp(method, "workspace_transfer"))
        *rc = ah_workspace_transfer(e, yyjson_get_str(yyjson_obj_get(obj, "from")),
                                    yyjson_get_str(yyjson_obj_get(obj, "to")),
                                    yyjson_get_str(yyjson_obj_get(obj, "path")), err);
    else
        return false;
    if (*rc == AH_OK && !*json)
        *rc = ah_workspace_json(e, json, err);
    return true;
}
