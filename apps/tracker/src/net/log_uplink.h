/* See log_uplink.c: Zephyr log backend that ships lines over LTE. The
 * backend self-registers with the logging core; this only hooks it up as an
 * uplink source. */
#ifndef LOG_UPLINK_H__
#define LOG_UPLINK_H__

void log_uplink_init(void);

#endif /* LOG_UPLINK_H__ */
