#include "tools/search.h"
#include <assert.h>

/* Drive a full hour with a virtual monotonic clock, including exact edges. */
int main(void) {
    search_quota quota = {0};
    uint64_t start = 1000;
    for (unsigned i = 0; i < SEARCH_HOURLY; ++i) {
        uint64_t t = start + 100 * i;
        assert(search_reserve(&quota, t) == 0);
        quota.active--;
        assert(search_reserve(&quota, t + 99) >= 1);
    }
    assert(search_reserve(&quota, start + 100000) == 3500000);
    assert(search_reserve(&quota, start + 3599999) == 1);
    assert(search_reserve(&quota, start + 3600000) == 0);
    assert(search_reserve(&quota, start + 3600100) == 0);
    quota.blocked = start + 7200000;
    assert(search_reserve(&quota, start + 3600200) == 3599800);
    return 0;
}
