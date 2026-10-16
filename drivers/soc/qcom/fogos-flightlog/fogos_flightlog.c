// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in fogos boot recorder. No watchdog pets, panic, or reset operations. */
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stacktrace.h>
#include <asm/io.h>
#include <asm/irq_regs.h>
#include <asm/ptrace.h>
#include <soc/qcom/minidump.h>

#define FL_CPUS 8
#define FL_RING 16
#define FL_TASKS 24
#define FL_DEPTH 16
#define FL_END_NS (40ULL * NSEC_PER_SEC)
#define FL_TICK_NS (250ULL * NSEC_PER_MSEC)

struct fl_tick {
	u64 generation;
	u64 ns;
	unsigned long pc;
	unsigned long lr;
	unsigned long sp;
	u32 pid;
	u32 user;
	char comm[TASK_COMM_LEN];
};

struct fl_cpu_data {
	u64 head;
	int start_result;
	u32 started;
	struct fl_tick ring[FL_RING];
};

struct fl_task {
	u64 ns;
	unsigned long state;
	u32 pid;
	u32 depth;
	char comm[TASK_COMM_LEN];
	unsigned long entries[FL_DEPTH];
};

/* Contiguous allocation registered with minidump. Commit counters let a
 * decoder recognize an interrupted update. Export is firmware-dependent.
 */
struct fl_data {
	char magic[8];
	u32 version;
	u32 size;
	u32 cpu_capacity;
	u32 task_capacity;
	u64 task_generation;
	u64 task_begin_ns;
	u64 task_end_ns;
	u32 task_count;
	u32 task_slot;
	struct fl_cpu_data cpus[FL_CPUS];
	struct fl_task tasks[2][FL_TASKS];
};

struct fl_timer {
	struct hrtimer timer;
	call_single_data_t start_call;
	u64 last_print_ns;
};

static struct fl_data *flight;
static struct fl_timer timers[FL_CPUS];
static struct md_region dump_region;
static bool armed;
module_param_named(arm, armed, bool, 0400);
MODULE_PARM_DESC(arm, "Explicit normal-boot diagnostic opt-in from first-stage init");

static enum hrtimer_restart fl_timer_tick(struct hrtimer *timer)
{
	struct fl_timer *source = container_of(timer, struct fl_timer, timer);
	struct pt_regs *regs = get_irq_regs();
	int cpu = smp_processor_id();
	struct fl_cpu_data *data = &flight->cpus[cpu];
	u64 now = ktime_get_boottime_ns();
	u64 next = READ_ONCE(data->head) + 1;
	struct fl_tick *tick = &data->ring[next % FL_RING];

	WRITE_ONCE(tick->generation, 0);
	smp_wmb();
	tick->ns = now;
	tick->pid = current->pid;
	/* Avoid task_lock/get_task_comm in hard IRQ context. A concurrent rename
 * can give a partial name, never an unterminated string or a lock wait.
 */
	memcpy(tick->comm, current->comm, TASK_COMM_LEN);
	tick->comm[TASK_COMM_LEN - 1] = '\0';
	tick->pc = regs ? instruction_pointer(regs) : 0;
	tick->lr = regs ? regs->regs[30] : 0;
	tick->sp = regs ? regs->sp : 0;
	tick->user = regs ? user_mode(regs) : 0;
	smp_store_release(&tick->generation, next);
	smp_store_release(&data->head, next);

	/* Each CPU prints its own heartbeat: the task-stack thread is not a
 * prerequisite. These are normal IRQs, not NMIs; a CPU with IRQs masked
 * cannot run this callback. No synchronous cross-CPU wait is introduced.
 */
	if (now >= 10ULL * NSEC_PER_SEC &&
	    now - source->last_print_ns >= NSEC_PER_SEC) {
		source->last_print_ns = now;
		if (tick->user)
			pr_info("fogos-flight: cpu=%d seq=%llu ns=%llu pid=%u comm=%s user=1 pc=0x%lx\n",
				cpu, next, now, tick->pid, tick->comm, tick->pc);
		else
			pr_info("fogos-flight: cpu=%d seq=%llu ns=%llu pid=%u comm=%s user=0 pc=%pS lr=%pS\n",
				cpu, next, now, tick->pid, tick->comm,
				(void *)tick->pc, (void *)tick->lr);
	}
	if (now >= FL_END_NS)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(timer, ns_to_ktime(FL_TICK_NS));
	return HRTIMER_RESTART;
}

