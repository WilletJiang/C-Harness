#include "parallel_test.h"
#include <assert.h>
#include <stdio.h>

static void concurrent_order_and_errors(void) {
    fixture f = {.names = {"read", "read", "read"},
                 .count = 3,
                 .error_id = 2,
                 .barrier_count = 3,
                 .expected_batch = 3};
    f.partner[1] = 3;
    f.verify_timing = true;
    f.partner[2] = 3;
    f.partner[3] = 1;
    f.wait_finished[1] = 2;
    f.wait_finished[2] = 3;
    for (unsigned id = 1; id <= 3; id++)
        f.expected_starts[id] = 3;
    open_fixture(&f, 3, 16, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_OK);
    assert(f.max_active == 3 && f.executions == 3 && f.provider_calls == 2);
    assert(f.timed == 3);
    assert(f.finish_order[0] == 3 && f.finish_order[1] == 2 && f.finish_order[2] == 1);
    assert(f.receipts == 3 && !f.late_receipts);
    assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='done' AND is_error=1") == 1);
    assert_receipt_order(&f, 3);
    close_fixture(&f);
}

static void write_barrier(void) {
    fixture f = {.names = {"read", "read", "write", "read", "read"}, .count = 5};
    f.partner[1] = 2;
    f.partner[2] = 1;
    f.partner[4] = 5;
    f.partner[5] = 4;
    f.expected_starts[1] = f.expected_starts[2] = 2;
    f.expected_starts[4] = f.expected_starts[5] = 5;
    open_fixture(&f, 4, 16, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_OK);
    assert(f.max_active == 2 && f.executions == 5 && f.provider_calls == 2);
    assert_receipt_order(&f, 5);
    close_fixture(&f);
}

static void owner_yield(void *context) {
    fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    pthread_mutex_lock(&f->mutex);
    f->yielded = true;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->mutex);
}

static void yields_while_readers_wait(void) {
    fixture f = {.names = {"read", "read"}, .count = 2, .barrier_count = 2, .wait_yield = true};
    open_fixture(&f, 2, 16, 10000);
    f.engine->yield = owner_yield;
    f.engine->yield_context = &f;
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_OK);
    assert(f.yielded && f.executions == 2 && f.max_active == 2);
    assert_receipt_order(&f, 2);
    close_fixture(&f);
}

static void opt_out(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        fixture f = {.names = {"read", "read"}, .count = 2, .expect_owner = true};
        if (mode == 1)
            f.names[0] = f.names[1] = "serial";
        open_fixture(&f, mode == 2 ? 0 : (mode ? 4 : 1), 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.max_active == 1 && f.executions == 2);
        close_fixture(&f);
    }
}

static void invalid_and_unavailable_barriers(void) {
    for (unsigned invalid = 0; invalid < 2; invalid++) {
        fixture f = {.names = {"read", "read", "missing", "read", "read"}, .count = 5};
        if (invalid) {
            f.names[2] = "read";
            f.invalid_id = 3;
        }
        f.partner[1] = 2;
        f.partner[2] = 1;
        f.partner[4] = 5;
        f.partner[5] = 4;
        f.minimum_receipts[4] = f.minimum_receipts[5] = 3;
        open_fixture(&f, 4, 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.executions == 4 && !f.entered[3] && f.provider_calls == 2);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE is_error=1") == 1);
        assert_receipt_order(&f, 5);
        close_fixture(&f);
    }
}

static void cancellation_and_deadline(void) {
    for (unsigned deadline = 0; deadline < 2; deadline++) {
        fixture f = {.names = {"read", "read", "read"}, .count = 3};
        if (!deadline) {
            /* Cancellation must wait for every callback, not just workers 1/3. */
            f.barrier_count = 3;
            f.wait_finished[1] = f.wait_finished[2] = 3;
        }
        f.cancel_id = deadline ? 0 : 3;
        f.wait_deadline = deadline != 0;
        open_fixture(&f, 3, 16, deadline ? 50 : 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == (deadline ? AH_LIMIT : AH_CANCELLED));
        /* A deadline can expire before a scheduled worker enters its callback. */
        assert((deadline ? f.executions <= 3 : f.executions == 3));
        assert(f.provider_calls == 1 && f.receipts == 3);
        assert(f.late_receipts == 3);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='executing'") == 0);
        assert(scalar(&f, "SELECT state FROM sessions") == AH_STOPPED);
        assert_receipt_order(&f, 3);
        close_fixture(&f);
    }
}

static void cancelled_before_launch(void) {
    fixture f = {.names = {"read", "read"}, .count = 2, .cancel_before_launch = true};
    open_fixture(&f, 2, 16, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_CANCELLED);
    assert(!f.executions && f.provider_calls == 1 && f.receipts == 2 && f.late_receipts == 2);
    assert_receipt_order(&f, 2);
    close_fixture(&f);
}

static void budget_boundary(void) {
    fixture f = {.names = {"read", "read", "read"}, .count = 3};
    f.partner[1] = 2;
    f.partner[2] = 1;
    open_fixture(&f, 4, 2, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_LIMIT);
    assert(f.executions == 2 && !f.entered[3] && f.provider_calls == 1);
    assert(scalar(&f, "SELECT tool_calls FROM sessions") == 2);
    assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='pending'") == 1);
    assert_receipt_order(&f, 2);
    close_fixture(&f);
}

static void approval_and_input_barriers(void) {
    const char *barriers[] = {"approved", "harness_ask", "harness_wait"};
    for (unsigned kind = 0; kind < 3; kind++) {
        fixture f = {.names = {"read", "read", barriers[kind], "read"}, .count = 4};
        f.partner[1] = 2;
        f.partner[2] = 1;
        open_fixture(&f, 4, 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.executions == 2 && !f.entered[3] && !f.entered[4] && f.provider_calls == 1);
        assert(scalar(&f, "SELECT state FROM sessions") ==
               (kind ? AH_WAITING_INPUT : AH_WAITING_APPROVAL));
        close_fixture(&f);
    }
}

int main(int argc, char **argv) {
    int child = parallel_worker_child(argc, argv);
    if (child >= 0)
        return child;
    parallel_crash_cases(argv[0]);
    yields_while_readers_wait();
    concurrent_order_and_errors();
    write_barrier();
    opt_out();
    invalid_and_unavailable_barriers();
    cancellation_and_deadline();
    cancelled_before_launch();
    budget_boundary();
    approval_and_input_barriers();
    parallel_recovery_cases();
    parallel_worker_cases();
    puts("parallel tests passed");
    return 0;
}
