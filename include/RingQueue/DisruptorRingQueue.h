
#ifndef _JIMI_DISRUPTOR_RINGQUEUE_H_
#define _JIMI_DISRUPTOR_RINGQUEUE_H_

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

#include "vs_stdint.h"
#include "port.h"
#include "sleep.h"

#include <atomic>

#ifndef _MSC_VER
#include <pthread.h>
#include "msvc/pthread.h"
#else
#include "msvc/pthread.h"
#endif  // !_MSC_VER

#ifdef _MSC_VER
#include <intrin.h>     // For _ReadWriteBarrier(), InterlockedCompareExchange()
#endif  // _MSC_VER
#include <emmintrin.h>

#include "Sequence.h"

#include <stdio.h>
#include <string.h>

#include "dump_mem.h"

namespace jimi {

#if 0
struct DisruptorRingQueueHead
{
    volatile uint32_t head;
    volatile uint32_t tail;
};
#else
struct DisruptorRingQueueHead
{
    volatile uint32_t head;
    char padding1[JIMI_CACHE_LINE_SIZE - sizeof(uint32_t)];

    volatile uint32_t tail;
    char padding2[JIMI_CACHE_LINE_SIZE - sizeof(uint32_t)];
};
#endif

typedef struct DisruptorRingQueueHead DisruptorRingQueueHead;

///////////////////////////////////////////////////////////////////
// class SmallDisruptorRingQueueCore<Capacity>
///////////////////////////////////////////////////////////////////

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
class SmallDisruptorRingQueueCore
{
public:
    typedef uint32_t    size_type;
    typedef uint32_t    flag_type;
    typedef T           item_type;

public:
    static const size_type  kCapacityCore   = (size_type)JIMI_MAX(JIMI_ROUND_TO_POW2(Capacity), 2);
    static const size_type  kProducers      = Producers;
    static const size_type  kConsumers      = Consumers;
    static const size_type  kConsumersAlloc = (Consumers <= 1) ? 1 : ((Consumers + 1) & ((size_type)(~1U)));
    static const bool       kIsAllocOnHeap  = false;

public:
    DisruptorRingQueueHead  info;

    Sequence                cursor, next;
    Sequence                gatingSequences[kConsumersAlloc];

    volatile item_type      entries[kCapacityCore];
    volatile flag_type      availableBuffer[kCapacityCore];
};

///////////////////////////////////////////////////////////////////
// class DisruptorRingQueueCore<Capacity>
///////////////////////////////////////////////////////////////////

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
class DisruptorRingQueueCore
{
public:
    typedef uint32_t    size_type;
    typedef uint32_t    flag_type;
    typedef T           item_type;

public:
    static const size_type  kCapacityCore   = (size_type)JIMI_MAX(JIMI_ROUND_TO_POW2(Capacity), 2);
    static const size_type  kProducers      = Producers;
    static const size_type  kConsumers      = Consumers;
    static const size_type  kConsumersAlloc = (Consumers <= 1) ? 1 : ((Consumers + 1) & ((size_type)(~1U)));
    static const bool       kIsAllocOnHeap  = true;

public:
    DisruptorRingQueueHead  info;

    Sequence                cursor, next;
    Sequence                gatingSequences[kConsumersAlloc];

    volatile item_type *    entries;
    volatile flag_type *    availableBuffer;
};

///////////////////////////////////////////////////////////////////
// class DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>
///////////////////////////////////////////////////////////////////

template <typename T, uint32_t Capacity = 1024U,
          uint32_t Producers = 0, uint32_t Consumers = 0,
          typename CoreTy = DisruptorRingQueueCore<T, Capacity, Producers, Consumers> >
class DisruptorRingQueueBase
{
public:
    typedef uint32_t                    size_type;
    typedef uint32_t                    index_type;
    typedef CoreTy                      core_type;
    typedef typename CoreTy::item_type  item_type;
    typedef typename CoreTy::flag_type  flag_type;
    typedef T *                         value_type;
    typedef T *                         pointer;
    typedef const T *                   const_pointer;
    typedef T &                         reference;
    typedef const T &                   const_reference;

public:
    static const size_type  kCapacity       = CoreTy::kCapacityCore;
    static const index_type kMask           = (index_type)(kCapacity - 1);