static void fl_start_cpu(void *unused)
{
	int cpu = smp_processor_id();
	struct fl_timer *timer = &timers[cpu];

	hrtimer_init(&timer->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	timer->timer.function = fl_timer_tick;
	WRITE_ONCE(flight->cpus[cpu].started, 1);
	hrtimer_start(&timer->timer, ns_to_ktime(FL_TICK_NS),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

static bool fl_named_task(struct task_struct *task)
{
	return task->pid == 1 || !strcmp(task->comm, "zygote64") ||
		!strcmp(task->comm, "main") || !strcmp(task->comm, "system_server") ||
		!strcmp(task->comm, "surfaceflinger") ||
		!strcmp(task->comm, "msm-watchdog") ||
		!strcmp(task->comm, "rcu_preempt") ||
		!strcmp(task->comm, "servicemanager") ||
		!strcmp(task->comm, "vold") ||
		!strcmp(task->comm, "adbd") ||
		!strncmp(task->comm, "ksoftirqd/", 10);
}

static void fl_dump_tasks(void)
{
	struct task_struct *selected[FL_TASKS];
	struct task_struct *group, *task;
	u64 generation = READ_ONCE(flight->task_generation) + 1;
	u32 slot = generation % 2;
	unsigned int count = 0, scanned, pass, i, depth;

	/* Retain references under RCU, then unwind outside the RCU section.
 * Named boot/watchdog tasks take priority over other blocked threads.
 */
	for (pass = 0; pass < 2 && count < FL_TASKS; pass++) {
		scanned = 0;
		rcu_read_lock();
		for_each_process_thread(group, task) {
			bool named = fl_named_task(task);

			if (++scanned > 4096)
				goto scan_done;
			if (pass == 0 ? !named :
			    (named || !(READ_ONCE(task->__state) & TASK_UNINTERRUPTIBLE)))
				continue;
			get_task_struct(task);
			selected[count++] = task;
			if (count == FL_TASKS)
				goto scan_done;
		}
	scan_done:
		rcu_read_unlock();
	}
	WRITE_ONCE(flight->task_begin_ns, ktime_get_boottime_ns());
	WRITE_ONCE(flight->task_end_ns, 0);
	pr_info("fogos-flight: tasks begin ns=%llu count=%u generation=%llu\n",
		READ_ONCE(flight->task_begin_ns), count, generation);
	for (i = 0; i < count; i++) {
		struct fl_task *record = &flight->tasks[slot][i];

		task = selected[i];
		record->ns = ktime_get_boottime_ns();
		record->pid = task->pid;
		record->state = READ_ONCE(task->__state);
		memcpy(record->comm, task->comm, TASK_COMM_LEN);
		record->comm[TASK_COMM_LEN - 1] = '\0';
		pr_info("fogos-flight: task begin pid=%u comm=%s state=0x%lx ns=%llu\n",
			record->pid, record->comm, record->state, record->ns);
		record->depth = stack_trace_save_tsk(task, record->entries, FL_DEPTH, 0);
		for (depth = 0; depth < record->depth; depth++)
			pr_info("fogos-flight: stack pid=%u depth=%u %pS\n",
				record->pid, depth, (void *)record->entries[depth]);
		put_task_struct(task);
	}
	WRITE_ONCE(flight->task_count, count);
	WRITE_ONCE(flight->task_slot, slot);
	WRITE_ONCE(flight->task_end_ns, ktime_get_boottime_ns());
	smp_store_release(&flight->task_generation, generation);
	pr_info("fogos-flight: tasks end ns=%llu generation=%llu\n",
		READ_ONCE(flight->task_end_ns), generation);
}

static int fl_thread(void *unused)
{
	static const unsigned int at_seconds[] = { 12, 14, 15, 16, 17, 18, 20, 24, 28, 32 };
	unsigned int next = 0;

	while (ktime_get_boottime_ns() < FL_END_NS) {
		u64 now = ktime_get_boottime_ns();

		if (next < ARRAY_SIZE(at_seconds) &&
		    now >= (u64)at_seconds[next] * NSEC_PER_SEC) {
			fl_dump_tasks();
			next++;
		}
		msleep(100);
	}
	pr_info("fogos-flight: observation window complete; no reset requested\n");
	return 0;
}

static int fl_show(struct seq_file *out, void *unused)
{
	unsigned int cpu, i, depth;
	u32 count = min_t(u32, READ_ONCE(flight->task_count), FL_TASKS);
	u32 slot = READ_ONCE(flight->task_slot) % 2;

	seq_printf(out, "Fogos kernel flight recorder v1 ns=%llu task_generation=%llu begin=%llu end=%llu\n",
		ktime_get_boottime_ns(), smp_load_acquire(&flight->task_generation),
		READ_ONCE(flight->task_begin_ns), READ_ONCE(flight->task_end_ns));
	for (cpu = 0; cpu < FL_CPUS; cpu++) {
		struct fl_cpu_data *data = &flight->cpus[cpu];
		u64 head = smp_load_acquire(&data->head);
		struct fl_tick tick;

		memcpy(&tick, &data->ring[head % FL_RING], sizeof(tick));
		smp_rmb();
		seq_printf(out, "cpu=%u online=%d started=%u start_result=%d seq=%llu record_generation=%llu ns=%llu pid=%u comm=%.*s user=%u pc=%px lr=%px\n",
			cpu, cpu_online(cpu), READ_ONCE(data->started), READ_ONCE(data->start_result),
			head, tick.generation, tick.ns, tick.pid, TASK_COMM_LEN, tick.comm,
			tick.user, (void *)tick.pc, (void *)tick.lr);
	}
	for (i = 0; i < count; i++) {
		struct fl_task *task = &flight->tasks[slot][i];

		seq_printf(out, "task pid=%u comm=%.*s state=0x%lx ns=%llu\n",
			READ_ONCE(task->pid), TASK_COMM_LEN, task->comm,
			READ_ONCE(task->state), READ_ONCE(task->ns));
		for (depth = 0; depth < min_t(u32, READ_ONCE(task->depth), FL_DEPTH); depth++)
			seq_printf(out, "  %pS\n", (void *)READ_ONCE(task->entries[depth]));
	}
	return 0;
}

static int __init fl_init(void)
{
	struct proc_dir_entry *proc;
	struct task_struct *thread;
	int cpu, result;

	if (!armed)
		return -EINVAL;
	flight = kzalloc(sizeof(*flight), GFP_KERNEL);
	if (!flight)
		return -ENOMEM;
	memcpy(flight->magic, "FGFLT001", 8);
	flight->version = 1;
	flight->size = sizeof(*flight);
	flight->cpu_capacity = FL_CPUS;
	flight->task_capacity = FL_TASKS;
	proc = proc_create_single("fogos-flightlog", 0400, NULL, fl_show);
	if (!proc) {
		kfree(flight);
		return -ENOMEM;
	}
	strscpy(dump_region.name, "FOGOSFLT", sizeof(dump_region.name));
	dump_region.virt_addr = (uintptr_t)flight;
	dump_region.phys_addr = virt_to_phys(flight);
	dump_region.size = sizeof(*flight);
	result = msm_minidump_add_region(&dump_region);
	pr_info("fogos-flight: minidump registration=%d bytes=%zu; export depends on firmware\n",
		result, sizeof(*flight));
	thread = kthread_run(fl_thread, NULL, "fogos-flightlog");
	if (IS_ERR(thread)) {
		if (result >= 0)
			msm_minidump_remove_region(&dump_region);
		proc_remove(proc);
		kfree(flight);
		return PTR_ERR(thread);
	}
	/* Async delivery avoids blocking boot waiting for another CPU. The
 * module has no unload entry: callbacks and minidump storage stay valid.
 */
	for_each_online_cpu(cpu) {
		if (cpu >= FL_CPUS)
			continue;
		INIT_CSD(&timers[cpu].start_call, fl_start_cpu, NULL);
		result = smp_call_function_single_async(cpu, &timers[cpu].start_call);
		WRITE_ONCE(flight->cpus[cpu].start_result, result);
	}
	pr_info("fogos-flight: armed; 250ms per-CPU hard timers, 1s heartbeat printk, bounded task stacks, stop at uptime40s\n");
	return 0;
}
module_init(fl_init);
MODULE_DESCRIPTION("Fogos opt-in boot flight recorder");
MODULE_LICENSE("GPL");
