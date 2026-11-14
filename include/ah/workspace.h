#ifndef AH_WORKSPACE_H
#define AH_WORKSPACE_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in, durable coordination for one pinned workspace per engine database.
 * New sessions, forks and children have no write authority. This coordinates
 * runtime callbacks, not external writers, processes or OS filesystem access. */
ah_code ah_workspace_enable(ah_engine *, const char *workspace, ah_error *);
/* Atomically replace a session's grants; count=0, broad=false releases them.
 * Paths are canonical workspace-relative files/directories ("." means root).
 * No overlapping ownership is allowed, including ancestors and descendants.
 * macOS reserves case/Unicode collisions using filesystem semantics. A missing
 * final filename must retain its granted spelling until it exists.
 * broad=true authorizes arbitrary effect callbacks/exec and requires exclusive
 * ownership across the engine. It is deliberately NOT a path sandbox.
 * Changes require an idle engine and no executing or unresolved effects. */
ah_code ah_workspace_assign(ah_engine *, const char *session, const char *const *paths,
                            size_t count, bool broad, ah_error *);
/* Move an exact grant between a parent and its direct child, atomically. A
 * broader parent grant must first be replaced with disjoint grants. Broad exec
 * authority is never delegated implicitly. Use the exact stored grant spelling. */
ah_code ah_workspace_transfer(ah_engine *, const char *from, const char *to, const char *path,
                              ah_error *);
ah_code ah_workspace_json(ah_engine *, char **json, ah_error *);

#ifdef __cplusplus
}
#endif
#endif
