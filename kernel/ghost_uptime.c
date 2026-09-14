// SPDX-License-Identifier: GPL-2.0
/*
 * Session-only uptime offset for the o1s test kernel.
 *
 * The offset is generated once while timekeeping is initialized, then all
 * user-visible auxiliary surfaces reuse that single value. RTC remains
 * unchanged; an opt-in cmdline mode can shift the initial realtime value.
 * Partition timestamps are presentation-only and restricted to the roots of
 * the o1s data and metadata filesystems.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/dcache.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/ghost_uptime.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/timex.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <uapi/linux/magic.h>

#define GHOST_UPTIME_MIN_DAYS	15ULL
#define GHOST_UPTIME_MAX_DAYS	25ULL
#define GHOST_UPTIME_DAY_SECS	86400ULL
#define GHOST_UPTIME_MIN_SECS	(GHOST_UPTIME_MIN_DAYS * GHOST_UPTIME_DAY_SECS)
#define GHOST_UPTIME_RANGE_SECS \
	((GHOST_UPTIME_MAX_DAYS - GHOST_UPTIME_MIN_DAYS) * \
	 GHOST_UPTIME_DAY_SECS + 1ULL)

u64 ghost_uptime_offset_ns __read_mostly;
EXPORT_SYMBOL_GPL(ghost_uptime_offset_ns);

static bool ghost_uptime_ready __read_mostly;

enum ghost_realtime_mode {
	GHOST_REALTIME_OFF,
	GHOST_REALTIME_BACKWARD,
	GHOST_REALTIME_FORWARD,
};

#ifndef GHOST_REALTIME_DEFAULT_MODE
#define GHOST_REALTIME_DEFAULT_MODE GHOST_REALTIME_OFF
#endif

static enum ghost_realtime_mode ghost_realtime_mode __read_mostly =
	GHOST_REALTIME_DEFAULT_MODE;
static bool ghost_realtime_cmdline_seen __read_mostly;
static bool ghost_realtime_applied __read_mostly;
static u32 ghost_realtime_set_hook_count __read_mostly;
static atomic64_t ghost_adjtimex_call_count = ATOMIC64_INIT(0);
static atomic64_t ghost_adjtimex_setoffset_count = ATOMIC64_INIT(0);
static atomic64_t ghost_adjtimex_frequency_count = ATOMIC64_INIT(0);
static atomic64_t ghost_timekeeping_inject_offset_count = ATOMIC64_INIT(0);
static atomic64_t ghost_timekeeping_inject_offset_success_count = ATOMIC64_INIT(0);
static atomic64_t ghost_timekeeping_inject_offset_failure_count = ATOMIC64_INIT(0);

static const char *ghost_realtime_mode_name(enum ghost_realtime_mode mode)
{
	switch (mode) {
	case GHOST_REALTIME_BACKWARD:
		return "backward";
	case GHOST_REALTIME_FORWARD:
		return "forward";
	case GHOST_REALTIME_OFF:
	default:
		return "off";
	}
}

static int __init ghost_realtime_setup(char *value)
{
	ghost_realtime_cmdline_seen = true;
	if (!value || !strcmp(value, "off"))
		ghost_realtime_mode = GHOST_REALTIME_OFF;
	else if (!strcmp(value, "backward"))
		ghost_realtime_mode = GHOST_REALTIME_BACKWARD;
	else if (!strcmp(value, "forward"))
		ghost_realtime_mode = GHOST_REALTIME_FORWARD;
	else {
		ghost_realtime_mode = GHOST_REALTIME_OFF;
		pr_warn("ghost_uptime: invalid ghost_realtime=%s; using off\n",
			value);
	}

	return 0;
}
early_param("ghost_realtime", ghost_realtime_setup);

static bool ghost_uptime_enabled __read_mostly = true;

static int __init ghost_uptime_setup(char *value)
{
	if (!value || !strcmp(value, "off") || !strcmp(value, "0"))
		ghost_uptime_enabled = false;
	else if (!strcmp(value, "on") || !strcmp(value, "1"))
		ghost_uptime_enabled = true;
	else
		pr_warn("ghost_uptime: invalid ghost_uptime=%s; defaulting to on\n", value);

	return 0;
}
early_param("ghost_uptime", ghost_uptime_setup);

enum ghost_uptime_partition_seen_bits {
	GHOST_UPTIME_DATA_ROOT_SEEN,
	GHOST_UPTIME_METADATA_ROOT_SEEN,
};

static unsigned long ghost_uptime_partition_seen __read_mostly;

static u64 ghost_uptime_mix64(u64 value)
{
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdULL;
	value ^= value >> 33;
	value *= 0xc4ceb9fe1a85ec53ULL;
	value ^= value >> 33;
	return value;
}

static u64 ghost_uptime_read_counter(void)
{
	u64 counter = 0;

#if defined(CONFIG_ARM64)
	asm volatile("mrs %0, cntpct_el0" : "=r" (counter));
#endif
	return counter;
}

static u64 ghost_uptime_make_offset_secs(time64_t wall_sec)
{
	u64 seed = ghost_uptime_read_counter();

	seed ^= (u64)wall_sec;
	seed ^= (u64)(unsigned long)&seed;
	seed = ghost_uptime_mix64(seed);

	return GHOST_UPTIME_MIN_SECS +
		(seed % GHOST_UPTIME_RANGE_SECS);
}

void ghost_uptime_apply_boot_offset(struct timespec64 *boot_offset,
				    time64_t wall_sec)
{
	u64 offset_secs;

	if (!boot_offset || ghost_uptime_ready)
		return;

	if (!ghost_uptime_enabled) {
		ghost_uptime_ready = true;
		ghost_uptime_offset_ns = 0;
		pr_info("ghost_uptime: disabled via cmdline (ghost_uptime=off)\n");
		return;
	}

	offset_secs = ghost_uptime_make_offset_secs(wall_sec);
	boot_offset->tv_sec += offset_secs;
	ghost_uptime_offset_ns = offset_secs * NSEC_PER_SEC;
	ghost_uptime_ready = true;

	pr_info("ghost_uptime: schema=v2 mode=session offset_secs=%llu range_days=15..25\n",
		offset_secs);
}

void ghost_uptime_apply_realtime(struct timespec64 *wall_time)
{
	u64 offset_secs = div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC);

	if (!wall_time || !offset_secs ||
	    ghost_realtime_mode == GHOST_REALTIME_OFF)
		return;

	if (ghost_realtime_mode == GHOST_REALTIME_BACKWARD) {
		if (wall_time->tv_sec < (time64_t)offset_secs) {
			pr_warn("ghost_uptime: realtime backward offset rejected\n");
			return;
		}
		wall_time->tv_sec -= (time64_t)offset_secs;
	} else {
		if (wall_time->tv_sec > TIME_SETTOD_SEC_MAX -
		    (time64_t)offset_secs) {
			pr_warn("ghost_uptime: realtime forward offset rejected\n");
			return;
		}
		wall_time->tv_sec += (time64_t)offset_secs;
	}

	ghost_realtime_applied = true;
	pr_info("ghost_uptime: realtime_mode=%s offset_secs=%llu\n",
		ghost_realtime_mode_name(ghost_realtime_mode), offset_secs);
}

void ghost_uptime_apply_realtime_for_settimeofday(struct timespec64 *wall_time)
{
	time64_t before_sec;

	if (!wall_time)
		return;

	before_sec = wall_time->tv_sec;
	ghost_uptime_apply_realtime(wall_time);
	if (wall_time->tv_sec != before_sec)
		ghost_realtime_set_hook_count++;
}
EXPORT_SYMBOL_GPL(ghost_uptime_apply_realtime_for_settimeofday);

void ghost_uptime_restore_realtime_for_rtc(struct timespec64 *wall_time)
{
	u64 offset_secs = div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC);

	if (!wall_time || !offset_secs ||
	    ghost_realtime_mode == GHOST_REALTIME_OFF)
		return;

	if (ghost_realtime_mode == GHOST_REALTIME_BACKWARD) {
		if (wall_time->tv_sec > TIME64_MAX - (time64_t)offset_secs)
			return;
		wall_time->tv_sec += (time64_t)offset_secs;
	} else {
		if (wall_time->tv_sec < (time64_t)offset_secs)
			return;
		wall_time->tv_sec -= (time64_t)offset_secs;
	}
}
EXPORT_SYMBOL_GPL(ghost_uptime_restore_realtime_for_rtc);

void ghost_uptime_audit_adjtimex(unsigned int modes)
{
	atomic64_inc(&ghost_adjtimex_call_count);
	if (modes & ADJ_SETOFFSET)
		atomic64_inc(&ghost_adjtimex_setoffset_count);
	if (modes & (ADJ_FREQUENCY | ADJ_TICK))
		atomic64_inc(&ghost_adjtimex_frequency_count);
}
EXPORT_SYMBOL_GPL(ghost_uptime_audit_adjtimex);

void ghost_uptime_audit_timekeeping_inject_offset(int result)
{
	atomic64_inc(&ghost_timekeeping_inject_offset_count);
	if (result)
		atomic64_inc(&ghost_timekeeping_inject_offset_failure_count);
	else
		atomic64_inc(&ghost_timekeeping_inject_offset_success_count);
}
EXPORT_SYMBOL_GPL(ghost_uptime_audit_timekeeping_inject_offset);

static int ghost_uptime_proc_show(struct seq_file *m, void *v)
{
	u64 offset_secs = div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC);

	seq_printf(m,
		   "schema=v7\n"
		   "mode=session\n"
		   "offset_secs=%llu\n"
		   "range_days=15..25\n"
		   "ready=%u\n"
		   "time_surface_audit_schema=v4\n"
		   "realtime_policy=opt-in\n"
		   "realtime_mode=%s\n"
		   "realtime_source=%s\n"
		   "realtime_applied=%u\n"
		   "realtime_set_policy=shift-absolute\n"
		   "realtime_set_hook_count=%u\n"
		   "time_sync_audit_schema=v1\n"
		   "time_sync_policy=observe-only\n"
		   "adjtimex_call_count=%lld\n"
		   "adjtimex_setoffset_count=%lld\n"
		   "adjtimex_frequency_count=%lld\n"
		   "timekeeping_inject_offset_count=%lld\n"
		   "timekeeping_inject_offset_success_count=%lld\n"
		   "timekeeping_inject_offset_failure_count=%lld\n"
		   "rtc_policy=unchanged\n"
		   "rtc_writeback_policy=restore-real\n"
		   "monotonic_policy=session-offset\n"
		   "boottime_policy=session-offset\n"
		   "proc_uptime_policy=session-offset\n"
		   "proc_btime_policy=shifted-realtime-derived\n"
		   "vdso_policy=shared-timekeeper\n"
		   "package_age_policy=unchanged\n"
		   "server_time_policy=not-controlled\n"
		   "partition_stat_scope=roots-only\n"
		   "partition_btime_policy=shift-when-available\n"
		   "data_partition=f2fs:sda34\n"
		   "metadata_partition=ext4:sda25\n"
		   "persist_partition=absent\n"
		   "data_root_stat_seen=%u\n"
		   "metadata_root_stat_seen=%u\n",
		   offset_secs, ghost_uptime_ready ? 1 : 0,
		   ghost_realtime_mode_name(ghost_realtime_mode),
		   ghost_realtime_cmdline_seen ? "cmdline" :
			"compiletime-default",
		   ghost_realtime_applied ? 1 : 0,
		   ghost_realtime_set_hook_count,
		   (long long)atomic64_read(&ghost_adjtimex_call_count),
		   (long long)atomic64_read(&ghost_adjtimex_setoffset_count),
		   (long long)atomic64_read(&ghost_adjtimex_frequency_count),
		   (long long)atomic64_read(&ghost_timekeeping_inject_offset_count),
		   (long long)atomic64_read(&ghost_timekeeping_inject_offset_success_count),
		   (long long)atomic64_read(&ghost_timekeeping_inject_offset_failure_count),
		   test_bit(GHOST_UPTIME_DATA_ROOT_SEEN,
			    &ghost_uptime_partition_seen) ? 1 : 0,
		   test_bit(GHOST_UPTIME_METADATA_ROOT_SEEN,
			    &ghost_uptime_partition_seen) ? 1 : 0);
	return 0;
}

static int __init ghost_uptime_proc_init(void)
{
	if (!proc_create_single("ghost_uptime", 0400, NULL,
				ghost_uptime_proc_show))
		pr_warn("ghost_uptime: unable to create /proc/ghost_uptime\n");

	return 0;
}
fs_initcall(ghost_uptime_proc_init);

unsigned long long ghost_uptime_apply_proc_start_time(
	struct task_struct *task, unsigned long long start_time)
{
	unsigned long long offset_ticks;

	if (!ghost_uptime_offset_ns)
		return start_time;

	offset_ticks = nsec_to_clock_t(ghost_uptime_offset_ns);
	if (start_time > offset_ticks)
		return start_time - offset_ticks;

	return 0;
}
EXPORT_SYMBOL_GPL(ghost_uptime_apply_proc_start_time);

static bool ghost_uptime_stat_magic_allowed(unsigned long magic)
{
	switch (magic) {
	case 0x9fa0:     /* procfs */
	case 0x62656572: /* sysfs */
	case 0x1021994:  /* tmpfs */
	case 0x64626720: /* debugfs */
	case 0x73636673: /* securityfs */
	case 0x27e0eb:  /* cgroup */
	case 0x6e736673: /* nsfs */
	case 0x1373:
		return true;
	default:
		return false;
	}
}

