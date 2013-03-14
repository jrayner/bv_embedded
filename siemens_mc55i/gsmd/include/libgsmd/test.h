#ifndef _LGSM_TEST_H
#define _LGSM_TEST_H

/* These API are only to be included in test/debug builds */

extern int lgsm_test_enable_scenario(struct lgsm_handle *lh, int type, int subtype,
	const char* scenario);

extern int lgsm_test_schedule_event(struct lgsm_handle *lh, int event_subtype,
	struct timeval relative_time);

#endif
