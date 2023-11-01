/*
 * fs/dax.c - Direct Access filesystem code
 * Copyright (c) 2013-2014 Intel Corporation
 * Author: Matthew Wilcox <matthew.r.wilcox@intel.com>
 * Author: Ross Zwisler <ross.zwisler@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pagevec.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/uio.h>
#include <linux/vmstat.h>
#include <linux/pfn_t.h>
#include <linux/sizes.h>
#include <linux/mmu_notifier.h>
#include <linux/iomap.h>
#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/fs_dax.h>

/* We choose 4096 entries - same as per-zone page wait tables */
#define DAX_WAIT_TABLE_BITS 12
#define DAX_WAIT_TABLE_ENTRIES (1 << DAX_WAIT_TABLE_BITS)

/* The 'colour' (ie low bits) within a PMD of a page offset.  */
#define PG_PMD_COLOUR	((PMD_SIZE >> PAGE_SHIFT) - 1)
#define PG_PMD_NR	(PMD_SIZE >> PAGE_SHIFT)

static wait_queue_head_t wait_table[DAX_WAIT_TABLE_ENTRIES];

static int __init init_dax_wait_table(void)
{
	int i;

	for (i = 0; i < DAX_WAIT_TABLE_ENTRIES; i++)
		init_waitqueue_head(wait_table + i);
	return 0;
}
fs_initcall(init_dax_wait_table);

/*
 * We use lowest available bit in exceptional entry for locking, one bit for
 * the entry size (PMD) and two more to tell us if the entry is a zero page or
 * an empty entry that is just used for locking.  In total four special bits.
 *
 * If the PMD bit isn't set the entry has size PAGE_SIZE, and if the ZERO_PAGE
 * and EMPTY bits aren't set the entry is a normal DAX entry with a filesystem
 * block allocation.
 */
#define RADIX_DAX_SHIFT		(RADIX_TREE_EXCEPTIONAL_SHIFT + 4)
#define RADIX_DAX_ENTRY_LOCK	(1 << RADIX_TREE_EXCEPTIONAL_SHIFT)
#define RADIX_DAX_PMD		(1 << (RADIX_TREE_EXCEPTIONAL_SHIFT + 1))
#define RADIX_DAX_ZERO_PAGE	(1 << (RADIX_TREE_EXCEPTIONAL_SHIFT + 2))
#define RADIX_DAX_EMPTY		(1 << (RADIX_TREE_EXCEPTIONAL_SHIFT + 3))

static unsigned long dax_radix_pfn(void *entry)
{
	return (unsigned long)entry >> RADIX_DAX_SHIFT;
}

static void *dax_radix_locked_entry(unsigned long pfn, unsigned long flags)
{
	return (void *)(RADIX_TREE_EXCEPTIONAL_ENTRY | flags |
			(pfn << RADIX_DAX_SHIFT) | RADIX_DAX_ENTRY_LOCK);
}

static unsigned int dax_radix_order(void *entry)
{
	if ((unsigned long)entry & RADIX_DAX_PMD)
		return PMD_SHIFT - PAGE_SHIFT;
	return 0;
}

static int dax_is_pmd_entry(void *entry)
{
	return (unsigned long)entry & RADIX_DAX_PMD;
}

static int dax_is_pte_entry(void *entry)
{
	return !((unsigned long)entry & RADIX_DAX_PMD);
}

static int dax_is_zero_entry(void *entry)
{
	return (unsigned long)entry & RADIX_DAX_ZERO_PAGE;
}

static int dax_is_empty_entry(void *entry)
{
	return (unsigned long)entry & RADIX_DAX_EMPTY;
}

/*
 * DAX radix tree locking
 */
struct exceptional_entry_key {
	struct address_space *mapping;
	pgoff_t entry_start;
};

struct wait_exceptional_entry_queue {
	wait_queue_entry_t wait;
	struct exceptional_entry_key key;
};

static wait_queue_head_t *dax_entry_waitqueue(struct address_space *mapping,
		pgoff_t index, void *entry, struct exceptional_entry_key *key)
{
	unsigned long hash;

	/*
	 * If 'entry' is a PMD, align the 'index' that we use for the wait
	 * queue to the start of that PMD.  This ensures that all offsets in
	 * the range covered by the PMD map to the same bit lock.
	 */
	if (dax_is_pmd_entry(entry))
		index &= ~PG_PMD_COLOUR;

	key->mapping = mapping;
	key->entry_start = index;

	hash = hash_long((unsigned long)mapping ^ index, DAX_WAIT_TABLE_BITS);
	return wait_table + hash;
}

static int wake_exceptional_entry_func(wait_queue_entry_t *wait, unsigned int mode,
				       int sync, void *keyp)
{
	struct exceptional_entry_key *key = keyp;
	struct wait_exceptional_entry_queue *ewait =
		container_of(wait, struct wait_exceptional_entry_queue, wait);

	if (key->mapping != ewait->key.mapping ||
	    key->entry_start != ewait->key.entry_start)
		return 0;
	return autoremove_wake_function(wait, mode, sync, NULL);
}

/*
 * @entry may no longer be the entry at the index in the mapping.
 * The important information it's conveying is whether the entry at
 * this index used to be a PMD entry.
 */
static void dax_wake_mapping_entry_waiter(struct address_space *mapping,
		pgoff_t index, void *entry, bool wake_all)
{
	struct exceptional_entry_key key;
	wait_queue_head_t *wq;

	wq = dax_entry_waitqueue(mapping, index, entry, &key);

	/*
	 * Checking for locked entry and prepare_to_wait_exclusive() happens
	 * under the i_pages lock, ditto for entry handling in our callers.
	 * So at this point all tasks that could have seen our entry locked
	 * must be in the waitqueue and the following check will see them.
	 */
	if (waitqueue_active(wq))
		__wake_up(wq, TASK_NORMAL, wake_all ? 0 : 1, &key);
}

/*
 * Check whether the given slot is locked.  Must be called with the i_pages
 * lock held.
 */
static inline int slot_locked(struct address_space *mapping, void **slot)
{
	unsigned long entry = (unsigned long)
		radix_tree_deref_slot_protected(slot, &mapping->i_pages.xa_lock);
	return entry & RADIX_DAX_ENTRY_LOCK;
}

/*
 * Mark the given slot as locked.  Must be called with the i_pages lock held.
 */
static inline void *lock_slot(struct address_space *mapping, void **slot)
{
	unsigned long entry = (unsigned long)
		radix_tree_deref_slot_protected(slot, &mapping->i_pages.xa_lock);

	entry |= RADIX_DAX_ENTRY_LOCK;
	radix_tree_replace_slot(&mapping->i_pages, slot, (void *)entry);
	return (void *)entry;
}

/*
 * Mark the given slot as unlocked.  Must be called with the i_pages lock held.
 */
static inline void *unlock_slot(struct address_space *mapping, void **slot)
{
	unsigned long entry = (unsigned long)
		radix_tree_deref_slot_protected(slot, &mapping->i_pages.xa_lock);

	entry &= ~(unsigned long)RADIX_DAX_ENTRY_LOCK;
	radix_tree_replace_slot(&mapping->i_pages, slot, (void *)entry);
	return (void *)entry;
}

static void put_unlocked_mapping_entry(struct address_space *mapping,
				       pgoff_t index, void *entry);

/*
 * Lookup entry in radix tree, wait for it to become unlocked if it is
 * exceptional entry and return it. The caller must call
 * put_unlocked_mapping_entry() when he decided not to lock the entry or
 * put_locked_mapping_entry() when he locked the entry and now wants to
 * unlock it.
 *
 * Must be called with the i_pages lock held.
 */