static void ghost_uptime_sub_time64(struct timespec64 *timestamp,
				    u64 offset_secs)
{
	if (timestamp->tv_sec > 0 && (u64)timestamp->tv_sec > offset_secs)
		timestamp->tv_sec -= offset_secs;
}

/* Return a seen-bit only for the exact partition-root identities on o1s. */
static int ghost_uptime_partition_root_bit(struct inode *inode)
{
	struct super_block *sb = inode ? inode->i_sb : NULL;
	char bdev_name[BDEVNAME_SIZE];

	if (!inode || !sb || !sb->s_root || !sb->s_bdev ||
	    inode != d_inode(sb->s_root))
		return -1;

	bdevname(sb->s_bdev, bdev_name);
	if (sb->s_magic == F2FS_SUPER_MAGIC && !strcmp(bdev_name, "sda34"))
		return GHOST_UPTIME_DATA_ROOT_SEEN;
	if (sb->s_magic == EXT4_SUPER_MAGIC && !strcmp(bdev_name, "sda25"))
		return GHOST_UPTIME_METADATA_ROOT_SEEN;

	return -1;
}

static void ghost_uptime_apply_basic_stat_times(struct kstat *stat,
					 u64 offset_secs)
{
	ghost_uptime_sub_time64(&stat->atime, offset_secs);
	ghost_uptime_sub_time64(&stat->mtime, offset_secs);
	ghost_uptime_sub_time64(&stat->ctime, offset_secs);
}

