/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The inter-IP fence.
 *
 * Please note that the meaning of fence is "signaled" and "unblocked" are different. It might be
 * confusing since IIF can be signaled multiple times by the signaler unlike general fences such as
 * the DMA fence.
 *
 * The meaning of "a fence is signaled" is that the fence has been signaled enough times as many as
 * the expected number of signals which should be decided when the fence is initialized nevertheless
 * it has been signaled with an error or not. That says every single signaler command are expected
 * to signal the fence (i.e., call `iif_fence_signal{_with_status}` function) even though commands
 * weren't processed normally.
 *
 * On the other hand, the meaning of "a fence is unblocked" is that the fence has been "signaled" or
 * at least one of signalers signaled the fence with an error even though the fence hasn't been
 * signaled enough times so that the waiters don't need to be blocked by the fence anymore. Note
 * that even though a fence has been unblocked with an error, remaining signalers are still expected
 * to signal the fence.
 *
 * Also, unblocking a fence here is only for the kernel perspective. Therefore, the IIF driver will
 * notify the fence unblock to only who are polling the fences (via poll callbacks or poll syscall).
 * It means that if the signaler is an IP, not AP, it is a responsibility of the IP side to unblock
 * a fence and propagate an error to waiter IPs. Therefore, unblocking fence by the IIF driver will
 * not unblock the fences in the IP side unless the IP kernel driver notices the fence unblock via
 * a poll callback and asks their IP to unblock the fence.
 *
 * If the signaler IP requires a support of the kernel driver to unblock the fence in case the IP is
 * already faulty and can't notify waiter IPs, the signaler IP kernel driver can unblock the fence
 * with an error and each waiter IP kernel driver can notice it by registering a poll callback to
 * the fence and propagate the error to the IP of each.
 *
 * Besides, one of the main roles of the IIF driver is creating fences with assigning fence IDs,
 * initializing the fence table and managing the life cycle of them.
 *
 * Copyright (C) 2023-2024 Google LLC
 */

#ifndef __IIF_IIF_FENCE_H__
#define __IIF_IIF_FENCE_H__

#include <linux/kref.h>
#include <linux/lockdep_types.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <iif/iif-manager.h>
#include <iif/iif.h>

struct iif_fence;
struct iif_fence_ops;
struct iif_fence_poll_cb;
struct iif_fence_all_signaler_submitted_cb;

/*
 * The callback which will be called when all signalers have signaled @fence.
 *
 * It will be called while @fence->signaled_signalers_lock is held and it is safe to read
 * @fence->signal_error inside.
 */
typedef void (*iif_fence_poll_cb_t)(struct iif_fence *fence, struct iif_fence_poll_cb *cb);

/*
 * The callback which will be called when all signalers have been submitted to @fence.
 *
 * It will be called while @fence->submitted_signalers_lock is held and it is safe to read
 * @fence->all_signaler_submitted_error inside.
 */
typedef void (*iif_fence_all_signaler_submitted_cb_t)(
	struct iif_fence *fence, struct iif_fence_all_signaler_submitted_cb *cb);

/*
 * The state of a fence object.
 * The state transition is
 *   INITED {-> FILE_CREATED -> FILE_RELEASED} -> RETIRED
 * i.e. Sync file creation is optional.
 */
enum iif_fence_state {
	/* Initial state. */
	IIF_FENCE_STATE_INITIALIZED,
	/* The fence ID has been retired. */
	IIF_FENCE_STATE_RETIRED,
};

/*
 * Contains the callback function which will be called when the fence has been unblocked.
 *
 * The callback can be registered to the fence by the `iif_fence_add_poll_callback` function.
 */
struct iif_fence_poll_cb {
	/* Node to be added to the list. */
	struct list_head node;
	/* Actual callback function to be called. */
	iif_fence_poll_cb_t func;
};

/*
 * Contains the callback function which will be called when all signalers have been submitted.
 *
 * The callback will be registered to the fence when the `iif_fence_submit_waiter` function fails
 * in the submission.
 */
struct iif_fence_all_signaler_submitted_cb {
	/* Node to be added to the list. */
	struct list_head node;
	/* Actual callback function to be called. */
	iif_fence_all_signaler_submitted_cb_t func;
	/* The number of remaining signalers to be submitted. */
	int remaining_signalers;
};