static void *get_unlocked_mapping_entry(struct address_space *mapping,
		pgoff_t index, void ***slotp)
{
	void *entry, **slot;
	struct wait_exceptional_entry_queue ewait;
	wait_queue_head_t *wq;

	init_wait(&ewait.wait);
	ewait.wait.func = wake_exceptional_entry_func;

	for (;;) {
		entry = __radix_tree_lookup(&mapping->i_pages, index, NULL,
					  &slot);
		if (!entry ||
		    WARN_ON_ONCE(!radix_tree_exceptional_entry(entry)) ||
		    !slot_locked(mapping, slot)) {
			if (slotp)
				*slotp = slot;
			return entry;
		}

		wq = dax_entry_waitqueue(mapping, index, entry, &ewait.key);
		prepare_to_wait_exclusive(wq, &ewait.wait,
					  TASK_UNINTERRUPTIBLE);
		xa_unlock_irq(&mapping->i_pages);
		schedule();
		finish_wait(wq, &ewait.wait);
		xa_lock_irq(&mapping->i_pages);
	}
}

/*
 * The only thing keeping the address space around is the i_pages lock
 * (it's cycled in clear_inode() after removing the entries from i_pages)
 * After we call xas_unlock_irq(), we cannot touch xas->xa.
 */
static void wait_entry_unlocked(struct address_space *mapping, pgoff_t index,
		void ***slotp, void *entry)
{
	struct wait_exceptional_entry_queue ewait;
	wait_queue_head_t *wq;

	init_wait(&ewait.wait);
	ewait.wait.func = wake_exceptional_entry_func;

	wq = dax_entry_waitqueue(mapping, index, entry, &ewait.key);
	/*
	 * Unlike get_unlocked_entry() there is no guarantee that this
	 * path ever successfully retrieves an unlocked entry before an
	 * inode dies. Perform a non-exclusive wait in case this path
	 * never successfully performs its own wake up.
	 */
	prepare_to_wait(wq, &ewait.wait, TASK_UNINTERRUPTIBLE);
	xa_unlock_irq(&mapping->i_pages);
	schedule();
	finish_wait(wq, &ewait.wait);
}

static void unlock_mapping_entry(struct address_space *mapping, pgoff_t index)
{
	void *entry, **slot;

	xa_lock_irq(&mapping->i_pages);
	entry = __radix_tree_lookup(&mapping->i_pages, index, NULL, &slot);
	if (WARN_ON_ONCE(!entry || !radix_tree_exceptional_entry(entry) ||
			 !slot_locked(mapping, slot))) {
		xa_unlock_irq(&mapping->i_pages);
		return;
	}
	unlock_slot(mapping, slot);
	xa_unlock_irq(&mapping->i_pages);
	dax_wake_mapping_entry_waiter(mapping, index, entry, false);
}

static void put_locked_mapping_entry(struct address_space *mapping,
		pgoff_t index)
{
	unlock_mapping_entry(mapping, index);
}

/*
 * Called when we are done with radix tree entry we looked up via
 * get_unlocked_mapping_entry() and which we didn't lock in the end.
 */
static void put_unlocked_mapping_entry(struct address_space *mapping,
				       pgoff_t index, void *entry)
{
	if (!entry)
		return;

	/* We have to wake up next waiter for the radix tree entry lock */
	dax_wake_mapping_entry_waiter(mapping, index, entry, false);
}

static unsigned long dax_entry_size(void *entry)
{
	if (dax_is_zero_entry(entry))
		return 0;
	else if (dax_is_empty_entry(entry))
		return 0;
	else if (dax_is_pmd_entry(entry))
		return PMD_SIZE;
	else
		return PAGE_SIZE;
}

static unsigned long dax_radix_end_pfn(void *entry)
{
	return dax_radix_pfn(entry) + dax_entry_size(entry) / PAGE_SIZE;
}

/*
 * Iterate through all mapped pfns represented by an entry, i.e. skip
 * 'empty' and 'zero' entries.
 */
#define for_each_mapped_pfn(entry, pfn) \
	for (pfn = dax_radix_pfn(entry); \
			pfn < dax_radix_end_pfn(entry); pfn++)

/*
 * TODO: for reflink+dax we need a way to associate a single page with
 * multiple address_space instances at different linear_page_index()
 * offsets.
 */
static void dax_associate_entry(void *entry, struct address_space *mapping,
		struct vm_area_struct *vma, unsigned long address)
{
	unsigned long size = dax_entry_size(entry), pfn, index;
	int i = 0;

	if (IS_ENABLED(CONFIG_FS_DAX_LIMITED))
		return;

	index = linear_page_index(vma, address & ~(size - 1));
	for_each_mapped_pfn(entry, pfn) {
		struct page *page = pfn_to_page(pfn);

		WARN_ON_ONCE(page->mapping);
		page->mapping = mapping;
		page->index = index + i++;
	}
}

static void dax_disassociate_entry(void *entry, struct address_space *mapping,
		bool trunc)
{
	unsigned long pfn;

	if (IS_ENABLED(CONFIG_FS_DAX_LIMITED))
		return;

	for_each_mapped_pfn(entry, pfn) {
		struct page *page = pfn_to_page(pfn);

		WARN_ON_ONCE(trunc && page_ref_count(page) > 1);
		WARN_ON_ONCE(page->mapping && page->mapping != mapping);
		page->mapping = NULL;
		page->index = 0;
	}
}

static struct page *dax_busy_page(void *entry)
{
	unsigned long pfn;

	for_each_mapped_pfn(entry, pfn) {
		struct page *page = pfn_to_page(pfn);

		if (page_ref_count(page) > 1)
			return page;
	}
	return NULL;
}

bool dax_lock_mapping_entry(struct page *page)
{
	pgoff_t index;
	struct inode *inode;
	bool did_lock = false;
	void *entry = NULL, **slot;
	struct address_space *mapping;

	rcu_read_lock();
	for (;;) {
		mapping = READ_ONCE(page->mapping);

		if (!mapping || !dax_mapping(mapping))
			break;

		/*
		 * In the device-dax case there's no need to lock, a
		 * struct dev_pagemap pin is sufficient to keep the
		 * inode alive, and we assume we have dev_pagemap pin
		 * otherwise we would not have a valid pfn_to_page()
		 * translation.
		 */
		inode = mapping->host;
		if (S_ISCHR(inode->i_mode)) {
			did_lock = true;
			break;
		}

		xa_lock_irq(&mapping->i_pages);
		if (mapping != page->mapping) {
			xa_unlock_irq(&mapping->i_pages);
			continue;
		}
		index = page->index;

		entry = __radix_tree_lookup(&mapping->i_pages, index,
						NULL, &slot);
		if (!entry) {
			xa_unlock_irq(&mapping->i_pages);
			break;
		} else if (slot_locked(mapping, slot)) {
			rcu_read_unlock();
			wait_entry_unlocked(mapping, index, &slot, entry);
			rcu_read_lock();
			continue;
		}
		lock_slot(mapping, slot);
		did_lock = true;
		xa_unlock_irq(&mapping->i_pages);
		break;
	}
	rcu_read_unlock();

	return did_lock;
}

void dax_unlock_mapping_entry(struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;

	if (S_ISCHR(inode->i_mode))
		return;

	unlock_mapping_entry(mapping, page->index);
}

