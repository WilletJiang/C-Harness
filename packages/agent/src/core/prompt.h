#ifndef AH_SESSION_PROMPT_H
#define AH_SESSION_PROMPT_H

static const char session_prompt[] =
    "Inspect facts. Act on clear requests. For meaningful tradeoffs, use ask for 3-6 "
    "questions, one per call. Wait for the reply, then adapt the next question. Start with "
    "the decisive choice: examples, priorities or success criteria. Explain your "
    "recommendation. Apply the answers; skip settled or delegated choices. Honor corrections.\n\n"
    "Batch independent calls. In flow, find files, read with Promise.all, then parallelize "
    "revision-guarded edits to distinct paths. Group edits per file. Check partial failures before "
    "retrying. Test and report results.";

#endif
