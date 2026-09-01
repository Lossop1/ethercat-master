#include "emaster/cyclic/exchange.h"

#include <string.h>

bool emaster_cyclic_exchange_init(emaster_cyclic_exchange_t *exchange,
                                   size_t axis_count, uint16_t expected_wkc)
{
    if (exchange == NULL || axis_count == 0U || expected_wkc == 0U)
    {
        return false;
    }
    memset(exchange, 0, sizeof(*exchange));
    exchange->axis_count = axis_count;
    exchange->expected_wkc = expected_wkc;
    return true;
}

static void initialize_result(emaster_cyclic_result_t *result, uint64_t sequence)
{
    memset(result, 0, sizeof(*result));
    result->status = EMASTER_CYCLIC_INVALID_ARGUMENT;
    result->sequence = sequence;
    result->actual_wkc = -1;
}

emaster_cyclic_status_t emaster_cyclic_exchange_step(
    emaster_cyclic_exchange_t *exchange,
    const emaster_cyclic_transport_t *transport,
    const emaster_cyclic_frame_t *frame,
    uint64_t now_ns,
    emaster_cyclic_result_t *result)
{
    int actual_wkc = -1;

    if (result != NULL)
    {
        initialize_result(result, frame == NULL ? UINT64_C(0) : frame->sequence);
    }
    if (exchange == NULL || transport == NULL || frame == NULL || result == NULL ||
        exchange->axis_count == 0U || exchange->expected_wkc == 0U ||
        transport->exchange == NULL)
    {
        return EMASTER_CYCLIC_INVALID_ARGUMENT;
    }
    if (frame->axis_count != exchange->axis_count)
    {
        result->status = EMASTER_CYCLIC_AXIS_COUNT_MISMATCH;
        return result->status;
    }
    if (frame->output_length > 0U && frame->output_bytes == NULL)
    {
        return result->status;
    }
    if (frame->input_capacity > 0U && frame->input_bytes == NULL)
    {
        return result->status;
    }
    if (exchange->has_last_sequence && frame->sequence <= exchange->last_sequence)
    {
        result->status = EMASTER_CYCLIC_SEQUENCE_NOT_MONOTONIC;
        return result->status;
    }
    if (now_ns > frame->deadline_ns)
    {
        result->status = EMASTER_CYCLIC_DEADLINE_EXPIRED;
        return result->status;
    }
    result->transport_succeeded = transport->exchange(
        transport->user_data, frame->output_bytes, frame->output_length, frame->input_bytes,
        frame->input_capacity, &actual_wkc);
    result->actual_wkc = actual_wkc;
    if (!result->transport_succeeded)
    {
        result->status = EMASTER_CYCLIC_TRANSPORT_FAILED;
        return result->status;
    }
    result->work_counter_match = actual_wkc == (int)exchange->expected_wkc;
    if (!result->work_counter_match)
    {
        result->status = EMASTER_CYCLIC_WORK_COUNTER_MISMATCH;
        return result->status;
    }
    exchange->last_sequence = frame->sequence;
    exchange->has_last_sequence = true;
    result->status = EMASTER_CYCLIC_OK;
    return result->status;
}
