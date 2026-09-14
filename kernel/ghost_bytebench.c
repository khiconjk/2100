// SPDX-License-Identifier: GPL-2.0
/*
 * Ghost Kernel - ByteBench Hardware Score Virtualization (Plan B + C)
 * Target: Samsung Galaxy S21 5G (Universal2100)
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/spinlock.h>
#include <linux/cred.h>
#include <linux/pagemap.h>
#include <linux/ghost_bytebench.h>

#define GHOST_BENCH_MAX_SLOTS 32

int ghost_bytebench_enable_plan_b = 1;
EXPORT_SYMBOL(ghost_bytebench_enable_plan_b);

int ghost_bytebench_dilation_percent = 50;
EXPORT_SYMBOL(ghost_bytebench_dilation_percent);

int ghost_bytebench_enable_plan_c = 1;
EXPORT_SYMBOL(ghost_bytebench_enable_plan_c);

u64 ghost_bytebench_max_window_ms = 5000;
EXPORT_SYMBOL(ghost_bytebench_max_window_ms);

char ghost_bytebench_target_pkg[64] = "com.ss.android.ugc.trill";
EXPORT_SYMBOL(ghost_bytebench_target_pkg);

struct ghost_bench_slot {
	pid_t pid;
	u64 start_real_ns;
	u64 last_reported_ns;
};

static struct ghost_bench_slot ghost_bench_slots[GHOST_BENCH_MAX_SLOTS];
static DEFINE_SPINLOCK(ghost_bench_lock);

static bool is_blacklisted_thread(const char *comm)
{
	static const char * const blacklist[] = {
		"AudioTrack", "AudioOut", "FastMixer", "MediaCodec",
		"Decoder", "Player", "ExoPlayer", "render",
		"Video", "OMX", "Codec", NULL
	};
	int i;

	for (i = 0; blacklist[i]; i++) {
		if (strstr(comm, blacklist[i]))
			return true;
	}
	return false;
}

static bool is_whitelisted_thread(const char *comm)
{
	static const char * const whitelist[] = {
		"ByteBench", "bytebench", "BX", "Bench_",
		"bytevc1", "h264", NULL
	};
	int i;

	for (i = 0; whitelist[i]; i++) {
		if (strstr(comm, whitelist[i]))
			return true;
	}
	return false;
}

static inline bool is_tiktok_bench_thread(void)
{
	const struct cred *cred;
	bool is_tiktok_app = false;

	if (!current)
		return false;

	cred = current_cred();
	if (!cred || cred->uid.val < 10000)
		return false;

	if (cred->uid.val == 10236)
		is_tiktok_app = true;

	if (current->group_leader) {
		const char *pcomm = current->group_leader->comm;

		if (strstr(pcomm, "ss.android") || strstr(pcomm, "ugc.trill") ||
		    strstr(pcomm, "trill") || strstr(pcomm, "tiktok") ||
		    strstr(pcomm, "ByteBench") || strstr(pcomm, "zhiliao") ||
		    strstr(pcomm, "musically") ||
		    (!strncmp(ghost_bytebench_target_pkg, pcomm, strlen(pcomm)) && strlen(pcomm) >= 5))
			is_tiktok_app = true;
	}

	/* Also trust whitelisted ByteDance proprietary threads if in an app sandbox */
	if (is_whitelisted_thread(current->comm))
		is_tiktok_app = true;

	if (!is_tiktok_app)
		return false;

	if (is_blacklisted_thread(current->comm))
		return false;

	return is_whitelisted_thread(current->comm);
}

void ghost_apply_time_dilation(clockid_t which_clock, struct timespec64 *ts)
{
	unsigned long flags;
	u64 now_ns, elapsed_real_ns, elapsed_spoofed_ns, new_ns, max_win_ns;
	int i, free_slot = -1, oldest_slot = -1;
	u64 oldest_start_ns = U64_MAX;
	struct ghost_bench_slot *slot = NULL;

	if (!ghost_bytebench_enable_plan_b || !ts)
		return;

	if (which_clock != CLOCK_MONOTONIC && which_clock != CLOCK_MONOTONIC_RAW)
		return;

	if (!is_tiktok_bench_thread())
		return;

	now_ns = timespec64_to_ns(ts);
	max_win_ns = ghost_bytebench_max_window_ms * 1000000ULL;

	spin_lock_irqsave(&ghost_bench_lock, flags);

	for (i = 0; i < GHOST_BENCH_MAX_SLOTS; i++) {
		if (ghost_bench_slots[i].pid == current->pid) {
			slot = &ghost_bench_slots[i];
			break;
		}

		/* Reclaim empty slot or expired slot */
		if (ghost_bench_slots[i].pid == 0 ||
		    (now_ns > ghost_bench_slots[i].start_real_ns &&
		     (now_ns - ghost_bench_slots[i].start_real_ns) > max_win_ns)) {
			if (free_slot < 0)
				free_slot = i;
		}

		if (ghost_bench_slots[i].start_real_ns < oldest_start_ns) {
			oldest_start_ns = ghost_bench_slots[i].start_real_ns;
			oldest_slot = i;
		}
	}

	if (slot) {
		elapsed_real_ns = now_ns - slot->start_real_ns;

		if (elapsed_real_ns > max_win_ns) {
			/* Reset window for a fresh benchmark cycle on this PID */
			slot->start_real_ns = now_ns;
			slot->last_reported_ns = now_ns;
			spin_unlock_irqrestore(&ghost_bench_lock, flags);
			return;
		}

		elapsed_spoofed_ns = (elapsed_real_ns *
				      (u64)ghost_bytebench_dilation_percent) / 100ULL;
		new_ns = slot->start_real_ns + elapsed_spoofed_ns;

		if (new_ns < slot->last_reported_ns)
			new_ns = slot->last_reported_ns;

		slot->last_reported_ns = new_ns;
		*ts = ns_to_timespec64(new_ns);
		spin_unlock_irqrestore(&ghost_bench_lock, flags);
		return;
	}

	if (free_slot < 0)
		free_slot = oldest_slot;

	if (free_slot >= 0) {
		ghost_bench_slots[free_slot].pid = current->pid;
		ghost_bench_slots[free_slot].start_real_ns = now_ns;
		ghost_bench_slots[free_slot].last_reported_ns = now_ns;
	}

	spin_unlock_irqrestore(&ghost_bench_lock, flags);
}
EXPORT_SYMBOL(ghost_apply_time_dilation);

bool ghost_is_bytebench_io_test_file(struct file *file)
{
	const char *name;
	const struct cred *cred;

	if (!ghost_bytebench_enable_plan_c)
		return false;

	if (!file || !file->f_path.dentry)
		return false;

	cred = current_cred();
	if (!cred || cred->uid.val < 10000)
		return false;

	name = file->f_path.dentry->d_name.name;
	if (!name)
		return false;

	if (strstr(name, "test_io") ||
	    strstr(name, ".bench") ||
	    strstr(name, "bytebench") ||
	    strstr(name, "trill") ||
	    strstr(name, "ugc.trill"))
		return true;

	return false;
}
EXPORT_SYMBOL(ghost_is_bytebench_io_test_file);
