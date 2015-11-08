/*
 *  linux/mm/oom_kill.c
 * 
 *  Copyright (C)  1998,2000  Rik van Riel
 *	Thanks go out to Claus Fischer for some serious inspiration and
 *	for goading me into coding this file...
 *  Copyright (C)  2010  Google, Inc.
 *	Rewritten by David Rientjes
 *
 *  The routines in this file are used to kill a process when
 *  we're seriously out of memory. This gets called from __alloc_pages()
 *  in mm/page_alloc.c when we really run out of memory.
 *
 *  Since we won't call these routines often (on a well-configured
 *  machine) this file will double as a 'coding guide' and a signpost
 *  for newbie kernel hackers. It features several pointers to major
 *  kernel subsystems and hints as to where to find out what things do.
 */

#include <linux/oom.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/timex.h>
#include <linux/jiffies.h>
#include <linux/cpuset.h>
#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/security.h>
#include <linux/ptrace.h>
#include <linux/freezer.h>
#include <linux/ftrace.h>
#include <linux/ratelimit.h>

#define CREATE_TRACE_POINTS
#include <trace/events/oom.h>

int sysctl_panic_on_oom;
int sysctl_oom_kill_allocating_task;
int sysctl_oom_dump_tasks = 1;
static DEFINE_SPINLOCK(zone_scan_lock);

#ifdef CONFIG_NUMA
static bool has_intersects_mems_allowed(struct task_struct *tsk,
					const nodemask_t *mask)
{
	struct task_struct *start = tsk;

	do {
		if (mask) {
			if (mempolicy_nodemask_intersects(tsk, mask))
				return true;
		} else {
			if (cpuset_mems_allowed_intersects(current, tsk))
				return true;
		}
	} while_each_thread(start, tsk);

	return false;
}
#else
static bool has_intersects_mems_allowed(struct task_struct *tsk,
					const nodemask_t *mask)
{
	return true;
}
#endif 

struct task_struct *find_lock_task_mm(struct task_struct *p)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (likely(t->mm))
			return t;
		task_unlock(t);
	} while_each_thread(p, t);

	return NULL;
}

static bool oom_unkillable_task(struct task_struct *p,
		const struct mem_cgroup *memcg, const nodemask_t *nodemask)
{
	if (is_global_init(p))
		return true;
	if (p->flags & PF_KTHREAD)
		return true;

	
	if (memcg && !task_in_mem_cgroup(p, memcg))
		return true;

	
	if (!has_intersects_mems_allowed(p, nodemask))
		return true;

	return false;
}

unsigned long oom_badness(struct task_struct *p, struct mem_cgroup *memcg,
			  const nodemask_t *nodemask, unsigned long totalpages)
{
	long points;
	long adj;

	if (oom_unkillable_task(p, memcg, nodemask))
		return 0;

	p = find_lock_task_mm(p);
	if (!p)
		return 0;

	adj = (long)p->signal->oom_score_adj;
	if (adj == OOM_SCORE_ADJ_MIN) {
		task_unlock(p);
		return 0;
	}

	points = get_mm_rss(p->mm) + p->mm->nr_ptes +
		 get_mm_counter(p->mm, MM_SWAPENTS);
	task_unlock(p);

	if (has_capability_noaudit(p, CAP_SYS_ADMIN))
		points -= (points * 3) / 100;

	
	adj *= totalpages / 1000;
	points += adj;

	return points > 0 ? points : 1;
}

