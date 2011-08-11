/* mdb.c - memory-mapped database library */
/*
 * Copyright 2011 Howard Chu, Symas Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 *
 * This code is derived from btree.c written by Martin Hedenfalk.
 *
 * Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#include <fcntl.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <endian.h>

#include "mdb.h"

#define ULONG		unsigned long
typedef ULONG		pgno_t;

#include "idl.h"

#ifndef DEBUG
#define DEBUG 1
#endif

#if DEBUG && defined(__GNUC__)
# define DPRINTF(fmt, ...) \
	fprintf(stderr, "%s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
# define DPRINTF(...)	((void) 0)
#endif

#define PAGESIZE	 4096
#define MDB_MINKEYS	 4
#define MDB_MAGIC	 0xBEEFC0DE
#define MDB_VERSION	 1
#define MAXKEYSIZE	 511

#define P_INVALID	 (~0UL)

#define F_ISSET(w, f)	 (((w) & (f)) == (f))

typedef uint16_t	 indx_t;

#define DEFAULT_READERS	126
#define DEFAULT_MAPSIZE	1048576

/* Lock descriptor stuff */
#define RXBODY	\
	ULONG		mr_txnid; \
	pid_t		mr_pid; \
	pthread_t	mr_tid
typedef struct MDB_rxbody {
	RXBODY;
} MDB_rxbody;

#ifndef CACHELINE
#define CACHELINE	64	/* most CPUs. Itanium uses 128 */
#endif

typedef struct MDB_reader {
	RXBODY;
	/* cache line alignment */
	char pad[CACHELINE-sizeof(MDB_rxbody)];
} MDB_reader;

#define	TXBODY \
	uint32_t	mt_magic;	\
	uint32_t	mt_version;	\
	pthread_mutex_t	mt_mutex;	\
	ULONG		mt_txnid;	\
	uint32_t	mt_numreaders
typedef struct MDB_txbody {
	TXBODY;
} MDB_txbody;

typedef struct MDB_txninfo {
	TXBODY;
	char pad[CACHELINE-sizeof(MDB_txbody)];
	pthread_mutex_t	mt_wmutex;
	char pad2[CACHELINE-sizeof(pthread_mutex_t)];
	MDB_reader	mt_readers[1];
} MDB_txninfo;

/* Common header for all page types. Overflow pages
 * occupy a number of contiguous pages with no
 * headers on any page after the first.
 */
typedef struct MDB_page {		/* represents a page of storage */
#define	mp_pgno		mp_p.p_pgno
	union padded {
		pgno_t		p_pgno;		/* page number */
		void *		p_pad;
	} mp_p;
#define	P_BRANCH	 0x01		/* branch page */
#define	P_LEAF		 0x02		/* leaf page */
#define	P_OVERFLOW	 0x04		/* overflow page */
#define	P_META		 0x08		/* meta page */
#define	P_DIRTY		 0x10		/* dirty page */
	uint32_t	mp_flags;
#define mp_lower	mp_pb.pb.pb_lower
#define mp_upper	mp_pb.pb.pb_upper
#define mp_pages	mp_pb.pb_pages
	union page_bounds {
		struct {
			indx_t		pb_lower;		/* lower bound of free space */
			indx_t		pb_upper;		/* upper bound of free space */
		} pb;
		uint32_t	pb_pages;	/* number of overflow pages */
	} mp_pb;
	indx_t		mp_ptrs[1];		/* dynamic size */
} MDB_page;

#define PAGEHDRSZ	 ((unsigned) offsetof(MDB_page, mp_ptrs))

#define NUMKEYS(p)	 (((p)->mp_lower - PAGEHDRSZ) >> 1)
#define SIZELEFT(p)	 (indx_t)((p)->mp_upper - (p)->mp_lower)
#define PAGEFILL(env, p) (1000L * ((env)->me_psize - PAGEHDRSZ - SIZELEFT(p)) / \
				((env)->me_psize - PAGEHDRSZ))
#define IS_LEAF(p)	 F_ISSET((p)->mp_flags, P_LEAF)
#define IS_BRANCH(p)	 F_ISSET((p)->mp_flags, P_BRANCH)
#define IS_OVERFLOW(p)	 F_ISSET((p)->mp_flags, P_OVERFLOW)

#define OVPAGES(size, psize)	(PAGEHDRSZ + size + psize - 1) / psize;

typedef struct MDB_db {
	uint32_t	md_pad;
	uint16_t	md_flags;
	uint16_t	md_depth;
	ULONG		md_branch_pages;
	ULONG		md_leaf_pages;
	ULONG		md_overflow_pages;
	ULONG		md_entries;
	pgno_t		md_root;
} MDB_db;

#define	FREE_DBI	0
#define	MAIN_DBI	1

typedef struct MDB_meta {			/* meta (footer) page content */
	uint32_t	mm_magic;
	uint32_t	mm_version;
	void		*mm_address;		/* address for fixed mapping */
	size_t		mm_mapsize;			/* size of mmap region */
	MDB_db		mm_dbs[2];			/* first is free space, 2nd is main db */
#define	mm_psize	mm_dbs[0].md_pad
#define	mm_flags	mm_dbs[0].md_flags
	pgno_t		mm_last_pg;			/* last used page in file */
	ULONG		mm_txnid;			/* txnid that committed this page */
} MDB_meta;

typedef struct MDB_dhead {					/* a dirty page */
	STAILQ_ENTRY(MDB_dpage)	 md_next;	/* queue of dirty pages */
	MDB_page	*md_parent;
	unsigned	md_pi;				/* parent index */
	int			md_num;
} MDB_dhead;

typedef struct MDB_dpage {
	MDB_dhead	h;
	MDB_page	p;
} MDB_dpage;

STAILQ_HEAD(dirty_queue, MDB_dpage);	/* FIXME: use a sorted data structure */

typedef struct MDB_oldpages {
	struct MDB_oldpages *mo_next;
	ULONG		mo_txnid;
	pgno_t		mo_pages[1];	/* dynamic */
} MDB_oldpages;

typedef struct MDB_pageparent {
	MDB_page *mp_page;
	MDB_page *mp_parent;
	unsigned mp_pi;
} MDB_pageparent;

static MDB_dpage *mdb_alloc_page(MDB_txn *txn, MDB_page *parent, unsigned int parent_idx, int num);
static int 		mdb_touch(MDB_txn *txn, MDB_pageparent *mp);

typedef struct MDB_ppage {					/* ordered list of pages */
	SLIST_ENTRY(MDB_ppage)	 mp_entry;
	MDB_page		*mp_page;
	unsigned int	mp_ki;		/* cursor index on page */
} MDB_ppage;
SLIST_HEAD(page_stack, MDB_ppage);

#define CURSOR_EMPTY(c)		 SLIST_EMPTY(&(c)->mc_stack)
#define CURSOR_TOP(c)		 SLIST_FIRST(&(c)->mc_stack)
#define CURSOR_POP(c)		 SLIST_REMOVE_HEAD(&(c)->mc_stack, mp_entry)
#define CURSOR_PUSH(c,p)	 SLIST_INSERT_HEAD(&(c)->mc_stack, p, mp_entry)

struct MDB_xcursor;

struct MDB_cursor {
	MDB_txn		*mc_txn;
	struct page_stack	 mc_stack;		/* stack of parent pages */
	MDB_dbi		mc_dbi;
	short		mc_initialized;	/* 1 if initialized */
	short		mc_eof;		/* 1 if end is reached */
	struct MDB_xcursor	*mc_xcursor;
};

#define METAHASHLEN	 offsetof(MDB_meta, mm_hash)
#define METADATA(p)	 ((void *)((char *)p + PAGEHDRSZ))

typedef struct MDB_node {
#define mn_pgno		 mn_p.np_pgno
#define mn_dsize	 mn_p.np_dsize
	union {
		pgno_t		 np_pgno;	/* child page number */
		uint32_t	 np_dsize;	/* leaf data size */
	} mn_p;
	unsigned int	mn_flags:4;
	unsigned int	mn_ksize:12;			/* key size */
#define F_BIGDATA	 0x01			/* data put on overflow page */
#define F_SUBDATA	 0x02			/* data is a sub-database */
	char		mn_data[1];
} MDB_node;

typedef struct MDB_dbx {
	MDB_val		md_name;
	MDB_cmp_func	*md_cmp;		/* user compare function */
	MDB_cmp_func	*md_dcmp;		/* user dupsort function */
	MDB_rel_func	*md_rel;		/* user relocate function */
	MDB_dbi	md_parent;
	unsigned int	md_dirty;
} MDB_dbx;

struct MDB_txn {
	pgno_t		mt_next_pgno;	/* next unallocated page */
	ULONG		mt_txnid;
	ULONG		mt_oldest;
	MDB_env		*mt_env;	
	pgno_t		*mt_free_pgs;	/* this is an IDL */
	union {
		struct dirty_queue	*dirty_queue;	/* modified pages */
		MDB_reader	*reader;
	} mt_u;
	MDB_dbx		*mt_dbxs;		/* array */
	MDB_db		*mt_dbs;
	unsigned int	mt_numdbs;

#define MDB_TXN_RDONLY		 0x01		/* read-only transaction */
#define MDB_TXN_ERROR		 0x02		/* an error has occurred */
#define MDB_TXN_METOGGLE	0x04		/* used meta page 1 */
	unsigned int	mt_flags;
};

/* Context for sorted-dup records */
typedef struct MDB_xcursor {
	MDB_cursor mx_cursor;
	MDB_txn mx_txn;
	MDB_dbx	mx_dbxs[4];
	MDB_db	mx_dbs[4];
} MDB_xcursor;

struct MDB_env {
	int			me_fd;
	int			me_lfd;
	uint32_t	me_flags;
	unsigned int	me_maxreaders;
	unsigned int	me_numdbs;
	unsigned int	me_maxdbs;
	char		*me_path;
	char		*me_map;
	MDB_txninfo	*me_txns;
	MDB_meta	*me_metas[2];
	MDB_meta	*me_meta;
	MDB_txn		*me_txn;		/* current write transaction */
	size_t		me_mapsize;
	off_t		me_size;		/* current file size */
	unsigned int	me_psize;
	int			me_db_toggle;
	MDB_dbx		*me_dbxs;		/* array */
	MDB_db		*me_dbs[2];
	MDB_oldpages *me_pghead;
	pthread_key_t	me_txkey;	/* thread-key for readers */
	pgno_t		me_free_pgs[MDB_IDL_UM_SIZE];
};

#define NODESIZE	 offsetof(MDB_node, mn_data)

#define INDXSIZE(k)	 (NODESIZE + ((k) == NULL ? 0 : (k)->mv_size))
#define LEAFSIZE(k, d)	 (NODESIZE + (k)->mv_size + (d)->mv_size)
#define NODEPTR(p, i)	 ((MDB_node *)((char *)(p) + (p)->mp_ptrs[i]))
#define NODEKEY(node)	 (void *)((node)->mn_data)
#define NODEDATA(node)	 (void *)((char *)(node)->mn_data + (node)->mn_ksize)
#define NODEPGNO(node)	 ((node)->mn_pgno)
#define NODEDSZ(node)	 ((node)->mn_dsize)

#define MDB_COMMIT_PAGES	 64	/* max number of pages to write in one commit */
#define MDB_MAXCACHE_DEF	 1024	/* max number of pages to keep in cache  */

static int  mdb_search_page_root(MDB_txn *txn,
			    MDB_dbi dbi, MDB_val *key,
			    MDB_cursor *cursor, int modify,
			    MDB_pageparent *mpp);
static int  mdb_search_page(MDB_txn *txn,
			    MDB_dbi dbi, MDB_val *key,
			    MDB_cursor *cursor, int modify,
			    MDB_pageparent *mpp);

static int  mdbenv_read_header(MDB_env *env, MDB_meta *meta);
static int  mdbenv_read_meta(MDB_env *env, int *which);
static int  mdbenv_write_meta(MDB_txn *txn);
static MDB_page *mdb_get_page(MDB_txn *txn, pgno_t pgno);

static MDB_node *mdb_search_node(MDB_txn *txn, MDB_dbi dbi, MDB_page *mp,
			    MDB_val *key, int *exactp, unsigned int *kip);
static int  mdb_add_node(MDB_txn *txn, MDB_dbi dbi, MDB_page *mp,
			    indx_t indx, MDB_val *key, MDB_val *data,
			    pgno_t pgno, uint8_t flags);
static void mdb_del_node(MDB_page *mp, indx_t indx);
static int mdb_del0(MDB_txn *txn, MDB_dbi dbi, unsigned int ki,
    MDB_pageparent *mpp, MDB_node *leaf);
static int mdb_put0(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data, unsigned int flags);
static int  mdb_read_data(MDB_txn *txn, MDB_node *leaf, MDB_val *data);

static int		 mdb_rebalance(MDB_txn *txn, MDB_dbi dbi, MDB_pageparent *mp);
static int		 mdb_update_key(MDB_page *mp, indx_t indx, MDB_val *key);
static int		 mdb_move_node(MDB_txn *txn, MDB_dbi dbi, 
				MDB_pageparent *src, indx_t srcindx,
				MDB_pageparent *dst, indx_t dstindx);
static int		 mdb_merge(MDB_txn *txn, MDB_dbi dbi, MDB_pageparent *src,
			    MDB_pageparent *dst);
static int		 mdb_split(MDB_txn *txn, MDB_dbi dbi, MDB_page **mpp,
			    unsigned int *newindxp, MDB_val *newkey,
			    MDB_val *newdata, pgno_t newpgno);
static MDB_dpage *mdb_new_page(MDB_txn *txn, MDB_dbi dbi, uint32_t flags, int num);

static void		 cursor_pop_page(MDB_cursor *cursor);
static MDB_ppage *cursor_push_page(MDB_cursor *cursor,
			    MDB_page *mp);

static int		 mdb_set_key(MDB_node *node, MDB_val *key);
static int		 mdb_sibling(MDB_cursor *cursor, int move_right);
static int		 mdb_cursor_next(MDB_cursor *cursor,
			    MDB_val *key, MDB_val *data, MDB_cursor_op op);
static int		 mdb_cursor_prev(MDB_cursor *cursor,
			    MDB_val *key, MDB_val *data, MDB_cursor_op op);