/*
 * Find radix tree entry at given index. If it points to an exceptional entry,
 * return it with the radix tree entry locked. If the radix tree doesn't
 * contain given index, create an empty exceptional entry for the index and
 * return with it locked.
 *
 * When requesting an entry with size RADIX_DAX_PMD, grab_mapping_entry() will
 * either return that locked entry or will return an error.  This error will
 * happen if there are any 4k entries within the 2MiB range that we are
 * requesting.
 *
 * We always favor 4k entries over 2MiB entries. There isn't a flow where we
 * evict 4k entries in order to 'upgrade' them to a 2MiB entry.  A 2MiB
 * insertion will fail if it finds any 4k entries already in the tree, and a
 * 4k insertion will cause an existing 2MiB entry to be unmapped and
 * downgraded to 4k entries.  This happens for both 2MiB huge zero pages as
 * well as 2MiB empty entries.
 *
 * The exception to this downgrade path is for 2MiB DAX PMD entries that have
 * real storage backing them.  We will leave these real 2MiB DAX entries in
 * the tree, and PTE writes will simply dirty the entire 2MiB DAX entry.
 *
 * Note: Unlike filemap_fault() we don't honor FAULT_FLAG_RETRY flags. For
 * persistent memory the benefit is doubtful. We can add that later if we can
 * show it helps.
 */
static void *grab_mapping_entry(struct address_space *mapping, pgoff_t index,
		unsigned long size_flag)
{
	bool pmd_downgrade = false; /* splitting 2MiB entry into 4k entries? */
	void *entry, **slot;

restart:
	xa_lock_irq(&mapping->i_pages);
	entry = get_unlocked_mapping_entry(mapping, index, &slot);

	if (WARN_ON_ONCE(entry && !radix_tree_exceptional_entry(entry))) {
		entry = ERR_PTR(-EIO);
		goto out_unlock;
	}

	if (entry) {
		if (size_flag & RADIX_DAX_PMD) {
			if (dax_is_pte_entry(entry)) {
				put_unlocked_mapping_entry(mapping, index,
						entry);
				entry = ERR_PTR(-EEXIST);
				goto out_unlock;
			}
		} else { /* trying to grab a PTE entry */
			if (dax_is_pmd_entry(entry) &&
			    (dax_is_zero_entry(entry) ||
			     dax_is_empty_entry(entry))) {
				pmd_downgrade = true;
			}
		}
	}

	/* No entry for given index? Make sure radix tree is big enough. */
	if (!entry || pmd_downgrade) {
		int err;

		if (pmd_downgrade) {
			/*
			 * Make sure 'entry' remains valid while we drop
			 * the i_pages lock.
			 */
			entry = lock_slot(mapping, slot);
		}

		xa_unlock_irq(&mapping->i_pages);
		/*
		 * Besides huge zero pages the only other thing that gets
		 * downgraded are empty entries which don't need to be
		 * unmapped.
		 */
		if (pmd_downgrade && dax_is_zero_entry(entry))
			unmap_mapping_pages(mapping, index & ~PG_PMD_COLOUR,
							PG_PMD_NR, false);

		err = radix_tree_preload(
				mapping_gfp_mask(mapping) & ~__GFP_HIGHMEM);
		if (err) {
			if (pmd_downgrade)
				put_locked_mapping_entry(mapping, index);
			return ERR_PTR(err);
		}
		xa_lock_irq(&mapping->i_pages);

		if (!entry) {
			/*
			 * We needed to drop the i_pages lock while calling
			 * radix_tree_preload() and we didn't have an entry to
			 * lock.  See if another thread inserted an entry at
			 * our index during this time.
			 */
			entry = __radix_tree_lookup(&mapping->i_pages, index,
					NULL, &slot);
			if (entry) {
				radix_tree_preload_end();
				xa_unlock_irq(&mapping->i_pages);
				goto restart;
			}
		}

		if (pmd_downgrade) {
			dax_disassociate_entry(entry, mapping, false);
			radix_tree_delete(&mapping->i_pages, index);
			mapping->nrexceptional--;
			dax_wake_mapping_entry_waiter(mapping, index, entry,
					true);
		}

		entry = dax_radix_locked_entry(0, size_flag | RADIX_DAX_EMPTY);

		err = __radix_tree_insert(&mapping->i_pages, index,
				dax_radix_order(entry), entry);
		radix_tree_preload_end();
		if (err) {
			xa_unlock_irq(&mapping->i_pages);
			/*
			 * Our insertion of a DAX entry failed, most likely
			 * because we were inserting a PMD entry and it
			 * collided with a PTE sized entry at a different
			 * index in the PMD range.  We haven't inserted
			 * anything into the radix tree and have no waiters to
			 * wake.
			 */
			return ERR_PTR(err);
		}
		/* Good, we have inserted empty locked entry into the tree. */
		mapping->nrexceptional++;
		xa_unlock_irq(&mapping->i_pages);
		return entry;
	}
	entry = lock_slot(mapping, slot);
 out_unlock:
	xa_unlock_irq(&mapping->i_pages);
	return entry;
}

/**
 * dax_layout_busy_page - find first pinned page in @mapping
 * @mapping: address space to scan for a page with ref count > 1
 *
 * DAX requires ZONE_DEVICE mapped pages. These pages are never
 * 'onlined' to the page allocator so they are considered idle when
 * page->count == 1. A filesystem uses this interface to determine if
 * any page in the mapping is busy, i.e. for DMA, or other
 * get_user_pages() usages.
 *
 * It is expected that the filesystem is holding locks to block the
 * establishment of new mappings in this address_space. I.e. it expects
 * to be able to run unmap_mapping_range() and subsequently not race
 * mapping_mapped() becoming true.
 */
struct page *dax_layout_busy_page(struct address_space *mapping)
{
	pgoff_t	indices[PAGEVEC_SIZE];
	struct page *page = NULL;
	struct pagevec pvec;
	pgoff_t	index, end;
	unsigned i;

	/*
	 * In the 'limited' case get_user_pages() for dax is disabled.
	 */
	if (IS_ENABLED(CONFIG_FS_DAX_LIMITED))
		return NULL;

	if (!dax_mapping(mapping) || !mapping_mapped(mapping))
		return NULL;

	pagevec_init(&pvec);
	index = 0;
	end = -1;

	/*
	 * If we race get_user_pages_fast() here either we'll see the
	 * elevated page count in the pagevec_lookup and wait, or
	 * get_user_pages_fast() will see that the page it took a reference
	 * against is no longer mapped in the page tables and bail to the
	 * get_user_pages() slow path.  The slow path is protected by
	 * pte_lock() and pmd_lock(). New references are not taken without
	 * holding those locks, and unmap_mapping_range() will not zero the
	 * pte or pmd without holding the respective lock, so we are
	 * guaranteed to either see new references or prevent new
	 * references from being established.
	 */
	unmap_mapping_range(mapping, 0, 0, 0);

	while (index < end && pagevec_lookup_entries(&pvec, mapping, index,
				min(end - index, (pgoff_t)PAGEVEC_SIZE),
				indices)) {
		pgoff_t nr_pages = 1;

		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *pvec_ent = pvec.pages[i];
			void *entry;

			index = indices[i];
			if (index >= end)
				break;

			if (WARN_ON_ONCE(
			     !radix_tree_exceptional_entry(pvec_ent)))
				continue;

			xa_lock_irq(&mapping->i_pages);
			entry = get_unlocked_mapping_entry(mapping, index, NULL);
			if (entry) {
				page = dax_busy_page(entry);
				/*
				 * Account for multi-order entries at
				 * the end of the pagevec.
				 */
				if (i + 1 >= pagevec_count(&pvec))
					nr_pages = 1UL << dax_radix_order(entry);
			}
			put_unlocked_mapping_entry(mapping, index, entry);
			xa_unlock_irq(&mapping->i_pages);
			if (page)
				break;
		}

		/*
		 * We don't expect normal struct page entries to exist in our
		 * tree, but we keep these pagevec calls so that this code is
		 * consistent with the common pattern for handling pagevecs
		 * throughout the kernel.
		 */
		pagevec_remove_exceptionals(&pvec);
		pagevec_release(&pvec);
		index += nr_pages;

		if (page)
			break;
	}
	return page;
}
EXPORT_SYMBOL_GPL(dax_layout_busy_page);

