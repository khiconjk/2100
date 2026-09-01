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
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/timex.h>
#include <uapi/linux/magic.h>

#define GHOST_UPTIME_MIN_DAYS	15ULL
#define GHOST_UPTIME_MAX_DAYS	25ULL
#define GHOST_UPTIME_DAY_SECS	86400ULL
#define GHOST_UPTIME_MIN_SECS	(GHOST_UPTIME_MIN_DAYS * GHOST_UPTIME_DAY_SECS)
#define GHOST_UPTIME_RANGE_SECS \
	((GHOST_UPTIME_MAX_DAYS - GHOST_UPTIME_MIN_DAYS) * \
	 GHOST_UPTIME_DAY_SECS + 1ULL)

/* === GHOST IMEI RANDOM PER BOOT (DUAL SIM SM-G991B/DS) === */
static char ghost_imei_buf[17];
static char ghost_imei2_buf[17];
static u8 ghost_imei_bcd_buf[8] = {0};
static u8 ghost_imei2_bcd_buf[8] = {0};
static bool ghost_imei_ready;
static DEFINE_MUTEX(ghost_imei_mutex);

static const char * const g991b_tacs[] = {
	"35971387", "35966984", "35918923", "35906784", "35895793",
	"35844692", "35833295", "35814433", "35787240", "35471978"
};

static const u8 luhn_doubled[10] = {0, 2, 4, 6, 8, 1, 3, 5, 7, 9};

static void ghost_build_imei_bcd_generic(const char *imei_str, u8 *out_bcd)
{
	int i;
	/* 3GPP TS 24.008 10.5.1.4:
	 * Byte 0: low nibble = 0x0A (type of identity), high nibble = digit 1
	 * Byte 1..7: low nibble = digit 2N, high nibble = digit 2N+1
	 */
	out_bcd[0] = ((imei_str[0] - '0') << 4) | 0x0A;
	for (i = 1; i < 8; i++) {
		u8 d_even = (2 * i - 1 < 15) ? (imei_str[2 * i - 1] - '0') : 0x0F;
		u8 d_odd  = (2 * i < 15) ? (imei_str[2 * i] - '0') : 0x0F;
		out_bcd[i] = (d_odd << 4) | (d_even & 0x0F);
	}
}

static void ghost_generate_g991b_imei(void)
{
	u8 rand_tac_idx;
	u8 rand_snr[6];
	u8 rand_snr2[6];
	const char *tac1, *tac2;
	int sum = 0, sum2 = 0;
	int val, val2;
	int i;
	u8 tac2_offset;

	/* Select two DIFFERENT TACs for dual SIM — real Samsung S21 Dual
	 * always has distinct TACs per modem slot. */
	get_random_bytes(&rand_tac_idx, 1);
	tac1 = g991b_tacs[rand_tac_idx % ARRAY_SIZE(g991b_tacs)];
	get_random_bytes(&tac2_offset, 1);
	tac2 = g991b_tacs[(rand_tac_idx + 1 + (tac2_offset % (ARRAY_SIZE(g991b_tacs) - 1))) % ARRAY_SIZE(g991b_tacs)];

	memcpy(ghost_imei_buf, tac1, 8);
	memcpy(ghost_imei2_buf, tac2, 8);

	get_random_bytes(rand_snr, sizeof(rand_snr));
	get_random_bytes(rand_snr2, sizeof(rand_snr2));
	for (i = 0; i < 6; i++) {
		ghost_imei_buf[8 + i] = '0' + (rand_snr[i] % 10);
		ghost_imei2_buf[8 + i] = '0' + (rand_snr2[i] % 10);
	}

	for (i = 0; i < 14; i++) {
		val = ghost_imei_buf[i] - '0';
		sum += (i & 1) ? luhn_doubled[val] : val;

		val2 = ghost_imei2_buf[i] - '0';
		sum2 += (i & 1) ? luhn_doubled[val2] : val2;
	}

	ghost_imei_buf[14] = '0' + ((10 - (sum % 10)) % 10);
	ghost_imei_buf[15] = '\0';
	ghost_build_imei_bcd_generic(ghost_imei_buf, ghost_imei_bcd_buf);

	ghost_imei2_buf[14] = '0' + ((10 - (sum2 % 10)) % 10);
	ghost_imei2_buf[15] = '\0';
	ghost_build_imei_bcd_generic(ghost_imei2_buf, ghost_imei2_bcd_buf);

	ghost_imei_ready = true;
}

const char *ghost_get_imei(void)
{
	mutex_lock(&ghost_imei_mutex);
	if (unlikely(!ghost_imei_ready))
		ghost_generate_g991b_imei();
	mutex_unlock(&ghost_imei_mutex);
	return ghost_imei_buf;
}
EXPORT_SYMBOL(ghost_get_imei);

const char *ghost_get_imei2(void)
{
	mutex_lock(&ghost_imei_mutex);
	if (unlikely(!ghost_imei_ready))
		ghost_generate_g991b_imei();
	mutex_unlock(&ghost_imei_mutex);
	return ghost_imei2_buf;
}
EXPORT_SYMBOL(ghost_get_imei2);

void ghost_get_imei_bcd(u8 out_bcd[8])
{
	mutex_lock(&ghost_imei_mutex);
	if (unlikely(!ghost_imei_ready))
		ghost_generate_g991b_imei();
	memcpy(out_bcd, ghost_imei_bcd_buf, 8);
	mutex_unlock(&ghost_imei_mutex);
}
EXPORT_SYMBOL(ghost_get_imei_bcd);