#ifdef CONFIG_NUMA
static enum oom_constraint constrained_alloc(struct zonelist *zonelist,
				gfp_t gfp_mask, nodemask_t *nodemask,
				unsigned long *totalpages)
{
	struct zone *zone;
	struct zoneref *z;
	enum zone_type high_zoneidx = gfp_zone(gfp_mask);
	bool cpuset_limited = false;
	int nid;

	
	*totalpages = totalram_pages + total_swap_pages;

	if (!zonelist)
		return CONSTRAINT_NONE;
	if (gfp_mask & __GFP_THISNODE)
		return CONSTRAINT_NONE;

	if (nodemask && !nodes_subset(node_states[N_MEMORY], *nodemask)) {
		*totalpages = total_swap_pages;
		for_each_node_mask(nid, *nodemask)
			*totalpages += node_spanned_pages(nid);
		return CONSTRAINT_MEMORY_POLICY;
	}

	/* Check this allocation failure is caused by cpuset's wall function */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
			high_zoneidx, nodemask)
		if (!cpuset_zone_allowed_softwall(zone, gfp_mask))
			cpuset_limited = true;

	if (cpuset_limited) {
		*totalpages = total_swap_pages;
		for_each_node_mask(nid, cpuset_current_mems_allowed)
			*totalpages += node_spanned_pages(nid);
		return CONSTRAINT_CPUSET;
	}
	return CONSTRAINT_NONE;
}
#else
static enum oom_constraint constrained_alloc(struct zonelist *zonelist,
				gfp_t gfp_mask, nodemask_t *nodemask,
				unsigned long *totalpages)
{
	*totalpages = totalram_pages + total_swap_pages;
	return CONSTRAINT_NONE;
}
#endif

enum oom_scan_t oom_scan_process_thread(struct task_struct *task,
		unsigned long totalpages, const nodemask_t *nodemask,
		bool force_kill)
{
	if (task->exit_state)
		return OOM_SCAN_CONTINUE;
	if (oom_unkillable_task(task, NULL, nodemask))
		return OOM_SCAN_CONTINUE;

	if (test_tsk_thread_flag(task, TIF_MEMDIE)) {
		if (unlikely(frozen(task)))
			__thaw_task(task);
		if (!force_kill)
			return OOM_SCAN_ABORT;
	}
	if (!task->mm)
		return OOM_SCAN_CONTINUE;

	if (oom_task_origin(task))
		return OOM_SCAN_SELECT;

	if (task->flags & PF_EXITING && !force_kill) {
		if (!(task->group_leader->ptrace & PT_TRACE_EXIT))
			return OOM_SCAN_ABORT;
	}
	return OOM_SCAN_OK;
}

static struct task_struct *select_bad_process(unsigned int *ppoints,
		unsigned long totalpages, const nodemask_t *nodemask,
		bool force_kill)
{
	struct task_struct *g, *p;
	struct task_struct *chosen = NULL;
	unsigned long chosen_points = 0;

	rcu_read_lock();
	do_each_thread(g, p) {
		unsigned int points;

		switch (oom_scan_process_thread(p, totalpages, nodemask,
						force_kill)) {
		case OOM_SCAN_SELECT:
			chosen = p;
			chosen_points = ULONG_MAX;
			/* fall through */
		case OOM_SCAN_CONTINUE:
			continue;
		case OOM_SCAN_ABORT:
			rcu_read_unlock();
			return ERR_PTR(-1UL);
		case OOM_SCAN_OK:
			break;
		};
		points = oom_badness(p, NULL, nodemask, totalpages);
		if (points > chosen_points) {
			chosen = p;
			chosen_points = points;
		}
	} while_each_thread(g, p);
	if (chosen)
		get_task_struct(chosen);
	rcu_read_unlock();

	*ppoints = chosen_points * 1000 / totalpages;
	return chosen;
}

void dump_tasks(const struct mem_cgroup *memcg, const nodemask_t *nodemask)
{
	struct task_struct *p;
	struct task_struct *task;

	pr_info("[ pid ]   uid  tgid total_vm      rss nr_ptes swapents oom_score_adj name\n");
	rcu_read_lock();
	for_each_process(p) {
		if (oom_unkillable_task(p, memcg, nodemask))
			continue;

		task = find_lock_task_mm(p);
		if (!task) {
			continue;
		}

		pr_info("[%5d] %5d %5d %8lu %8lu %7lu %8lu         %5hd %s\n",
			task->pid, from_kuid(&init_user_ns, task_uid(task)),
			task->tgid, task->mm->total_vm, get_mm_rss(task->mm),
			task->mm->nr_ptes,
			get_mm_counter(task->mm, MM_SWAPENTS),
			task->signal->oom_score_adj, task->comm);
		task_unlock(task);
	}
	rcu_read_unlock();
}