static int		 mdb_cursor_set(MDB_cursor *cursor,
			    MDB_val *key, MDB_val *data, MDB_cursor_op op, int *exactp);
static int		 mdb_cursor_first(MDB_cursor *cursor,
			    MDB_val *key, MDB_val *data);
static int		 mdb_cursor_last(MDB_cursor *cursor,
			    MDB_val *key, MDB_val *data);

static void		mdb_xcursor_init0(MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx);
static void		mdb_xcursor_init1(MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx, MDB_node *node);
static void		mdb_xcursor_fini(MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx);

static size_t		 mdb_leaf_size(MDB_env *env, MDB_val *key,
			    MDB_val *data);
static size_t		 mdb_branch_size(MDB_env *env, MDB_val *key);

static int		 memncmp(const void *s1, size_t n1,
				 const void *s2, size_t n2);
static int		 memnrcmp(const void *s1, size_t n1,
				  const void *s2, size_t n2);

static int
memncmp(const void *s1, size_t n1, const void *s2, size_t n2)
{
	int diff, len_diff = -1;

	if (n1 >= n2) {
		len_diff = (n1 > n2);
		n1 = n2;
	}
	diff = memcmp(s1, s2, n1);
	return diff ? diff : len_diff;
}

static int
memnrcmp(const void *s1, size_t n1, const void *s2, size_t n2)
{
	const unsigned char	*p1, *p2, *p1_lim;

	if (n2 == 0)
		return n1 != 0;
	if (n1 == 0)
		return -1;

	p1 = (const unsigned char *)s1 + n1 - 1;
	p2 = (const unsigned char *)s2 + n2 - 1;

	for (p1_lim = (n1 <= n2 ? s1 : s2);  *p1 == *p2;  p1--, p2--) {
		if (p1 == p1_lim)
			return (p1 != s1) ? (p1 != p2) : (p2 != s2) ? -1 : 0;
	}
	return *p1 - *p2;
}

int
mdb_cmp(MDB_txn *txn, MDB_dbi dbi, const MDB_val *a, const MDB_val *b)
{
	return txn->mt_dbxs[dbi].md_cmp(a, b);
}

static int
_mdb_cmp(MDB_txn *txn, MDB_dbi dbi, const MDB_val *key1, const MDB_val *key2)
{
	if (txn->mt_dbs[dbi].md_flags & (MDB_REVERSEKEY
#if __BYTE_ORDER == __LITTLE_ENDIAN
		|MDB_INTEGERKEY
#endif
	))
		return memnrcmp(key1->mv_data, key1->mv_size, key2->mv_data, key2->mv_size);
	else
		return memncmp((char *)key1->mv_data, key1->mv_size, key2->mv_data, key2->mv_size);
}

/* Allocate new page(s) for writing */
static MDB_dpage *
mdb_alloc_page(MDB_txn *txn, MDB_page *parent, unsigned int parent_idx, int num)
{
	MDB_dpage *dp;
	pgno_t pgno = P_INVALID;
	ULONG oldest;

	if (txn->mt_txnid > 2) {

	oldest = txn->mt_txnid - 2;
	if (!txn->mt_env->me_pghead && txn->mt_dbs[FREE_DBI].md_root != P_INVALID) {
		/* See if there's anything in the free DB */
		MDB_pageparent mpp;
		MDB_node *leaf;
		ULONG *kptr;

		mpp.mp_parent = NULL;
		mpp.mp_pi = 0;
		mdb_search_page(txn, FREE_DBI, NULL, NULL, 0, &mpp);
		leaf = NODEPTR(mpp.mp_page, 0);
		kptr = (ULONG *)NODEKEY(leaf);

		/* It's potentially usable, unless there are still
		 * older readers outstanding. Grab it.
		 */
		if (oldest > *kptr) {
			MDB_oldpages *mop;
			MDB_val data;
			pgno_t *idl;

			mdb_read_data(txn, leaf, &data);
			idl = (ULONG *)data.mv_data;
			mop = malloc(sizeof(MDB_oldpages) + MDB_IDL_SIZEOF(idl) - sizeof(pgno_t));
			mop->mo_next = txn->mt_env->me_pghead;
			mop->mo_txnid = *kptr;
			txn->mt_env->me_pghead = mop;
			memcpy(mop->mo_pages, idl, MDB_IDL_SIZEOF(idl));

#if DEBUG > 1
			{
				unsigned int i;
				DPRINTF("IDL read txn %lu root %lu num %lu",
					mop->mo_txnid, txn->mt_dbs[FREE_DBI].md_root, idl[0]);
				for (i=0; i<idl[0]; i++) {
					DPRINTF("IDL %lu", idl[i+1]);
				}
			}
#endif
			/* drop this IDL from the DB */
			mpp.mp_parent = NULL;
			mpp.mp_pi = 0;
			mdb_search_page(txn, FREE_DBI, NULL, NULL, 1, &mpp);
			leaf = NODEPTR(mpp.mp_page, 0);
			mdb_del0(txn, FREE_DBI, 0, &mpp, leaf);
		}
	}
	if (txn->mt_env->me_pghead) {
		unsigned int i;
		for (i=0; i<txn->mt_env->me_txns->mt_numreaders; i++) {
			ULONG mr = txn->mt_env->me_txns->mt_readers[i].mr_txnid;
			if (!mr) continue;
			if (mr < oldest)
				oldest = txn->mt_env->me_txns->mt_readers[i].mr_txnid;
		}
		if (oldest > txn->mt_env->me_pghead->mo_txnid) {
			MDB_oldpages *mop = txn->mt_env->me_pghead;
			txn->mt_oldest = oldest;
			if (num > 1) {
				/* FIXME: For now, always use fresh pages. We
				 * really ought to search the free list for a
				 * contiguous range.
				 */
				;
			} else {
				/* peel pages off tail, so we only have to truncate the list */
				pgno = MDB_IDL_LAST(mop->mo_pages);
				if (MDB_IDL_IS_RANGE(mop->mo_pages)) {
					mop->mo_pages[2]++;
					if (mop->mo_pages[2] > mop->mo_pages[1])
						mop->mo_pages[0] = 0;
				} else {
					mop->mo_pages[0]--;
				}
				if (MDB_IDL_IS_ZERO(mop->mo_pages)) {
					txn->mt_env->me_pghead = mop->mo_next;
					free(mop);
				}
			}
		}
	}
	}

	if ((dp = malloc(txn->mt_env->me_psize * num + sizeof(MDB_dhead))) == NULL)
		return NULL;
	dp->h.md_num = num;
	dp->h.md_parent = parent;
	dp->h.md_pi = parent_idx;
	STAILQ_INSERT_TAIL(txn->mt_u.dirty_queue, dp, h.md_next);
	if (pgno == P_INVALID) {
		dp->p.mp_pgno = txn->mt_next_pgno;
		txn->mt_next_pgno += num;
	} else {
		dp->p.mp_pgno = pgno;
	}

	return dp;
}

/* Touch a page: make it dirty and re-insert into tree with updated pgno.
 */
static int
mdb_touch(MDB_txn *txn, MDB_pageparent *pp)
{
	MDB_page *mp = pp->mp_page;
	pgno_t	pgno;
	assert(txn != NULL);
	assert(pp != NULL);

	if (!F_ISSET(mp->mp_flags, P_DIRTY)) {
		MDB_dpage *dp;
		if ((dp = mdb_alloc_page(txn, pp->mp_parent, pp->mp_pi, 1)) == NULL)
			return ENOMEM;
		DPRINTF("touched page %lu -> %lu", mp->mp_pgno, dp->p.mp_pgno);
		mdb_idl_insert(txn->mt_free_pgs, mp->mp_pgno);
		pgno = dp->p.mp_pgno;
		memcpy(&dp->p, mp, txn->mt_env->me_psize);
		mp = &dp->p;
		mp->mp_pgno = pgno;
		mp->mp_flags |= P_DIRTY;

		/* Update the page number to new touched page. */
		if (pp->mp_parent != NULL)
			NODEPGNO(NODEPTR(pp->mp_parent, pp->mp_pi)) = mp->mp_pgno;
		pp->mp_page = mp;
	}
	return 0;
}

int
mdbenv_sync(MDB_env *env)
{
	int rc = 0;
	if (!F_ISSET(env->me_flags, MDB_NOSYNC)) {
		if (fsync(env->me_fd))
			rc = errno;
	}
	return rc;
}

int
mdb_txn_begin(MDB_env *env, int rdonly, MDB_txn **ret)
{
	MDB_txn	*txn;
	int rc, toggle;

	if ((txn = calloc(1, sizeof(MDB_txn))) == NULL) {
		DPRINTF("calloc: %s", strerror(errno));
		return ENOMEM;
	}

	if (rdonly) {
		txn->mt_flags |= MDB_TXN_RDONLY;
	} else {
		txn->mt_u.dirty_queue = calloc(1, sizeof(*txn->mt_u.dirty_queue));
		if (txn->mt_u.dirty_queue == NULL) {
			free(txn);
			return ENOMEM;
		}
		STAILQ_INIT(txn->mt_u.dirty_queue);

		pthread_mutex_lock(&env->me_txns->mt_wmutex);
		env->me_txns->mt_txnid++;
		txn->mt_free_pgs = env->me_free_pgs;
		txn->mt_free_pgs[0] = 0;
	}

	txn->mt_txnid = env->me_txns->mt_txnid;
	if (rdonly) {
		MDB_reader *r = pthread_getspecific(env->me_txkey);
		if (!r) {
			unsigned int i;
			pthread_mutex_lock(&env->me_txns->mt_mutex);
			for (i=0; i<env->me_txns->mt_numreaders; i++)
				if (env->me_txns->mt_readers[i].mr_pid == 0)
					break;
			if (i == env->me_maxreaders) {
				pthread_mutex_unlock(&env->me_txns->mti_mutex);
				return ENOSPC;
			}
			env->me_txns->mt_readers[i].mr_pid = getpid();
			env->me_txns->mt_readers[i].mr_tid = pthread_self();
			r = &env->me_txns->mt_readers[i];
			pthread_setspecific(env->me_txkey, r);
			if (i >= env->me_txns->mt_numreaders)
				env->me_txns->mt_numreaders = i+1;
			pthread_mutex_unlock(&env->me_txns->mt_mutex);
		}
		r->mr_txnid = txn->mt_txnid;
		txn->mt_u.reader = r;
	} else {
		env->me_txn = txn;
	}

	txn->mt_env = env;

	if ((rc = mdbenv_read_meta(env, &toggle)) != MDB_SUCCESS) {
		mdb_txn_abort(txn);
		return rc;
	}

	/* Copy the DB arrays */
	txn->mt_numdbs = env->me_numdbs;
	txn->mt_dbxs = env->me_dbxs;	/* mostly static anyway */
	txn->mt_dbs = malloc(env->me_maxdbs * sizeof(MDB_db));
	memcpy(txn->mt_dbs, env->me_meta->mm_dbs, 2 * sizeof(MDB_db));
	if (txn->mt_numdbs > 2)
		memcpy(txn->mt_dbs+2, env->me_dbs[env->me_db_toggle]+2,
			(txn->mt_numdbs - 2) * sizeof(MDB_db));

	if (!rdonly) {
		if (toggle)
			txn->mt_flags |= MDB_TXN_METOGGLE;
		txn->mt_next_pgno = env->me_meta->mm_last_pg+1;
	}

	DPRINTF("begin transaction %lu on mdbenv %p, root page %lu",
		txn->mt_txnid, (void *) env, txn->mt_dbs[MAIN_DBI].md_root);

	*ret = txn;
	return MDB_SUCCESS;
}

void
mdb_txn_abort(MDB_txn *txn)
{
	MDB_dpage *dp;
	MDB_env	*env;

	if (txn == NULL)
		return;

	env = txn->mt_env;
	DPRINTF("abort transaction %lu on mdbenv %p, root page %lu",
		txn->mt_txnid, (void *) env, txn->mt_dbs[MAIN_DBI].md_root);

	free(txn->mt_dbs);

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		txn->mt_u.reader->mr_txnid = 0;
	} else {
		MDB_oldpages *mop;
		unsigned int i;

		/* Discard all dirty pages. */
		while (!STAILQ_EMPTY(txn->mt_u.dirty_queue)) {
			dp = STAILQ_FIRST(txn->mt_u.dirty_queue);
			STAILQ_REMOVE_HEAD(txn->mt_u.dirty_queue, h.md_next);
			free(dp);
		}
		free(txn->mt_u.dirty_queue);

		while ((mop = txn->mt_env->me_pghead)) {
			txn->mt_env->me_pghead = mop->mo_next;
			free(mop);
		}

		env->me_txn = NULL;
		env->me_txns->mt_txnid--;
		for (i=2; i<env->me_numdbs; i++)
			env->me_dbxs[i].md_dirty = 0;
		pthread_mutex_unlock(&env->me_txns->mt_wmutex);
	}

	free(txn);
}