static int __dax_invalidate_mapping_entry(struct address_space *mapping,
					  pgoff_t index, bool trunc)
{
	int ret = 0;
	void *entry;
	struct radix_tree_root *pages = &mapping->i_pages;

	xa_lock_irq(pages);
	entry = get_unlocked_mapping_entry(mapping, index, NULL);
	if (!entry || WARN_ON_ONCE(!radix_tree_exceptional_entry(entry)))
		goto out;
	if (!trunc &&
	    (radix_tree_tag_get(pages, index, PAGECACHE_TAG_DIRTY) ||
	     radix_tree_tag_get(pages, index, PAGECACHE_TAG_TOWRITE)))
		goto out;
	dax_disassociate_entry(entry, mapping, trunc);
	radix_tree_delete(pages, index);
	mapping->nrexceptional--;
	ret = 1;
out:
	put_unlocked_mapping_entry(mapping, index, entry);
	xa_unlock_irq(pages);
	return ret;
}
/*
 * Delete exceptional DAX entry at @index from @mapping. Wait for radix tree
 * entry to get unlocked before deleting it.
 */
int dax_delete_mapping_entry(struct address_space *mapping, pgoff_t index)
{
	int ret = __dax_invalidate_mapping_entry(mapping, index, true);

	/*
	 * This gets called from truncate / punch_hole path. As such, the caller
	 * must hold locks protecting against concurrent modifications of the
	 * radix tree (usually fs-private i_mmap_sem for writing). Since the
	 * caller has seen exceptional entry for this index, we better find it
	 * at that index as well...
	 */
	WARN_ON_ONCE(!ret);
	return ret;
}

/*
 * Invalidate exceptional DAX entry if it is clean.
 */
int dax_invalidate_mapping_entry_sync(struct address_space *mapping,
				      pgoff_t index)
{
	return __dax_invalidate_mapping_entry(mapping, index, false);
}

static int copy_user_dax(struct block_device *bdev, struct dax_device *dax_dev,
		sector_t sector, size_t size, struct page *to,
		unsigned long vaddr)
{
	void *vto, *kaddr;
	pgoff_t pgoff;
	long rc;
	int id;

	rc = bdev_dax_pgoff(bdev, sector, size, &pgoff);
	if (rc)
		return rc;

	id = dax_read_lock();
	rc = dax_direct_access(dax_dev, pgoff, PHYS_PFN(size), &kaddr, NULL);
	if (rc < 0) {
		dax_read_unlock(id);
		return rc;
	}
	vto = kmap_atomic(to);
	copy_user_page(vto, (void __force *)kaddr, vaddr, to);
	kunmap_atomic(vto);
	dax_read_unlock(id);
	return 0;
}

/*
 * By this point grab_mapping_entry() has ensured that we have a locked entry
 * of the appropriate size so we don't have to worry about downgrading PMDs to
 * PTEs.  If we happen to be trying to insert a PTE and there is a PMD
 * already in the tree, we will skip the insertion and just dirty the PMD as
 * appropriate.
 */
static void *dax_insert_mapping_entry(struct address_space *mapping,
				      struct vm_fault *vmf,
				      void *entry, pfn_t pfn_t,
				      unsigned long flags, bool dirty)
{
	struct radix_tree_root *pages = &mapping->i_pages;
	unsigned long pfn = pfn_t_to_pfn(pfn_t);
	pgoff_t index = vmf->pgoff;
	void *new_entry;

	if (dirty)
		__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);

	if (dax_is_zero_entry(entry) && !(flags & RADIX_DAX_ZERO_PAGE)) {
		/* we are replacing a zero page with block mapping */
		if (dax_is_pmd_entry(entry))
			unmap_mapping_pages(mapping, index & ~PG_PMD_COLOUR,
							PG_PMD_NR, false);
		else /* pte entry */
			unmap_mapping_pages(mapping, vmf->pgoff, 1, false);
	}

	xa_lock_irq(pages);
	new_entry = dax_radix_locked_entry(pfn, flags);
	if (dax_entry_size(entry) != dax_entry_size(new_entry)) {
		dax_disassociate_entry(entry, mapping, false);
		dax_associate_entry(new_entry, mapping, vmf->vma, vmf->address);
	}

	if (dax_is_zero_entry(entry) || dax_is_empty_entry(entry)) {
		/*
		 * Only swap our new entry into the radix tree if the current
		 * entry is a zero page or an empty entry.  If a normal PTE or
		 * PMD entry is already in the tree, we leave it alone.  This
		 * means that if we are trying to insert a PTE and the
		 * existing entry is a PMD, we will just leave the PMD in the
		 * tree and dirty it if necessary.
		 */
		struct radix_tree_node *node;
		void **slot;
		void *ret;

		ret = __radix_tree_lookup(pages, index, &node, &slot);
		WARN_ON_ONCE(ret != entry);
		__radix_tree_replace(pages, node, slot,
				     new_entry, NULL);
		entry = new_entry;
	}

	if (dirty)
		radix_tree_tag_set(pages, index, PAGECACHE_TAG_DIRTY);

	xa_unlock_irq(pages);
	return entry;
}

static inline unsigned long
pgoff_address(pgoff_t pgoff, struct vm_area_struct *vma)
{
	unsigned long address;

	address = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	VM_BUG_ON_VMA(address < vma->vm_start || address >= vma->vm_end, vma);
	return address;
}

/* Walk all mappings of a given index of a file and writeprotect them */
static void dax_mapping_entry_mkclean(struct address_space *mapping,
				      pgoff_t index, unsigned long pfn)
{
	struct vm_area_struct *vma;
	pte_t pte, *ptep = NULL;
	pmd_t *pmdp = NULL;
	spinlock_t *ptl;

	i_mmap_lock_read(mapping);
	vma_interval_tree_foreach(vma, &mapping->i_mmap, index, index) {
		unsigned long address, start, end;

		cond_resched();

		if (!(vma->vm_flags & VM_SHARED))
			continue;

		address = pgoff_address(index, vma);

		/*
		 * Note because we provide start/end to follow_pte_pmd it will
		 * call mmu_notifier_invalidate_range_start() on our behalf
		 * before taking any lock.
		 */
		if (follow_pte_pmd(vma->vm_mm, address, &start, &end, &ptep, &pmdp, &ptl))
			continue;

		/*
		 * No need to call mmu_notifier_invalidate_range() as we are
		 * downgrading page table protection not changing it to point
		 * to a new page.
		 *
		 * See Documentation/vm/mmu_notifier.rst
		 */
		if (pmdp) {
#ifdef CONFIG_FS_DAX_PMD
			pmd_t pmd;

			if (pfn != pmd_pfn(*pmdp))
				goto unlock_pmd;
			if (!pmd_dirty(*pmdp) && !pmd_write(*pmdp))
				goto unlock_pmd;

			flush_cache_range(vma, address,
					  address + HPAGE_PMD_SIZE);
			pmd = pmdp_invalidate(vma, address, pmdp);
			pmd = pmd_wrprotect(pmd);
			pmd = pmd_mkclean(pmd);
			set_pmd_at(vma->vm_mm, address, pmdp, pmd);
unlock_pmd:
#endif
			spin_unlock(ptl);
		} else {
			if (pfn != pte_pfn(*ptep))
				goto unlock_pte;
			if (!pte_dirty(*ptep) && !pte_write(*ptep))
				goto unlock_pte;

			flush_cache_page(vma, address, pfn);
			pte = ptep_clear_flush(vma, address, ptep);
			pte = pte_wrprotect(pte);
			pte = pte_mkclean(pte);
			set_pte_at(vma->vm_mm, address, ptep, pte);
unlock_pte:
			pte_unmap_unlock(ptep, ptl);
		}

		mmu_notifier_invalidate_range_end(vma->vm_mm, start, end);
	}
	i_mmap_unlock_read(mapping);
}