void ghost_uptime_apply_stat(struct inode *inode, struct kstat *stat)
{
	u64 offset_secs;
	int partition_bit;

	if (!inode || !inode->i_sb || !stat || !ghost_uptime_offset_ns)
		return;

	offset_secs = div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC);
	if (ghost_uptime_stat_magic_allowed(inode->i_sb->s_magic)) {
		ghost_uptime_apply_basic_stat_times(stat, offset_secs);
		return;
	}

	partition_bit = ghost_uptime_partition_root_bit(inode);
	if (partition_bit < 0)
		return;

	ghost_uptime_apply_basic_stat_times(stat, offset_secs);
	if (stat->result_mask & STATX_BTIME)
		ghost_uptime_sub_time64(&stat->btime, offset_secs);
	set_bit(partition_bit, &ghost_uptime_partition_seen);
}
EXPORT_SYMBOL_GPL(ghost_uptime_apply_stat);

static void *ghost_memmem(const void *haystack, size_t haystacklen,
			  const void *needle, size_t needlelen)
{
	const char *h = haystack;
	const char *n = needle;
	size_t i;

	if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen)
		return NULL;

	for (i = 0; i <= haystacklen - needlelen; i++) {
		if (h[i] == n[0] && !memcmp(&h[i], needle, needlelen))
			return (void *)&h[i];
	}
	return NULL;
}

