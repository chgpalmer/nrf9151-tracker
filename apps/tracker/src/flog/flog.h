/*
 * flog: flash flight recorder — every compiled log line, durably, in a
 * circular flash log (FCB over the fixed-partition "flight_log").
 *
 * Extraction is the shell (`flog dump/stats`); nothing here touches the
 * radio. See docs (module comment in flog.c) for the full design.
 */
#ifndef TRACKER_FLOG_H
#define TRACKER_FLOG_H

/* Bring the recorder up: recover the ring head/tail from flash and start
 * the drain thread. Returns 0 or -errno; failure leaves the tracker
 * running with the recorder offline (it must never brick the app). */
int flog_init(void);

#endif /* TRACKER_FLOG_H */