static int dax_writeback_one(struct dax_device *dax_dev,
		struct address_space *mapping, pgoff_t index, void *entry)
{
	struct radix_tree_root *pages = &mapping->i_pages;
	void *entry2, **slot;
	unsigned long pfn;
	long ret = 0;
	size_t size;

	/*
	 * A page got tagged dirty in DAX mapping? Something is seriously
	 * wrong.
	 */
	if (WARN_ON(!radix_tree_exceptional_entry(entry)))
		return -EIO;

	xa_lock_irq(pages);
	entry2 = get_unlocked_mapping_entry(mapping, index, &slot);
	/* Entry got punched out / reallocated? */
	if (!entry2 || WARN_ON_ONCE(!radix_tree_exceptional_entry(entry2)))
		goto put_unlocked;
	/*
	 * Entry got reallocated elsewhere? No need to writeback. We have to
	 * compare pfns as we must not bail out due to difference in lockbit
	 * or entry type.
	 */
	if (dax_radix_pfn(entry2) != dax_radix_pfn(entry))
		goto put_unlocked;
	if (WARN_ON_ONCE(dax_is_empty_entry(entry) ||
				dax_is_zero_entry(entry))) {
		ret = -EIO;
		goto put_unlocked;
	}

	/* Another fsync thread may have already written back this entry */
	if (!radix_tree_tag_get(pages, index, PAGECACHE_TAG_TOWRITE))
		goto put_unlocked;
	/* Lock the entry to serialize with page faults */
	entry = lock_slot(mapping, slot);
	/*
	 * We can clear the tag now but we have to be careful so that concurrent
	 * dax_writeback_one() calls for the same index cannot finish before we
	 * actually flush the caches. This is achieved as the calls will look
	 * at the entry only under the i_pages lock and once they do that
	 * they will see the entry locked and wait for it to unlock.
	 */
	radix_tree_tag_clear(pages, index, PAGECACHE_TAG_TOWRITE);
	xa_unlock_irq(pages);

	/*
	 * Even if dax_writeback_mapping_range() was given a wbc->range_start
	 * in the middle of a PMD, the 'index' we are given will be aligned to
	 * the start index of the PMD, as will the pfn we pull from 'entry'.
	 * This allows us to flush for PMD_SIZE and not have to worry about
	 * partial PMD writebacks.
	 */
	pfn = dax_radix_pfn(entry);
	size = PAGE_SIZE << dax_radix_order(entry);

	dax_mapping_entry_mkclean(mapping, index, pfn);
	dax_flush(dax_dev, page_address(pfn_to_page(pfn)), size);
	/*
	 * After we have flushed the cache, we can clear the dirty tag. There
	 * cannot be new dirty data in the pfn after the flush has completed as
	 * the pfn mappings are writeprotected and fault waits for mapping
	 * entry lock.
	 */
	xa_lock_irq(pages);
	radix_tree_tag_clear(pages, index, PAGECACHE_TAG_DIRTY);
	xa_unlock_irq(pages);
	trace_dax_writeback_one(mapping->host, index, size >> PAGE_SHIFT);
	put_locked_mapping_entry(mapping, index);
	return ret;

 put_unlocked:
	put_unlocked_mapping_entry(mapping, index, entry2);
	xa_unlock_irq(pages);
	return ret;
}

/*
 * Flush the mapping to the persistent domain within the byte range of [start,
 * end]. This is required by data integrity operations to ensure file data is
 * on persistent storage prior to completion of the operation.
 */
int dax_writeback_mapping_range(struct address_space *mapping,
		struct block_device *bdev, struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	pgoff_t start_index, end_index;
	pgoff_t indices[PAGEVEC_SIZE];
	struct dax_device *dax_dev;
	struct pagevec pvec;
	bool done = false;
	int i, ret = 0;

	if (WARN_ON_ONCE(inode->i_blkbits != PAGE_SHIFT))
		return -EIO;

	if (!mapping->nrexceptional || wbc->sync_mode != WB_SYNC_ALL)
		return 0;

	dax_dev = dax_get_by_host(bdev->bd_disk->disk_name);
	if (!dax_dev)
		return -EIO;

	start_index = wbc->range_start >> PAGE_SHIFT;
	end_index = wbc->range_end >> PAGE_SHIFT;

	trace_dax_writeback_range(inode, start_index, end_index);

	tag_pages_for_writeback(mapping, start_index, end_index);

	pagevec_init(&pvec);
	while (!done) {
		pvec.nr = find_get_entries_tag(mapping, start_index,
				PAGECACHE_TAG_TOWRITE, PAGEVEC_SIZE,
				pvec.pages, indices);

		if (pvec.nr == 0)
			break;

		for (i = 0; i < pvec.nr; i++) {
			if (indices[i] > end_index) {
				done = true;
				break;
			}

			ret = dax_writeback_one(dax_dev, mapping, indices[i],
					pvec.pages[i]);
			if (ret < 0) {
				mapping_set_error(mapping, ret);
				goto out;
			}
		}
		start_index = indices[pvec.nr - 1] + 1;
	}
out:
	put_dax(dax_dev);
	trace_dax_writeback_range_done(inode, start_index, end_index);
	return (ret < 0 ? ret : 0);
}
EXPORT_SYMBOL_GPL(dax_writeback_mapping_range);

static sector_t dax_iomap_sector(struct iomap *iomap, loff_t pos)
{
	return (iomap->addr + (pos & PAGE_MASK) - iomap->offset) >> 9;
}

static int dax_iomap_pfn(struct iomap *iomap, loff_t pos, size_t size,
			 pfn_t *pfnp)
{
	const sector_t sector = dax_iomap_sector(iomap, pos);
	pgoff_t pgoff;
	int id, rc;
	long length;

	rc = bdev_dax_pgoff(iomap->bdev, sector, size, &pgoff);
	if (rc)
		return rc;
	id = dax_read_lock();
	length = dax_direct_access(iomap->dax_dev, pgoff, PHYS_PFN(size),
				   NULL, pfnp);
	if (length < 0) {
		rc = length;
		goto out;
	}
	rc = -EINVAL;
	if (PFN_PHYS(length) < size)
		goto out;
	if (pfn_t_to_pfn(*pfnp) & (PHYS_PFN(size)-1))
		goto out;
	/* For larger pages we need devmap */
	if (length > 1 && !pfn_t_devmap(*pfnp))
		goto out;
	rc = 0;
out:
	dax_read_unlock(id);
	return rc;
}

/*
 * The user has performed a load from a hole in the file.  Allocating a new
 * page in the file would cause excessive storage usage for workloads with
 * sparse files.  Instead we insert a read-only mapping of the 4k zero page.
 * If this page is ever written to we will re-fault and change the mapping to
 * point to real DAX storage instead.
 */
static vm_fault_t dax_load_hole(struct address_space *mapping, void *entry,
			 struct vm_fault *vmf)
{
	struct inode *inode = mapping->host;
	unsigned long vaddr = vmf->address;
	pfn_t pfn = pfn_to_pfn_t(my_zero_pfn(vaddr));
	vm_fault_t ret;

	dax_insert_mapping_entry(mapping, vmf, entry, pfn, RADIX_DAX_ZERO_PAGE,
			false);
	ret = vmf_insert_mixed(vmf->vma, vaddr, pfn);
	trace_dax_load_hole(inode, vmf, ret);
	return ret;
}

static bool dax_range_is_aligned(struct block_device *bdev,
				 unsigned int offset, unsigned int length)
{
	unsigned short sector_size = bdev_logical_block_size(bdev);

	if (!IS_ALIGNED(offset, sector_size))
		return false;
	if (!IS_ALIGNED(length, sector_size))
		return false;

	return true;
}

