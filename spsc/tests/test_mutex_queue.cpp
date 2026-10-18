#include <iostream>
#include <cassert>

#include "spsc/mutex_queue.hpp"

int main() {

    spsc::MutexQueue<int, 4> mq;

    bool ok;

    for(int i = 0; i < 4; i++) {
        ok = mq.try_push(10 * i);
        assert(ok);
    }

    ok = mq.try_push(50);
    assert(!ok);

    int val;

    for(int i = 0; i < 4; i++) {
        ok = mq.try_pop(val);
        assert(ok);
        assert(val == (i*10));
    }

    ok = mq.try_pop(val);
    assert(!ok);

    return 0;
}