static void dump_header(struct task_struct *p, gfp_t gfp_mask, int order,
			struct mem_cgroup *memcg, const nodemask_t *nodemask)
{
	task_lock(current);
	pr_warning("%s invoked oom-killer: gfp_mask=0x%x, order=%d, "
		"oom_score_adj=%hd\n",
		current->comm, gfp_mask, order,
		current->signal->oom_score_adj);
	cpuset_print_task_mems_allowed(current);
	task_unlock(current);
	dump_stack();
	if (memcg)
		mem_cgroup_print_oom_info(memcg, p);
	else
		show_mem(SHOW_MEM_FILTER_NODES);
	if (sysctl_oom_dump_tasks)
		dump_tasks(memcg, nodemask);
}

#define K(x) ((x) << (PAGE_SHIFT-10))
void oom_kill_process(struct task_struct *p, gfp_t gfp_mask, int order,
		      unsigned int points, unsigned long totalpages,
		      struct mem_cgroup *memcg, nodemask_t *nodemask,
		      const char *message)
{
	struct task_struct *victim = p;
	struct task_struct *child;
	struct task_struct *t = p;
	struct mm_struct *mm;
	unsigned int victim_points = 0;
	static DEFINE_RATELIMIT_STATE(oom_rs, DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);

	if (p->flags & PF_EXITING) {
		set_tsk_thread_flag(p, TIF_MEMDIE);
		put_task_struct(p);
		return;
	}

	if (__ratelimit(&oom_rs))
		dump_header(p, gfp_mask, order, memcg, nodemask);

	task_lock(p);
	pr_err("%s: Kill process %d (%s) score %d or sacrifice child\n",
		message, task_pid_nr(p), p->comm, points);
	task_unlock(p);

	read_lock(&tasklist_lock);
	do {
		list_for_each_entry(child, &t->children, sibling) {
			unsigned int child_points;

			if (child->mm == p->mm)
				continue;
			child_points = oom_badness(child, memcg, nodemask,
								totalpages);
			if (child_points > victim_points) {
				put_task_struct(victim);
				victim = child;
				victim_points = child_points;
				get_task_struct(victim);
			}
		}
	} while_each_thread(p, t);
	read_unlock(&tasklist_lock);

	rcu_read_lock();
	p = find_lock_task_mm(victim);
	if (!p) {
		rcu_read_unlock();
		put_task_struct(victim);
		return;
	} else if (victim != p) {
		get_task_struct(p);
		put_task_struct(victim);
		victim = p;
	}

	/* mm cannot safely be dereferenced after task_unlock(victim) */
	mm = victim->mm;
	pr_err("Killed process %d (%s) total-vm:%lukB, anon-rss:%lukB, file-rss:%lukB\n",
		task_pid_nr(victim), victim->comm, K(victim->mm->total_vm),
		K(get_mm_counter(victim->mm, MM_ANONPAGES)),
		K(get_mm_counter(victim->mm, MM_FILEPAGES)));
	task_unlock(victim);

	for_each_process(p)
		if (p->mm == mm && !same_thread_group(p, victim) &&
		    !(p->flags & PF_KTHREAD)) {
			if (p->signal->oom_score_adj == OOM_SCORE_ADJ_MIN)
				continue;

			task_lock(p);	
			pr_err("Kill process %d (%s) sharing same memory\n",
				task_pid_nr(p), p->comm);
			task_unlock(p);
			do_send_sig_info(SIGKILL, SEND_SIG_FORCED, p, true);
		}
	rcu_read_unlock();

	set_tsk_thread_flag(victim, TIF_MEMDIE);
	do_send_sig_info(SIGKILL, SEND_SIG_FORCED, victim, true);
	put_task_struct(victim);
}
#undef K

