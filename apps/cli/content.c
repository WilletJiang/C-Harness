#include "content.h"
#include "ah/content.h"
#include "internal.h"
#include <stdio.h>

cli_option_status cli_content_option(cli_content_options *options, int argc, char **argv, int *at) {
    const char *arg = argv[*at];
    if (!strcmp(arg, "--forget-tree")) {
        options->tree = true;
        return CLI_OPTION_HANDLED;
    }
    const char **out = !strcmp(arg, "--content")         ? &options->json
                       : !strcmp(arg, "--artifact-path") ? &options->path
                       : !strcmp(arg, "--mime")          ? &options->mime
                                                         : NULL;
    if (!out)
        return CLI_OPTION_UNHANDLED;
    if (*at + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", arg);
        return CLI_OPTION_INVALID;
    }
    *out = argv[++*at];
    return CLI_OPTION_HANDLED;
}
bool cli_content_command(const char *command) {
    return !strcmp(command, "submit-content") || !strcmp(command, "artifact-import") ||
           !strcmp(command, "artifacts") || !strcmp(command, "forget") ||
           !strcmp(command, "vacuum");
}
bool cli_content_options_valid(const cli_content_options *options, const char *command,
                               const char *prompt, const char *command_id) {
    const char *error = NULL;
    if (prompt && options->json)
        error = "--prompt and --content are mutually exclusive";
    else if (options->json && !command_id)
        error = "--content requires --command-id";
    else if (options->json && strcmp(command, "new") && strcmp(command, "run") &&
             strcmp(command, "submit-content"))
        error = "--content is supported by new, run, and submit-content";
    else if (!strcmp(command, "submit-content") && !options->json)
        error = "submit-content requires --content";
    else if ((!strcmp(command, "artifact-import") && (!options->path || !options->mime)) ||
             ((options->path || options->mime) && strcmp(command, "artifact-import")))
        error = "artifact-import requires --artifact-path and --mime together";
    else if (options->tree && strcmp(command, "forget"))
        error = "--forget-tree is supported by forget";
    else if (prompt && cli_content_command(command))
        error = "content maintenance commands do not accept --prompt";
    if (error)
        fprintf(stderr, "%s\n", error);
    return error == NULL;
}
static ah_code imported(ah_engine *e, const char *sid, const char *path, const char *mime,
                        char **json, ah_error *err) {
    char id[AH_ARTIFACT_ID_CAP];
    ah_code rc = ah_artifact_import(e, sid, path, mime, id, err);
    if (rc != AH_OK)
        return rc;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "artifact_id", id);
    *json = ah_mut_write(d);
    ah_mut_doc_free(d);
    return *json ? AH_OK : ah_fail(err, AH_IO, "out of memory");
}
ah_code cli_content_execute(ah_engine *e, const char *command, const char *sid,
                            const char *command_id, const cli_content_options *options, char **json,
                            ah_error *err) {
    if (!strcmp(command, "submit-content"))
        return ah_submit_content(e, sid, command_id, options->json, err);
    if (!strcmp(command, "artifact-import"))
        return imported(e, sid, options->path, options->mime, json, err);
    if (!strcmp(command, "artifacts"))
        return ah_artifacts_json(e, sid, json, err);
    ah_code rc = !strcmp(command, "forget")   ? ah_forget(e, sid, options->tree, err)
                 : !strcmp(command, "vacuum") ? ah_storage_vacuum(e, err)
                                              : AH_INVALID;
    if (rc == AH_OK) {
        *json =
            ah_strdup(!strcmp(command, "forget") ? "{\"forgotten\":true}" : "{\"vacuumed\":true}");
        if (!*json)
            rc = ah_fail(err, AH_IO, "out of memory");
    }
    return rc;
}
bool cli_content_rpc(ah_engine *e, const char *method, yyjson_val *obj, ah_code *rc, char **json,
                     ah_error *err) {
    const char *command = !strcmp(method, "submit_content")    ? "submit-content"
                          : !strcmp(method, "artifact_import") ? "artifact-import"
                          : !strcmp(method, "storage_vacuum")  ? "vacuum"
                                                               : method;
    if (!cli_content_command(command))
        return false;
    cli_content_options options = {.path = yyjson_get_str(yyjson_obj_get(obj, "path")),
                                   .mime = yyjson_get_str(yyjson_obj_get(obj, "mime"))};
    yyjson_val *tree = yyjson_obj_get(obj, "tree");
    if (tree && !yyjson_is_bool(tree)) {
        *rc = ah_fail(err, AH_INVALID, "tree must be boolean");
        return true;
    }
    options.tree = yyjson_get_bool(tree);
    yyjson_val *content = yyjson_obj_get(obj, "content");
    char *encoded = content ? ah_json_write(content) : NULL;
    options.json = encoded;
    const char *sid = yyjson_get_str(yyjson_obj_get(obj, "session"));
    const char *command_id = yyjson_get_str(yyjson_obj_get(obj, "command"));
    *rc = cli_content_execute(e, command, sid, command_id, &options, json, err);
    free(encoded);
    return true;
}
