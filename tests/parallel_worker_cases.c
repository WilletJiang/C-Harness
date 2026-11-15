#include "parallel_test.h"
#include <assert.h>

static void background_reads_capture_before_ordered_publication(void) {
    for (unsigned mixed = 0; mixed < 2; mixed++) {
        fixture f = {.names = {"background", "background", "read", "background"},
                     .count = 4,
                     .verify_capture = true};
        if (mixed)
            f.names[1] = "read";
        f.partner[1] = 2;
        f.wait_captured[1] = 2;
        f.partner[3] = 4;
        open_fixture(&f, 2, 4, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.max_active == 2 && f.executions == 4 && f.provider_calls == 2);
        assert(f.captured[2] && f.finish_order[0] == 2);
        assert(!core_jobs_running(f.engine, NULL));
        assert_receipt_order(&f, 4);
        close_fixture(&f);
    }
}

static void one_read_stays_inline(void) {
    fixture f = {.names = {"read"}, .count = 1, .expect_owner = true};
    open_fixture(&f, 4, 4, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_OK);
    assert(f.max_active == 1 && f.executions == 1 && !f.captured[1]);
    assert_receipt_order(&f, 1);
    close_fixture(&f);
}

static void exhausted_task_budget_drains_dispatched_reads(void) {
    fixture f = {.names = {"background", "read", "background"}, .count = 3};
    f.partner[1] = 2;
    f.wait_captured[1] = 2;
    open_fixture(&f, 3, 8, 10000);
    char sid[AH_ID_CAP];
    ah_error error = {0};
    ok(ah_session_create(f.engine, "mock", "", sid, &error), &error);
    const ah_task_config task = {
        "{}", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 4, 2, 0, 60000};
    ok(ah_task_create(f.engine, sid, &task, &error), &error);
    ok(ah_submit(f.engine, sid, "command", "run tools", &error), &error);
    assert(ah_run(f.engine, sid, &error) == AH_LIMIT);
    assert(f.executions == 2 && !f.entered[3] && f.receipts == 2 && !f.late_receipts);
    assert(scalar(&f, "SELECT tool_used FROM tasks") == 2);
    assert_receipt_order(&f, 2);
    close_fixture(&f);
}

void parallel_worker_cases(void) {
    background_reads_capture_before_ordered_publication();
    one_read_stays_inline();
    exhausted_task_budget_drains_dispatched_reads();
}