int
mdb_txn_commit(MDB_txn *txn)
{
	int		 n, done;
	unsigned int i;
	ssize_t		 rc;
	off_t		 size;
	MDB_dpage	*dp;
	MDB_env	*env;
	pgno_t	next;
	struct iovec	 iov[MDB_COMMIT_PAGES];

	assert(txn != NULL);
	assert(txn->mt_env != NULL);

	env = txn->mt_env;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		DPRINTF("attempt to commit read-only transaction");
		mdb_txn_abort(txn);
		return EPERM;
	}

	if (txn != env->me_txn) {
		DPRINTF("attempt to commit unknown transaction");
		mdb_txn_abort(txn);
		return EINVAL;
	}

	if (F_ISSET(txn->mt_flags, MDB_TXN_ERROR)) {
		DPRINTF("error flag is set, can't commit");
		mdb_txn_abort(txn);
		return EINVAL;
	}

	if (STAILQ_EMPTY(txn->mt_u.dirty_queue))
		goto done;

	DPRINTF("committing transaction %lu on mdbenv %p, root page %lu",
	    txn->mt_txnid, (void *) env, txn->mt_dbs[MAIN_DBI].md_root);

	/* should only be one record now */
	if (env->me_pghead) {
		MDB_val key, data;
		MDB_oldpages *mop;

		mop = env->me_pghead;
		key.mv_size = sizeof(pgno_t);
		key.mv_data = (char *)&mop->mo_txnid;
		data.mv_size = MDB_IDL_SIZEOF(mop->mo_pages);
		data.mv_data = mop->mo_pages;
		mdb_put0(txn, FREE_DBI, &key, &data, 0);
		free(env->me_pghead);
		env->me_pghead = NULL;
	}
	/* save to free list */
	if (!MDB_IDL_IS_ZERO(txn->mt_free_pgs)) {
		MDB_val key, data;
		MDB_pageparent mpp;

		/* make sure last page of freeDB is touched and on freelist */
		key.mv_size = MAXKEYSIZE+1;
		key.mv_data = NULL;
		mpp.mp_parent = NULL;
		mpp.mp_pi = 0;
		mdb_search_page(txn, FREE_DBI, &key, NULL, 1, &mpp);

#if DEBUG > 1
		{
			unsigned int i;
			ULONG *idl = txn->mt_free_pgs;
			DPRINTF("IDL write txn %lu root %lu num %lu",
				txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, idl[0]);
			for (i=0; i<idl[0]; i++) {
				DPRINTF("IDL %lu", idl[i+1]);
			}
		}
#endif
		/* write to last page of freeDB */
		key.mv_size = sizeof(pgno_t);
		key.mv_data = (char *)&txn->mt_txnid;
		data.mv_size = MDB_IDL_SIZEOF(txn->mt_free_pgs);
		data.mv_data = txn->mt_free_pgs;
		mdb_put0(txn, FREE_DBI, &key, &data, 0);
	}

	/* Update DB root pointers. Their pages have already been
	 * touched so this is all in-place and cannot fail.
	 */
	{
		MDB_val data;
		data.mv_size = sizeof(MDB_db);

		for (i = 2; i < txn->mt_numdbs; i++) {
			if (txn->mt_dbxs[i].md_dirty) {
				data.mv_data = &txn->mt_dbs[i];
				mdb_put0(txn, MAIN_DBI, &txn->mt_dbxs[i].md_name, &data, 0);
			}
		}
	}

	/* Commit up to MDB_COMMIT_PAGES dirty pages to disk until done.
	 */
	next = 0;
	do {
		n = 0;
		done = 1;
		size = 0;
		STAILQ_FOREACH(dp, txn->mt_u.dirty_queue, h.md_next) {
			if (dp->p.mp_pgno != next) {
				if (n) {
					DPRINTF("committing %u dirty pages", n);
					rc = writev(env->me_fd, iov, n);
					if (rc != size) {
						n = errno;
						if (rc > 0)
							DPRINTF("short write, filesystem full?");
						else
							DPRINTF("writev: %s", strerror(errno));
						mdb_txn_abort(txn);
						return n;
					}
					n = 0;
					size = 0;
				}
				lseek(env->me_fd, dp->p.mp_pgno * env->me_psize, SEEK_SET);
				next = dp->p.mp_pgno;
			}
			DPRINTF("committing page %lu", dp->p.mp_pgno);
			iov[n].iov_len = env->me_psize * dp->h.md_num;
			iov[n].iov_base = &dp->p;
			size += iov[n].iov_len;
			next = dp->p.mp_pgno + dp->h.md_num;
			/* clear dirty flag */
			dp->p.mp_flags &= ~P_DIRTY;
			if (++n >= MDB_COMMIT_PAGES) {
				done = 0;
				break;
			}
		}

		if (n == 0)
			break;

		DPRINTF("committing %u dirty pages", n);
		rc = writev(env->me_fd, iov, n);
		if (rc != size) {
			n = errno;
			if (rc > 0)
				DPRINTF("short write, filesystem full?");
			else
				DPRINTF("writev: %s", strerror(errno));
			mdb_txn_abort(txn);
			return n;
		}

	} while (!done);

	/* Drop the dirty pages.
	 */
	while (!STAILQ_EMPTY(txn->mt_u.dirty_queue)) {
		dp = STAILQ_FIRST(txn->mt_u.dirty_queue);
		STAILQ_REMOVE_HEAD(txn->mt_u.dirty_queue, h.md_next);
		free(dp);
	}

	if ((n = mdbenv_sync(env)) != 0 ||
	    (n = mdbenv_write_meta(txn)) != MDB_SUCCESS ||
	    (n = mdbenv_sync(env)) != 0) {
		mdb_txn_abort(txn);
		return n;
	}
	env->me_txn = NULL;

	/* update the DB tables */
	{
		int toggle = !env->me_db_toggle;

		for (i = 2; i < env->me_numdbs; i++) {
			if (txn->mt_dbxs[i].md_dirty) {
				env->me_dbs[toggle][i] = txn->mt_dbs[i];
				txn->mt_dbxs[i].md_dirty = 0;
			}
		}
		for (i = env->me_numdbs; i < txn->mt_numdbs; i++) {
			txn->mt_dbxs[i].md_dirty = 0;
			env->me_dbxs[i] = txn->mt_dbxs[i];
			env->me_dbs[toggle][i] = txn->mt_dbs[i];
		}
		env->me_db_toggle = toggle;
		env->me_numdbs = txn->mt_numdbs;

		free(txn->mt_dbs);
	}

	pthread_mutex_unlock(&env->me_txns->mt_wmutex);
	free(txn->mt_u.dirty_queue);
	free(txn);
	txn = NULL;

done:
	mdb_txn_abort(txn);

	return MDB_SUCCESS;
}

static int
mdbenv_read_header(MDB_env *env, MDB_meta *meta)
{
	char		 page[PAGESIZE];
	MDB_page	*p;
	MDB_meta	*m;
	int		 rc;

	assert(env != NULL);

	/* We don't know the page size yet, so use a minimum value.
	 */

	if ((rc = pread(env->me_fd, page, PAGESIZE, 0)) == 0) {
		return ENOENT;
	} else if (rc != PAGESIZE) {
		if (rc > 0)
			errno = EINVAL;
		DPRINTF("read: %s", strerror(errno));
		return errno;
	}

	p = (MDB_page *)page;

	if (!F_ISSET(p->mp_flags, P_META)) {
		DPRINTF("page %lu not a meta page", p->mp_pgno);
		return EINVAL;
	}

	m = METADATA(p);
	if (m->mm_magic != MDB_MAGIC) {
		DPRINTF("meta has invalid magic");
		return EINVAL;
	}

	if (m->mm_version != MDB_VERSION) {
		DPRINTF("database is version %u, expected version %u",
		    m->mm_version, MDB_VERSION);
		return MDB_VERSION_MISMATCH;
	}

	memcpy(meta, m, sizeof(*m));
	return 0;
}

static int
mdbenv_init_meta(MDB_env *env, MDB_meta *meta)
{
	MDB_page *p, *q;
	MDB_meta *m;
	int rc;
	unsigned int	 psize;

	DPRINTF("writing new meta page");
	psize = sysconf(_SC_PAGE_SIZE);

	meta->mm_magic = MDB_MAGIC;
	meta->mm_version = MDB_VERSION;
	meta->mm_psize = psize;
	meta->mm_last_pg = 1;
	meta->mm_flags = env->me_flags & 0xffff;
	meta->mm_flags |= MDB_INTEGERKEY;
	meta->mm_dbs[0].md_root = P_INVALID;
	meta->mm_dbs[1].md_root = P_INVALID;

	p = calloc(2, psize);
	p->mp_pgno = 0;
	p->mp_flags = P_META;

	m = METADATA(p);
	memcpy(m, meta, sizeof(*meta));

	q = (MDB_page *)((char *)p + psize);

	q->mp_pgno = 1;
	q->mp_flags = P_META;

	m = METADATA(q);
	memcpy(m, meta, sizeof(*meta));

	rc = write(env->me_fd, p, psize * 2);
	free(p);
	return (rc == (int)psize * 2) ? MDB_SUCCESS : errno;
}

static int
mdbenv_write_meta(MDB_txn *txn)
{
	MDB_env *env;
	MDB_meta	meta;
	off_t off;
	int rc, len;
	char *ptr;

	assert(txn != NULL);
	assert(txn->mt_env != NULL);

	DPRINTF("writing meta page %d for root page %lu",
		!F_ISSET(txn->mt_flags, MDB_TXN_METOGGLE), txn->mt_dbs[MAIN_DBI].md_root);

	env = txn->mt_env;

	ptr = (char *)&meta;
	off = offsetof(MDB_meta, mm_dbs[0].md_depth);
	len = sizeof(MDB_meta) - off;

	ptr += off;
	meta.mm_dbs[0] = txn->mt_dbs[0];
	meta.mm_dbs[1] = txn->mt_dbs[1];
	meta.mm_last_pg = txn->mt_next_pgno - 1;
	meta.mm_txnid = txn->mt_txnid;

	if (!F_ISSET(txn->mt_flags, MDB_TXN_METOGGLE))
		off += env->me_psize;
	off += PAGEHDRSZ;

	lseek(env->me_fd, off, SEEK_SET);
	rc = write(env->me_fd, ptr, len);
	if (rc != len) {
		DPRINTF("write failed, disk error?");
		return errno;
	}

	return MDB_SUCCESS;
}

static int
mdbenv_read_meta(MDB_env *env, int *which)
{
	int toggle = 0;

	assert(env != NULL);

	if (env->me_metas[0]->mm_txnid < env->me_metas[1]->mm_txnid)
		toggle = 1;

	if (env->me_meta != env->me_metas[toggle])
		env->me_meta = env->me_metas[toggle];
	if (which)
		*which = toggle;

	DPRINTF("Using meta page %d", toggle);

	return MDB_SUCCESS;
}

int
mdbenv_create(MDB_env **env)
{
	MDB_env *e;

	e = calloc(1, sizeof(MDB_env));
	if (!e) return ENOMEM;

	e->me_maxreaders = DEFAULT_READERS;
	e->me_maxdbs = 2;
	e->me_fd = -1;
	e->me_lfd = -1;
	*env = e;
	return MDB_SUCCESS;
}

int
mdbenv_set_mapsize(MDB_env *env, size_t size)
{
	if (env->me_map)
		return EINVAL;
	env->me_mapsize = size;
	return MDB_SUCCESS;
}

int
mdbenv_set_maxdbs(MDB_env *env, int dbs)
{
	env->me_maxdbs = dbs;
	return MDB_SUCCESS;
}

int
mdbenv_set_maxreaders(MDB_env *env, int readers)
{
	env->me_maxreaders = readers;
	return MDB_SUCCESS;
}

int
mdbenv_get_maxreaders(MDB_env *env, int *readers)
{
	if (!env || !readers)
		return EINVAL;
	*readers = env->me_maxreaders;
	return MDB_SUCCESS;
}

static int
mdbenv_open2(MDB_env *env, unsigned int flags)
{
	int i, newenv = 0;
	MDB_meta meta;
	MDB_page *p;

	env->me_flags = flags;

	memset(&meta, 0, sizeof(meta));

	if ((i = mdbenv_read_header(env, &meta)) != 0) {
		if (i != ENOENT)
			return i;
		DPRINTF("new mdbenv");
		newenv = 1;
	}

	if (!env->me_mapsize) {
		env->me_mapsize = newenv ? DEFAULT_MAPSIZE : meta.mm_mapsize;
	}

	i = MAP_SHARED;
	if (meta.mm_address && (flags & MDB_FIXEDMAP))
		i |= MAP_FIXED;
	env->me_map = mmap(meta.mm_address, env->me_mapsize, PROT_READ, i,
		env->me_fd, 0);
	if (env->me_map == MAP_FAILED)
		return errno;

	if (newenv) {
		meta.mm_mapsize = env->me_mapsize;
		if (flags & MDB_FIXEDMAP)
			meta.mm_address = env->me_map;
		i = mdbenv_init_meta(env, &meta);
		if (i != MDB_SUCCESS) {
			munmap(env->me_map, env->me_mapsize);
			return i;
		}
	}
	env->me_psize = meta.mm_psize;

	p = (MDB_page *)(MDB_page *)(MDB_page *)(MDB_page *)(MDB_page *)(MDB_page *)(MDB_page *)(MDB_page *)(MDB_page *)env->me_map;
	env->me_metas[0] = METADATA(p);
	env->me_metas[1] = (MDB_meta *)((char *)env->me_metas[0] + meta.mm_psize);

	if ((i = mdbenv_read_meta(env, NULL)) != 0)
		return i;

	DPRINTF("opened database version %u, pagesize %u",
	    env->me_meta->mm_version, env->me_psize);
	DPRINTF("depth: %u", env->me_meta->mm_dbs[MAIN_DBI].md_depth);
	DPRINTF("entries: %lu", env->me_meta->mm_dbs[MAIN_DBI].md_entries);
	DPRINTF("branch pages: %lu", env->me_meta->mm_dbs[MAIN_DBI].md_branch_pages);
	DPRINTF("leaf pages: %lu", env->me_meta->mm_dbs[MAIN_DBI].md_leaf_pages);
	DPRINTF("overflow pages: %lu", env->me_meta->mm_dbs[MAIN_DBI].md_overflow_pages);
	DPRINTF("root: %lu", env->me_meta->mm_dbs[MAIN_DBI].md_root);

	return MDB_SUCCESS;
}

static void
mdbenv_reader_dest(void *ptr)
{
	MDB_reader *reader = ptr;

	reader->mr_txnid = 0;
	reader->mr_pid = 0;
	reader->mr_tid = 0;
}

/* downgrade the exclusive lock on the region back to shared */
static void
mdbenv_share_locks(MDB_env *env)
{
	struct flock lock_info;

	env->me_txns->mt_txnid = env->me_meta->mm_txnid;

	memset((void *)&lock_info, 0, sizeof(lock_info));
	lock_info.l_type = F_RDLCK;
	lock_info.l_whence = SEEK_SET;
	lock_info.l_start = 0;
	lock_info.l_len = 1;
	fcntl(env->me_lfd, F_SETLK, &lock_info);
}