int __dax_zero_page_range(struct block_device *bdev,
		struct dax_device *dax_dev, sector_t sector,
		unsigned int offset, unsigned int size)
{
	if (dax_range_is_aligned(bdev, offset, size)) {
		sector_t start_sector = sector + (offset >> 9);

		return blkdev_issue_zeroout(bdev, start_sector,
				size >> 9, GFP_NOFS, 0);
	} else {
		pgoff_t pgoff;
		long rc, id;
		void *kaddr;

		rc = bdev_dax_pgoff(bdev, sector, PAGE_SIZE, &pgoff);
		if (rc)
			return rc;

		id = dax_read_lock();
		rc = dax_direct_access(dax_dev, pgoff, 1, &kaddr, NULL);
		if (rc < 0) {
			dax_read_unlock(id);
			return rc;
		}
		memset(kaddr + offset, 0, size);
		dax_flush(dax_dev, kaddr + offset, size);
		dax_read_unlock(id);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(__dax_zero_page_range);

static loff_t
dax_iomap_actor(struct inode *inode, loff_t pos, loff_t length, void *data,
		struct iomap *iomap)
{
	struct block_device *bdev = iomap->bdev;
	struct dax_device *dax_dev = iomap->dax_dev;
	struct iov_iter *iter = data;
	loff_t end = pos + length, done = 0;
	ssize_t ret = 0;
	size_t xfer;
	int id;

	if (iov_iter_rw(iter) == READ) {
		end = min(end, i_size_read(inode));
		if (pos >= end)
			return 0;

		if (iomap->type == IOMAP_HOLE || iomap->type == IOMAP_UNWRITTEN)
			return iov_iter_zero(min(length, end - pos), iter);
	}

	if (WARN_ON_ONCE(iomap->type != IOMAP_MAPPED))
		return -EIO;

	/*
	 * Write can allocate block for an area which has a hole page mapped
	 * into page tables. We have to tear down these mappings so that data
	 * written by write(2) is visible in mmap.
	 */
	if (iomap->flags & IOMAP_F_NEW) {
		invalidate_inode_pages2_range(inode->i_mapping,
					      pos >> PAGE_SHIFT,
					      (end - 1) >> PAGE_SHIFT);
	}

	id = dax_read_lock();
	while (pos < end) {
		unsigned offset = pos & (PAGE_SIZE - 1);
		const size_t size = ALIGN(length + offset, PAGE_SIZE);
		const sector_t sector = dax_iomap_sector(iomap, pos);
		ssize_t map_len;
		pgoff_t pgoff;
		void *kaddr;

		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		ret = bdev_dax_pgoff(bdev, sector, size, &pgoff);
		if (ret)
			break;

		map_len = dax_direct_access(dax_dev, pgoff, PHYS_PFN(size),
				&kaddr, NULL);
		if (map_len < 0) {
			ret = map_len;
			break;
		}

		map_len = PFN_PHYS(map_len);
		kaddr += offset;
		map_len -= offset;
		if (map_len > end - pos)
			map_len = end - pos;

		/*
		 * The userspace address for the memory copy has already been
		 * validated via access_ok() in either vfs_read() or
		 * vfs_write(), depending on which operation we are doing.
		 */
		if (iov_iter_rw(iter) == WRITE)
			xfer = dax_copy_from_iter(dax_dev, pgoff, kaddr,
					map_len, iter);
		else
			xfer = dax_copy_to_iter(dax_dev, pgoff, kaddr,
					map_len, iter);

		pos += xfer;
		length -= xfer;
		done += xfer;

		if (xfer == 0)
			ret = -EFAULT;
		if (xfer < map_len)
			break;
	}
	dax_read_unlock(id);

	return done ? done : ret;
}

/**
 * dax_iomap_rw - Perform I/O to a DAX file
 * @iocb:	The control block for this I/O
 * @iter:	The addresses to do I/O from or to
 * @ops:	iomap ops passed from the file system
 *
 * This function performs read and write operations to directly mapped
 * persistent memory.  The callers needs to take care of read/write exclusion
 * and evicting any page cache pages in the region under I/O.
 */
ssize_t
dax_iomap_rw(struct kiocb *iocb, struct iov_iter *iter,
		const struct iomap_ops *ops)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	struct inode *inode = mapping->host;
	loff_t pos = iocb->ki_pos, ret = 0, done = 0;
	unsigned flags = 0;

	if (iov_iter_rw(iter) == WRITE) {
		lockdep_assert_held_exclusive(&inode->i_rwsem);
		flags |= IOMAP_WRITE;
	} else {
		lockdep_assert_held(&inode->i_rwsem);
	}

	if (iocb->ki_flags & IOCB_NOWAIT)
		flags |= IOMAP_NOWAIT;

	while (iov_iter_count(iter)) {
		ret = iomap_apply(inode, pos, iov_iter_count(iter), flags, ops,
				iter, dax_iomap_actor);
		if (ret <= 0)
			break;
		pos += ret;
		done += ret;
	}

	iocb->ki_pos += done;
	return done ? done : ret;
}
EXPORT_SYMBOL_GPL(dax_iomap_rw);

static vm_fault_t dax_fault_return(int error)
{
	if (error == 0)
		return VM_FAULT_NOPAGE;
	if (error == -ENOMEM)
		return VM_FAULT_OOM;
	return VM_FAULT_SIGBUS;
}

/*
 * MAP_SYNC on a dax mapping guarantees dirty metadata is
 * flushed on write-faults (non-cow), but not read-faults.
 */
static bool dax_fault_is_synchronous(unsigned long flags,
		struct vm_area_struct *vma, struct iomap *iomap)
{
	return (flags & IOMAP_WRITE) && (vma->vm_flags & VM_SYNC)
		&& (iomap->flags & IOMAP_F_DIRTY);
}

static vm_fault_t dax_iomap_pte_fault(struct vm_fault *vmf, pfn_t *pfnp,
			       int *iomap_errp, const struct iomap_ops *ops)
{
	struct vm_area_struct *vma = vmf->vma;
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned long vaddr = vmf->address;
	loff_t pos = (loff_t)vmf->pgoff << PAGE_SHIFT;
	struct iomap iomap = { 0 };
	unsigned flags = IOMAP_FAULT;
	int error, major = 0;
	bool write = vmf->flags & FAULT_FLAG_WRITE;
	bool sync;
	vm_fault_t ret = 0;
	void *entry;
	pfn_t pfn;

	trace_dax_pte_fault(inode, vmf, ret);
	/*
	 * Check whether offset isn't beyond end of file now. Caller is supposed
	 * to hold locks serializing us with truncate / punch hole so this is
	 * a reliable test.
	 */
	if (pos >= i_size_read(inode)) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	if (write && !vmf->cow_page)
		flags |= IOMAP_WRITE;

	entry = grab_mapping_entry(mapping, vmf->pgoff, 0);
	if (IS_ERR(entry)) {
		ret = dax_fault_return(PTR_ERR(entry));
		goto out;
	}

	/*
	 * It is possible, particularly with mixed reads & writes to private
	 * mappings, that we have raced with a PMD fault that overlaps with
	 * the PTE we need to set up.  If so just return and the fault will be
	 * retried.
	 */
	if (pmd_trans_huge(*vmf->pmd) || pmd_devmap(*vmf->pmd)) {
		ret = VM_FAULT_NOPAGE;
		goto unlock_entry;
	}

	/*
	 * Note that we don't bother to use iomap_apply here: DAX required
	 * the file system block size to be equal the page size, which means
	 * that we never have to deal with more than a single extent here.
	 */
	error = ops->iomap_begin(inode, pos, PAGE_SIZE, flags, &iomap);
	if (iomap_errp)
		*iomap_errp = error;
	if (error) {
		ret = dax_fault_return(error);
		goto unlock_entry;
	}
	if (WARN_ON_ONCE(iomap.offset + iomap.length < pos + PAGE_SIZE)) {
		error = -EIO;	/* fs corruption? */
		goto error_finish_iomap;
	}

	if (vmf->cow_page) {
		sector_t sector = dax_iomap_sector(&iomap, pos);

		switch (iomap.type) {
		case IOMAP_HOLE:
		case IOMAP_UNWRITTEN:
			clear_user_highpage(vmf->cow_page, vaddr);
			break;
		case IOMAP_MAPPED:
			error = copy_user_dax(iomap.bdev, iomap.dax_dev,
					sector, PAGE_SIZE, vmf->cow_page, vaddr);
			break;
		default:
			WARN_ON_ONCE(1);
			error = -EIO;
			break;
		}

		if (error)
			goto error_finish_iomap;

		__SetPageUptodate(vmf->cow_page);
		ret = finish_fault(vmf);
		if (!ret)
			ret = VM_FAULT_DONE_COW;
		goto finish_iomap;
	}

	sync = dax_fault_is_synchronous(flags, vma, &iomap);

	switch (iomap.type) {
	case IOMAP_MAPPED:
		if (iomap.flags & IOMAP_F_NEW) {
			count_vm_event(PGMAJFAULT);
			count_memcg_event_mm(vma->vm_mm, PGMAJFAULT);
			major = VM_FAULT_MAJOR;
		}
		error = dax_iomap_pfn(&iomap, pos, PAGE_SIZE, &pfn);
		if (error < 0)
			goto error_finish_iomap;

		entry = dax_insert_mapping_entry(mapping, vmf, entry, pfn,
						 0, write && !sync);

		/*
		 * If we are doing synchronous page fault and inode needs fsync,
		 * we can insert PTE into page tables only after that happens.
		 * Skip insertion for now and return the pfn so that caller can
		 * insert it after fsync is done.
		 */
		if (sync) {
			if (WARN_ON_ONCE(!pfnp)) {
				error = -EIO;
				goto error_finish_iomap;
			}
			*pfnp = pfn;
			ret = VM_FAULT_NEEDDSYNC | major;
			goto finish_iomap;
		}
		trace_dax_insert_mapping(inode, vmf, entry);
		if (write)
			ret = vmf_insert_mixed_mkwrite(vma, vaddr, pfn);
		else
			ret = vmf_insert_mixed(vma, vaddr, pfn);

		goto finish_iomap;
	case IOMAP_UNWRITTEN:
	case IOMAP_HOLE:
		if (!write) {
			ret = dax_load_hole(mapping, entry, vmf);
			goto finish_iomap;
		}
		/*FALLTHRU*/
	default:
		WARN_ON_ONCE(1);
		error = -EIO;
		break;
	}

 error_finish_iomap:
	ret = dax_fault_return(error);
 finish_iomap:
	if (ops->iomap_end) {
		int copied = PAGE_SIZE;

		if (ret & VM_FAULT_ERROR)
			copied = 0;
		/*
		 * The fault is done by now and there's no way back (other
		 * thread may be already happily using PTE we have installed).
		 * Just ignore error from ->iomap_end since we cannot do much
		 * with it.
		 */
		ops->iomap_end(inode, pos, PAGE_SIZE, copied, flags, &iomap);
	}
 unlock_entry:
	put_locked_mapping_entry(mapping, vmf->pgoff);
 out:
	trace_dax_pte_fault_done(inode, vmf, ret);
	return ret | major;
}

#ifdef CONFIG_FS_DAX_PMD
static vm_fault_t dax_pmd_load_hole(struct vm_fault *vmf, struct iomap *iomap,
		void *entry)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	unsigned long pmd_addr = vmf->address & PMD_MASK;
	struct inode *inode = mapping->host;
	struct page *zero_page;
	void *ret = NULL;
	spinlock_t *ptl;
	pmd_t pmd_entry;
	pfn_t pfn;

	zero_page = mm_get_huge_zero_page(vmf->vma->vm_mm);

	if (unlikely(!zero_page))
		goto fallback;

	pfn = page_to_pfn_t(zero_page);
	ret = dax_insert_mapping_entry(mapping, vmf, entry, pfn,
			RADIX_DAX_PMD | RADIX_DAX_ZERO_PAGE, false);

	ptl = pmd_lock(vmf->vma->vm_mm, vmf->pmd);
	if (!pmd_none(*(vmf->pmd))) {
		spin_unlock(ptl);
		goto fallback;
	}

	pmd_entry = mk_pmd(zero_page, vmf->vma->vm_page_prot);
	pmd_entry = pmd_mkhuge(pmd_entry);
	set_pmd_at(vmf->vma->vm_mm, pmd_addr, vmf->pmd, pmd_entry);
	spin_unlock(ptl);
	trace_dax_pmd_load_hole(inode, vmf, zero_page, ret);
	return VM_FAULT_NOPAGE;

fallback:
	trace_dax_pmd_load_hole_fallback(inode, vmf, zero_page, ret);
	return VM_FAULT_FALLBACK;
}