/* The fence object. */
struct iif_fence {
	/* IIF manager. */
	struct iif_manager *mgr;
	/* Fence ID. */
	int id;
	/* Signaler IP type. */
	enum iif_ip_type signaler_ip;
	/* The number of total signalers to be submitted. */
	uint16_t total_signalers;
	/* The number of submitted signalers. */
	uint16_t submitted_signalers;
	/*
	 * Protects @submitted_signalers, @all_signaler_submitted_cb_list and
	 * @all_signaler_submitted_error.
	 */
	spinlock_t submitted_signalers_lock;
#if IS_ENABLED(CONFIG_DEBUG_SPINLOCK)
	struct lock_class_key submitted_signalers_key;
#endif /* IS_ENABLED(CONFIG_DEBUG_SPINLOCK) */
	/* The interrupt state before holding @submitted_signalers_lock. */
	unsigned long submitted_signalers_lock_flags;
	/* The number of signaled signalers. */
	uint16_t signaled_signalers;
	/* Protects @signaled_signalers, @poll_cb_list and @signal_error. */
	spinlock_t signaled_signalers_lock;
	/* The number of outstanding waiters. */
	uint16_t outstanding_waiters;
	/* Protects @outstanding_waiters. */
	spinlock_t outstanding_waiters_lock;
	/* Reference count. */
	struct kref kref;
	/* Operators. */
	const struct iif_fence_ops *ops;
	/* State of this fence object. */
	enum iif_fence_state state;
	/* List of callbacks which will be called when the fence is unblocked. */
	struct list_head poll_cb_list;
	/* List of callbacks which will be called when all signalers have been submitted. */
	struct list_head all_signaler_submitted_cb_list;
	/* Will be set to a negative errno if the fence is signaled with an error. */
	int signal_error;
	/* Will be set to a negative errno if waiting the signaler submission fails. */
	int all_signaler_submitted_error;
	/* The number of sync_file(s) bound to the fence. */
	atomic_t num_sync_file;
	/* The callback called if the fence's signaler is AP and the fence is unblocked. */
	struct iif_fence_poll_cb ap_poll_cb;
};

/* Operators of `struct iif_fence`. */
struct iif_fence_ops {
	/*
	 * Called on destruction of @fence to release additional resources when its reference count
	 * becomes zero.
	 *
	 * This callback is optional.
	 * Context: normal and in_interrupt().
	 */
	void (*on_release)(struct iif_fence *fence);
};

/*
 * Initializes @fence which will be signaled by @signaler_ip IP. @total_signalers is the number of
 * signalers which must be submitted to the fence. Its initial reference count is 1.
 *
 * The initialized fence will be assigned an ID which depends on @signaler_ip. Each IP will have at
 * most `IIF_NUM_FENCES_PER_IP` number of fences and the assigned fence ID for IP[i] will be one of
 * [i * IIF_NUM_FENCES_PER_IP ~ (i + 1) * IIF_NUM_FENCES_PER_IP - 1].
 */
int iif_fence_init(struct iif_manager *mgr, struct iif_fence *fence,
		   const struct iif_fence_ops *ops, enum iif_ip_type signaler_ip,
		   uint16_t total_signalers);

/*
 * Opens a file which syncs with @fence and returns its FD. The file will hold a reference to
 * @fence until it is closed.
 */
int iif_fence_install_fd(struct iif_fence *fence);

/*
 * Has @fence know the sync file bound to it is about to be released. This function would try to
 * retire the fence if applicable.
 */
void iif_fence_on_sync_file_release(struct iif_fence *fence);

/* Increases the reference count of @fence. */
struct iif_fence *iif_fence_get(struct iif_fence *fence);

/*
 * Gets a fence from @fd and increments its reference count of the file pointer.
 *
 * Returns the fence pointer, if @fd is for IIF. Otherwise, returns a negative errno.
 */
struct iif_fence *iif_fence_fdget(int fd);

/* Decreases the reference count of @fence and if it becomes 0, releases @fence. */
void iif_fence_put(struct iif_fence *fence);

/*
 * Submits a signaler. @fence->submitted_signalers will be incremented by 1.
 *
 * This function can be called in the IRQ context.
 *
 * Returns 0 if the submission succeeds. Otherwise, returns a negative errno.
 */
int iif_fence_submit_signaler(struct iif_fence *fence);

/*
 * Its functionality is the same with the `iif_fence_submit_signaler` function, but the caller
 * is holding @fence->submitted_signalers_lock.
 */
int iif_fence_submit_signaler_locked(struct iif_fence *fence);

/*
 * Submits a waiter of @ip IP. @fence->outstanding_waiters will be incremented by 1.
 * Note that the waiter submission will not be done when not all signalers have been submitted.
 * (i.e., @fence->submitted_signalers < @fence->total_signalers)
 *
 * This function can be called in the IRQ context.
 *
 * Returns the number of remaining signalers to be submitted (i.e., returning 0 means the submission
 * actually succeeded). Otherwise, returns a negative errno if it fails with other reasons.
 */
int iif_fence_submit_waiter(struct iif_fence *fence, enum iif_ip_type ip);