static int
mdbenv_setup_locks(MDB_env *env, char *lpath, int mode, int *excl)
{
	int rc;
	off_t size, rsize;
	struct flock lock_info;

	*excl = 0;

	if ((env->me_lfd = open(lpath, O_RDWR|O_CREAT, mode)) == -1) {
		rc = errno;
		return rc;
	}
	/* Try to get exclusive lock. If we succeed, then
	 * nobody is using the lock region and we should initialize it.
	 */
	memset((void *)&lock_info, 0, sizeof(lock_info));
	lock_info.l_type = F_WRLCK;
	lock_info.l_whence = SEEK_SET;
	lock_info.l_start = 0;
	lock_info.l_len = 1;
	rc = fcntl(env->me_lfd, F_SETLK, &lock_info);
	if (rc == 0) {
		*excl = 1;
	} else {
		lock_info.l_type = F_RDLCK;
		rc = fcntl(env->me_lfd, F_SETLK, &lock_info);
		if (rc) {
			rc = errno;
			goto fail;
		}
	}
	size = lseek(env->me_lfd, 0, SEEK_END);
	rsize = (env->me_maxreaders-1) * sizeof(MDB_reader) + sizeof(MDB_txninfo);
	if (size < rsize && *excl) {
		if (ftruncate(env->me_lfd, rsize) != 0) {
			rc = errno;
			goto fail;
		}
	} else {
		rsize = size;
		size = rsize - sizeof(MDB_txninfo);
		env->me_maxreaders = size/sizeof(MDB_reader) + 1;
	}
	env->me_txns = mmap(0, rsize, PROT_READ|PROT_WRITE, MAP_SHARED,
		env->me_lfd, 0);
	if (env->me_txns == MAP_FAILED) {
		rc = errno;
		goto fail;
	}
	if (*excl) {
		pthread_mutexattr_t mattr;

		pthread_mutexattr_init(&mattr);
		pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
		pthread_mutex_init(&env->me_txns->mt_mutex, &mattr);
		pthread_mutex_init(&env->me_txns->mt_wmutex, &mattr);
		env->me_txns->mt_version = MDB_VERSION;
		env->me_txns->mt_magic = MDB_MAGIC;
		env->me_txns->mt_txnid = 0;
		env->me_txns->mt_numreaders = 0;

	} else {
		if (env->me_txns->mt_magic != MDB_MAGIC) {
			DPRINTF("lock region has invalid magic");
			rc = EINVAL;
			goto fail;
		}
		if (env->me_txns->mt_version != MDB_VERSION) {
			DPRINTF("lock region is version %u, expected version %u",
				env->me_txns->mt_version, MDB_VERSION);
			rc = MDB_VERSION_MISMATCH;
			goto fail;
		}
		if (errno != EACCES && errno != EAGAIN) {
			rc = errno;
			goto fail;
		}
	}
	return MDB_SUCCESS;

fail:
	close(env->me_lfd);
	return rc;

}

#define LOCKNAME	"/lock.mdb"
#define DATANAME	"/data.mdb"
int
mdbenv_open(MDB_env *env, const char *path, unsigned int flags, mode_t mode)
{
	int		oflags, rc, len, excl;
	char *lpath, *dpath;

	len = strlen(path);
	lpath = malloc(len + sizeof(LOCKNAME) + len + sizeof(DATANAME));
	if (!lpath)
		return ENOMEM;
	dpath = lpath + len + sizeof(LOCKNAME);
	sprintf(lpath, "%s" LOCKNAME, path);
	sprintf(dpath, "%s" DATANAME, path);

	rc = mdbenv_setup_locks(env, lpath, mode, &excl);
	if (rc)
		goto leave;

	if (F_ISSET(flags, MDB_RDONLY))
		oflags = O_RDONLY;
	else
		oflags = O_RDWR | O_CREAT;

	if ((env->me_fd = open(dpath, oflags, mode)) == -1)
		return errno;

	if ((rc = mdbenv_open2(env, flags)) != MDB_SUCCESS) {
		close(env->me_fd);
		env->me_fd = -1;
	} else {
		env->me_path = strdup(path);
		DPRINTF("opened dbenv %p", (void *) env);
		pthread_key_create(&env->me_txkey, mdbenv_reader_dest);
		if (excl)
			mdbenv_share_locks(env);
		env->me_dbxs = calloc(env->me_maxdbs, sizeof(MDB_dbx));
		env->me_dbs[0] = calloc(env->me_maxdbs, sizeof(MDB_db));
		env->me_dbs[1] = calloc(env->me_maxdbs, sizeof(MDB_db));
		env->me_numdbs = 2;
	}

leave:
	free(lpath);
	return rc;
}

void
mdbenv_close(MDB_env *env)
{
	if (env == NULL)
		return;

	free(env->me_dbs[1]);
	free(env->me_dbs[0]);
	free(env->me_dbxs);
	free(env->me_path);

	if (env->me_map) {
		munmap(env->me_map, env->me_mapsize);
	}
	close(env->me_fd);
	if (env->me_txns) {
		size_t size = (env->me_maxreaders-1) * sizeof(MDB_reader) + sizeof(MDB_txninfo);
		munmap(env->me_txns, size);
	}
	close(env->me_lfd);
	free(env);
}

/* Search for key within a leaf page, using binary search.
 * Returns the smallest entry larger or equal to the key.
 * If exactp is non-null, stores whether the found entry was an exact match
 * in *exactp (1 or 0).
 * If kip is non-null, stores the index of the found entry in *kip.
 * If no entry larger or equal to the key is found, returns NULL.
 */
static MDB_node *
mdb_search_node(MDB_txn *txn, MDB_dbi dbi, MDB_page *mp, MDB_val *key,
    int *exactp, unsigned int *kip)
{
	unsigned int	 i = 0;
	int		 low, high;
	int		 rc = 0;
	MDB_node	*node;
	MDB_val	 nodekey;

	DPRINTF("searching %u keys in %s page %lu",
	    NUMKEYS(mp),
	    IS_LEAF(mp) ? "leaf" : "branch",
	    mp->mp_pgno);

	assert(NUMKEYS(mp) > 0);

	memset(&nodekey, 0, sizeof(nodekey));

	low = IS_LEAF(mp) ? 0 : 1;
	high = NUMKEYS(mp) - 1;
	while (low <= high) {
		i = (low + high) >> 1;
		node = NODEPTR(mp, i);

		nodekey.mv_size = node->mn_ksize;
		nodekey.mv_data = NODEKEY(node);

		if (txn->mt_dbxs[dbi].md_cmp)
			rc = txn->mt_dbxs[dbi].md_cmp(key, &nodekey);
		else
			rc = _mdb_cmp(txn, dbi, key, &nodekey);

		if (IS_LEAF(mp))
			DPRINTF("found leaf index %u [%.*s], rc = %i",
			    i, (int)nodekey.mv_size, (char *)nodekey.mv_data, rc);
		else
			DPRINTF("found branch index %u [%.*s -> %lu], rc = %i",
			    i, (int)node->mn_ksize, (char *)NODEKEY(node),
			    node->mn_pgno, rc);

		if (rc == 0)
			break;
		if (rc > 0)
			low = i + 1;
		else
			high = i - 1;
	}

	if (rc > 0) {	/* Found entry is less than the key. */
		i++;	/* Skip to get the smallest entry larger than key. */
		if (i >= NUMKEYS(mp))
			/* There is no entry larger or equal to the key. */
			return NULL;
	}
	if (exactp)
		*exactp = (rc == 0);
	if (kip)	/* Store the key index if requested. */
		*kip = i;

	return NODEPTR(mp, i);
}

static void
cursor_pop_page(MDB_cursor *cursor)
{
	MDB_ppage	*top;

	top = CURSOR_TOP(cursor);
	CURSOR_POP(cursor);

	DPRINTF("popped page %lu off cursor %p", top->mp_page->mp_pgno, (void *) cursor);

	free(top);
}

static MDB_ppage *
cursor_push_page(MDB_cursor *cursor, MDB_page *mp)
{
	MDB_ppage	*ppage;

	DPRINTF("pushing page %lu on cursor %p", mp->mp_pgno, (void *) cursor);

	if ((ppage = calloc(1, sizeof(MDB_ppage))) == NULL)
		return NULL;
	ppage->mp_page = mp;
	CURSOR_PUSH(cursor, ppage);
	return ppage;
}

static MDB_page *
mdb_get_page(MDB_txn *txn, pgno_t pgno)
{
	MDB_page *p = NULL;
	int found = 0;

	if (!F_ISSET(txn->mt_flags, MDB_TXN_RDONLY) && !STAILQ_EMPTY(txn->mt_u.dirty_queue)) {
		MDB_dpage *dp;
		STAILQ_FOREACH(dp, txn->mt_u.dirty_queue, h.md_next) {
			if (dp->p.mp_pgno == pgno) {
				p = &dp->p;
				found = 1;
				break;
			}
		}
	}
	if (!found) {
		if (pgno > txn->mt_env->me_meta->mm_last_pg)
			return NULL;
		p = (MDB_page *)(txn->mt_env->me_map + txn->mt_env->me_psize * pgno);
	}
	return p;
}

static int
mdb_search_page_root(MDB_txn *txn, MDB_dbi dbi, MDB_val *key,
    MDB_cursor *cursor, int modify, MDB_pageparent *mpp)
{
	MDB_page	*mp = mpp->mp_page;
	int rc;

	if (cursor && cursor_push_page(cursor, mp) == NULL)
		return MDB_FAIL;

	while (IS_BRANCH(mp)) {
		unsigned int	 i = 0;
		MDB_node	*node;

		DPRINTF("branch page %lu has %u keys", mp->mp_pgno, NUMKEYS(mp));
		assert(NUMKEYS(mp) > 1);
		DPRINTF("found index 0 to page %lu", NODEPGNO(NODEPTR(mp, 0)));

		if (key == NULL)	/* Initialize cursor to first page. */
			i = 0;
		else if (key->mv_size > MAXKEYSIZE && key->mv_data == NULL) {
							/* cursor to last page */
			i = NUMKEYS(mp)-1;
		} else {
			int	 exact;
			node = mdb_search_node(txn, dbi, mp, key, &exact, &i);
			if (node == NULL)
				i = NUMKEYS(mp) - 1;
			else if (!exact) {
				assert(i > 0);
				i--;
			}
		}

		if (key)
			DPRINTF("following index %u for key %.*s",
			    i, (int)key->mv_size, (char *)key->mv_data);
		assert(i < NUMKEYS(mp));
		node = NODEPTR(mp, i);

		if (cursor)
			CURSOR_TOP(cursor)->mp_ki = i;

		mpp->mp_parent = mp;
		if ((mp = mdb_get_page(txn, NODEPGNO(node))) == NULL)
			return MDB_FAIL;
		mpp->mp_pi = i;
		mpp->mp_page = mp;

		if (cursor && cursor_push_page(cursor, mp) == NULL)
			return MDB_FAIL;

		if (modify) {
			MDB_dhead *dh = ((MDB_dhead *)mp)-1;
			if ((rc = mdb_touch(txn, mpp)) != 0)
				return rc;
			dh = ((MDB_dhead *)mpp->mp_page)-1;
			dh->md_parent = mpp->mp_parent;
			dh->md_pi = mpp->mp_pi;
		}

		mp = mpp->mp_page;
	}

	if (!IS_LEAF(mp)) {
		DPRINTF("internal error, index points to a %02X page!?",
		    mp->mp_flags);
		return MDB_FAIL;
	}

	DPRINTF("found leaf page %lu for key %.*s", mp->mp_pgno,
	    key ? (int)key->mv_size : 0, key ? (char *)key->mv_data : NULL);

	return MDB_SUCCESS;
}

/* Search for the page a given key should be in.
 * Stores a pointer to the found page in *mpp.
 * If key is NULL, search for the lowest page (used by mdb_cursor_first).
 * If cursor is non-null, pushes parent pages on the cursor stack.
 * If modify is true, visited pages are updated with new page numbers.
 */
static int
mdb_search_page(MDB_txn *txn, MDB_dbi dbi, MDB_val *key,
    MDB_cursor *cursor, int modify, MDB_pageparent *mpp)
{
	int		 rc;
	pgno_t		 root;

	/* Choose which root page to start with. If a transaction is given
	 * use the root page from the transaction, otherwise read the last
	 * committed root page.
	 */
	if (F_ISSET(txn->mt_flags, MDB_TXN_ERROR)) {
		DPRINTF("transaction has failed, must abort");
		return EINVAL;
	} else
		root = txn->mt_dbs[dbi].md_root;

	if (root == P_INVALID) {		/* Tree is empty. */
		DPRINTF("tree is empty");
		return MDB_NOTFOUND;
	}

	if ((mpp->mp_page = mdb_get_page(txn, root)) == NULL)
		return MDB_FAIL;

	DPRINTF("root page has flags 0x%X", mpp->mp_page->mp_flags);

	if (modify) {
		/* For sub-databases, update main root first */
		if (dbi > MAIN_DBI && !txn->mt_dbxs[dbi].md_dirty) {
			MDB_pageparent mp2;
			rc = mdb_search_page(txn, MAIN_DBI, &txn->mt_dbxs[dbi].md_name,
				NULL, 1, &mp2);
			if (rc)
				return rc;
			txn->mt_dbxs[dbi].md_dirty = 1;
		}
		if (!F_ISSET(mpp->mp_page->mp_flags, P_DIRTY)) {
			mpp->mp_parent = NULL;
			mpp->mp_pi = 0;
			if ((rc = mdb_touch(txn, mpp)))
				return rc;
			txn->mt_dbs[dbi].md_root = mpp->mp_page->mp_pgno;
		}
	}

	return mdb_search_page_root(txn, dbi, key, cursor, modify, mpp);
}

static int
mdb_read_data(MDB_txn *txn, MDB_node *leaf, MDB_val *data)
{
	MDB_page	*omp;		/* overflow mpage */
	pgno_t		 pgno;

	if (!F_ISSET(leaf->mn_flags, F_BIGDATA)) {
		data->mv_size = leaf->mn_dsize;
		data->mv_data = NODEDATA(leaf);
		return MDB_SUCCESS;
	}

	/* Read overflow data.
	 */
	data->mv_size = leaf->mn_dsize;
	memcpy(&pgno, NODEDATA(leaf), sizeof(pgno));
	if ((omp = mdb_get_page(txn, pgno)) == NULL) {
		DPRINTF("read overflow page %lu failed", pgno);
		return MDB_FAIL;
	}
	data->mv_data = omp;

	return MDB_SUCCESS;
}

