/*
 * This confidential and proprietary software may be used only as
 * authorised by a licensing agreement from ARM Limited
 * (C) COPYRIGHT 2012 ARM Limited
 * ALL RIGHTS RESERVED
 * The entire notice above must be reproduced on all authorised
 * copies and copies may only be made to the extent permitted
 * by a licensing agreement from ARM Limited.
 */

/**
 * @file mali_kbase_sync.c
 *
 */

#ifdef CONFIG_SYNC

#include <linux/sync.h>
#include <kbase/src/common/mali_kbase.h>

struct mali_sync_timeline
{
	struct sync_timeline timeline;
	osk_atomic counter;
	osk_atomic signalled;
};

struct mali_sync_pt
{
	struct sync_pt pt;
	u32 order;
};

static struct mali_sync_timeline *to_mali_sync_timeline(struct sync_timeline *timeline)
{
	return container_of(timeline, struct mali_sync_timeline, timeline);
}

static struct mali_sync_pt *to_mali_sync_pt(struct sync_pt *pt)
{
	return container_of(pt, struct mali_sync_pt, pt);
}

static struct sync_pt *timeline_dup(struct sync_pt *pt)
{
	struct mali_sync_pt *mpt = to_mali_sync_pt(pt);
	struct mali_sync_pt *new_mpt;
	struct sync_pt *new_pt = sync_pt_create(pt->parent, sizeof(struct mali_sync_pt));

	if (!new_pt)
	{
		return NULL;
	}

	new_mpt = to_mali_sync_pt(new_pt);
	new_mpt->order = mpt->order;

	return new_pt;

}

static int timeline_has_signaled(struct sync_pt *pt)
{
	struct mali_sync_pt *mpt = to_mali_sync_pt(pt);
	struct mali_sync_timeline *mtl = to_mali_sync_timeline(pt->parent);

	long diff = osk_atomic_get(&mtl->signalled) - mpt->order;

	return diff >= 0;
}

static int timeline_compare(struct sync_pt *a, struct sync_pt *b)
{
	struct mali_sync_pt *ma = container_of(a, struct mali_sync_pt, pt);
	struct mali_sync_pt *mb = container_of(b, struct mali_sync_pt, pt);

	long diff = ma->order - mb->order;

	if (diff < 0)
	{
		return -1;
	}
	else if (diff == 0)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

static struct sync_timeline_ops mali_timeline_ops = {
	.driver_name    = "Mali",
	.dup            = timeline_dup,
	.has_signaled   = timeline_has_signaled,
	.compare        = timeline_compare,
#if 0
	.free_pt        = timeline_free_pt,
	.release_obj    = timeline_release_obj
#endif
};

int kbase_sync_timeline_is_ours(struct sync_timeline *timeline)
{
	return (timeline->ops == &mali_timeline_ops);
}

struct sync_timeline *kbase_sync_timeline_alloc(const char * name)
{
	struct sync_timeline *tl;
	struct mali_sync_timeline *mtl;

	tl = sync_timeline_create(&mali_timeline_ops,
	                          sizeof(struct mali_sync_timeline), name);
	if (!tl)
	{
		return NULL;
	}

	/* Set the counter in our private struct */
	mtl = to_mali_sync_timeline(tl);
	osk_atomic_set(&mtl->counter, 0);
	osk_atomic_set(&mtl->signalled, 0);

	return tl;
}

struct sync_pt *kbase_sync_pt_alloc(struct sync_timeline *parent)
{
	struct sync_pt *pt = sync_pt_create(parent, sizeof(struct mali_sync_pt));
	struct mali_sync_timeline *mtl = to_mali_sync_timeline(parent);
	struct mali_sync_pt *mpt;

	if (!pt)
	{
		return NULL;
	}

	mpt = to_mali_sync_pt(pt);
	mpt->order = osk_atomic_inc(&mtl->counter);

	return pt;
}

void kbase_sync_signal_pt(struct sync_pt *pt)
{
	struct mali_sync_pt *mpt = to_mali_sync_pt(pt);
	struct mali_sync_timeline *mtl = to_mali_sync_timeline(pt->parent);
	u32 signalled;
	long diff;

	do {

		signalled = osk_atomic_get(&mtl->signalled);

		diff = signalled - mpt->order;

		if (diff > 0)
		{
			/* The timeline is already at or ahead of this point. This should not happen unless userspace
			 * has been signalling fences out of order, so warn but don't violate the sync_pt API.
			 * The warning is only in release builds to prevent a malicious user being able to spam dmesg.
			 */
#if MALI_DEBUG
			OSK_PRINT_ERROR(OSK_BASE_JD, "Fence's were triggered in a different order to allocation!");
#endif
			return;
		}
	} while (osk_atomic_compare_and_swap(&mtl->signalled, signalled, mpt->order) != signalled);
}

#endif /* CONFIG_SYNC */