    static const size_type  kProducers      = CoreTy::kProducers;
    static const size_type  kConsumers      = CoreTy::kConsumers;
    static const size_type  kConsumersAlloc = CoreTy::kConsumersAlloc;

public:
    DisruptorRingQueueBase(bool bInitHead = false);
    ~DisruptorRingQueueBase();

public:
    void dump();
    void dump_core();
    void dump_info();
    void dump_detail();

    index_type mask() const       { return kMask;     };
    size_type  capacity() const   { return kCapacity; };
    size_type  length() const     { return sizes();   };
    size_type  sizes() const;

    void init(bool bInitHead = false);

    int push(T & entry);
    int pop (T & entry);

    int spin_push(T & entry);
    int spin_pop (T & entry);

    int mutex_push(T & entry);
    int mutex_pop (T & entry);

protected:
    core_type       core;

    spin_mutex_t    spin_mutex;
    pthread_mutex_t queue_mutex;
};

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::DisruptorRingQueueBase(bool bInitHead /* = false */)
{
    init(bInitHead);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::~DisruptorRingQueueBase()
{
    // Do nothing!
    Jimi_ReadWriteBarrier();

    spin_mutex.locked = 0;

    pthread_mutex_destroy(&queue_mutex);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
void DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::init(bool bInitHead /* = false */)
{
    if (!bInitHead) {
        core.info.head = 0;
        core.info.tail = 0;
    }
    else {
        memset((void *)&core.info, 0, sizeof(core.info));
    }

    core.cursor.set(0x1234);
    core.next.set(0x5678);

#if defined(_DEBUG) || !defined(NDEBUG)
    printf("CoreTy::kConsumersAlloc = %d\n", kConsumersAlloc);
#endif

    for (int i = 0; i < kConsumersAlloc; ++i) {
        core.gatingSequences[i].set(0x00111111U * (i + 1));
    }

    Jimi_ReadWriteBarrier();

    // Initilized spin mutex
    spin_mutex.locked = 0;
    spin_mutex.spin_counter = MUTEX_MAX_SPIN_COUNT;
    spin_mutex.recurse_counter = 0;
    spin_mutex.thread_id = 0;
    spin_mutex.reserve = 0;

    // Initilized mutex
    pthread_mutex_init(&queue_mutex, NULL);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
void DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::dump()
{
    //ReleaseUtils::dump(&core, sizeof(core));
    memory_dump(this, sizeof(*this), false, 16, 0, 0);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
void DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::dump_core()
{
    //ReleaseUtils::dump(&core, sizeof(core));
    memory_dump(&core, sizeof(core), false, 16, 0, 0);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
void DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::dump_info()
{
    //ReleaseUtils::dump(&core.info, sizeof(core.info));
    memory_dump(&core.info, sizeof(core.info), false, 16, 0, 0);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
void DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::dump_detail()
{
#if 0
    printf("---------------------------------------------------------\n");
    printf("DisruptorRingQueueBase.p.head = %u\nDisruptorRingQueueBase.p.tail = %u\n\n", core.info.p.head, core.info.p.tail);
    printf("DisruptorRingQueueBase.c.head = %u\nDisruptorRingQueueBase.c.tail = %u\n",   core.info.c.head, core.info.c.tail);
    printf("---------------------------------------------------------\n\n");
#else
    printf("DisruptorRingQueueBase: (head = %u, tail = %u)\n",
           core.info.head, core.info.tail);
#endif
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
typename DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::size_type
DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::sizes() const
{
    index_type head, tail;

    Jimi_ReadWriteBarrier();

    head = core.info.head;

    tail = core.info.tail;

    return (size_type)((head - tail) <= kMask) ? (head - tail) : (size_type)-1;
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
int DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::push(T & entry)
{
    index_type head, tail, next;
    bool ok = false;

    Jimi_ReadWriteBarrier();

    do {
        head = core.info.head;
        tail = core.info.tail;
        if ((head - tail) > kMask)
            return -1;
        next = head + 1;
        ok = jimi_bool_compare_and_swap32(&core.info.head, head, next);
    } while (!ok);

    core.entries[head & kMask].value = entry.value;

    Jimi_ReadWriteBarrier();

    return 0;
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
int DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::pop(T & entry)
{
    index_type head, tail, next;
    bool ok = false;

    Jimi_ReadWriteBarrier();

    do {
        head = core.info.head;
        tail = core.info.tail;
        if ((tail == head) || (tail > head && (head - tail) > kMask))
            return -1;
        next = tail + 1;
        ok = jimi_bool_compare_and_swap32(&core.info.tail, tail, next);
    } while (!ok);

    entry.value = core.entries[tail & kMask].value;

    Jimi_ReadWriteBarrier();

    return 0;
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
int DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::spin_push(T & entry)
{
    index_type head, tail, next;
    int32_t pause_cnt;
    uint32_t loop_count, yield_cnt, spin_count;
    static const uint32_t YIELD_THRESHOLD = SPIN_YIELD_THRESHOLD;

    Jimi_ReadWriteBarrier();

    /* atomic_exchange usually takes less instructions than
       atomic_compare_and_exchange.  On the other hand,
       atomic_compare_and_exchange potentially generates less bus traffic
       when the lock is locked.
       We assume that the first try mostly will be successful, and we use
       atomic_exchange.  For the subsequent tries we use
       atomic_compare_and_exchange.  */
    if (jimi_lock_test_and_set32(&spin_mutex.locked, 1U) != 0U) {
        loop_count = 0;
        spin_count = 1;
        do {
            if (loop_count < YIELD_THRESHOLD) {
                for (pause_cnt = spin_count; pause_cnt > 0; --pause_cnt) {
                    jimi_mm_pause();
                }
                spin_count *= 2;
            }
            else {
                yield_cnt = loop_count - YIELD_THRESHOLD;
#if defined(__MINGW32__) || defined(__CYGWIN__)
                if ((yield_cnt & 3) == 3) {
                    jimi_wsleep(0);
                }
                else {
                    if (!jimi_yield()) {
                        jimi_wsleep(0);
                        //jimi_mm_pause();
                    }
                }
#else
                if ((yield_cnt & 63) == 63) {
                    jimi_wsleep(1);
                }
                else if ((yield_cnt & 3) == 3) {
                    jimi_wsleep(0);
                }
                else {
                    if (!jimi_yield()) {
                        jimi_wsleep(0);
                        //jimi_mm_pause();
                    }
                }
#endif
            }
            loop_count++;
            //jimi_mm_pause();
        } while (jimi_val_compare_and_swap32(&spin_mutex.locked, 0U, 1U) != 0U);
    }

    head = core.info.head;
    tail = core.info.tail;
    if ((head - tail) > kMask) {
        Jimi_ReadWriteBarrier();
        spin_mutex.locked = 0;
        return -1;
    }
    next = head + 1;
    core.info.head = next;

    core.entries[head & kMask].value = entry.value;

    Jimi_ReadWriteBarrier();

    spin_mutex.locked = 0;

    return 0;
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
int DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::spin_pop(T & entry)
{
    index_type head, tail, next;
    value_type item;
    int32_t pause_cnt;
    uint32_t loop_count, yield_cnt, spin_count;
    static const uint32_t YIELD_THRESHOLD = SPIN_YIELD_THRESHOLD;

    Jimi_ReadWriteBarrier();

    /* atomic_exchange usually takes less instructions than
       atomic_compare_and_exchange.  On the other hand,
       atomic_compare_and_exchange potentially generates less bus traffic
       when the lock is locked.
       We assume that the first try mostly will be successful, and we use
       atomic_exchange.  For the subsequent tries we use
       atomic_compare_and_exchange.  */
    if (jimi_lock_test_and_set32(&spin_mutex.locked, 1U) != 0U) {
        loop_count = 0;
        spin_count = 1;
        do {
            if (loop_count < YIELD_THRESHOLD) {
                for (pause_cnt = spin_count; pause_cnt > 0; --pause_cnt) {
                    jimi_mm_pause();
                }
                spin_count *= 2;
            }
            else {
                yield_cnt = loop_count - YIELD_THRESHOLD;
#if defined(__MINGW32__) || defined(__CYGWIN__)
                if ((yield_cnt & 3) == 3) {
                    jimi_wsleep(0);
                }
                else {
                    if (!jimi_yield()) {
                        jimi_wsleep(0);
                        //jimi_mm_pause();
                    }
                }
#else
                if ((yield_cnt & 63) == 63) {
                    jimi_wsleep(1);
                }
                else if ((yield_cnt & 3) == 3) {
                    jimi_wsleep(0);
                }
                else {
                    if (!jimi_yield()) {
                        jimi_wsleep(0);
                        //jimi_mm_pause();
                    }
                }
#endif
            }
            loop_count++;
            //jimi_mm_pause();
        } while (jimi_val_compare_and_swap32(&spin_mutex.locked, 0U, 1U) != 0U);
    }

    head = core.info.head;
    tail = core.info.tail;
    if ((tail == head) || (tail > head && (head - tail) > kMask)) {
        Jimi_ReadWriteBarrier();
        spin_mutex.locked = 0;
        return -1;
    }
    next = tail + 1;
    core.info.tail = next;

    entry.value = core.entries[tail & kMask].value;

    Jimi_ReadWriteBarrier();

    spin_mutex.locked = 0;

    return 0;
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
int DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::mutex_push(T & entry)
{
    index_type head, tail, next;

    Jimi_ReadWriteBarrier();

    pthread_mutex_lock(&queue_mutex);

    head = core.info.head;
    tail = core.info.tail;
    if ((head - tail) > kMask) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    }
    next = head + 1;
    core.info.head = next;

    core.entries[head & kMask].value = entry.value;

    Jimi_ReadWriteBarrier();

    pthread_mutex_unlock(&queue_mutex);

    return 0;
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers, typename CoreTy>
inline
int DisruptorRingQueueBase<T, Capacity, Producers, Consumers, CoreTy>::mutex_pop(T & entry)
{
    index_type head, tail, next;

    Jimi_ReadWriteBarrier();

    pthread_mutex_lock(&queue_mutex);

    head = core.info.head;
    tail = core.info.tail;
    //if (tail >= head && (head - tail) <= kMask)
    if ((tail == head) || (tail > head && (head - tail) > kMask)) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    }
    next = tail + 1;
    core.info.tail = next;

    entry.value = core.entries[tail & kMask].value;

    Jimi_ReadWriteBarrier();

    pthread_mutex_unlock(&queue_mutex);

    return 0;
}

///////////////////////////////////////////////////////////////////
// class SmallRingQueue<T, Capacity>
///////////////////////////////////////////////////////////////////

template <typename T, uint32_t Capacity = 1024U,
          uint32_t Producers = 0, uint32_t Consumers = 0>
class SmallDisruptorRingQueue : public DisruptorRingQueueBase<T, Capacity, Producers, Consumers,
                                         SmallDisruptorRingQueueCore<T, Capacity, Producers, Consumers> >
{
public:
    typedef uint32_t                    size_type;
    typedef uint32_t                    index_type;
    typedef T                           item_type;
    typedef T *                         value_type;
    typedef T *                         pointer;
    typedef const T *                   const_pointer;
    typedef T &                         reference;
    typedef const T &                   const_reference;

    static const size_type kCapacity = DisruptorRingQueueBase<T, Capacity, Producers, Consumers,
                                         SmallDisruptorRingQueueCore<T, Capacity, Producers, Consumers> >::kCapacity;

public:
    SmallDisruptorRingQueue(bool bFillQueue = true, bool bInitHead = false);
    ~SmallDisruptorRingQueue();

public:
    void dump();
    void dump_detail();

protected:
    void init_queue(bool bFillQueue = true);
};

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
SmallDisruptorRingQueue<T, Capacity, Producers, Consumers>::SmallDisruptorRingQueue(bool bFillQueue /* = true */,
                                                                                    bool bInitHead  /* = false */)
: DisruptorRingQueueBase<T, Capacity, Producers, Consumers,
    SmallDisruptorRingQueueCore<T, Capacity, Producers, Consumers> >(bInitHead)
{
    init_queue(bFillQueue);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
SmallDisruptorRingQueue<T, Capacity, Producers, Consumers>::~SmallDisruptorRingQueue()
{
    // Do nothing!
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
inline
void SmallDisruptorRingQueue<T, Capacity, Producers, Consumers>::init_queue(bool bFillQueue /* = true */)
{
    if (bFillQueue) {
        memset((void *)this->core.entries, 0, sizeof(item_type) * kCapacity);
    }
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
void SmallDisruptorRingQueue<T, Capacity, Producers, Consumers>::dump()
{
    memory_dump(&core, sizeof(core), false, 16, 0, 0);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
void SmallDisruptorRingQueue<T, Capacity, Producers, Consumers>::dump_detail()
{
    printf("SmallRingQueue: (head = %u, tail = %u)\n",
           this->core.info.head, this->core.info.tail);
}

///////////////////////////////////////////////////////////////////
// class DisruptorRingQueue<T, Capacity>
///////////////////////////////////////////////////////////////////

template <typename T, uint32_t Capacity = 1024U,
          uint32_t Producers = 0, uint32_t Consumers = 0>
class DisruptorRingQueue : public DisruptorRingQueueBase<T, Capacity, Producers, Consumers,
                                    DisruptorRingQueueCore<T, Capacity, Producers, Consumers> >
{
public:
    typedef uint32_t                    size_type;
    typedef uint32_t                    index_type;
    typedef T                           item_type;
    typedef T *                         value_type;
    typedef T *                         pointer;
    typedef const T *                   const_pointer;
    typedef T &                         reference;
    typedef const T &                   const_reference;

    typedef DisruptorRingQueueCore<T, Capacity, Producers, Consumers>   core_type;

    static const size_type kCapacity = DisruptorRingQueueBase<T, Capacity, Producers, Consumers,
                                         DisruptorRingQueueCore<T, Capacity, Producers, Consumers> >::kCapacity;

public:
    DisruptorRingQueue(bool bFillQueue = true, bool bInitHead = false);
    ~DisruptorRingQueue();

public:
    void dump_detail();

protected:
    void init_queue(bool bFillQueue = true);
};

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
DisruptorRingQueue<T, Capacity, Producers, Consumers>::DisruptorRingQueue(bool bFillQueue /* = true */,
                                                                          bool bInitHead  /* = false */)
: DisruptorRingQueueBase<T, Capacity, Producers, Consumers,
    DisruptorRingQueueCore<T, Capacity, Producers, Consumers> >(bInitHead)
{
    init_queue(bFillQueue);
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
DisruptorRingQueue<T, Capacity, Producers, Consumers>::~DisruptorRingQueue()
{
    // If the queue is allocated on system heap, release them.
    if (DisruptorRingQueueCore<T, Capacity, Producers, Consumers>::kIsAllocOnHeap) {
        if (this->core.availableBuffer) {
            delete [] this->core.availableBuffer;
            this->core.availableBuffer = NULL;
        }

        if (this->core.entries != NULL) {
            delete [] this->core.entries;
            this->core.entries = NULL;
        }
    }
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
inline
void DisruptorRingQueue<T, Capacity, Producers, Consumers>::init_queue(bool bFillQueue /* = true */)
{
    item_type *newData = new T[kCapacity];
    if (newData != NULL) {
        if (bFillQueue) {
            memset((void *)newData, 0, sizeof(item_type) * kCapacity);
        }
        this->core.entries = newData;
    }

    flag_type *newBufferData = new flag_type[kCapacity];
    if (newBufferData != NULL) {
        if (bFillQueue) {
            memset((void *)newBufferData, 0, sizeof(flag_type) * kCapacity);
        }
        this->core.availableBuffer = newBufferData;
    }
}

template <typename T, uint32_t Capacity, uint32_t Producers, uint32_t Consumers>
void DisruptorRingQueue<T, Capacity, Producers, Consumers>::dump_detail()
{
    printf("DisruptorRingQueue: (head = %u, tail = %u)\n",
           this->core.info.head, this->core.info.tail);
}

}  /* namespace jimi */

#endif  /* _JIMI_DISRUPTOR_RINGQUEUE_H_ */