int
mdb_get(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data)
{
	int		 rc, exact;
	MDB_node	*leaf;
	MDB_pageparent mpp;

	assert(key);
	assert(data);
	DPRINTF("===> get key [%.*s]", (int)key->mv_size, (char *)key->mv_data);

	if (txn == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	if (key->mv_size == 0 || key->mv_size > MAXKEYSIZE) {
		return EINVAL;
	}

	if ((rc = mdb_search_page(txn, dbi, key, NULL, 0, &mpp)) != MDB_SUCCESS)
		return rc;

	leaf = mdb_search_node(txn, dbi, mpp.mp_page, key, &exact, NULL);
	if (leaf && exact) {
		/* Return first duplicate data item */
		if (F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
			MDB_xcursor mx;

			mdb_xcursor_init0(txn, dbi, &mx);
			mdb_xcursor_init1(txn, dbi, &mx, leaf);
			rc = mdb_search_page(&mx.mx_txn, mx.mx_cursor.mc_dbi, NULL, NULL, 0, &mpp);
			if (rc != MDB_SUCCESS)
				return rc;
			leaf = NODEPTR(mpp.mp_page, 0);
		}
		rc = mdb_read_data(txn, leaf, data);
	} else {
		rc = MDB_NOTFOUND;
	}

	return rc;
}

static int
mdb_sibling(MDB_cursor *cursor, int move_right)
{
	int		 rc;
	MDB_node	*indx;
	MDB_ppage	*parent, *top;
	MDB_page	*mp;

	top = CURSOR_TOP(cursor);
	if ((parent = SLIST_NEXT(top, mp_entry)) == NULL) {
		return MDB_NOTFOUND;		/* root has no siblings */
	}

	DPRINTF("parent page is page %lu, index %u",
	    parent->mp_page->mp_pgno, parent->mp_ki);

	cursor_pop_page(cursor);
	if (move_right ? (parent->mp_ki + 1 >= NUMKEYS(parent->mp_page))
		       : (parent->mp_ki == 0)) {
		DPRINTF("no more keys left, moving to %s sibling",
		    move_right ? "right" : "left");
		if ((rc = mdb_sibling(cursor, move_right)) != MDB_SUCCESS)
			return rc;
		parent = CURSOR_TOP(cursor);
	} else {
		if (move_right)
			parent->mp_ki++;
		else
			parent->mp_ki--;
		DPRINTF("just moving to %s index key %u",
		    move_right ? "right" : "left", parent->mp_ki);
	}
	assert(IS_BRANCH(parent->mp_page));

	indx = NODEPTR(parent->mp_page, parent->mp_ki);
	if ((mp = mdb_get_page(cursor->mc_txn, indx->mn_pgno)) == NULL)
		return MDB_FAIL;
#if 0
	mp->parent = parent->mp_page;
	mp->parent_index = parent->mp_ki;
#endif

	cursor_push_page(cursor, mp);

	return MDB_SUCCESS;
}

static int
mdb_set_key(MDB_node *node, MDB_val *key)
{
	if (key == NULL)
		return 0;

	key->mv_size = node->mn_ksize;
	key->mv_data = NODEKEY(node);

	return 0;
}

static int
mdb_cursor_next(MDB_cursor *cursor, MDB_val *key, MDB_val *data, MDB_cursor_op op)
{
	MDB_ppage	*top;
	MDB_page	*mp;
	MDB_node	*leaf;
	int rc;

	if (cursor->mc_eof) {
		return MDB_NOTFOUND;
	}

	assert(cursor->mc_initialized);

	if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
		if (op == MDB_NEXT || op == MDB_NEXT_DUP) {
			rc = mdb_cursor_next(&cursor->mc_xcursor->mx_cursor, data, NULL, MDB_NEXT);
			if (op != MDB_NEXT || rc == MDB_SUCCESS)
				return rc;
		}
	}

	top = CURSOR_TOP(cursor);
	mp = top->mp_page;

	DPRINTF("cursor_next: top page is %lu in cursor %p", mp->mp_pgno, (void *) cursor);

	if (top->mp_ki + 1 >= NUMKEYS(mp)) {
		DPRINTF("=====> move to next sibling page");
		if (mdb_sibling(cursor, 1) != MDB_SUCCESS) {
			cursor->mc_eof = 1;
			return MDB_NOTFOUND;
		}
		top = CURSOR_TOP(cursor);
		mp = top->mp_page;
		DPRINTF("next page is %lu, key index %u", mp->mp_pgno, top->mp_ki);
	} else
		top->mp_ki++;

	DPRINTF("==> cursor points to page %lu with %u keys, key index %u",
	    mp->mp_pgno, NUMKEYS(mp), top->mp_ki);

	assert(IS_LEAF(mp));
	leaf = NODEPTR(mp, top->mp_ki);

	if (data) {
		if ((rc = mdb_read_data(cursor->mc_txn, leaf, data) != MDB_SUCCESS))
			return rc;

		if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
			mdb_xcursor_init1(cursor->mc_txn, cursor->mc_dbi, cursor->mc_xcursor, leaf);
			rc = mdb_cursor_first(&cursor->mc_xcursor->mx_cursor, data, NULL);
			if (rc != MDB_SUCCESS)
				return rc;
		}
	}

	return mdb_set_key(leaf, key);
}

static int
mdb_cursor_prev(MDB_cursor *cursor, MDB_val *key, MDB_val *data, MDB_cursor_op op)
{
	MDB_ppage	*top;
	MDB_page	*mp;
	MDB_node	*leaf;
	int rc;

	assert(cursor->mc_initialized);

	if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
		if (op == MDB_PREV || op == MDB_PREV_DUP) {
			rc = mdb_cursor_prev(&cursor->mc_xcursor->mx_cursor, data, NULL, MDB_PREV);
			if (op != MDB_PREV || rc == MDB_SUCCESS)
				return rc;
		}
	}

	top = CURSOR_TOP(cursor);
	mp = top->mp_page;

	DPRINTF("cursor_prev: top page is %lu in cursor %p", mp->mp_pgno, (void *) cursor);

	if (top->mp_ki == 0)  {
		DPRINTF("=====> move to prev sibling page");
		if (mdb_sibling(cursor, 0) != MDB_SUCCESS) {
			return MDB_NOTFOUND;
		}
		top = CURSOR_TOP(cursor);
		mp = top->mp_page;
		top->mp_ki = NUMKEYS(mp) - 1;
		DPRINTF("prev page is %lu, key index %u", mp->mp_pgno, top->mp_ki);
	} else
		top->mp_ki--;

	cursor->mc_eof = 0;

	DPRINTF("==> cursor points to page %lu with %u keys, key index %u",
	    mp->mp_pgno, NUMKEYS(mp), top->mp_ki);

	assert(IS_LEAF(mp));
	leaf = NODEPTR(mp, top->mp_ki);

	if (data) {
		if ((rc = mdb_read_data(cursor->mc_txn, leaf, data) != MDB_SUCCESS))
			return rc;

		if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
			mdb_xcursor_init1(cursor->mc_txn, cursor->mc_dbi, cursor->mc_xcursor, leaf);
			rc = mdb_cursor_last(&cursor->mc_xcursor->mx_cursor, data, NULL);
			if (rc != MDB_SUCCESS)
				return rc;
		}
	}

	return mdb_set_key(leaf, key);
}

static int
mdb_cursor_set(MDB_cursor *cursor, MDB_val *key, MDB_val *data,
    MDB_cursor_op op, int *exactp)
{
	int		 rc;
	MDB_node	*leaf;
	MDB_ppage	*top;
	MDB_pageparent mpp;

	assert(cursor);
	assert(key);
	assert(key->mv_size > 0);

	while (CURSOR_TOP(cursor) != NULL)
		cursor_pop_page(cursor);

	rc = mdb_search_page(cursor->mc_txn, cursor->mc_dbi, key, cursor, 0, &mpp);
	if (rc != MDB_SUCCESS)
		return rc;
	assert(IS_LEAF(mpp.mp_page));

	top = CURSOR_TOP(cursor);
	leaf = mdb_search_node(cursor->mc_txn, cursor->mc_dbi, mpp.mp_page, key, exactp, &top->mp_ki);
	if (exactp != NULL && !*exactp) {
		/* MDB_SET specified and not an exact match. */
		return MDB_NOTFOUND;
	}

	if (leaf == NULL) {
		DPRINTF("===> inexact leaf not found, goto sibling");
		if ((rc = mdb_sibling(cursor, 1)) != MDB_SUCCESS)
			return rc;		/* no entries matched */
		top = CURSOR_TOP(cursor);
		top->mp_ki = 0;
		mpp.mp_page = top->mp_page;
		assert(IS_LEAF(mpp.mp_page));
		leaf = NODEPTR(mpp.mp_page, 0);
	}

	cursor->mc_initialized = 1;
	cursor->mc_eof = 0;

	if (data) {
		if ((rc = mdb_read_data(cursor->mc_txn, leaf, data)) != MDB_SUCCESS)
			return rc;

		if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
			mdb_xcursor_init1(cursor->mc_txn, cursor->mc_dbi, cursor->mc_xcursor, leaf);
			if (op == MDB_SET || op == MDB_SET_RANGE) {
				rc = mdb_cursor_first(&cursor->mc_xcursor->mx_cursor, data, NULL);
			} else {
				int ex2, *ex2p;
				MDB_cursor_op op2;
				if (op == MDB_GET_BOTH) {
					ex2p = &ex2;
					op2 = MDB_SET;
				} else {
					ex2p = NULL;
					op2 = MDB_SET_RANGE;
				}
				rc = mdb_cursor_set(&cursor->mc_xcursor->mx_cursor, data, NULL, op2, ex2p);
				if (rc != MDB_SUCCESS)
					return rc;
			}
		}
	}

	rc = mdb_set_key(leaf, key);
	if (rc == MDB_SUCCESS) {
		DPRINTF("==> cursor placed on key %.*s",
			(int)key->mv_size, (char *)key->mv_data);
		;
	}

	return rc;
}

static int
mdb_cursor_first(MDB_cursor *cursor, MDB_val *key, MDB_val *data)
{
	int		 rc;
	MDB_pageparent	mpp;
	MDB_node	*leaf;

	while (CURSOR_TOP(cursor) != NULL)
		cursor_pop_page(cursor);

	rc = mdb_search_page(cursor->mc_txn, cursor->mc_dbi, NULL, cursor, 0, &mpp);
	if (rc != MDB_SUCCESS)
		return rc;
	assert(IS_LEAF(mpp.mp_page));

	leaf = NODEPTR(mpp.mp_page, 0);
	cursor->mc_initialized = 1;
	cursor->mc_eof = 0;

	if (data) {
		if ((rc = mdb_read_data(cursor->mc_txn, leaf, data)) != MDB_SUCCESS)
			return rc;

		if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
			mdb_xcursor_init1(cursor->mc_txn, cursor->mc_dbi, cursor->mc_xcursor, leaf);
			rc = mdb_cursor_first(&cursor->mc_xcursor->mx_cursor, data, NULL);
			if (rc)
				return rc;
		}
	}
	return mdb_set_key(leaf, key);
}

static int
mdb_cursor_last(MDB_cursor *cursor, MDB_val *key, MDB_val *data)
{
	int		 rc;
	MDB_ppage	*top;
	MDB_pageparent	mpp;
	MDB_node	*leaf;
	MDB_val	lkey;

	while (CURSOR_TOP(cursor) != NULL)
		cursor_pop_page(cursor);

	lkey.mv_size = MAXKEYSIZE+1;
	lkey.mv_data = NULL;

	rc = mdb_search_page(cursor->mc_txn, cursor->mc_dbi, &lkey, cursor, 0, &mpp);
	if (rc != MDB_SUCCESS)
		return rc;
	assert(IS_LEAF(mpp.mp_page));

	leaf = NODEPTR(mpp.mp_page, NUMKEYS(mpp.mp_page)-1);
	cursor->mc_initialized = 1;
	cursor->mc_eof = 0;

	top = CURSOR_TOP(cursor);
	top->mp_ki = NUMKEYS(top->mp_page) - 1;

	if (data) {
		if ((rc = mdb_read_data(cursor->mc_txn, leaf, data)) != MDB_SUCCESS)
			return rc;

		if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
			mdb_xcursor_init1(cursor->mc_txn, cursor->mc_dbi, cursor->mc_xcursor, leaf);
			rc = mdb_cursor_last(&cursor->mc_xcursor->mx_cursor, data, NULL);
			if (rc)
				return rc;
		}
	}

	return mdb_set_key(leaf, key);
}

int
mdb_cursor_get(MDB_cursor *cursor, MDB_val *key, MDB_val *data,
    MDB_cursor_op op)
{
	int		 rc;
	int		 exact = 0;

	assert(cursor);

	switch (op) {
	case MDB_GET_BOTH:
	case MDB_GET_BOTH_RANGE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		/* FALLTHRU */
	case MDB_SET:
	case MDB_SET_RANGE:
		if (key == NULL || key->mv_size == 0 || key->mv_size > MAXKEYSIZE) {
			rc = EINVAL;
		} else if (op != MDB_SET_RANGE)
			rc = mdb_cursor_set(cursor, key, data, op, NULL);
		else
			rc = mdb_cursor_set(cursor, key, data, op, &exact);
		break;
	case MDB_NEXT:
	case MDB_NEXT_DUP:
	case MDB_NEXT_NODUP:
		if (!cursor->mc_initialized)
			rc = mdb_cursor_first(cursor, key, data);
		else
			rc = mdb_cursor_next(cursor, key, data, op);
		break;
	case MDB_PREV:
	case MDB_PREV_DUP:
	case MDB_PREV_NODUP:
		if (!cursor->mc_initialized || cursor->mc_eof)
			rc = mdb_cursor_last(cursor, key, data);
		else
			rc = mdb_cursor_prev(cursor, key, data, op);
		break;
	case MDB_FIRST:
		rc = mdb_cursor_first(cursor, key, data);
		break;
	case MDB_LAST:
		rc = mdb_cursor_last(cursor, key, data);
		break;
	default:
		DPRINTF("unhandled/unimplemented cursor operation %u", op);
		rc = EINVAL;
		break;
	}

	return rc;
}

/* Allocate a page and initialize it
 */
