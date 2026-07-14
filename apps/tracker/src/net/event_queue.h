/*
 * event_queue: things worth telling a human about, right now.
 *
 * Separate from obs_queue on purpose. Observations describe where the asset
 * WAS; an event says something HAPPENED — and the server may act on it (alert
 * the owner that their asset moved). It is therefore the highest-priority
 * uplink source (prio 0, drained first) and rides the urgent flush the FSM
 * already fires on a state change, so an alert reaches the server in seconds
 * rather than at the next cadence tick.
 *
 * Today the only kind is an IMU wake on a quiescent tracker. The ring is tiny:
 * events are rare by construction, and a device that generates more than a
 * handful before it can send has bigger problems than a lost alert.
 */
#ifndef EVENT_QUEUE_H__
#define EVENT_QUEUE_H__

#include <stdint.h>

/* Wire values (proto MotionEvent.reason) — server-visible, so append only. */
#define EVENT_REASON_IMU_WAKE 1

void event_queue_init(void);

/* Queue one event. Drops the oldest if the (tiny) ring is full. */
void event_queue_add(uint32_t reason, int64_t now_ms);

#endif /* EVENT_QUEUE_H__ */