static vm_fault_t dax_iomap_pmd_fault(struct vm_fault *vmf, pfn_t *pfnp,
			       const struct iomap_ops *ops)
{
	struct vm_area_struct *vma = vmf->vma;
	struct address_space *mapping = vma->vm_file->f_mapping;
	unsigned long pmd_addr = vmf->address & PMD_MASK;
	bool write = vmf->flags & FAULT_FLAG_WRITE;
	bool sync;
	unsigned int iomap_flags = (write ? IOMAP_WRITE : 0) | IOMAP_FAULT;
	struct inode *inode = mapping->host;
	vm_fault_t result = VM_FAULT_FALLBACK;
	struct iomap iomap = { 0 };
	pgoff_t max_pgoff, pgoff;
	void *entry;
	loff_t pos;
	int error;
	pfn_t pfn;

	/*
	 * Check whether offset isn't beyond end of file now. Caller is
	 * supposed to hold locks serializing us with truncate / punch hole so
	 * this is a reliable test.
	 */
	pgoff = linear_page_index(vma, pmd_addr);
	max_pgoff = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);

	trace_dax_pmd_fault(inode, vmf, max_pgoff, 0);

	/*
	 * Make sure that the faulting address's PMD offset (color) matches
	 * the PMD offset from the start of the file.  This is necessary so
	 * that a PMD range in the page table overlaps exactly with a PMD
	 * range in the radix tree.
	 */
	if ((vmf->pgoff & PG_PMD_COLOUR) !=
	    ((vmf->address >> PAGE_SHIFT) & PG_PMD_COLOUR))
		goto fallback;

	/* Fall back to PTEs if we're going to COW */
	if (write && !(vma->vm_flags & VM_SHARED))
		goto fallback;

	/* If the PMD would extend outside the VMA */
	if (pmd_addr < vma->vm_start)
		goto fallback;
	if ((pmd_addr + PMD_SIZE) > vma->vm_end)
		goto fallback;

	if (pgoff >= max_pgoff) {
		result = VM_FAULT_SIGBUS;
		goto out;
	}

	/* If the PMD would extend beyond the file size */
	if ((pgoff | PG_PMD_COLOUR) >= max_pgoff)
		goto fallback;

	/*
	 * grab_mapping_entry() will make sure we get a 2MiB empty entry, a
	 * 2MiB zero page entry or a DAX PMD.  If it can't (because a 4k page
	 * is already in the tree, for instance), it will return -EEXIST and
	 * we just fall back to 4k entries.
	 */
	entry = grab_mapping_entry(mapping, pgoff, RADIX_DAX_PMD);
	if (IS_ERR(entry))
		goto fallback;

	/*
	 * It is possible, particularly with mixed reads & writes to private
	 * mappings, that we have raced with a PTE fault that overlaps with
	 * the PMD we need to set up.  If so just return and the fault will be
	 * retried.
	 */
	if (!pmd_none(*vmf->pmd) && !pmd_trans_huge(*vmf->pmd) &&
			!pmd_devmap(*vmf->pmd)) {
		result = 0;
		goto unlock_entry;
	}

	/*
	 * Note that we don't use iomap_apply here.  We aren't doing I/O, only
	 * setting up a mapping, so really we're using iomap_begin() as a way
	 * to look up our filesystem block.
	 */
	pos = (loff_t)pgoff << PAGE_SHIFT;
	error = ops->iomap_begin(inode, pos, PMD_SIZE, iomap_flags, &iomap);
	if (error)
		goto unlock_entry;

	if (iomap.offset + iomap.length < pos + PMD_SIZE)
		goto finish_iomap;

	sync = dax_fault_is_synchronous(iomap_flags, vma, &iomap);

	switch (iomap.type) {
	case IOMAP_MAPPED:
		error = dax_iomap_pfn(&iomap, pos, PMD_SIZE, &pfn);
		if (error < 0)
			goto finish_iomap;

		entry = dax_insert_mapping_entry(mapping, vmf, entry, pfn,
						RADIX_DAX_PMD, write && !sync);

		/*
		 * If we are doing synchronous page fault and inode needs fsync,
		 * we can insert PMD into page tables only after that happens.
		 * Skip insertion for now and return the pfn so that caller can
		 * insert it after fsync is done.
		 */
		if (sync) {
			if (WARN_ON_ONCE(!pfnp))
				goto finish_iomap;
			*pfnp = pfn;
			result = VM_FAULT_NEEDDSYNC;
			goto finish_iomap;
		}

		trace_dax_pmd_insert_mapping(inode, vmf, PMD_SIZE, pfn, entry);
		result = vmf_insert_pfn_pmd(vmf, pfn, write);
		break;
	case IOMAP_UNWRITTEN:
	case IOMAP_HOLE:
		if (WARN_ON_ONCE(write))
			break;
		result = dax_pmd_load_hole(vmf, &iomap, entry);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

 finish_iomap:
	if (ops->iomap_end) {
		int copied = PMD_SIZE;

		if (result == VM_FAULT_FALLBACK)
			copied = 0;
		/*
		 * The fault is done by now and there's no way back (other
		 * thread may be already happily using PMD we have installed).
		 * Just ignore error from ->iomap_end since we cannot do much
		 * with it.
		 */
		ops->iomap_end(inode, pos, PMD_SIZE, copied, iomap_flags,
				&iomap);
	}
 unlock_entry:
	put_locked_mapping_entry(mapping, pgoff);
 fallback:
	if (result == VM_FAULT_FALLBACK) {
		split_huge_pmd(vma, vmf->pmd, vmf->address);
		count_vm_event(THP_FAULT_FALLBACK);
	}
out:
	trace_dax_pmd_fault_done(inode, vmf, max_pgoff, result);
	return result;
}
#else
static vm_fault_t dax_iomap_pmd_fault(struct vm_fault *vmf, pfn_t *pfnp,
			       const struct iomap_ops *ops)
{
	return VM_FAULT_FALLBACK;
}
#endif /* CONFIG_FS_DAX_PMD */