static MDB_dpage *
mdb_new_page(MDB_txn *txn, MDB_dbi dbi, uint32_t flags, int num)
{
	MDB_dpage	*dp;

	if ((dp = mdb_alloc_page(txn, NULL, 0, num)) == NULL)
		return NULL;
	DPRINTF("allocated new mpage %lu, page size %u",
	    dp->p.mp_pgno, txn->mt_env->me_psize);
	dp->p.mp_flags = flags | P_DIRTY;
	dp->p.mp_lower = PAGEHDRSZ;
	dp->p.mp_upper = txn->mt_env->me_psize;

	if (IS_BRANCH(&dp->p))
		txn->mt_dbs[dbi].md_branch_pages++;
	else if (IS_LEAF(&dp->p))
		txn->mt_dbs[dbi].md_leaf_pages++;
	else if (IS_OVERFLOW(&dp->p)) {
		txn->mt_dbs[dbi].md_overflow_pages += num;
		dp->p.mp_pages = num;
	}

	return dp;
}

static size_t
mdb_leaf_size(MDB_env *env, MDB_val *key, MDB_val *data)
{
	size_t		 sz;

	sz = LEAFSIZE(key, data);
	if (data->mv_size >= env->me_psize / MDB_MINKEYS) {
		/* put on overflow page */
		sz -= data->mv_size - sizeof(pgno_t);
	}

	return sz + sizeof(indx_t);
}

static size_t
mdb_branch_size(MDB_env *env, MDB_val *key)
{
	size_t		 sz;

	sz = INDXSIZE(key);
	if (sz >= env->me_psize / MDB_MINKEYS) {
		/* put on overflow page */
		/* not implemented */
		/* sz -= key->size - sizeof(pgno_t); */
	}

	return sz + sizeof(indx_t);
}

static int
mdb_add_node(MDB_txn *txn, MDB_dbi dbi, MDB_page *mp, indx_t indx,
    MDB_val *key, MDB_val *data, pgno_t pgno, uint8_t flags)
{
	unsigned int	 i;
	size_t		 node_size = NODESIZE;
	indx_t		 ofs;
	MDB_node	*node;
	MDB_dpage	*ofp = NULL;		/* overflow page */

	assert(mp->mp_upper >= mp->mp_lower);

	DPRINTF("add node [%.*s] to %s page %lu at index %i, key size %zu",
	    key ? (int)key->mv_size : 0, key ? (char *)key->mv_data : NULL,
	    IS_LEAF(mp) ? "leaf" : "branch",
	    mp->mp_pgno, indx, key ? key->mv_size : 0);

	if (key != NULL)
		node_size += key->mv_size;

	if (IS_LEAF(mp)) {
		assert(data);
		if (F_ISSET(flags, F_BIGDATA)) {
			/* Data already on overflow page. */
			node_size += sizeof(pgno_t);
		} else if (data->mv_size >= txn->mt_env->me_psize / MDB_MINKEYS) {
			int ovpages = OVPAGES(data->mv_size, txn->mt_env->me_psize);
			/* Put data on overflow page. */
			DPRINTF("data size is %zu, put on overflow page",
			    data->mv_size);
			node_size += sizeof(pgno_t);
			if ((ofp = mdb_new_page(txn, dbi, P_OVERFLOW, ovpages)) == NULL)
				return MDB_FAIL;
			DPRINTF("allocated overflow page %lu", ofp->p.mp_pgno);
			flags |= F_BIGDATA;
		} else {
			node_size += data->mv_size;
		}
	}

	if (node_size + sizeof(indx_t) > SIZELEFT(mp)) {
		DPRINTF("not enough room in page %lu, got %u ptrs",
		    mp->mp_pgno, NUMKEYS(mp));
		DPRINTF("upper - lower = %u - %u = %u", mp->mp_upper, mp->mp_lower,
		    mp->mp_upper - mp->mp_lower);
		DPRINTF("node size = %zu", node_size);
		return ENOSPC;
	}

	/* Move higher pointers up one slot. */
	for (i = NUMKEYS(mp); i > indx; i--)
		mp->mp_ptrs[i] = mp->mp_ptrs[i - 1];

	/* Adjust free space offsets. */
	ofs = mp->mp_upper - node_size;
	assert(ofs >= mp->mp_lower + sizeof(indx_t));
	mp->mp_ptrs[indx] = ofs;
	mp->mp_upper = ofs;
	mp->mp_lower += sizeof(indx_t);

	/* Write the node data. */
	node = NODEPTR(mp, indx);
	node->mn_ksize = (key == NULL) ? 0 : key->mv_size;
	node->mn_flags = flags;
	if (IS_LEAF(mp))
		node->mn_dsize = data->mv_size;
	else
		node->mn_pgno = pgno;

	if (key)
		memcpy(NODEKEY(node), key->mv_data, key->mv_size);

	if (IS_LEAF(mp)) {
		assert(key);
		if (ofp == NULL) {
			if (F_ISSET(flags, F_BIGDATA))
				memcpy(node->mn_data + key->mv_size, data->mv_data,
				    sizeof(pgno_t));
			else
				memcpy(node->mn_data + key->mv_size, data->mv_data,
				    data->mv_size);
		} else {
			memcpy(node->mn_data + key->mv_size, &ofp->p.mp_pgno,
			    sizeof(pgno_t));
			memcpy(METADATA(&ofp->p), data->mv_data, data->mv_size);
		}
	}

	return MDB_SUCCESS;
}

static void
mdb_del_node(MDB_page *mp, indx_t indx)
{
	unsigned int	 sz;
	indx_t		 i, j, numkeys, ptr;
	MDB_node	*node;
	char		*base;

	DPRINTF("delete node %u on %s page %lu", indx,
	    IS_LEAF(mp) ? "leaf" : "branch", mp->mp_pgno);
	assert(indx < NUMKEYS(mp));

	node = NODEPTR(mp, indx);
	sz = NODESIZE + node->mn_ksize;
	if (IS_LEAF(mp)) {
		if (F_ISSET(node->mn_flags, F_BIGDATA))
			sz += sizeof(pgno_t);
		else
			sz += NODEDSZ(node);
	}

	ptr = mp->mp_ptrs[indx];
	numkeys = NUMKEYS(mp);
	for (i = j = 0; i < numkeys; i++) {
		if (i != indx) {
			mp->mp_ptrs[j] = mp->mp_ptrs[i];
			if (mp->mp_ptrs[i] < ptr)
				mp->mp_ptrs[j] += sz;
			j++;
		}
	}

	base = (char *)mp + mp->mp_upper;
	memmove(base + sz, base, ptr - mp->mp_upper);

	mp->mp_lower -= sizeof(indx_t);
	mp->mp_upper += sz;
}

static void
mdb_xcursor_init0(MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx)
{
	MDB_dbi dbn;

	mx->mx_txn = *txn;
	mx->mx_txn.mt_dbxs = mx->mx_dbxs;
	mx->mx_txn.mt_dbs = mx->mx_dbs;
	mx->mx_dbxs[0] = txn->mt_dbxs[0];
	mx->mx_dbxs[1] = txn->mt_dbxs[1];
	if (dbi > 1) {
		mx->mx_dbxs[2] = txn->mt_dbxs[dbi];
		dbn = 2;
	} else {
		dbn = 1;
	}
	mx->mx_dbxs[dbn+1].md_parent = dbn;
	mx->mx_dbxs[dbn+1].md_cmp = mx->mx_dbxs[dbn].md_dcmp;
	mx->mx_dbxs[dbn+1].md_rel = mx->mx_dbxs[dbn].md_rel;
	mx->mx_dbxs[dbn+1].md_dirty = 0;
	mx->mx_txn.mt_numdbs = dbn+2;

	SLIST_INIT(&mx->mx_cursor.mc_stack);
	mx->mx_cursor.mc_txn = &mx->mx_txn;
	mx->mx_cursor.mc_dbi = dbn+1;
}

static void
mdb_xcursor_init1(MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx, MDB_node *node)
{
	MDB_db *db = NODEDATA(node);
	MDB_dbi dbn;
	mx->mx_dbs[0] = txn->mt_dbs[0];
	mx->mx_dbs[1] = txn->mt_dbs[1];
	if (dbi > 1) {
		mx->mx_dbs[2] = txn->mt_dbs[dbi];
		dbn = 3;
	} else {
		dbn = 2;
	}
	mx->mx_dbs[dbn] = *db;
	mx->mx_dbxs[dbn].md_name.mv_data = NODEKEY(node);
	mx->mx_dbxs[dbn].md_name.mv_size = node->mn_ksize;
	mx->mx_txn.mt_next_pgno = txn->mt_next_pgno;
	mx->mx_txn.mt_oldest = txn->mt_oldest;
	mx->mx_txn.mt_u = txn->mt_u;
}

static void
mdb_xcursor_fini(MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx)
{
	txn->mt_next_pgno = mx->mx_txn.mt_next_pgno;
	txn->mt_oldest = mx->mx_txn.mt_oldest;
	txn->mt_u = mx->mx_txn.mt_u;
	txn->mt_dbs[0] = mx->mx_dbs[0];
	txn->mt_dbs[1] = mx->mx_dbs[1];
	txn->mt_dbxs[0].md_dirty = mx->mx_dbxs[0].md_dirty;
	txn->mt_dbxs[1].md_dirty = mx->mx_dbxs[1].md_dirty;
	if (dbi > 1) {
		txn->mt_dbs[dbi] = mx->mx_dbs[2];
		txn->mt_dbxs[dbi].md_dirty = mx->mx_dbxs[2].md_dirty;
	}
}

int
mdb_cursor_open(MDB_txn *txn, MDB_dbi dbi, MDB_cursor **ret)
{
	MDB_cursor	*cursor;
	size_t size = sizeof(MDB_cursor);

	if (txn == NULL || ret == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT)
		size += sizeof(MDB_xcursor);

	if ((cursor = calloc(1, size)) != NULL) {
		SLIST_INIT(&cursor->mc_stack);
		cursor->mc_dbi = dbi;
		cursor->mc_txn = txn;
		if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT) {
			MDB_xcursor *mx = (MDB_xcursor *)(cursor + 1);
			cursor->mc_xcursor = mx;
			mdb_xcursor_init0(txn, dbi, mx);
		}
	} else {
		return ENOMEM;
	}

	*ret = cursor;

	return MDB_SUCCESS;
}

/* Return the count of duplicate data items for the current key */
int
mdb_cursor_count(MDB_cursor *mc, unsigned long *countp)
{
	if (mc == NULL || countp == NULL)
		return EINVAL;

	if (!(mc->mc_txn->mt_dbs[mc->mc_dbi].md_flags & MDB_DUPSORT))
		return EINVAL;

	if (!mc->mc_xcursor->mx_cursor.mc_initialized)
		return EINVAL;

	*countp = mc->mc_xcursor->mx_txn.mt_dbs[mc->mc_xcursor->mx_cursor.mc_dbi].md_entries;
	return MDB_SUCCESS;
}

void
mdb_cursor_close(MDB_cursor *cursor)
{
	if (cursor != NULL) {
		while(!CURSOR_EMPTY(cursor))
			cursor_pop_page(cursor);
		if (cursor->mc_txn->mt_dbs[cursor->mc_dbi].md_flags & MDB_DUPSORT) {
			mdb_xcursor_fini(cursor->mc_txn, cursor->mc_dbi, cursor->mc_xcursor);
			while(!CURSOR_EMPTY(&cursor->mc_xcursor->mx_cursor))
				cursor_pop_page(&cursor->mc_xcursor->mx_cursor);
		}

		free(cursor);
	}
}

static int
mdb_update_key(MDB_page *mp, indx_t indx, MDB_val *key)
{
	indx_t			 ptr, i, numkeys;
	int			 delta;
	size_t			 len;
	MDB_node		*node;
	char			*base;

	node = NODEPTR(mp, indx);
	ptr = mp->mp_ptrs[indx];
	DPRINTF("update key %u (ofs %u) [%.*s] to [%.*s] on page %lu",
	    indx, ptr,
	    (int)node->mn_ksize, (char *)NODEKEY(node),
	    (int)key->mv_size, (char *)key->mv_data,
	    mp->mp_pgno);

	delta = key->mv_size - node->mn_ksize;
	if (delta) {
		if (delta > 0 && SIZELEFT(mp) < delta) {
			DPRINTF("OUCH! Not enough room, delta = %d", delta);
			return ENOSPC;
		}

		numkeys = NUMKEYS(mp);
		for (i = 0; i < numkeys; i++) {
			if (mp->mp_ptrs[i] <= ptr)
				mp->mp_ptrs[i] -= delta;
		}

		base = (char *)mp + mp->mp_upper;
		len = ptr - mp->mp_upper + NODESIZE;
		memmove(base - delta, base, len);
		mp->mp_upper -= delta;

		node = NODEPTR(mp, indx);
		node->mn_ksize = key->mv_size;
	}

	memcpy(NODEKEY(node), key->mv_data, key->mv_size);

	return MDB_SUCCESS;
}

/* Move a node from src to dst.
 */