static bool ghost_replace_string(char *buf, size_t count, const char *search, const char *replace, size_t len)
{
	char *p = buf;
	bool found = false;

	while (p <= buf + count - len) {
		p = ghost_memmem(p, buf + count - p, search, len);
		if (!p)
			break;
		memcpy(p, replace, len);
		found = true;
		p += len;
	}
	return found;
}

static void ghost_shift_date_str(char *p, u64 offset_secs)
{
	int y, m, d, h, min, s;
	time64_t epoch;
	struct tm tm;
	char tmp[24];

	if (sscanf(p, "%4d-%2d-%2d-%2d-%2d-%2d", &y, &m, &d, &h, &min, &s) != 6)
		return;

	if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31)
		return;

	epoch = mktime64(y, m, d, h, min, s);
	if (epoch <= (time64_t)offset_secs)
		return;

	time64_to_tm(epoch - offset_secs, 0, &tm);
	snprintf(tmp, sizeof(tmp), "%04ld-%02d-%02d-%02d-%02d-%02d",
		 (long)tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);

	memcpy(p, tmp, 19);
}

bool ghost_sanitize_batterystats_dump(char *buf, size_t *count_ptr, size_t max_count)
{
	u64 offset_secs;
	char *p;
	bool modified = false;

	if (!buf || !count_ptr || *count_ptr < 16)
		return false;

	offset_secs = div64_u64(ghost_uptime_offset_ns, NSEC_PER_SEC);
	if (!offset_secs)
		return false;

	/* 1. Start clock time: YYYY-MM-DD-HH-mm-ss */
	p = buf;
	while (p < buf + *count_ptr - 37) {
		p = ghost_memmem(p, buf + *count_ptr - p, "Start clock time: ", 18);
		if (!p)
			break;
		ghost_shift_date_str(p + 18, offset_secs);
		modified = true;
		p += 37;
	}

	/* 2. Current start time: YYYY-MM-DD-HH-mm-ss */
	p = buf;
	while (p < buf + *count_ptr - 39) {
		p = ghost_memmem(p, buf + *count_ptr - p, "Current start time: ", 20);
		if (!p)
			break;
		ghost_shift_date_str(p + 20, offset_secs);
		modified = true;
		p += 39;
	}

	/* 3. Any "TIME: 20XX-XX-XX-XX-XX-XX" in history (covers RESET:TIME: and (19) TIME:) */
	p = buf;
	while (p < buf + *count_ptr - 25) {
		p = ghost_memmem(p, buf + *count_ptr - p, "TIME: 20", 8);
		if (!p)
			break;
		ghost_shift_date_str(p + 6, offset_secs);
		modified = true;
		p += 25;
	}

	/* 4. Total run time: In-place exact length replacement (delta == 0) */
	p = ghost_memmem(buf, *count_ptr, "Total run time: ", 16);
	if (p) {
		char *eol = memchr(p, '\n', buf + *count_ptr - p);

		if (eol) {
			size_t old_len = eol - p;
			struct timespec64 uptime;
			u64 total_real_sec;
			u64 total_awake_sec;
			u64 r_days, u_days;
			u32 r_rem, u_rem, r_hours, u_hours, r_mins, u_mins, r_secs, u_secs;
			char new_line[128];
			int new_len;

			ktime_get_boottime_ts64(&uptime);
			total_real_sec = uptime.tv_sec;
			total_awake_sec = div64_u64(total_real_sec * 27, 100);

			r_days = div64_u64(total_real_sec, 86400);
			r_rem = total_real_sec % 86400;
			r_hours = r_rem / 3600;
			r_rem %= 3600;
			r_mins = r_rem / 60;
			r_secs = r_rem % 60;

			u_days = div64_u64(total_awake_sec, 86400);
			u_rem = total_awake_sec % 86400;
			u_hours = u_rem / 3600;
			u_rem %= 3600;
			u_mins = u_rem / 60;
			u_secs = u_rem % 60;

			new_len = snprintf(new_line, sizeof(new_line),
				"Total run time: %llud %uh %um %us realtime, %llud %uh %um %us uptime",
				r_days, r_hours, r_mins, r_secs,
				u_days, u_hours, u_mins, u_secs);

			if (new_len > (int)old_len) {
				new_len = snprintf(new_line, sizeof(new_line),
					"Total run time: %llud %uh %um realtime, %llud %uh %um uptime",
					r_days, r_hours, r_mins,
					u_days, u_hours, u_mins);
			}

			if (new_len > (int)old_len) {
				new_len = snprintf(new_line, sizeof(new_line),
					"Total run time: %llud %uh realtime, %llud %uh uptime",
					r_days, r_hours,
					u_days, u_hours);
			}

			if (new_len <= (int)old_len) {
				memcpy(p, new_line, new_len);
				if (old_len > new_len)
					memset(p + new_len, ' ', old_len - new_len);
				modified = true;
			}
		}
	}

	/* 5. Pure Kernel In-place zero-delta framework sanitization */
	if (ghost_replace_string(buf, *count_ptr, "type=DEVICE_STARTUP", "type=NONE          ", 19))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "type=DEVICE_SHUTDOWN", "type=NONE           ", 20))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "due to SYSTEM_BOOT", "due to USER_ACTION", 18))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "ams_boot_progress", "ams_data_progress", 17))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "action.BOOT_COMPLETED", "action.LOCALE_CHANGED", 21))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "Subject: BootReceiver", "Subject: StatReceiver", 21))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "event_log_start", "event_log_entry", 15))
		modified = true;

	/* 6. Telephony in-service sanitization for dumpsys telephony.registry */
	if (ghost_replace_string(buf, *count_ptr, "mVoiceRegState=1(OUT_OF_SERVICE)", "mVoiceRegState=0(IN_SERVICE)    ", 32))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "mDataRegState=1(OUT_OF_SERVICE)", "mDataRegState=0(IN_SERVICE)    ", 31))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "registrationState=NOT_REG_OR_SEARCHING", "registrationState=HOME_NETWORK        ", 38))
		modified = true;
	if (ghost_replace_string(buf, *count_ptr, "MobileData=OUT_OF_SERVICE", "MobileData=IN_SERVICE    ", 25))
		modified = true;

	return modified;
}
EXPORT_SYMBOL_GPL(ghost_sanitize_batterystats_dump);