/**
 * dax_iomap_fault - handle a page fault on a DAX file
 * @vmf: The description of the fault
 * @pe_size: Size of the page to fault in
 * @pfnp: PFN to insert for synchronous faults if fsync is required
 * @iomap_errp: Storage for detailed error code in case of error
 * @ops: Iomap ops passed from the file system
 *
 * When a page fault occurs, filesystems may call this helper in
 * their fault handler for DAX files. dax_iomap_fault() assumes the caller
 * has done all the necessary locking for page fault to proceed
 * successfully.
 */
vm_fault_t dax_iomap_fault(struct vm_fault *vmf, enum page_entry_size pe_size,
		    pfn_t *pfnp, int *iomap_errp, const struct iomap_ops *ops)
{
	switch (pe_size) {
	case PE_SIZE_PTE:
		return dax_iomap_pte_fault(vmf, pfnp, iomap_errp, ops);
	case PE_SIZE_PMD:
		return dax_iomap_pmd_fault(vmf, pfnp, ops);
	default:
		return VM_FAULT_FALLBACK;
	}
}
EXPORT_SYMBOL_GPL(dax_iomap_fault);

/**
 * dax_insert_pfn_mkwrite - insert PTE or PMD entry into page tables
 * @vmf: The description of the fault
 * @pe_size: Size of entry to be inserted
 * @pfn: PFN to insert
 *
 * This function inserts writeable PTE or PMD entry into page tables for mmaped
 * DAX file.  It takes care of marking corresponding radix tree entry as dirty
 * as well.
 */
static vm_fault_t dax_insert_pfn_mkwrite(struct vm_fault *vmf,
				  enum page_entry_size pe_size,
				  pfn_t pfn)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	void *entry, **slot;
	pgoff_t index = vmf->pgoff;
	vm_fault_t ret;

	xa_lock_irq(&mapping->i_pages);
	entry = get_unlocked_mapping_entry(mapping, index, &slot);
	/* Did we race with someone splitting entry or so? */
	if (!entry ||
	    (pe_size == PE_SIZE_PTE && !dax_is_pte_entry(entry)) ||
	    (pe_size == PE_SIZE_PMD && !dax_is_pmd_entry(entry))) {
		put_unlocked_mapping_entry(mapping, index, entry);
		xa_unlock_irq(&mapping->i_pages);
		trace_dax_insert_pfn_mkwrite_no_entry(mapping->host, vmf,
						      VM_FAULT_NOPAGE);
		return VM_FAULT_NOPAGE;
	}
	radix_tree_tag_set(&mapping->i_pages, index, PAGECACHE_TAG_DIRTY);
	entry = lock_slot(mapping, slot);
	xa_unlock_irq(&mapping->i_pages);
	switch (pe_size) {
	case PE_SIZE_PTE:
		ret = vmf_insert_mixed_mkwrite(vmf->vma, vmf->address, pfn);
		break;
#ifdef CONFIG_FS_DAX_PMD
	case PE_SIZE_PMD:
		ret = vmf_insert_pfn_pmd(vmf, pfn, FAULT_FLAG_WRITE);
		break;
#endif
	default:
		ret = VM_FAULT_FALLBACK;
	}
	put_locked_mapping_entry(mapping, index);
	trace_dax_insert_pfn_mkwrite(mapping->host, vmf, ret);
	return ret;
}

/**
 * dax_finish_sync_fault - finish synchronous page fault
 * @vmf: The description of the fault
 * @pe_size: Size of entry to be inserted
 * @pfn: PFN to insert
 *
 * This function ensures that the file range touched by the page fault is
 * stored persistently on the media and handles inserting of appropriate page
 * table entry.
 */
vm_fault_t dax_finish_sync_fault(struct vm_fault *vmf,
		enum page_entry_size pe_size, pfn_t pfn)
{
	int err;
	loff_t start = ((loff_t)vmf->pgoff) << PAGE_SHIFT;
	size_t len = 0;

	if (pe_size == PE_SIZE_PTE)
		len = PAGE_SIZE;
	else if (pe_size == PE_SIZE_PMD)
		len = PMD_SIZE;
	else
		WARN_ON_ONCE(1);
	err = vfs_fsync_range(vmf->vma->vm_file, start, start + len - 1, 1);
	if (err)
		return VM_FAULT_SIGBUS;
	return dax_insert_pfn_mkwrite(vmf, pe_size, pfn);
}
EXPORT_SYMBOL_GPL(dax_finish_sync_fault);