static int
mdb_move_node(MDB_txn *txn, MDB_dbi dbi, MDB_pageparent *src, indx_t srcindx,
    MDB_pageparent *dst, indx_t dstindx)
{
	int			 rc;
	MDB_node		*srcnode;
	MDB_val		 key, data;

	srcnode = NODEPTR(src->mp_page, srcindx);
	DPRINTF("moving %s node %u [%.*s] on page %lu to node %u on page %lu",
	    IS_LEAF(src->mp_page) ? "leaf" : "branch",
	    srcindx,
	    (int)srcnode->mn_ksize, (char *)NODEKEY(srcnode),
	    src->mp_page->mp_pgno,
	    dstindx, dst->mp_page->mp_pgno);

	/* Mark src and dst as dirty. */
	if ((rc = mdb_touch(txn, src)) ||
	    (rc = mdb_touch(txn, dst)))
		return rc;;

	/* Add the node to the destination page.
	 */
	key.mv_size = srcnode->mn_ksize;
	key.mv_data = NODEKEY(srcnode);
	data.mv_size = NODEDSZ(srcnode);
	data.mv_data = NODEDATA(srcnode);
	rc = mdb_add_node(txn, dbi, dst->mp_page, dstindx, &key, &data, NODEPGNO(srcnode),
	    srcnode->mn_flags);
	if (rc != MDB_SUCCESS)
		return rc;

	/* Delete the node from the source page.
	 */
	mdb_del_node(src->mp_page, srcindx);

	/* Update the parent separators.
	 */
	if (srcindx == 0 && src->mp_pi != 0) {
		DPRINTF("update separator for source page %lu to [%.*s]",
		    src->mp_page->mp_pgno, (int)key.mv_size, (char *)key.mv_data);
		if ((rc = mdb_update_key(src->mp_parent, src->mp_pi,
		    &key)) != MDB_SUCCESS)
			return rc;
	}

	if (srcindx == 0 && IS_BRANCH(src->mp_page)) {
		MDB_val	 nullkey;
		nullkey.mv_size = 0;
		assert(mdb_update_key(src->mp_page, 0, &nullkey) == MDB_SUCCESS);
	}

	if (dstindx == 0 && dst->mp_pi != 0) {
		DPRINTF("update separator for destination page %lu to [%.*s]",
		    dst->mp_page->mp_pgno, (int)key.mv_size, (char *)key.mv_data);
		if ((rc = mdb_update_key(dst->mp_parent, dst->mp_pi,
		    &key)) != MDB_SUCCESS)
			return rc;
	}

	if (dstindx == 0 && IS_BRANCH(dst->mp_page)) {
		MDB_val	 nullkey;
		nullkey.mv_size = 0;
		assert(mdb_update_key(dst->mp_page, 0, &nullkey) == MDB_SUCCESS);
	}

	return MDB_SUCCESS;
}

static int
mdb_merge(MDB_txn *txn, MDB_dbi dbi, MDB_pageparent *src, MDB_pageparent *dst)
{
	int			 rc;
	indx_t			 i;
	MDB_node		*srcnode;
	MDB_val		 key, data;
	MDB_pageparent	mpp;
	MDB_dhead *dh;

	DPRINTF("merging page %lu and %lu", src->mp_page->mp_pgno, dst->mp_page->mp_pgno);

	assert(txn != NULL);
	assert(src->mp_parent);	/* can't merge root page */
	assert(dst->mp_parent);

	/* Mark src and dst as dirty. */
	if ((rc = mdb_touch(txn, src)) ||
	    (rc = mdb_touch(txn, dst)))
		return rc;

	/* Move all nodes from src to dst.
	 */
	for (i = 0; i < NUMKEYS(src->mp_page); i++) {
		srcnode = NODEPTR(src->mp_page, i);

		key.mv_size = srcnode->mn_ksize;
		key.mv_data = NODEKEY(srcnode);
		data.mv_size = NODEDSZ(srcnode);
		data.mv_data = NODEDATA(srcnode);
		rc = mdb_add_node(txn, dbi, dst->mp_page, NUMKEYS(dst->mp_page), &key,
		    &data, NODEPGNO(srcnode), srcnode->mn_flags);
		if (rc != MDB_SUCCESS)
			return rc;
	}

	DPRINTF("dst page %lu now has %u keys (%.1f%% filled)",
	    dst->mp_page->mp_pgno, NUMKEYS(dst->mp_page), (float)PAGEFILL(txn->mt_env, dst->mp_page) / 10);

	/* Unlink the src page from parent.
	 */
	mdb_del_node(src->mp_parent, src->mp_pi);
	if (src->mp_pi == 0) {
		key.mv_size = 0;
		if ((rc = mdb_update_key(src->mp_parent, 0, &key)) != MDB_SUCCESS)
			return rc;
	}

	if (IS_LEAF(src->mp_page))
		txn->mt_dbs[dbi].md_leaf_pages--;
	else
		txn->mt_dbs[dbi].md_branch_pages--;

	mpp.mp_page = src->mp_parent;
	dh = (MDB_dhead *)src->mp_parent;
	dh--;
	mpp.mp_parent = dh->md_parent;
	mpp.mp_pi = dh->md_pi;

	return mdb_rebalance(txn, dbi, &mpp);
}

#define FILL_THRESHOLD	 250

static int
mdb_rebalance(MDB_txn *txn, MDB_dbi dbi, MDB_pageparent *mpp)
{
	MDB_node	*node;
	MDB_page	*root;
	MDB_pageparent npp;
	indx_t		 si = 0, di = 0;

	assert(txn != NULL);
	assert(mpp != NULL);

	DPRINTF("rebalancing %s page %lu (has %u keys, %.1f%% full)",
	    IS_LEAF(mpp->mp_page) ? "leaf" : "branch",
	    mpp->mp_page->mp_pgno, NUMKEYS(mpp->mp_page), (float)PAGEFILL(txn->mt_env, mpp->mp_page) / 10);

	if (PAGEFILL(txn->mt_env, mpp->mp_page) >= FILL_THRESHOLD) {
		DPRINTF("no need to rebalance page %lu, above fill threshold",
		    mpp->mp_page->mp_pgno);
		return MDB_SUCCESS;
	}

	if (mpp->mp_parent == NULL) {
		if (NUMKEYS(mpp->mp_page) == 0) {
			DPRINTF("tree is completely empty");
			txn->mt_dbs[dbi].md_root = P_INVALID;
			txn->mt_dbs[dbi].md_depth--;
			txn->mt_dbs[dbi].md_leaf_pages--;
		} else if (IS_BRANCH(mpp->mp_page) && NUMKEYS(mpp->mp_page) == 1) {
			DPRINTF("collapsing root page!");
			txn->mt_dbs[dbi].md_root = NODEPGNO(NODEPTR(mpp->mp_page, 0));
			if ((root = mdb_get_page(txn, txn->mt_dbs[dbi].md_root)) == NULL)
				return MDB_FAIL;
			txn->mt_dbs[dbi].md_depth--;
			txn->mt_dbs[dbi].md_branch_pages--;
		} else
			DPRINTF("root page doesn't need rebalancing");
		return MDB_SUCCESS;
	}

	/* The parent (branch page) must have at least 2 pointers,
	 * otherwise the tree is invalid.
	 */
	assert(NUMKEYS(mpp->mp_parent) > 1);

	/* Leaf page fill factor is below the threshold.
	 * Try to move keys from left or right neighbor, or
	 * merge with a neighbor page.
	 */

	/* Find neighbors.
	 */
	if (mpp->mp_pi == 0) {
		/* We're the leftmost leaf in our parent.
		 */
		DPRINTF("reading right neighbor");
		node = NODEPTR(mpp->mp_parent, mpp->mp_pi + 1);
		if ((npp.mp_page = mdb_get_page(txn, NODEPGNO(node))) == NULL)
			return MDB_FAIL;
		npp.mp_pi = mpp->mp_pi + 1;
		si = 0;
		di = NUMKEYS(mpp->mp_page);
	} else {
		/* There is at least one neighbor to the left.
		 */
		DPRINTF("reading left neighbor");
		node = NODEPTR(mpp->mp_parent, mpp->mp_pi - 1);
		if ((npp.mp_page = mdb_get_page(txn, NODEPGNO(node))) == NULL)
			return MDB_FAIL;
		npp.mp_pi = mpp->mp_pi - 1;
		si = NUMKEYS(npp.mp_page) - 1;
		di = 0;
	}
	npp.mp_parent = mpp->mp_parent;

	DPRINTF("found neighbor page %lu (%u keys, %.1f%% full)",
	    npp.mp_page->mp_pgno, NUMKEYS(npp.mp_page), (float)PAGEFILL(txn->mt_env, npp.mp_page) / 10);

	/* If the neighbor page is above threshold and has at least two
	 * keys, move one key from it.
	 *
	 * Otherwise we should try to merge them.
	 */
	if (PAGEFILL(txn->mt_env, npp.mp_page) >= FILL_THRESHOLD && NUMKEYS(npp.mp_page) >= 2)
		return mdb_move_node(txn, dbi, &npp, si, mpp, di);
	else { /* FIXME: if (has_enough_room()) */
		if (mpp->mp_pi == 0)
			return mdb_merge(txn, dbi, &npp, mpp);
		else
			return mdb_merge(txn, dbi, mpp, &npp);
	}
}

static int
mdb_del0(MDB_txn *txn, MDB_dbi dbi, unsigned int ki, MDB_pageparent *mpp, MDB_node *leaf)
{
	int rc;

	/* add overflow pages to free list */
	if (F_ISSET(leaf->mn_flags, F_BIGDATA)) {
		int i, ovpages;
		pgno_t pg;

		memcpy(&pg, NODEDATA(leaf), sizeof(pg));
		ovpages = OVPAGES(NODEDSZ(leaf), txn->mt_env->me_psize);
		for (i=0; i<ovpages; i++) {
			DPRINTF("freed ov page %lu", pg);
			mdb_idl_insert(txn->mt_free_pgs, pg);
			pg++;
		}
	}
	mdb_del_node(mpp->mp_page, ki);
	txn->mt_dbs[dbi].md_entries--;
	rc = mdb_rebalance(txn, dbi, mpp);
	if (rc != MDB_SUCCESS)
		txn->mt_flags |= MDB_TXN_ERROR;

	return rc;
}

int
mdb_del(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data,
	unsigned int flags)
{
	int		 rc, exact;
	unsigned int	 ki;
	MDB_node	*leaf;
	MDB_pageparent	mpp;

	DPRINTF("========> delete key %.*s", (int)key->mv_size, (char *)key->mv_data);

	assert(key != NULL);

	if (txn == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		return EINVAL;
	}

	if (key->mv_size == 0 || key->mv_size > MAXKEYSIZE) {
		return EINVAL;
	}

	mpp.mp_parent = NULL;
	mpp.mp_pi = 0;
	if ((rc = mdb_search_page(txn, dbi, key, NULL, 1, &mpp)) != MDB_SUCCESS)
		return rc;

	leaf = mdb_search_node(txn, dbi, mpp.mp_page, key, &exact, &ki);
	if (leaf == NULL || !exact) {
		return MDB_NOTFOUND;
	}

	if (F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
		MDB_xcursor mx;
		MDB_pageparent mp2;

		mdb_xcursor_init0(txn, dbi, &mx);
		mdb_xcursor_init1(txn, dbi, &mx, leaf);
		if (flags == MDB_DEL_DUP) {
			rc = mdb_del(&mx.mx_txn, mx.mx_cursor.mc_dbi, data, NULL, 0);
			mdb_xcursor_fini(txn, dbi, &mx);
			if (rc != MDB_SUCCESS)
				return rc;
			/* If sub-DB still has entries, we're done */
			if (mx.mx_txn.mt_dbs[mx.mx_cursor.mc_dbi].md_root != P_INVALID) {
				memcpy(NODEDATA(leaf), &mx.mx_txn.mt_dbs[mx.mx_cursor.mc_dbi],
					sizeof(MDB_db));
				return rc;
			}
			/* otherwise fall thru and delete the sub-DB */
		} else {
			/* add all the child DB's pages to the free list */
			rc = mdb_search_page(&mx.mx_txn, mx.mx_cursor.mc_dbi,
				NULL, &mx.mx_cursor, 0, &mp2);
			if (rc == MDB_SUCCESS) {
				MDB_ppage *top, *parent;
				MDB_node *ni;
				unsigned int i;

				cursor_pop_page(&mx.mx_cursor);
				top = CURSOR_TOP(&mx.mx_cursor);
				if (top != NULL) {
					parent = SLIST_NEXT(top, mp_entry);
					while (parent != NULL) {
						for (i=0; i<NUMKEYS(top->mp_page); i++) {
							ni = NODEPTR(top->mp_page, i);
							mdb_idl_insert(txn->mt_free_pgs, ni->mn_pgno);
						}
						if (parent) {
							parent->mp_ki++;
							if (parent->mp_ki >= NUMKEYS(parent->mp_page)) {
								cursor_pop_page(&mx.mx_cursor);
								top = CURSOR_TOP(&mx.mx_cursor);
								parent = SLIST_NEXT(top, mp_entry);
							} else {
								ni = NODEPTR(parent->mp_page, parent->mp_ki);
								top->mp_page = mdb_get_page(&mx.mx_txn, ni->mn_pgno);
							}
						}
					}
				}
				mdb_idl_insert(txn->mt_free_pgs, mx.mx_txn.mt_dbs[mx.mx_cursor.mc_dbi].md_root);
			}
		}
	}

	if (data && (rc = mdb_read_data(txn, leaf, data)) != MDB_SUCCESS)
		return rc;

	return mdb_del0(txn, dbi, ki, &mpp, leaf);
}

/* Split page <*mpp>, and insert <key,(data|newpgno)> in either left or
 * right sibling, at index <*newindxp> (as if unsplit). Updates *mpp and
 * *newindxp with the actual values after split, ie if *mpp and *newindxp
 * refer to a node in the new right sibling page.
 */
