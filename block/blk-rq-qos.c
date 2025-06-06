// SPDX-License-Identifier: GPL-2.0

#include "blk-rq-qos.h"

__read_mostly DEFINE_STATIC_KEY_FALSE(block_rq_qos);

/*
 * Increment 'v', if 'v' is below 'below'. Returns true if we succeeded,
 * false if 'v' + 1 would be bigger than 'below'.
 */
static bool atomic_inc_below(atomic_t *v, unsigned int below)
{
	unsigned int cur = atomic_read(v);

	do {
		if (cur >= below)
			return false;
	} while (!atomic_try_cmpxchg(v, &cur, cur + 1));

	return true;
}

bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit)
{
	return atomic_inc_below(&rq_wait->inflight, limit);
}

void __rq_qos_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->cleanup)
			rqos->ops->cleanup(rqos, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_done(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->done)
			rqos->ops->done(rqos, rq);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_issue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->issue)
			rqos->ops->issue(rqos, rq);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_requeue(struct rq_qos *rqos, struct request *rq)
{
	do {
		if (rqos->ops->requeue)
			rqos->ops->requeue(rqos, rq);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->throttle)
			rqos->ops->throttle(rqos, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_track(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->track)
			rqos->ops->track(rqos, rq, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_merge(struct rq_qos *rqos, struct request *rq, struct bio *bio)
{
	do {
		if (rqos->ops->merge)
			rqos->ops->merge(rqos, rq, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	do {
		if (rqos->ops->done_bio)
			rqos->ops->done_bio(rqos, bio);
		rqos = rqos->next;
	} while (rqos);
}

void __rq_qos_queue_depth_changed(struct rq_qos *rqos)
{
	do {
		if (rqos->ops->queue_depth_changed)
			rqos->ops->queue_depth_changed(rqos);
		rqos = rqos->next;
	} while (rqos);
}

/*
 * Return true, if we can't increase the depth further by scaling
 */
bool rq_depth_calc_max_depth(struct rq_depth *rqd)
{
	unsigned int depth;
	bool ret = false;

	/*
	 * For QD=1 devices, this is a special case. It's important for those
	 * to have one request ready when one completes, so force a depth of
	 * 2 for those devices. On the backend, it'll be a depth of 1 anyway,
	 * since the device can't have more than that in flight. If we're
	 * scaling down, then keep a setting of 1/1/1.
	 */
	if (rqd->queue_depth == 1) {
		if (rqd->scale_step > 0)
			rqd->max_depth = 1;
		else {
			rqd->max_depth = 2;
			ret = true;
		}
	} else {
		/*
		 * scale_step == 0 is our default state. If we have suffered
		 * latency spikes, step will be > 0, and we shrink the
		 * allowed write depths. If step is < 0, we're only doing
		 * writes, and we allow a temporarily higher depth to
		 * increase performance.
		 */
		depth = min_t(unsigned int, rqd->default_depth,
			      rqd->queue_depth);
		if (rqd->scale_step > 0)
			depth = 1 + ((depth - 1) >> min(31, rqd->scale_step));
		else if (rqd->scale_step < 0) {
			unsigned int maxd = 3 * rqd->queue_depth / 4;

			depth = 1 + ((depth - 1) << -rqd->scale_step);
			if (depth > maxd) {
				depth = maxd;
				ret = true;
			}
		}

		rqd->max_depth = depth;
	}

	return ret;
}

/* Returns true on success and false if scaling up wasn't possible */
bool rq_depth_scale_up(struct rq_depth *rqd)
{
	/*
	 * Hit max in previous round, stop here
	 */
	if (rqd->scaled_max)
		return false;

	rqd->scale_step--;

	rqd->scaled_max = rq_depth_calc_max_depth(rqd);
	return true;
}

/*
 * Scale rwb down. If 'hard_throttle' is set, do it quicker, since we
 * had a latency violation. Returns true on success and returns false if
 * scaling down wasn't possible.
 */
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle)
{
	/*
	 * Stop scaling down when we've hit the limit. This also prevents
	 * ->scale_step from going to crazy values, if the device can't
	 * keep up.
	 */
	if (rqd->max_depth == 1)
		return false;

	if (rqd->scale_step < 0 && hard_throttle)
		rqd->scale_step = 0;
	else
		rqd->scale_step++;

	rqd->scaled_max = false;
	rq_depth_calc_max_depth(rqd);
	return true;
}

struct rq_qos_wait_data {
	struct wait_queue_entry wq;
	struct rq_wait *rqw;
	acquire_inflight_cb_t *cb;
	void *private_data;
	bool got_token;
};

static int rq_qos_wake_function(struct wait_queue_entry *curr,
				unsigned int mode, int wake_flags, void *key)
{
	struct rq_qos_wait_data *data = container_of(curr,
						     struct rq_qos_wait_data,
						     wq);

	/*
	 * If we fail to get a budget, return -1 to interrupt the wake up loop
	 * in __wake_up_common.
	 */
	if (!data->cb(data->rqw, data->private_data))
		return -1;

	data->got_token = true;
	/*
	 * autoremove_wake_function() removes the wait entry only when it
	 * actually changed the task state. We want the wait always removed.
	 * Remove explicitly and use default_wake_function().
	 */
	default_wake_function(curr, mode, wake_flags, key);
	/*
	 * Note that the order of operations is important as finish_wait()
	 * tests whether @curr is removed without grabbing the lock. This
	 * should be the last thing to do to make sure we will not have a
	 * UAF access to @data. And the semantics of memory barrier in it
	 * also make sure the waiter will see the latest @data->got_token
	 * once list_empty_careful() in finish_wait() returns true.
	 */
	list_del_init_careful(&curr->entry);
	return 1;
}

/**
 * rq_qos_wait - throttle on a rqw if we need to
 * @rqw: rqw to throttle on
 * @private_data: caller provided specific data
 * @acquire_inflight_cb: inc the rqw->inflight counter if we can
 * @cleanup_cb: the callback to cleanup in case we race with a waker
 *
 * This provides a uniform place for the rq_qos users to do their throttling.
 * Since you can end up with a lot of things sleeping at once, this manages the
 * waking up based on the resources available.  The acquire_inflight_cb should
 * inc the rqw->inflight if we have the ability to do so, or return false if not
 * and then we will sleep until the room becomes available.
 *
 * cleanup_cb is in case that we race with a waker and need to cleanup the
 * inflight count accordingly.
 */
void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb)
{
	struct rq_qos_wait_data data = {
		.rqw		= rqw,
		.cb		= acquire_inflight_cb,
		.private_data	= private_data,
		.got_token	= false,
	};
	bool first_waiter;

	/*
	 * If there are no waiters in the waiting queue, try to increase the
	 * inflight counter if we can. Otherwise, prepare for adding ourselves
	 * to the waiting queue.
	 */
	if (!waitqueue_active(&rqw->wait) && acquire_inflight_cb(rqw, private_data))
		return;

	init_wait_func(&data.wq, rq_qos_wake_function);
	first_waiter = prepare_to_wait_exclusive(&rqw->wait, &data.wq,
						 TASK_UNINTERRUPTIBLE);
	/*
	 * Make sure there is at least one inflight process; otherwise, waiters
	 * will never be woken up. Since there may be no inflight process before
	 * adding ourselves to the waiting queue above, we need to try to
	 * increase the inflight counter for ourselves. And it is sufficient to
	 * guarantee that at least the first waiter to enter the waiting queue
	 * will re-check the waiting condition before going to sleep, thus
	 * ensuring forward progress.
	 */
	if (!data.got_token && first_waiter && acquire_inflight_cb(rqw, private_data)) {
		finish_wait(&rqw->wait, &data.wq);
		/*
		 * We raced with rq_qos_wake_function() getting a token,
		 * which means we now have two. Put our local token
		 * and wake anyone else potentially waiting for one.
		 *
		 * Enough memory barrier in list_empty_careful() in
		 * finish_wait() is paired with list_del_init_careful()
		 * in rq_qos_wake_function() to make sure we will see
		 * the latest @data->got_token.
		 */
		if (data.got_token)
			cleanup_cb(rqw, private_data);
		return;
	}

	/* we are now relying on the waker to increase our inflight counter. */
	do {
		if (data.got_token)
			break;
		io_schedule();
		set_current_state(TASK_UNINTERRUPTIBLE);
	} while (1);
	finish_wait(&rqw->wait, &data.wq);
}

void rq_qos_exit(struct request_queue *q)
{
	mutex_lock(&q->rq_qos_mutex);
	while (q->rq_qos) {
		struct rq_qos *rqos = q->rq_qos;
		q->rq_qos = rqos->next;
		rqos->ops->exit(rqos);
		static_branch_dec(&block_rq_qos);
	}
	mutex_unlock(&q->rq_qos_mutex);
}

int rq_qos_add(struct rq_qos *rqos, struct gendisk *disk, enum rq_qos_id id,
		const struct rq_qos_ops *ops)
{
	struct request_queue *q = disk->queue;
	unsigned int memflags;

	lockdep_assert_held(&q->rq_qos_mutex);

	rqos->disk = disk;
	rqos->id = id;
	rqos->ops = ops;

	/*
	 * No IO can be in-flight when adding rqos, so freeze queue, which
	 * is fine since we only support rq_qos for blk-mq queue.
	 */
	memflags = blk_mq_freeze_queue(q);

	if (rq_qos_id(q, rqos->id))
		goto ebusy;
	rqos->next = q->rq_qos;
	q->rq_qos = rqos;
	static_branch_inc(&block_rq_qos);

	blk_mq_unfreeze_queue(q, memflags);

	if (rqos->ops->debugfs_attrs) {
		mutex_lock(&q->debugfs_mutex);
		blk_mq_debugfs_register_rqos(rqos);
		mutex_unlock(&q->debugfs_mutex);
	}

	return 0;
ebusy:
	blk_mq_unfreeze_queue(q, memflags);
	return -EBUSY;
}

void rq_qos_del(struct rq_qos *rqos)
{
	struct request_queue *q = rqos->disk->queue;
	struct rq_qos **cur;
	unsigned int memflags;

	lockdep_assert_held(&q->rq_qos_mutex);

	memflags = blk_mq_freeze_queue(q);
	for (cur = &q->rq_qos; *cur; cur = &(*cur)->next) {
		if (*cur == rqos) {
			*cur = rqos->next;
			break;
		}
	}
	blk_mq_unfreeze_queue(q, memflags);

	mutex_lock(&q->debugfs_mutex);
	blk_mq_debugfs_unregister_rqos(rqos);
	mutex_unlock(&q->debugfs_mutex);
}
