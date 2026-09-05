#define _POSIX_C_SOURCE 200809L

#include "session_mailbox.h"

#include <time.h>

enum { MAILBOX_IDLE, MAILBOX_REQUESTED, MAILBOX_COMPLETE };

static void *mailbox_worker(void *argument) {
    emaster_session_mailbox_t *mailbox = argument;
    const struct timespec pause = {.tv_sec = (time_t)(mailbox->poll_ns / UINT32_C(1000000000)),
                                   .tv_nsec = (long)(mailbox->poll_ns % UINT32_C(1000000000))};

    while (!atomic_load_explicit(&mailbox->stopping, memory_order_acquire)) {
        if (atomic_load_explicit(&mailbox->state, memory_order_acquire) == MAILBOX_REQUESTED) {
            int size = sizeof(mailbox->value);
            mailbox->value = 0;
            mailbox->succeeded =
                ecx_SDOread(mailbox->context, mailbox->slave, mailbox->index, mailbox->subindex,
                            FALSE, &size, &mailbox->value, EC_TIMEOUTRXM) > 0 &&
                size == sizeof(mailbox->value);
            atomic_store_explicit(&mailbox->state, MAILBOX_COMPLETE, memory_order_release);
        } else {
            (void)nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

bool emaster_session_mailbox_start(emaster_session_mailbox_t *mailbox, ecx_contextt *context,
                                   uint32_t cycle_ns) {
    mailbox->context = context;
    mailbox->poll_ns = cycle_ns;
    atomic_init(&mailbox->state, MAILBOX_IDLE);
    atomic_init(&mailbox->stopping, false);
    mailbox->started = pthread_create(&mailbox->thread, NULL, mailbox_worker, mailbox) == 0;
    return mailbox->started;
}

bool emaster_session_mailbox_request_i8(emaster_session_mailbox_t *mailbox, uint16_t slave,
                                        uint16_t index, uint8_t subindex) {
    if (!mailbox->started ||
        atomic_load_explicit(&mailbox->state, memory_order_acquire) != MAILBOX_IDLE) {
        return false;
    }
    mailbox->slave = slave;
    mailbox->index = index;
    mailbox->subindex = subindex;
    atomic_store_explicit(&mailbox->state, MAILBOX_REQUESTED, memory_order_release);
    return true;
}

bool emaster_session_mailbox_take_i8(emaster_session_mailbox_t *mailbox, uint16_t *slave,
                                     uint16_t *index, uint8_t *subindex, int8_t *value,
                                     bool *succeeded) {
    if (!mailbox->started ||
        atomic_load_explicit(&mailbox->state, memory_order_acquire) != MAILBOX_COMPLETE) {
        return false;
    }
    *slave = mailbox->slave;
    *index = mailbox->index;
    *subindex = mailbox->subindex;
    *value = mailbox->value;
    *succeeded = mailbox->succeeded;
    atomic_store_explicit(&mailbox->state, MAILBOX_IDLE, memory_order_release);
    return true;
}

void emaster_session_mailbox_service(emaster_session_mailbox_t *mailbox) {
    if (mailbox->started) {
        (void)ecx_mbxhandler(mailbox->context, 0U, 1);
    }
}

void emaster_session_mailbox_stop(emaster_session_mailbox_t *mailbox) {
    if (mailbox->started) {
        atomic_store_explicit(&mailbox->stopping, true, memory_order_release);
        (void)pthread_join(mailbox->thread, NULL);
        mailbox->started = false;
        for (int slave = 1; slave <= mailbox->context->slavecount; ++slave) {
            mailbox->context->slavelist[slave].mbxhandlerstate = ECT_MBXH_NONE;
        }
    }
}