/*
 * Signals @fence.
 *
 * If all signaler commands have called this function for the fence and it has been unblocked, all
 * registered poll callbacks will be executed.
 *
 * Returns the number of remaining signals on success. Otherwise, returns a negative errno.
 */
int iif_fence_signal(struct iif_fence *fence);

/*
 * Signals @fence with a status.
 *
 * Basically, its functionality is the same as the `iif_fence_signal` function above, but the user
 * can supply an optional error status.
 *
 * If the signal error was set, it will make the fence as unblocked even though the number of
 * remaining signals before this function call was bigger than 1. That says all registered poll
 * callbacks will be executed to propagate the fence error to the ones polling the fence
 * immediately.
 *
 * If the caller passes 0 to @error, its functionality is the same as the `iif_fence_signal`
 * function.
 *
 * Returns the number of remaining signals on success. Otherwise, returns a negative errno.
 */
int iif_fence_signal_with_status(struct iif_fence *fence, int error);

/*
 * Returns the signal status of @fence.
 *
 * Returns 0 if the fence hasn't been unblocked yet, 1 if the fence has been unblocked without any
 * error, or a negative errno if the fence has been signaled with an error at least once.
 */
int iif_fence_get_signal_status(struct iif_fence *fence);

/*
 * Returns whether all signalers have signaled @fence.
 *
 * As this function doesn't require to hold any lock, even if this function returns false, @fence
 * can be signaled right after this function returns. One should care about this and may not use
 * this function directly. This function will be mostly used when iif_sync_file is polling @fence.
 */
bool iif_fence_is_signaled(struct iif_fence *fence);

/* Notifies the driver that a waiter of @ip finished waiting on @fence. */
void iif_fence_waited(struct iif_fence *fence, enum iif_ip_type ip);

/*
 * Registers a callback which will be called when @fence has been unblocked. Once the callback is
 * called, it will be automatically unregistered from @fence. The @func can be called in the IRQ
 * context.
 *
 * Returns 0 if succeeded. Otherwise, returns a negative errno on failure. Note that even when
 * @fence is already unblocked, it won't add the callback and return -EPERM.
 */
int iif_fence_add_poll_callback(struct iif_fence *fence, struct iif_fence_poll_cb *poll_cb,
				iif_fence_poll_cb_t func);

/*
 * Unregisters the callback from @fence.
 *
 * Returns true if the callback is removed before @fence is unblocked.
 */
bool iif_fence_remove_poll_callback(struct iif_fence *fence, struct iif_fence_poll_cb *poll_cb);

/*
 * Registers a callback which will be called when all signalers are submitted for @fence and
 * returns the number of remaining signalers to be submitted to @cb->remaining_signalers. Once the
 * callback is called, it will be automatically unregistered from @fence.
 *
 * Returns 0 if succeeded. If all signalers are already submitted, returns -EPERM.
 */
int iif_fence_add_all_signaler_submitted_callback(struct iif_fence *fence,
						  struct iif_fence_all_signaler_submitted_cb *cb,
						  iif_fence_all_signaler_submitted_cb_t func);

/*
 * Unregisters the callback which is registered by the callback above.
 *
 * Returns true if the callback is removed before its being called.
 */
bool iif_fence_remove_all_signaler_submitted_callback(
	struct iif_fence *fence, struct iif_fence_all_signaler_submitted_cb *cb);

/*
 * Returns the number of signalers or waiters information accordingly.
 *
 * Note that these functions hold required locks internally and read the value. Therefore, the value
 * of them can be changed after the function returns. The one must use these functions only for the
 * debugging purpose.
 *
 * These functions can be called in the IRQ context.
 */
int iif_fence_unsubmitted_signalers(struct iif_fence *fence);
int iif_fence_submitted_signalers(struct iif_fence *fence);
int iif_fence_signaled_signalers(struct iif_fence *fence);
int iif_fence_outstanding_waiters(struct iif_fence *fence);

/*
 * Returns true if a waiter or a signaler is submittable to @fence.
 *
 * The caller must hold @fence->submitted_signalers_lock.
 */
bool iif_fence_is_waiter_submittable_locked(struct iif_fence *fence);
bool iif_fence_is_signaler_submittable_locked(struct iif_fence *fence);

/* Holds @fence->submitted_signalers_lock. */
static inline void iif_fence_submitted_signalers_lock(struct iif_fence *fence)
{
	spin_lock_irqsave(&fence->submitted_signalers_lock, fence->submitted_signalers_lock_flags);
}

/* Releases @fence->submitted_signalers_lock. */
static inline void iif_fence_submitted_signalers_unlock(struct iif_fence *fence)
{
	spin_unlock_irqrestore(&fence->submitted_signalers_lock,
			       fence->submitted_signalers_lock_flags);
}

#endif /* __IIF_IIF_FENCE_H__ */
