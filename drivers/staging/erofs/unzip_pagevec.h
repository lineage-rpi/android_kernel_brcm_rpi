/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/drivers/staging/erofs/unzip_pagevec.h
 *
 * Copyright (C) 2018 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of the Linux
 * distribution for more details.
 */
#ifndef __EROFS_UNZIP_PAGEVEC_H
#define __EROFS_UNZIP_PAGEVEC_H

#include <linux/tagptr.h>

/* page type in pagevec for unzip subsystem */
enum z_erofs_page_type {
	/* including Z_EROFS_VLE_PAGE_TAIL_EXCLUSIVE */
	Z_EROFS_PAGE_TYPE_EXCLUSIVE,

	Z_EROFS_VLE_PAGE_TYPE_TAIL_SHARED,

	Z_EROFS_VLE_PAGE_TYPE_HEAD,
	Z_EROFS_VLE_PAGE_TYPE_MAX
};

extern void __compiletime_error("Z_EROFS_PAGE_TYPE_EXCLUSIVE != 0")
	__bad_page_type_exclusive(void);

/* pagevec tagged pointer */
typedef tagptr2_t	erofs_vtptr_t;

/* pagevec collector */
struct z_erofs_pagevec_ctor {
	struct page *curr, *next;
	erofs_vtptr_t *pages;

	unsigned int nr, index;
};

static inline void z_erofs_pagevec_ctor_exit(struct z_erofs_pagevec_ctor *ctor,
					     bool atomic)
{
	if (ctor->curr == NULL)
		return;

	if (atomic)
		kunmap_atomic(ctor->pages);
	else
		kunmap(ctor->curr);
}

static inline struct page *
z_erofs_pagevec_ctor_next_page(struct z_erofs_pagevec_ctor *ctor,
			       unsigned nr)
{
	unsigned index;

	/* keep away from occupied pages */
	if (ctor->next != NULL)
		return ctor->next;

	for (index = 0; index < nr; ++index) {
		const erofs_vtptr_t t = ctor->pages[index];
		const unsigned tags = tagptr_unfold_tags(t);

		if (tags == Z_EROFS_PAGE_TYPE_EXCLUSIVE)
			return tagptr_unfold_ptr(t);
	}

	if (unlikely(nr >= ctor->nr))
		BUG();

	return NULL;
}

static inline void
z_erofs_pagevec_ctor_pagedown(struct z_erofs_pagevec_ctor *ctor,
			      bool atomic)
{
	struct page *next = z_erofs_pagevec_ctor_next_page(ctor, ctor->nr);

	z_erofs_pagevec_ctor_exit(ctor, atomic);

	ctor->curr = next;
	ctor->next = NULL;
	ctor->pages = atomic ?
		kmap_atomic(ctor->curr) : kmap(ctor->curr);

	ctor->nr = PAGE_SIZE / sizeof(struct page *);
	ctor->index = 0;
}

static inline void z_erofs_pagevec_ctor_init(struct z_erofs_pagevec_ctor *ctor,
					     unsigned nr,
					     erofs_vtptr_t *pages, unsigned i)
{
	ctor->nr = nr;
	ctor->curr = ctor->next = NULL;
	ctor->pages = pages;

	if (i >= nr) {
		i -= nr;
		z_erofs_pagevec_ctor_pagedown(ctor, false);
		while (i > ctor->nr) {
			i -= ctor->nr;
			z_erofs_pagevec_ctor_pagedown(ctor, false);
		}
	}

	ctor->next = z_erofs_pagevec_ctor_next_page(ctor, i);
	ctor->index = i;
}

static inline bool
z_erofs_pagevec_ctor_enqueue(struct z_erofs_pagevec_ctor *ctor,
			     struct page *page,
			     enum z_erofs_page_type type,
			     bool pvec_safereuse)
{
	if (!ctor->next) {
		/* some pages cannot be reused as pvec safely without I/O */
		if (type == Z_EROFS_PAGE_TYPE_EXCLUSIVE && !pvec_safereuse)
			type = Z_EROFS_VLE_PAGE_TYPE_TAIL_SHARED;

		if (type != Z_EROFS_PAGE_TYPE_EXCLUSIVE &&
		    ctor->index + 1 == ctor->nr)
			return false;
	}

	if (unlikely(ctor->index >= ctor->nr))
		z_erofs_pagevec_ctor_pagedown(ctor, false);

	/* exclusive page type must be 0 */
	if (Z_EROFS_PAGE_TYPE_EXCLUSIVE != (uintptr_t)NULL)
		__bad_page_type_exclusive();

	/* should remind that collector->next never equal to 1, 2 */
	if (type == (uintptr_t)ctor->next) {
		ctor->next = page;
	}

	ctor->pages[ctor->index++] =
		tagptr_fold(erofs_vtptr_t, page, type);
	return true;
}

static inline struct page *
z_erofs_pagevec_ctor_dequeue(struct z_erofs_pagevec_ctor *ctor,
			     enum z_erofs_page_type *type)
{
	erofs_vtptr_t t;

	if (unlikely(ctor->index >= ctor->nr)) {
		DBG_BUGON(!ctor->next);
		z_erofs_pagevec_ctor_pagedown(ctor, true);
	}

	t = ctor->pages[ctor->index];

	*type = tagptr_unfold_tags(t);

	/* should remind that collector->next never equal to 1, 2 */
	if (*type == (uintptr_t)ctor->next)
		ctor->next = tagptr_unfold_ptr(t);

	ctor->pages[ctor->index++] =
		tagptr_fold(erofs_vtptr_t, NULL, 0);

	return tagptr_unfold_ptr(t);
}

#endif