static int
mdb_split(MDB_txn *txn, MDB_dbi dbi, MDB_page **mpp, unsigned int *newindxp,
    MDB_val *newkey, MDB_val *newdata, pgno_t newpgno)
{
	uint8_t		 flags;
	int		 rc = MDB_SUCCESS, ins_new = 0;
	indx_t		 newindx;
	pgno_t		 pgno = 0;
	unsigned int	 i, j, split_indx;
	MDB_node	*node;
	MDB_val	 sepkey, rkey, rdata;
	MDB_page	*copy;
	MDB_dpage	*mdp, *rdp, *pdp;
	MDB_dhead *dh;

	assert(txn != NULL);

	dh = ((MDB_dhead *)*mpp) - 1;
	mdp = (MDB_dpage *)dh;
	newindx = *newindxp;

	DPRINTF("-----> splitting %s page %lu and adding [%.*s] at index %i",
	    IS_LEAF(&mdp->p) ? "leaf" : "branch", mdp->p.mp_pgno,
	    (int)newkey->mv_size, (char *)newkey->mv_data, *newindxp);

	if (mdp->h.md_parent == NULL) {
		if ((pdp = mdb_new_page(txn, dbi, P_BRANCH, 1)) == NULL)
			return MDB_FAIL;
		mdp->h.md_pi = 0;
		mdp->h.md_parent = &pdp->p;
		txn->mt_dbs[dbi].md_root = pdp->p.mp_pgno;
		DPRINTF("root split! new root = %lu", pdp->p.mp_pgno);
		txn->mt_dbs[dbi].md_depth++;

		/* Add left (implicit) pointer. */
		if (mdb_add_node(txn, dbi, &pdp->p, 0, NULL, NULL,
		    mdp->p.mp_pgno, 0) != MDB_SUCCESS)
			return MDB_FAIL;
	} else {
		DPRINTF("parent branch page is %lu", mdp->h.md_parent->mp_pgno);
	}

	/* Create a right sibling. */
	if ((rdp = mdb_new_page(txn, dbi, mdp->p.mp_flags, 1)) == NULL)
		return MDB_FAIL;
	rdp->h.md_parent = mdp->h.md_parent;
	rdp->h.md_pi = mdp->h.md_pi + 1;
	DPRINTF("new right sibling: page %lu", rdp->p.mp_pgno);

	/* Move half of the keys to the right sibling. */
	if ((copy = malloc(txn->mt_env->me_psize)) == NULL)
		return MDB_FAIL;
	memcpy(copy, &mdp->p, txn->mt_env->me_psize);
	memset(&mdp->p.mp_ptrs, 0, txn->mt_env->me_psize - PAGEHDRSZ);
	mdp->p.mp_lower = PAGEHDRSZ;
	mdp->p.mp_upper = txn->mt_env->me_psize;

	split_indx = NUMKEYS(copy) / 2 + 1;

	/* First find the separating key between the split pages.
	 */
	memset(&sepkey, 0, sizeof(sepkey));
	if (newindx == split_indx) {
		sepkey.mv_size = newkey->mv_size;
		sepkey.mv_data = newkey->mv_data;
	} else {
		node = NODEPTR(copy, split_indx);
		sepkey.mv_size = node->mn_ksize;
		sepkey.mv_data = NODEKEY(node);
	}

	DPRINTF("separator is [%.*s]", (int)sepkey.mv_size, (char *)sepkey.mv_data);

	/* Copy separator key to the parent.
	 */
	if (SIZELEFT(rdp->h.md_parent) < mdb_branch_size(txn->mt_env, &sepkey)) {
		rc = mdb_split(txn, dbi, &rdp->h.md_parent, &rdp->h.md_pi,
		    &sepkey, NULL, rdp->p.mp_pgno);

		/* Right page might now have changed parent.
		 * Check if left page also changed parent.
		 */
		if (rdp->h.md_parent != mdp->h.md_parent &&
		    mdp->h.md_pi >= NUMKEYS(mdp->h.md_parent)) {
			mdp->h.md_parent = rdp->h.md_parent;
			mdp->h.md_pi = rdp->h.md_pi - 1;
		}
	} else {
		rc = mdb_add_node(txn, dbi, rdp->h.md_parent, rdp->h.md_pi,
		    &sepkey, NULL, rdp->p.mp_pgno, 0);
	}
	if (rc != MDB_SUCCESS) {
		free(copy);
		return MDB_FAIL;
	}

	for (i = j = 0; i <= NUMKEYS(copy); j++) {
		if (i < split_indx) {
			/* Re-insert in left sibling. */
			pdp = mdp;
		} else {
			/* Insert in right sibling. */
			if (i == split_indx)
				/* Reset insert index for right sibling. */
				j = (i == newindx && ins_new);
			pdp = rdp;
		}

		if (i == newindx && !ins_new) {
			/* Insert the original entry that caused the split. */
			rkey.mv_data = newkey->mv_data;
			rkey.mv_size = newkey->mv_size;
			if (IS_LEAF(&mdp->p)) {
				rdata.mv_data = newdata->mv_data;
				rdata.mv_size = newdata->mv_size;
			} else
				pgno = newpgno;
			flags = 0;

			ins_new = 1;

			/* Update page and index for the new key. */
			*newindxp = j;
			*mpp = &pdp->p;
		} else if (i == NUMKEYS(copy)) {
			break;
		} else {
			node = NODEPTR(copy, i);
			rkey.mv_data = NODEKEY(node);
			rkey.mv_size = node->mn_ksize;
			if (IS_LEAF(&mdp->p)) {
				rdata.mv_data = NODEDATA(node);
				rdata.mv_size = node->mn_dsize;
			} else
				pgno = node->mn_pgno;
			flags = node->mn_flags;

			i++;
		}

		if (!IS_LEAF(&mdp->p) && j == 0) {
			/* First branch index doesn't need key data. */
			rkey.mv_size = 0;
		}

		rc = mdb_add_node(txn, dbi, &pdp->p, j, &rkey, &rdata, pgno,flags);
	}

	free(copy);
	return rc;
}

static int
mdb_put0(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data, unsigned int flags)
{
	int		 rc = MDB_SUCCESS, exact;
	unsigned int	 ki;
	MDB_node	*leaf;
	MDB_pageparent	mpp;
	MDB_val	xdata, *rdata;
	MDB_db dummy;

	DPRINTF("==> put key %.*s, size %zu, data size %zu",
		(int)key->mv_size, (char *)key->mv_data, key->mv_size, data->mv_size);

	mpp.mp_parent = NULL;
	mpp.mp_pi = 0;
	rc = mdb_search_page(txn, dbi, key, NULL, 1, &mpp);
	if (rc == MDB_SUCCESS) {
		leaf = mdb_search_node(txn, dbi, mpp.mp_page, key, &exact, &ki);
		if (leaf && exact) {
			if (F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
				goto put_sub;
			}
			if (flags == MDB_NOOVERWRITE) {
				DPRINTF("duplicate key %.*s",
				    (int)key->mv_size, (char *)key->mv_data);
				return MDB_KEYEXIST;
			}
			/* same size, just replace it */
			if (NODEDSZ(leaf) == data->mv_size) {
				memcpy(NODEDATA(leaf), data->mv_data, data->mv_size);
				goto done;
			}
			mdb_del_node(mpp.mp_page, ki);
		}
		if (leaf == NULL) {		/* append if not found */
			ki = NUMKEYS(mpp.mp_page);
			DPRINTF("appending key at index %i", ki);
		}
	} else if (rc == MDB_NOTFOUND) {
		MDB_dpage *dp;
		/* new file, just write a root leaf page */
		DPRINTF("allocating new root leaf page");
		if ((dp = mdb_new_page(txn, dbi, P_LEAF, 1)) == NULL) {
			return ENOMEM;
		}
		mpp.mp_page = &dp->p;
		txn->mt_dbs[dbi].md_root = mpp.mp_page->mp_pgno;
		txn->mt_dbs[dbi].md_depth++;
		ki = 0;
	}
	else
		goto done;

	assert(IS_LEAF(mpp.mp_page));
	DPRINTF("there are %u keys, should insert new key at index %i",
		NUMKEYS(mpp.mp_page), ki);

	/* For sorted dups, the data item at this level is a DB record
	 * for a child DB; the actual data elements are stored as keys
	 * in the child DB.
	 */
	if (F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
		rdata = &xdata;
		xdata.mv_size = sizeof(MDB_db);
		xdata.mv_data = &dummy;
		memset(&dummy, 0, sizeof(dummy));
		dummy.md_root = P_INVALID;
	} else {
		rdata = data;
	}

	if (SIZELEFT(mpp.mp_page) < mdb_leaf_size(txn->mt_env, key, rdata)) {
		rc = mdb_split(txn, dbi, &mpp.mp_page, &ki, key, rdata, P_INVALID);
	} else {
		/* There is room already in this leaf page. */
		rc = mdb_add_node(txn, dbi, mpp.mp_page, ki, key, rdata, 0, 0);
	}

	if (rc != MDB_SUCCESS)
		txn->mt_flags |= MDB_TXN_ERROR;
	else {
		txn->mt_dbs[dbi].md_entries++;

		/* Remember if we just added a subdatabase */
		if (flags & F_SUBDATA) {
			leaf = NODEPTR(mpp.mp_page, ki);
			leaf->mn_flags |= F_SUBDATA;
		}

		/* Now store the actual data in the child DB. Note that we're
		 * storing the user data in the keys field, so there are strict
		 * size limits on dupdata. The actual data fields of the child
		 * DB are all zero size.
		 */
		if (F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
			MDB_xcursor mx;

			leaf = NODEPTR(mpp.mp_page, ki);
put_sub:
			mdb_xcursor_init0(txn, dbi, &mx);
			mdb_xcursor_init1(txn, dbi, &mx, leaf);
			xdata.mv_size = 0;
			xdata.mv_data = "";
			if (flags == MDB_NODUPDATA)
				flags = MDB_NOOVERWRITE;
			rc = mdb_put0(&mx.mx_txn, mx.mx_cursor.mc_dbi, data, &xdata, flags);
			mdb_xcursor_fini(txn, dbi, &mx);
			memcpy(NODEDATA(leaf), &mx.mx_txn.mt_dbs[mx.mx_cursor.mc_dbi],
				sizeof(MDB_db));
		}
	}

done:
	return rc;
}

int
mdb_put(MDB_txn *txn, MDB_dbi dbi,
    MDB_val *key, MDB_val *data, unsigned int flags)
{
	assert(key != NULL);
	assert(data != NULL);

	if (txn == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		return EINVAL;
	}

	if (key->mv_size == 0 || key->mv_size > MAXKEYSIZE) {
		return EINVAL;
	}

	if ((flags & (MDB_NOOVERWRITE|MDB_NODUPDATA)) != flags)
		return EINVAL;

	return mdb_put0(txn, dbi, key, data, flags);
}

int
mdbenv_get_flags(MDB_env *env, unsigned int *arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_flags;
	return MDB_SUCCESS;
}

int
mdbenv_get_path(MDB_env *env, const char **arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_path;
	return MDB_SUCCESS;
}

static int
mdb_stat0(MDB_env *env, MDB_db *db, MDB_stat *arg)
{
	arg->ms_psize = env->me_psize;
	arg->ms_depth = db->md_depth;
	arg->ms_branch_pages = db->md_branch_pages;
	arg->ms_leaf_pages = db->md_leaf_pages;
	arg->ms_overflow_pages = db->md_overflow_pages;
	arg->ms_entries = db->md_entries;

	return MDB_SUCCESS;
}
int
mdbenv_stat(MDB_env *env, MDB_stat *arg)
{
	if (env == NULL || arg == NULL)
		return EINVAL;

	return mdb_stat0(env, &env->me_meta->mm_dbs[MAIN_DBI], arg);
}

int mdb_open(MDB_txn *txn, const char *name, unsigned int flags, MDB_dbi *dbi)
{
	MDB_val key, data;
	MDB_dbi i;
	int rc, dirty = 0;
	size_t len;

	/* main DB? */
	if (!name) {
		*dbi = MAIN_DBI;
		if (flags & (MDB_DUPSORT|MDB_REVERSEKEY|MDB_INTEGERKEY))
			txn->mt_dbs[MAIN_DBI].md_flags |= (flags & (MDB_DUPSORT|MDB_REVERSEKEY|MDB_INTEGERKEY));
		return MDB_SUCCESS;
	}

	/* Is the DB already open? */
	len = strlen(name);
	for (i=2; i<txn->mt_numdbs; i++) {
		if (len == txn->mt_dbxs[i].md_name.mv_size &&
			!strncmp(name, txn->mt_dbxs[i].md_name.mv_data, len)) {
			*dbi = i;
			return MDB_SUCCESS;
		}
	}

	if (txn->mt_numdbs >= txn->mt_env->me_maxdbs - 1)
		return ENFILE;

	/* Find the DB info */
	key.mv_size = len;
	key.mv_data = (void *)name;
	rc = mdb_get(txn, MAIN_DBI, &key, &data);

	/* Create if requested */
	if (rc == MDB_NOTFOUND && (flags & MDB_CREATE)) {
		MDB_db dummy;
		data.mv_size = sizeof(MDB_db);
		data.mv_data = &dummy;
		memset(&dummy, 0, sizeof(dummy));
		dummy.md_root = P_INVALID;
		dummy.md_flags = flags & 0xffff;
		rc = mdb_put0(txn, MAIN_DBI, &key, &data, F_SUBDATA);
		dirty = 1;
	}

	/* OK, got info, add to table */
	if (rc == MDB_SUCCESS) {
		txn->mt_dbxs[txn->mt_numdbs].md_name.mv_data = strdup(name);
		txn->mt_dbxs[txn->mt_numdbs].md_name.mv_size = len;
		txn->mt_dbxs[txn->mt_numdbs].md_cmp = NULL;
		txn->mt_dbxs[txn->mt_numdbs].md_dcmp = NULL;
		txn->mt_dbxs[txn->mt_numdbs].md_rel = NULL;
		txn->mt_dbxs[txn->mt_numdbs].md_parent = MAIN_DBI;
		txn->mt_dbxs[txn->mt_numdbs].md_dirty = dirty;
		memcpy(&txn->mt_dbs[txn->mt_numdbs], data.mv_data, sizeof(MDB_db));
		*dbi = txn->mt_numdbs;
		txn->mt_numdbs++;
	}

	return rc;
}

int mdb_stat(MDB_txn *txn, MDB_dbi dbi, MDB_stat *arg)
{
	if (txn == NULL || arg == NULL || dbi >= txn->mt_numdbs)
		return EINVAL;

	return mdb_stat0(txn->mt_env, &txn->mt_dbs[dbi], arg);
}

void mdb_close(MDB_txn *txn, MDB_dbi dbi)
{
	char *ptr;
	if (dbi <= MAIN_DBI || dbi >= txn->mt_numdbs)
		return;
	ptr = txn->mt_dbxs[dbi].md_name.mv_data;
	txn->mt_dbxs[dbi].md_name.mv_data = NULL;
	txn->mt_dbxs[dbi].md_name.mv_size = 0;
	free(ptr);
}

int mdb_set_compare(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *cmp)
{
	if (txn == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	txn->mt_dbxs[dbi].md_cmp = cmp;
	return MDB_SUCCESS;
}

int mdb_set_dupsort(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *cmp)
{
	if (txn == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	txn->mt_dbxs[dbi].md_dcmp = cmp;
	return MDB_SUCCESS;
}

int mdb_set_relfunc(MDB_txn *txn, MDB_dbi dbi, MDB_rel_func *rel)
{
	if (txn == NULL || !dbi || dbi >= txn->mt_numdbs)
		return EINVAL;

	txn->mt_dbxs[dbi].md_rel = rel;
	return MDB_SUCCESS;
}