void check_panic_on_oom(enum oom_constraint constraint, gfp_t gfp_mask,
			int order, const nodemask_t *nodemask)
{
	if (likely(!sysctl_panic_on_oom))
		return;
	if (sysctl_panic_on_oom != 2) {
		if (constraint != CONSTRAINT_NONE)
			return;
	}
	dump_header(NULL, gfp_mask, order, NULL, nodemask);
	panic("Out of memory: %s panic_on_oom is enabled\n",
		sysctl_panic_on_oom == 2 ? "compulsory" : "system-wide");
}

static BLOCKING_NOTIFIER_HEAD(oom_notify_list);

int register_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_oom_notifier);

int unregister_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_oom_notifier);

int try_set_zonelist_oom(struct zonelist *zonelist, gfp_t gfp_mask)
{
	struct zoneref *z;
	struct zone *zone;
	int ret = 1;

	spin_lock(&zone_scan_lock);
	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask)) {
		if (zone_is_oom_locked(zone)) {
			ret = 0;
			goto out;
		}
	}

	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask)) {
		zone_set_flag(zone, ZONE_OOM_LOCKED);
	}

out:
	spin_unlock(&zone_scan_lock);
	return ret;
}

void clear_zonelist_oom(struct zonelist *zonelist, gfp_t gfp_mask)
{
	struct zoneref *z;
	struct zone *zone;

	spin_lock(&zone_scan_lock);
	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask)) {
		zone_clear_flag(zone, ZONE_OOM_LOCKED);
	}
	spin_unlock(&zone_scan_lock);
}

void out_of_memory(struct zonelist *zonelist, gfp_t gfp_mask,
		int order, nodemask_t *nodemask, bool force_kill)
{
	const nodemask_t *mpol_mask;
	struct task_struct *p;
	unsigned long totalpages;
	unsigned long freed = 0;
	unsigned int uninitialized_var(points);
	enum oom_constraint constraint = CONSTRAINT_NONE;
	int killed = 0;

	blocking_notifier_call_chain(&oom_notify_list, 0, &freed);
	if (freed > 0)
		
		return;

	if (fatal_signal_pending(current) || current->flags & PF_EXITING) {
		set_thread_flag(TIF_MEMDIE);
		return;
	}

	constraint = constrained_alloc(zonelist, gfp_mask, nodemask,
						&totalpages);
	mpol_mask = (constraint == CONSTRAINT_MEMORY_POLICY) ? nodemask : NULL;
	check_panic_on_oom(constraint, gfp_mask, order, mpol_mask);

	if (sysctl_oom_kill_allocating_task && current->mm &&
	    !oom_unkillable_task(current, NULL, nodemask) &&
	    current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN) {
		get_task_struct(current);
		oom_kill_process(current, gfp_mask, order, 0, totalpages, NULL,
				 nodemask,
				 "Out of memory (oom_kill_allocating_task)");
		goto out;
	}

	p = select_bad_process(&points, totalpages, mpol_mask, force_kill);
	/* Found nothing?!?! Either we hang forever, or we panic. */
	if (!p) {
		dump_header(NULL, gfp_mask, order, NULL, mpol_mask);
		panic("Out of memory and no killable processes...\n");
	}
	if (PTR_ERR(p) != -1UL) {
		oom_kill_process(p, gfp_mask, order, points, totalpages, NULL,
				 nodemask, "Out of memory");
		killed = 1;
	}
out:
	if (killed)
		schedule_timeout_killable(1);
}

void pagefault_out_of_memory(void)
{
	struct zonelist *zonelist = node_zonelist(first_online_node,
						  GFP_KERNEL);

	if (try_set_zonelist_oom(zonelist, GFP_KERNEL)) {
		out_of_memory(NULL, 0, 0, NULL, false);
		clear_zonelist_oom(zonelist, GFP_KERNEL);
	}
}
