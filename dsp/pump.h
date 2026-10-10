/*
 *
 *    pump.h - a data pump as a class
 *
 *    The modem core knows a data pump by its struct dp and the table of
 *    functions it registers (modem_dp.h).  A pump class derives from
 *    Pump and has
 *
 *        static constexpr const char *name    for the table
 *        int start(struct modem *m, enum DP_ID id, int caller, int srate,
 *                  int max_frag)               0, or -1 if it cannot run
 *        int process(void *in, void *out, int cnt)    DPSTAT_*
 *        int hangup()                          (optional)
 *
 *    and PumpOps<T>::ops is the table to register.  A pump is created
 *    zeroed (value initialized), as the C pumps were with calloc, so its
 *    plain members need no constructor; its destructor frees what start
 *    took.
 *
 */

#ifndef __PUMP_H__
#define __PUMP_H__

#include <new>

#include "dsp.h"

namespace dsp {

class Pump {
public:
	virtual ~Pump() {}
	virtual int process(void *in, void *out, int cnt) = 0;
	virtual int hangup() { return 0; }

	struct dp dp;           /* what the modem core holds */
};

template <class T>
struct PumpOps {
	static struct dp *create(struct modem *m, enum DP_ID id, int caller,
				 int srate, int max_frag,
				 struct dp_operations *op)
	{
		T *p = new (std::nothrow) T();
		if (!p)
			return NULL;
		p->dp.id = id;
		p->dp.modem = m;
		p->dp.op = op;
		p->dp.dp_data = static_cast<Pump *>(p);
		if (p->start(m, id, caller, srate, max_frag)) {
			delete p;
			return NULL;
		}
		return &p->dp;
	}
	static int destroy(struct dp *d)
	{
		delete static_cast<Pump *>(d->dp_data);
		return 0;
	}
	static int process(struct dp *d, void *in, void *out, int cnt)
	{
		return static_cast<Pump *>(d->dp_data)->process(in, out, cnt);
	}
	static int hangup(struct dp *d)
	{
		return static_cast<Pump *>(d->dp_data)->hangup();
	}
	static struct dp_operations ops;
};

template <class T>
struct dp_operations PumpOps<T>::ops = {
	T::name, 0, PumpOps<T>::create, PumpOps<T>::destroy,
	PumpOps<T>::process, PumpOps<T>::hangup,
};

} // namespace dsp

#endif /* __PUMP_H__ */
