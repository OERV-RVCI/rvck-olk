/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SPE_H
#define __SPE_H

#define SPE_BUFFER_MAX_SIZE		(PAGE_SIZE)
#define SPE_BUFFER_SIZE			(PAGE_SIZE / 32)

#define SPE_SAMPLE_PERIOD		1024

#define SPE_RECORD_BUFFER_MAX_RECORDS	(100)
#define SPE_RECORD_ENTRY_SIZE		sizeof(struct mem_sampling_record)
#define ARMV8_SPE_MEM_SAMPLING_PDEV_NAME "arm,mm_spe,spe-v1"

struct mm_spe {
	struct pmu			pmu;
	struct platform_device		*pdev;
	cpumask_t			supported_cpus;
	struct hlist_node		hotplug_node;
	int				irq; /* PPI */
	u16				pmsver;
	u16				min_period;
	u16				counter_sz;
	u64				features;
	u16				max_record_sz;
	u16				align;
	u64				sample_period;
	local64_t			period_left;
	bool				jitter;
	bool				load_filter;
	bool				store_filter;
	bool				branch_filter;
	u64				inv_event_filter;
	u16				min_latency;
	u64				event_filter;
	bool				ts_enable;
	bool				pa_enable;
	u8				pct_enable;
	bool				exclude_user;
	bool				exclude_kernel;
};

struct mm_spe_buf {
	void				*cur;		/* for spe raw data buffer */
	int				size;
	int				period;
	void				*base;

	void				*record_base;	/* for spe record buffer */
	int				record_size;
	int				nr_records;
};

#ifdef CONFIG_ARM_SPE_MEM_SAMPLING
void mm_spe_add_probe_status(void);
int mm_spe_percpu_buffer_alloc(int cpu);
int mm_spe_buffer_alloc(void);
void mm_spe_percpu_buffer_free(int cpu);
void mm_spe_buffer_free(void);
struct mm_spe *mm_spe_get_desc(void);
#else
static inline void mm_spe_add_probe_status(void) { }
static inline int mm_spe_percpu_buffer_alloc(int cpu) { return 0; }
static inline int mm_spe_buffer_alloc(void) { return 0; }
static inline void mm_spe_percpu_buffer_free(int cpu) { }
static inline void mm_spe_buffer_free(void) { }
static inline struct mm_spe *mm_spe_get_desc(void) { return NULL; }
#endif
#endif /* __SPE_H */