#ifndef GHOST_PROP_CLOAK
#define GHOST_PROP_CLOAK 0
#endif

void ghost_sanitize_persistent_properties(char *buf, size_t count)
{
#if !GHOST_PROP_CLOAK
	(void)buf;
	(void)count;
	return;
#else
	char *p;
	struct timespec64 boottime;
	char anchor_str[16];
	size_t anchor_len;

	if (!buf || count < 24)
		return;

	getboottime64(&boottime);
	if (boottime.tv_sec < 1000000000ULL)
		return;

	anchor_len = snprintf(anchor_str, sizeof(anchor_str), "%llu",
			      (unsigned long long)(boottime.tv_sec + 90));

	/* 1. Replace "reboot,factory_reset" with "reboot,kernel,normal" */
	p = buf;
	while (p < buf + count - 20) {
		p = ghost_memmem(p, buf + count - p, "reboot,factory_reset", 20);
		if (!p)
			break;
		memcpy(p, "reboot,kernel,normal", 20);
		p += 20;
	}

	/* 2. Replace any remaining "factory_reset" with "reboot,kernel" */
	p = buf;
	while (p < buf + count - 13) {
		p = ghost_memmem(p, buf + count - p, "factory_reset", 13);
		if (!p)
			break;
		memcpy(p, "reboot,kernel", 13);
		p += 13;
	}

	/* 3. Replace any "recovery" in boot reason with "watchdog" */
	p = buf;
	while (p < buf + count - 8) {
		p = ghost_memmem(p, buf + count - p, "recovery", 8);
		if (!p)
			break;
		memcpy(p, "watchdog", 8);
		p += 8;
	}

	/* 3b. Collapse previous same-length cloaks that used space padding. */
	p = buf;
	while (p < buf + count - 20) {
		p = ghost_memmem(p, buf + count - p, "reboot              ", 20);
		if (!p)
			break;
		memcpy(p, "reboot,kernel,normal", 20);
		p += 20;
	}
	p = buf;
	while (p < buf + count - 13) {
		p = ghost_memmem(p, buf + count - p, "reboot       ", 13);
		if (!p)
			break;
		memcpy(p, "reboot,kernel", 13);
		p += 13;
	}

	/* 4. Find persist.sys.boot.reason.history and align timestamp to btime */
	p = ghost_memmem(buf, count, "persist.sys.boot.reason.history", 31);
	if (p && anchor_len == 10) {
		char *end = buf + count;
		char *val = p + 31;
		char *scan_end = (val + 300 < end) ? val + 300 : end;

		while (val < scan_end - 10) {
			if (val[0] == '1' && val[1] == '7' &&
			    (val[2] >= '6' && val[2] <= '9')) {
				bool is_all_digits = true;
				int i;

				for (i = 0; i < 10; i++) {
					if (val[i] < '0' || val[i] > '9') {
						is_all_digits = false;
						break;
					}
				}
				if (is_all_digits) {
					memcpy(val, anchor_str, 10);
					val += 10;
					continue;
				}
			}
			val++;
		}
	}
#endif
}
EXPORT_SYMBOL_GPL(ghost_sanitize_persistent_properties);
