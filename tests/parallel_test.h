#ifndef AH_PARALLEL_TEST_H
#define AH_PARALLEL_TEST_H
#include "core/core.h"

#define CALLS 8

typedef struct {
    ah_engine *engine;
    pthread_t owner;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    const char *database_path;
    const char *names[CALLS];
    unsigned count, provider_calls, active, max_active, executions;
    atomic_uint started, receipts;
    unsigned partner[CALLS + 1], wait_finished[CALLS + 1], expected_starts[CALLS + 1];
    unsigned minimum_receipts[CALLS + 1];
    unsigned finish_order[CALLS], finished_count, late_receipts;
    unsigned cancel_id, error_id, barrier_count, expected_batch, invalid_id;
    bool cancel_before_launch, verify_capture, crash_on_capture;
    unsigned wait_captured[CALLS + 1];
    bool captured[CALLS + 1];
    char operations[CALLS + 1][AH_ID_CAP];
    bool entered[CALLS + 1], finished[CALLS + 1], unknown[CALLS + 1], timed_out;
    bool expect_owner, wait_deadline, wait_yield, yielded;
    bool verify_timing;
    unsigned timed;
    uint64_t began[CALLS + 1], ended[CALLS + 1];
    bool interrupt_intent, interrupted;
    const char *interrupt_event;
    char *last_request;
} fixture;

void ok(ah_code, const ah_error *);
void open_fixture(fixture *, unsigned, unsigned, uint64_t);
void close_fixture(fixture *);
void reopen_fixture(fixture *);
ah_code run_fixture(fixture *, char[AH_ID_CAP]);
int scalar(fixture *, const char *);
void assert_receipt_order(fixture *, unsigned);
void parallel_recovery_cases(void);
void parallel_worker_cases(void);
int parallel_worker_child(int, char **);
void parallel_crash_cases(const char *);
#endif