void ghost_get_imei2_bcd(u8 out_bcd[8])
{
	mutex_lock(&ghost_imei_mutex);
	if (unlikely(!ghost_imei_ready))
		ghost_generate_g991b_imei();
	memcpy(out_bcd, ghost_imei2_bcd_buf, 8);
	mutex_unlock(&ghost_imei_mutex);
}
EXPORT_SYMBOL(ghost_get_imei2_bcd);

void ghost_reroll_imei(void)
{
	mutex_lock(&ghost_imei_mutex);
	ghost_generate_g991b_imei();
	mutex_unlock(&ghost_imei_mutex);
	pr_debug("ghost_imei [SM-G991B]: rerolled IMEI1 %s, IMEI2 %s\n", ghost_imei_buf, ghost_imei2_buf);
}
EXPORT_SYMBOL(ghost_reroll_imei);

static int ghost_imei_show(struct seq_file *m, void *v)
{
	seq_printf(m, "imei1: %s\n", ghost_imei_buf);
	seq_printf(m, "imei2: %s\n", ghost_imei2_buf);
	return 0;
}

static int __init ghost_imei_proc_init(void)
{
	if (!ghost_imei_ready)
		ghost_generate_g991b_imei();
	if (!proc_create_single("ghost_imei", 0400, NULL, ghost_imei_show)) {
		pr_err("ghost_imei: proc entry creation failed\n");
		return -ENOMEM;
	}
	return 0;
}
late_initcall(ghost_imei_proc_init);
/* === END GHOST IMEI === */

#define GHOST_UPTIME_MIN_SLEEP_PCT	68ULL
#define GHOST_UPTIME_MAX_SLEEP_PCT	82ULL
#define GHOST_UPTIME_RANGE_SLEEP_PCT	(GHOST_UPTIME_MAX_SLEEP_PCT - GHOST_UPTIME_MIN_SLEEP_PCT + 1ULL)

u64 ghost_uptime_offset_ns __read_mostly;
EXPORT_SYMBOL_GPL(ghost_uptime_offset_ns);

u64 ghost_uptime_mono_offset_ns __read_mostly;
EXPORT_SYMBOL_GPL(ghost_uptime_mono_offset_ns);

u64 ghost_uptime_sleep_offset_ns __read_mostly;
EXPORT_SYMBOL_GPL(ghost_uptime_sleep_offset_ns);

static u32 ghost_uptime_sleep_ratio_pct __read_mostly;
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
				    struct timespec64 *sleep_offset,
				    time64_t wall_sec)
{
	u64 offset_secs, sleep_secs, mono_secs;
	u64 seed;

	if (!boot_offset || !sleep_offset || ghost_uptime_ready)
		return;

	offset_secs = ghost_uptime_make_offset_secs(wall_sec);

	/* Derive sleep ratio percentage (68% .. 82%) using secondary mix */
	seed = ghost_uptime_mix64(ghost_uptime_read_counter() ^ (u64)wall_sec ^ offset_secs);
	ghost_uptime_sleep_ratio_pct = GHOST_UPTIME_MIN_SLEEP_PCT +
		(seed % GHOST_UPTIME_RANGE_SLEEP_PCT);

	sleep_secs = div64_u64(offset_secs * (u64)ghost_uptime_sleep_ratio_pct, 100ULL);
	mono_secs = offset_secs - sleep_secs;

	/*
	 * Mono offset shifts CLOCK_MONOTONIC (CPU awake time),
	 * Sleep offset shifts CLOCK_BOOTTIME via tk->offs_boot (deep sleep time).
	 * Total Uptime = Mono Offset + Sleep Offset = offset_secs (15..25 days).
	 * Preserve original boot_offset->tv_nsec for natural appearance.
	 */
	boot_offset->tv_sec += mono_secs;

	sleep_offset->tv_sec = sleep_secs;
	sleep_offset->tv_nsec = 0;

	ghost_uptime_offset_ns = offset_secs * NSEC_PER_SEC;
	ghost_uptime_mono_offset_ns = mono_secs * NSEC_PER_SEC;
	ghost_uptime_sleep_offset_ns = sleep_secs * NSEC_PER_SEC;
	ghost_uptime_ready = true;

	pr_info("ghost_uptime: schema=v3 mode=session total_secs=%llu mono_secs=%llu sleep_secs=%llu sleep_pct=%u%%\n",
		offset_secs, mono_secs, sleep_secs, ghost_uptime_sleep_ratio_pct);
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
	u64 mono_secs = div64_u64(ghost_uptime_mono_offset_ns, NSEC_PER_SEC);
	u64 sleep_secs = div64_u64(ghost_uptime_sleep_offset_ns, NSEC_PER_SEC);

	seq_printf(m,
		   "schema=v8\n"
		   "mode=session\n"
		   "offset_secs=%llu\n"
		   "mono_offset_secs=%llu\n"
		   "sleep_offset_secs=%llu\n"
		   "sleep_ratio_pct=%u\n"
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
		   offset_secs, mono_secs, sleep_secs,
		   ghost_uptime_sleep_ratio_pct,
		   ghost_uptime_ready ? 1 : 0,
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
	u64 offset_ticks;

	if (!task || !ghost_uptime_mono_offset_ns)
		return start_time;

	/*
	 * /proc/[pid]/stat starttime is relative to CLOCK_MONOTONIC epoch,
	 * so use mono offset (CPU awake time) not total boottime offset.
	 */
	offset_ticks = nsec_to_clock_t(ghost_uptime_mono_offset_ns);
	if (start_time > offset_ticks)
		return start_time - offset_ticks;

	return 1;
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
