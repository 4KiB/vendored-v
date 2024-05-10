/*
 * Copyright 1988, 1989 Hans-J. Boehm, Alan J. Demers
 * Copyright (c) 1991-1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1998-1999 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1999 by Hewlett-Packard Company. All rights reserved.
 * Copyright (c) 2008-2022 Ivan Maidanski
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

#include "private/gc_priv.h"

#ifdef GC_USE_ENTIRE_HEAP
  int GC_use_entire_heap = TRUE;
#else
  int GC_use_entire_heap = FALSE;
#endif

/*
 * Free heap blocks are kept on one of several free lists,
 * depending on the size of the block.  Each free list is doubly linked.
 * Adjacent free blocks are coalesced.
 */


# define MAX_BLACK_LIST_ALLOC (2*HBLKSIZE)
                /* largest block we will allocate starting on a black   */
                /* listed block.  Must be >= HBLKSIZE.                  */


# define UNIQUE_THRESHOLD 32
        /* Sizes up to this many HBLKs each have their own free list    */
# define HUGE_THRESHOLD 256
        /* Sizes of at least this many heap blocks are mapped to a      */
        /* single free list.                                            */
# define FL_COMPRESSION 8
        /* In between sizes map this many distinct sizes to a single    */
        /* bin.                                                         */

# define N_HBLK_FLS ((HUGE_THRESHOLD - UNIQUE_THRESHOLD) / FL_COMPRESSION \
                     + UNIQUE_THRESHOLD)

#ifndef GC_GCJ_SUPPORT
  STATIC
#endif
  struct hblk * GC_hblkfreelist[N_HBLK_FLS+1] = { 0 };
                                /* List of completely empty heap blocks */
                                /* Linked through hb_next field of      */
                                /* header structure associated with     */
                                /* block.  Remains externally visible   */
                                /* as used by GNU GCJ currently.        */

GC_API void GC_CALL GC_iterate_free_hblks(GC_walk_free_blk_fn fn,
                                          GC_word client_data)
{
  int i;

  for (i = 0; i <= N_HBLK_FLS; ++i) {
    struct hblk *h;

    for (h = GC_hblkfreelist[i]; h != NULL; h = HDR(h) -> hb_next) {
      (*fn)(h, i, client_data);
    }
  }
}

#ifndef GC_GCJ_SUPPORT
  STATIC
#endif
  word GC_free_bytes[N_HBLK_FLS+1] = { 0 };
        /* Number of free bytes on each list.  Remains visible to GCJ.  */

/* Return the largest n such that the number of free bytes on lists     */
/* n .. N_HBLK_FLS is greater or equal to GC_max_large_allocd_bytes     */
/* minus GC_large_allocd_bytes.  If there is no such n, return 0.       */
GC_INLINE int GC_enough_large_bytes_left(void)
{
    int n;
    word bytes = GC_large_allocd_bytes;

    GC_ASSERT(GC_max_large_allocd_bytes <= GC_heapsize);
    for (n = N_HBLK_FLS; n >= 0; --n) {
        bytes += GC_free_bytes[n];
        if (bytes >= GC_max_large_allocd_bytes) return n;
    }
    return 0;
}

/* Map a number of blocks to the appropriate large block free list index. */
STATIC int GC_hblk_fl_from_blocks(size_t blocks_needed)
{
    if (blocks_needed <= UNIQUE_THRESHOLD) return (int)blocks_needed;
    if (blocks_needed >= HUGE_THRESHOLD) return N_HBLK_FLS;
    return (int)(blocks_needed - UNIQUE_THRESHOLD)/FL_COMPRESSION
                                        + UNIQUE_THRESHOLD;
}

# define PHDR(hhdr) HDR((hhdr) -> hb_prev)
# define NHDR(hhdr) HDR((hhdr) -> hb_next)

# ifdef USE_MUNMAP
#   define IS_MAPPED(hhdr) (((hhdr) -> hb_flags & WAS_UNMAPPED) == 0)
# else
#   define IS_MAPPED(hhdr) TRUE
# endif /* !USE_MUNMAP */

#if !defined(NO_DEBUGGING) || defined(GC_ASSERTIONS)
  static void GC_CALLBACK add_hb_sz(struct hblk *h, int i, GC_word client_data)
  {
      UNUSED_ARG(i);
      *(word *)client_data += HDR(h) -> hb_sz;
  }

  /* Should return the same value as GC_large_free_bytes.       */
  GC_INNER word GC_compute_large_free_bytes(void)
  {
      word total_free = 0;

      GC_iterate_free_hblks(add_hb_sz, (word)(&total_free));
      return total_free;
  }
#endif /* !NO_DEBUGGING || GC_ASSERTIONS */

# if !defined(NO_DEBUGGING)
  static void GC_CALLBACK print_hblkfreelist_item(struct hblk *h, int i,
                                                  GC_word prev_index_ptr)
  {
    hdr *hhdr = HDR(h);

    if (i != *(int *)prev_index_ptr) {
      GC_printf("Free list %d (total size %lu):\n",
                i, (unsigned long)GC_free_bytes[i]);
      *(int *)prev_index_ptr = i;
    }

    GC_printf("\t%p size %lu %s black listed\n",
              (void *)h, (unsigned long)(hhdr -> hb_sz),
              GC_is_black_listed(h, HBLKSIZE) != NULL ? "start"
                : GC_is_black_listed(h, hhdr -> hb_sz) != NULL ? "partially"
                : "not");
  }

  void GC_print_hblkfreelist(void)
  {
    word total;
    int prev_index = -1;

    GC_iterate_free_hblks(print_hblkfreelist_item, (word)(&prev_index));
    GC_printf("GC_large_free_bytes: %lu\n",
              (unsigned long)GC_large_free_bytes);
    total = GC_compute_large_free_bytes();
    if (total != GC_large_free_bytes)
      GC_err_printf("GC_large_free_bytes INCONSISTENT!! Should be: %lu\n",
                    (unsigned long)total);
  }

/* Return the free list index on which the block described by the header */
/* appears, or -1 if it appears nowhere.                                 */
static int free_list_index_of(const hdr *wanted)
{
    int i;

    for (i = 0; i <= N_HBLK_FLS; ++i) {
      struct hblk * h;
      hdr * hhdr;

      for (h = GC_hblkfreelist[i]; h != 0; h = hhdr -> hb_next) {
        hhdr = HDR(h);
        if (hhdr == wanted) return i;
      }
    }
    return -1;
}

GC_API void GC_CALL GC_dump_regions(void)
{
    unsigned i;

    for (i = 0; i < GC_n_heap_sects; ++i) {
        ptr_t start = GC_heap_sects[i].hs_start;
        size_t bytes = GC_heap_sects[i].hs_bytes;
        ptr_t end = start + bytes;
        ptr_t p;

        /* Merge in contiguous sections.        */
          while (i+1 < GC_n_heap_sects && GC_heap_sects[i+1].hs_start == end) {
            ++i;
            end = GC_heap_sects[i].hs_start + GC_heap_sects[i].hs_bytes;
          }
        GC_printf("***Section from %p to %p\n", (void *)start, (void *)end);
        for (p = start; ADDR_LT(p, end); ) {
            hdr *hhdr = HDR(p);

            if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
                GC_printf("\t%p Missing header!!(%p)\n",
                          (void *)p, (void *)hhdr);
                p += HBLKSIZE;
                continue;
            }
            if (HBLK_IS_FREE(hhdr)) {
                int correct_index = GC_hblk_fl_from_blocks(
                                        (size_t)divHBLKSZ(hhdr -> hb_sz));
                int actual_index;

                GC_printf("\t%p\tfree block of size 0x%lx bytes%s\n",
                          (void *)p, (unsigned long)(hhdr -> hb_sz),
                          IS_MAPPED(hhdr) ? "" : " (unmapped)");
                actual_index = free_list_index_of(hhdr);
                if (-1 == actual_index) {
                    GC_printf("\t\tBlock not on free list %d!!\n",
                              correct_index);
                } else if (correct_index != actual_index) {
                    GC_printf("\t\tBlock on list %d, should be on %d!!\n",
                              actual_index, correct_index);
                }
                p += hhdr -> hb_sz;
            } else {
                GC_printf("\t%p\tused for blocks of size 0x%lx bytes\n",
                          (void *)p, (unsigned long)(hhdr -> hb_sz));
                p += HBLKSIZE * OBJ_SZ_TO_BLOCKS(hhdr -> hb_sz);
            }
        }
    }
}

# endif /* NO_DEBUGGING */

/* Initialize hdr for a block containing the indicated size and         */
/* kind of objects.  Return FALSE on failure.                           */
static GC_bool setup_header(hdr *hhdr, struct hblk *block, size_t lb_adjusted,
                            int k, unsigned flags)
{
    const struct obj_kind *ok;
    word descr;

    GC_ASSERT(I_HOLD_LOCK());
    GC_ASSERT(lb_adjusted >= ALIGNMENT);
#   ifndef MARK_BIT_PER_OBJ
      if (lb_adjusted > MAXOBJBYTES)
        flags |= LARGE_BLOCK;
#   endif
    ok = &GC_obj_kinds[k];
#   ifdef ENABLE_DISCLAIM
      if (ok -> ok_disclaim_proc)
        flags |= HAS_DISCLAIM;
      if (ok -> ok_mark_unconditionally)
        flags |= MARK_UNCONDITIONALLY;
#   endif

    /* Set size, kind and mark proc fields.     */
    hhdr -> hb_sz = lb_adjusted;
    hhdr -> hb_obj_kind = (unsigned char)k;
    hhdr -> hb_flags = (unsigned char)flags;
    hhdr -> hb_block = block;
    descr = ok -> ok_descriptor;
#   if ALIGNMENT > GC_DS_TAGS
      /* An extra byte is not added in case of ignore-off-page  */
      /* allocated objects not smaller than HBLKSIZE.           */
      if (EXTRA_BYTES != 0 && (flags & IGNORE_OFF_PAGE) != 0
          && k == NORMAL && lb_adjusted >= HBLKSIZE)
        descr += ALIGNMENT; /* or set to 0 */
#   endif
    if (ok -> ok_relocate_descr) descr += lb_adjusted;
    hhdr -> hb_descr = descr;

#   ifdef MARK_BIT_PER_OBJ
      /* Set hb_inv_sz as portably as possible.  We set it to the       */
      /* smallest value such that lb_adjusted * inv_sz >= 2**32.        */
      /* This may be more precision than necessary.                     */
      if (lb_adjusted > MAXOBJBYTES) {
        hhdr -> hb_inv_sz = LARGE_INV_SZ;
      } else {
        unsigned32 inv_sz;

        GC_ASSERT(lb_adjusted > 1);
#       if CPP_WORDSZ > 32
          inv_sz = (unsigned32)(((word)1 << 32) / lb_adjusted);
          if (((inv_sz * (word)lb_adjusted) >> 32) == 0) ++inv_sz;
#       else
          inv_sz = (((unsigned32)1 << 31) / lb_adjusted) << 1;
          while ((inv_sz * lb_adjusted) > lb_adjusted)
            inv_sz++;
#       endif
#       if (CPP_WORDSZ == 32) && defined(__GNUC__)
          GC_ASSERT(((1ULL << 32) + lb_adjusted - 1) / lb_adjusted == inv_sz);
#       endif
        hhdr -> hb_inv_sz = inv_sz;
      }
#   else
      {
        size_t lg = BYTES_TO_GRANULES(lb_adjusted);

        if (EXPECT(!GC_add_map_entry(lg), FALSE)) {
          /* Make it look like a valid block.   */
          hhdr -> hb_sz = HBLKSIZE;
          hhdr -> hb_descr = 0;
          hhdr -> hb_flags |= LARGE_BLOCK;
          hhdr -> hb_map = 0;
          return FALSE;
        }
        hhdr -> hb_map = GC_obj_map[(hhdr -> hb_flags & LARGE_BLOCK) != 0 ?
                                    0 : lg];
      }
#   endif

    /* Clear mark bits */
    GC_clear_hdr_marks(hhdr);

    hhdr -> hb_last_reclaimed = (unsigned short)GC_gc_no;
    return TRUE;
}

/* Remove hhdr from the free list (it is assumed to specified by index). */
STATIC void GC_remove_from_fl_at(hdr *hhdr, int index)
{
    GC_ASSERT(modHBLKSZ(hhdr -> hb_sz) == 0);
    if (hhdr -> hb_prev == 0) {
        GC_ASSERT(HDR(GC_hblkfreelist[index]) == hhdr);
        GC_hblkfreelist[index] = hhdr -> hb_next;
    } else {
        hdr *phdr;
        GET_HDR(hhdr -> hb_prev, phdr);
        phdr -> hb_next = hhdr -> hb_next;
    }
    /* We always need index to maintain free counts.    */
    GC_ASSERT(GC_free_bytes[index] >= hhdr -> hb_sz);
    GC_free_bytes[index] -= hhdr -> hb_sz;
    if (0 != hhdr -> hb_next) {
        hdr *nhdr;

        GC_ASSERT(!IS_FORWARDING_ADDR_OR_NIL(NHDR(hhdr)));
        GET_HDR(hhdr -> hb_next, nhdr);
        nhdr -> hb_prev = hhdr -> hb_prev;
    }
}

/* Remove hhdr from the appropriate free list (we assume it is on the   */
/* size-appropriate free list).                                         */
GC_INLINE void GC_remove_from_fl(hdr *hhdr)
{
  GC_remove_from_fl_at(hhdr, GC_hblk_fl_from_blocks(
                                        (size_t)divHBLKSZ(hhdr -> hb_sz)));
}

/* Return a pointer to the block ending just before h, if any.  */
static struct hblk * get_block_ending_at(struct hblk *h)
{
    struct hblk *p = h - 1;
    hdr *hhdr;

    GET_HDR(p, hhdr);
    if (hhdr != NULL) {
        return GC_find_starting_hblk(p, &hhdr);
    }
    p = GC_prev_block(p);
    if (p != NULL) {
        hhdr = HDR(p);
        if ((ptr_t)p + hhdr -> hb_sz == (ptr_t)h) {
            return p;
        }
    }
    return NULL;
}

/* Return a pointer to the free block ending just before h, if any.     */
STATIC struct hblk * GC_free_block_ending_at(struct hblk *h)
{
    struct hblk * p = get_block_ending_at(h);

    if (p /* != NULL */) { /* CPPCHECK */
      const hdr *hhdr = HDR(p);

      if (HBLK_IS_FREE(hhdr)) {
        return p;
      }
    }
    return 0;
}

/* Add hhdr to the appropriate free list.               */
/* We maintain individual free lists sorted by address. */
STATIC void GC_add_to_fl(struct hblk *h, hdr *hhdr)
{
    int index = GC_hblk_fl_from_blocks((size_t)divHBLKSZ(hhdr -> hb_sz));
    struct hblk *second = GC_hblkfreelist[index];

#   if defined(GC_ASSERTIONS) && !defined(USE_MUNMAP)
    {
      struct hblk *next = (struct hblk *)((word)h + hhdr -> hb_sz);
      const hdr *nexthdr = HDR(next);
      struct hblk *prev = GC_free_block_ending_at(h);
      const hdr *prevhdr = HDR(prev);

      GC_ASSERT(NULL == nexthdr || !HBLK_IS_FREE(nexthdr)
                || (GC_heapsize & SIGNB) != 0);
                /* In the last case, blocks may be too large to merge. */
      GC_ASSERT(NULL == prev || !HBLK_IS_FREE(prevhdr)
                || (GC_heapsize & SIGNB) != 0);
    }
#   endif
    GC_ASSERT(modHBLKSZ(hhdr -> hb_sz) == 0);
    GC_hblkfreelist[index] = h;
    GC_free_bytes[index] += hhdr -> hb_sz;
    GC_ASSERT(GC_free_bytes[index] <= GC_large_free_bytes);
    hhdr -> hb_next = second;
    hhdr -> hb_prev = 0;
    if (second /* != NULL */) { /* CPPCHECK */
      hdr * second_hdr;

      GET_HDR(second, second_hdr);
      second_hdr -> hb_prev = h;
    }
    hhdr -> hb_flags |= FREE_BLK;
}

#ifdef USE_MUNMAP

#ifdef COUNT_UNMAPPED_REGIONS
  /* GC_unmap_old will avoid creating more than this many unmapped regions, */
  /* but an unmapped region may be split again so exceeding the limit.      */

  /* Return the change in number of unmapped regions if the block h swaps   */
  /* from its current state of mapped/unmapped to the opposite state.       */
  static int calc_num_unmapped_regions_delta(struct hblk *h, hdr *hhdr)
  {
    struct hblk * prev = get_block_ending_at(h);
    struct hblk * next;
    GC_bool prev_unmapped = FALSE;
    GC_bool next_unmapped = FALSE;

    next = GC_next_block((struct hblk *)((ptr_t)h + hhdr -> hb_sz), TRUE);
    /* Ensure next is contiguous with h.        */
    if (next != HBLK_PAGE_ALIGNED((ptr_t)h + hhdr -> hb_sz)) {
      next = NULL;
    }
    if (prev != NULL) {
      const hdr *prevhdr = HDR(prev);
      prev_unmapped = !IS_MAPPED(prevhdr);
    }
    if (next != NULL) {
      const hdr *nexthdr = HDR(next);
      next_unmapped = !IS_MAPPED(nexthdr);
    }

    if (prev_unmapped && next_unmapped) {
      /* If h unmapped, merge two unmapped regions into one.    */
      /* If h remapped, split one unmapped region into two.     */
      return IS_MAPPED(hhdr) ? -1 : 1;
    }
    if (!prev_unmapped && !next_unmapped) {
      /* If h unmapped, create an isolated unmapped region.     */
      /* If h remapped, remove it.                              */
      return IS_MAPPED(hhdr) ? 1 : -1;
    }
    /* If h unmapped, merge it with previous or next unmapped region.   */
    /* If h remapped, reduce either previous or next unmapped region.   */
    /* In either way, no change to the number of unmapped regions.      */
    return 0;
  }
#endif /* COUNT_UNMAPPED_REGIONS */

/* Update GC_num_unmapped_regions assuming the block h changes      */
/* from its current state of mapped/unmapped to the opposite state. */
GC_INLINE void GC_adjust_num_unmapped(struct hblk *h, hdr *hhdr)
{
# ifdef COUNT_UNMAPPED_REGIONS
    GC_num_unmapped_regions += calc_num_unmapped_regions_delta(h, hhdr);
# else
    UNUSED_ARG(h);
    UNUSED_ARG(hhdr);
# endif
}

/* Unmap blocks that haven't been recently touched.  This is the only   */
/* way blocks are ever unmapped.                                        */
GC_INNER void GC_unmap_old(unsigned threshold)
{
    int i;

# ifdef COUNT_UNMAPPED_REGIONS
    /* Skip unmapping if we have already exceeded the soft limit.       */
    /* This forgoes any opportunities to merge unmapped regions though. */
    if (GC_num_unmapped_regions >= GC_UNMAPPED_REGIONS_SOFT_LIMIT)
      return;
# endif

    for (i = 0; i <= N_HBLK_FLS; ++i) {
      struct hblk * h;
      hdr * hhdr;

      for (h = GC_hblkfreelist[i]; 0 != h; h = hhdr -> hb_next) {
        hhdr = HDR(h);
        if (!IS_MAPPED(hhdr)) continue;

        /* Check that the interval is not smaller than the threshold.   */
        /* The truncated counter value wrapping is handled correctly.   */
        if ((unsigned short)(GC_gc_no - hhdr->hb_last_reclaimed)
            >= (unsigned short)threshold) {
#         ifdef COUNT_UNMAPPED_REGIONS
            /* Continue with unmapping the block only if it will not    */
            /* create too many unmapped regions, or if unmapping        */
            /* reduces the number of regions.                           */
            int delta = calc_num_unmapped_regions_delta(h, hhdr);
            signed_word regions = GC_num_unmapped_regions + delta;

            if (delta >= 0 && regions >= GC_UNMAPPED_REGIONS_SOFT_LIMIT) {
              GC_COND_LOG_PRINTF("Unmapped regions limit reached!\n");
              return;
            }
            GC_num_unmapped_regions = regions;
#         endif
          GC_unmap((ptr_t)h, (size_t)(hhdr -> hb_sz));
          hhdr -> hb_flags |= WAS_UNMAPPED;
        }
      }
    }
}

/* Merge all unmapped blocks that are adjacent to other free            */
/* blocks.  This may involve remapping, since all blocks are either     */
/* fully mapped or fully unmapped.                                      */
GC_INNER void GC_merge_unmapped(void)
{
    int i;

    for (i = 0; i <= N_HBLK_FLS; ++i) {
      struct hblk *h = GC_hblkfreelist[i];

      while (h != 0) {
        struct hblk *next;
        hdr *hhdr, *nexthdr;
        word size, nextsize;

        GET_HDR(h, hhdr);
        size = hhdr -> hb_sz;
        next = (struct hblk *)((word)h + size);
        GET_HDR(next, nexthdr);
        /* Coalesce with successor, if possible. */
        if (nexthdr != NULL && HBLK_IS_FREE(nexthdr)
              && !((size + (nextsize = nexthdr -> hb_sz)) & SIGNB)
                 /* no overflow */) {
            /* Note that we usually try to avoid adjacent free blocks   */
            /* that are either both mapped or both unmapped.  But that  */
            /* isn't guaranteed to hold since we remap blocks when we   */
            /* split them, and don't merge at that point.  It may also  */
            /* not hold if the merged block would be too big.           */
            if (IS_MAPPED(hhdr) && !IS_MAPPED(nexthdr)) {
              /* Make both consistent, so that we can merge.    */
                if (size > nextsize) {
                  GC_adjust_num_unmapped(next, nexthdr);
                  GC_remap((ptr_t)next, nextsize);
                } else {
                  GC_adjust_num_unmapped(h, hhdr);
                  GC_unmap((ptr_t)h, size);
                  GC_unmap_gap((ptr_t)h, size, (ptr_t)next, nextsize);
                  hhdr -> hb_flags |= WAS_UNMAPPED;
                }
            } else if (IS_MAPPED(nexthdr) && !IS_MAPPED(hhdr)) {
              if (size > nextsize) {
                GC_adjust_num_unmapped(next, nexthdr);
                GC_unmap((ptr_t)next, nextsize);
                GC_unmap_gap((ptr_t)h, size, (ptr_t)next, nextsize);
              } else {
                GC_adjust_num_unmapped(h, hhdr);
                GC_remap((ptr_t)h, size);
                hhdr -> hb_flags &= (unsigned char)~WAS_UNMAPPED;
                hhdr -> hb_last_reclaimed = nexthdr -> hb_last_reclaimed;
              }
            } else if (!IS_MAPPED(hhdr) && !IS_MAPPED(nexthdr)) {
              /* Unmap any gap in the middle */
                GC_unmap_gap((ptr_t)h, size, (ptr_t)next, nextsize);
            }
            /* If they are both unmapped, we merge, but leave unmapped. */
            GC_remove_from_fl_at(hhdr, i);
            GC_remove_from_fl(nexthdr);
            hhdr -> hb_sz += nexthdr -> hb_sz;
            GC_remove_header(next);
            GC_add_to_fl(h, hhdr);
            /* Start over at beginning of list. */
            h = GC_hblkfreelist[i];
        } else /* not mergeable with successor */ {
            h = hhdr -> hb_next;
        }
      } /* while (h != 0) ... */
    } /* for ... */
}

#endif /* USE_MUNMAP */

/*
 * Return a pointer to a block starting at h of length bytes.
 * Memory for the block is mapped.
 * Remove the block from its free list, and return the remainder (if any)
 * to its appropriate free list.
 * May fail by returning 0.
 * The header for the returned block must be set up by the caller.
 * If the return value is not 0, then hhdr is the header for it.
 */
STATIC struct hblk * GC_get_first_part(struct hblk *h, hdr *hhdr,
                                       size_t bytes, int index)
{
    size_t total_size;
    struct hblk * rest;
    hdr * rest_hdr;

    GC_ASSERT(I_HOLD_LOCK());
    GC_ASSERT(modHBLKSZ(bytes) == 0);
    total_size = (size_t)(hhdr -> hb_sz);
    GC_ASSERT(modHBLKSZ(total_size) == 0);
    GC_remove_from_fl_at(hhdr, index);
    if (total_size == bytes) return h;

    rest = (struct hblk *)((word)h + bytes);
    rest_hdr = GC_install_header(rest);
    if (EXPECT(NULL == rest_hdr, FALSE)) {
        /* FIXME: This is likely to be very bad news ... */
        WARN("Header allocation failed: dropping block\n", 0);
        return NULL;
    }
    rest_hdr -> hb_sz = total_size - bytes;
    rest_hdr -> hb_flags = 0;
#   ifdef GC_ASSERTIONS
      /* Mark h not free, to avoid assertion about adjacent free blocks. */
        hhdr -> hb_flags &= (unsigned char)~FREE_BLK;
#   endif
    GC_add_to_fl(rest, rest_hdr);
    return h;
}

/* Split the block.  hbp is a free block; last_hbp points at an address */
/* inside it; a new header for last_hbp is assumed to be already set    */
/* up.  Fix up the header of hbp to reflect the fact that it is being   */
/* split, move it to the appropriate free list.  last_hbp replaces hbp  */
/* in the original free list.  last_hdr is not completely filled in,    */
/* since it is about to be allocated.  It may in fact end up on the     */
/* wrong free list for its size.  That is not a disaster, since         */
/* last_hbp is to be allocated by our caller.  (Hence adding it to      */
/* a free list is silly.  But this path is hopefully rare enough that   */
/* it does not matter.  The code is cleaner this way.)                  */
STATIC void GC_split_block(struct hblk *hbp, hdr *hhdr, struct hblk *last_hbp,
                           hdr *last_hdr, int index /* of free list */)
{
    word total_size = hhdr -> hb_sz;
    word h_size = (word)((ptr_t)last_hbp - (ptr_t)hbp);
    struct hblk *prev = hhdr -> hb_prev;
    struct hblk *next = hhdr -> hb_next;

    /* Replace hbp with last_hbp on its freelist.      */
    last_hdr -> hb_prev = prev;
    last_hdr -> hb_next = next;
    last_hdr -> hb_sz = total_size - h_size;
    last_hdr -> hb_flags = 0;
    if (prev /* != NULL */) { /* CPPCHECK */
      HDR(prev) -> hb_next = last_hbp;
    } else {
      GC_hblkfreelist[index] = last_hbp;
    }
    if (next /* != NULL */) {
      HDR(next) -> hb_prev = last_hbp;
    }
    GC_ASSERT(GC_free_bytes[index] > h_size);
    GC_free_bytes[index] -= h_size;
#   ifdef USE_MUNMAP
      hhdr -> hb_last_reclaimed = (unsigned short)GC_gc_no;
#   endif
    hhdr -> hb_sz = h_size;
    GC_add_to_fl(hbp, hhdr);
    last_hdr -> hb_flags |= FREE_BLK;
}

STATIC struct hblk *GC_allochblk_nth(size_t lb_adjusted, int k,
                                     unsigned flags, int index, int may_split,
                                     size_t align_m1);

#ifdef USE_MUNMAP
# define AVOID_SPLIT_REMAPPED 2
#endif

GC_INNER struct hblk *GC_allochblk(size_t lb_adjusted, int k,
                                   unsigned flags /* IGNORE_OFF_PAGE or 0 */,
                                   size_t align_m1)
{
    size_t blocks;
    int start_list;
    struct hblk *result;
    int may_split;
    int split_limit; /* highest index of free list whose blocks we split */

    GC_ASSERT(I_HOLD_LOCK());
    GC_ASSERT((lb_adjusted & (GC_GRANULE_BYTES-1)) == 0);
    blocks = OBJ_SZ_TO_BLOCKS_CHECKED(lb_adjusted);
    if (EXPECT(SIZET_SAT_ADD(blocks * HBLKSIZE, align_m1)
                >= (GC_SIZE_MAX >> 1), FALSE))
      return NULL; /* overflow */

    start_list = GC_hblk_fl_from_blocks(blocks);
    /* Try for an exact match first.    */
    result = GC_allochblk_nth(lb_adjusted, k, flags, start_list, FALSE,
                              align_m1);
    if (result != NULL) return result;

    may_split = TRUE;
    if (GC_use_entire_heap || GC_dont_gc
        || GC_heapsize - GC_large_free_bytes < GC_requested_heapsize
        || GC_incremental || !GC_should_collect()) {
        /* Should use more of the heap, even if it requires splitting. */
        split_limit = N_HBLK_FLS;
    } else if (GC_finalizer_bytes_freed > (GC_heapsize >> 4)) {
          /* If we are deallocating lots of memory from         */
          /* finalizers, fail and collect sooner rather         */
          /* than later.                                        */
          split_limit = 0;
    } else {
          /* If we have enough large blocks left to cover any   */
          /* previous request for large blocks, we go ahead     */
          /* and split.  Assuming a steady state, that should   */
          /* be safe.  It means that we can use the full        */
          /* heap if we allocate only small objects.            */
          split_limit = GC_enough_large_bytes_left();
#         ifdef USE_MUNMAP
            if (split_limit > 0)
              may_split = AVOID_SPLIT_REMAPPED;
#         endif
    }
    if (start_list < UNIQUE_THRESHOLD && 0 == align_m1) {
      /* No reason to try start_list again, since all blocks are exact  */
      /* matches.                                                       */
      ++start_list;
    }
    for (; start_list <= split_limit; ++start_list) {
      result = GC_allochblk_nth(lb_adjusted, k, flags, start_list, may_split,
                                align_m1);
      if (result != NULL) break;
    }
    return result;
}

STATIC long GC_large_alloc_warn_suppressed = 0;
                        /* Number of warnings suppressed so far.        */

STATIC unsigned GC_drop_blacklisted_count = 0;
                        /* Counter of the cases when found block by     */
                        /* GC_allochblk_nth is blacklisted completely.  */

#define ALIGN_PAD_SZ(p, align_m1) \
               (((align_m1) + 1 - (size_t)ADDR(p)) & (align_m1))

static GC_bool next_hblk_fits_better(const hdr *hhdr, word size_avail,
                                     word size_needed, size_t align_m1)
{
  const hdr *nexthdr;
  word next_size;
  size_t next_ofs;
  struct hblk *next_hbp = hhdr -> hb_next;

  if (NULL == next_hbp) return FALSE; /* no next block */
  GET_HDR(next_hbp, nexthdr);
  next_size = nexthdr -> hb_sz;
  if (size_avail <= next_size) return FALSE; /* not enough size */

  next_ofs = ALIGN_PAD_SZ(next_hbp, align_m1);
  return next_size >= size_needed + next_ofs
         && !GC_is_black_listed(next_hbp + divHBLKSZ(next_ofs), size_needed);
}

static struct hblk *find_nonbl_hblk(struct hblk *last_hbp, word size_remain,
                                    word eff_size_needed, size_t align_m1)
{
  ptr_t search_end = PTR_ALIGN_DOWN((ptr_t)last_hbp + size_remain,
                                    align_m1 + 1);

  do {
    struct hblk *next_hbp;

    last_hbp += divHBLKSZ(ALIGN_PAD_SZ(last_hbp, align_m1));
    next_hbp = GC_is_black_listed(last_hbp, eff_size_needed);
    if (NULL == next_hbp) return last_hbp; /* not black-listed */
    last_hbp = next_hbp;
  } while (ADDR_GE(search_end, (ptr_t)last_hbp));
  return NULL;
}

/* Allocate and drop the block in small chunks, to maximize the chance  */
/* that we will recover some later.  hhdr should correspond to hbp.     */
static void drop_hblk_in_chunks(int n, struct hblk *hbp, hdr *hhdr)
{
  size_t total_size = (size_t)(hhdr -> hb_sz);
  const struct hblk *limit = hbp + divHBLKSZ(total_size);

  GC_ASSERT(HDR(hbp) == hhdr);
  GC_ASSERT(modHBLKSZ(total_size) == 0 && total_size > 0);
  GC_large_free_bytes -= total_size;
  GC_bytes_dropped += total_size;
  GC_remove_from_fl_at(hhdr, n);
  do {
    (void)setup_header(hhdr, hbp, HBLKSIZE, PTRFREE, 0); /* cannot fail */
    if (GC_debugging_started) BZERO(hbp, HBLKSIZE);
    hbp++;
    if (ADDR_GE(hbp, limit)) break;

    hhdr = GC_install_header(hbp);
  } while (EXPECT(hhdr != NULL, TRUE)); /* no header allocation failure? */
}

#if defined(MPROTECT_VDB) && defined(DONT_PROTECT_PTRFREE)
  static GC_bool is_hblks_mix_in_page(struct hblk *hbp, GC_bool is_ptrfree)
  {
    struct hblk *h = HBLK_PAGE_ALIGNED(hbp);
    size_t i, cnt = divHBLKSZ(GC_page_size);

    /* Iterate over blocks in the page to check if all the    */
    /* occupied blocks are pointer-free if we are going to    */
    /* allocate a pointer-free one, and vice versa.           */
    for (i = 0; i < cnt; i++) {
      hdr *hhdr;

      GET_HDR(&h[i], hhdr);
      if (NULL == hhdr) continue;
      (void)GC_find_starting_hblk(&h[i], &hhdr);
      if (!HBLK_IS_FREE(hhdr)) {
        /* It is OK to check only the first found occupied block.   */
        return IS_PTRFREE(hhdr) != is_ptrfree;
      }
    }
    return FALSE; /* all blocks are free */
  }
#endif /* MPROTECT_VDB && DONT_PROTECT_PTRFREE */

/* The same as GC_allochblk, but with search restricted to the index-th */
/* free list.  flags should be IGNORE_OFF_PAGE or zero; may_split       */
/* indicates whether it is OK to split larger blocks; size is in bytes. */
/* If may_split is set to AVOID_SPLIT_REMAPPED, then memory remapping   */
/* followed by splitting should be generally avoided.  Rounded-up       */
/* lb_adjusted plus align_m1 value should be less than GC_SIZE_MAX / 2. */
STATIC struct hblk *GC_allochblk_nth(size_t lb_adjusted, int k,
                                     unsigned flags, int index, int may_split,
                                     size_t align_m1)
{
    struct hblk *hbp, *last_hbp;
    hdr *hhdr; /* header corresponding to hbp */
    word size_needed = HBLKSIZE * OBJ_SZ_TO_BLOCKS_CHECKED(lb_adjusted);
                                /* number of bytes in requested objects */

    GC_ASSERT(I_HOLD_LOCK());
    GC_ASSERT(((align_m1 + 1) & align_m1) == 0 && lb_adjusted > 0);
    GC_ASSERT(0 == align_m1 || modHBLKSZ(align_m1 + 1) == 0);
  retry:
    /* Search for a big enough block in free list.      */
    for (hbp = GC_hblkfreelist[index];; hbp = hhdr -> hb_next) {
      word size_avail; /* bytes available in this block */
      size_t align_ofs;

      if (hbp /* != NULL */) {
        /* CPPCHECK */
      } else {
        return NULL;
      }
      GET_HDR(hbp, hhdr); /* set hhdr value */
      size_avail = hhdr -> hb_sz;
      if (!may_split && size_avail != size_needed) continue;

      align_ofs = ALIGN_PAD_SZ(hbp, align_m1);
      if (size_avail < size_needed + align_ofs)
        continue; /* the block is too small */

      if (size_avail != size_needed) {
        /* If the next heap block is obviously better, go on.   */
        /* This prevents us from disassembling a single large   */
        /* block to get tiny blocks.                            */
        if (next_hblk_fits_better(hhdr, size_avail, size_needed, align_m1))
          continue;
      }

#     if defined(MPROTECT_VDB) && defined(DONT_PROTECT_PTRFREE)
        /* Avoid write-protecting pointer-free blocks (only the */
        /* case if page size is larger than the block size).    */
        GC_ASSERT(GC_page_size != 0);
        if (GC_page_size != HBLKSIZE
            && (!GC_incremental /* not enabled yet */
                || GC_incremental_protection_needs() != GC_PROTECTS_NONE)
            && is_hblks_mix_in_page(hbp, k == PTRFREE))
          continue;
#     endif

      if (IS_UNCOLLECTABLE(k)
          || (k == PTRFREE && size_needed <= MAX_BLACK_LIST_ALLOC)) {
        last_hbp = hbp + divHBLKSZ(align_ofs);
        break;
      }

      last_hbp = find_nonbl_hblk(hbp, size_avail - size_needed,
                    (flags & IGNORE_OFF_PAGE) != 0 ? HBLKSIZE : size_needed,
                    align_m1);
      /* Is non-blacklisted part of enough size?        */
      if (last_hbp != NULL) {
#       ifdef USE_MUNMAP
          /* Avoid remapping followed by splitting.     */
          if (may_split == AVOID_SPLIT_REMAPPED && last_hbp != hbp
              && !IS_MAPPED(hhdr))
            continue;
#       endif
        break;
      }

      /* The block is completely blacklisted.  If so, we need to        */
      /* drop some such blocks, since otherwise we spend all our        */
      /* time traversing them if pointer-free blocks are unpopular.     */
      /* A dropped block will be reconsidered at next GC.               */
      if (size_needed == HBLKSIZE && 0 == align_m1
          && !GC_find_leak && IS_MAPPED(hhdr)
          && (++GC_drop_blacklisted_count & 3) == 0) {
        struct hblk *prev = hhdr -> hb_prev;

        drop_hblk_in_chunks(index, hbp, hhdr);
        if (NULL == prev) goto retry;
        /* Restore hhdr to point at free block. */
        hhdr = HDR(prev);
        continue;
      }

      if (size_needed > BL_LIMIT && size_avail - size_needed > BL_LIMIT) {
        /* Punt, since anything else risks unreasonable heap growth.    */
        if (++GC_large_alloc_warn_suppressed
            >= GC_large_alloc_warn_interval) {
          WARN("Repeated allocation of very large block"
               " (appr. size %" WARN_PRIuPTR " KiB):\n"
               "\tMay lead to memory leak and poor performance\n",
               size_needed >> 10);
          GC_large_alloc_warn_suppressed = 0;
        }
        last_hbp = hbp + divHBLKSZ(align_ofs);
        break;
      }
    }

    GC_ASSERT((ADDR(last_hbp) & align_m1) == 0);
    if (last_hbp != hbp) {
      hdr *last_hdr = GC_install_header(last_hbp);

      if (EXPECT(NULL == last_hdr, FALSE)) return NULL;
      /* Make sure it's mapped before we mangle it.     */
#     ifdef USE_MUNMAP
        if (!IS_MAPPED(hhdr)) {
          GC_adjust_num_unmapped(hbp, hhdr);
          GC_remap((ptr_t)hbp, (size_t)(hhdr -> hb_sz));
          hhdr -> hb_flags &= (unsigned char)~WAS_UNMAPPED;
        }
#     endif
      /* Split the block at last_hbp. */
      GC_split_block(hbp, hhdr, last_hbp, last_hdr, index);
      /* We must now allocate last_hbp, since it may be on the  */
      /* wrong free list.                                       */
      hbp = last_hbp;
      hhdr = last_hdr;
    }
    GC_ASSERT(hhdr -> hb_sz >= size_needed);

#   ifdef USE_MUNMAP
      if (!IS_MAPPED(hhdr)) {
        GC_adjust_num_unmapped(hbp, hhdr);
        GC_remap((ptr_t)hbp, (size_t)(hhdr -> hb_sz));
        hhdr -> hb_flags &= (unsigned char)~WAS_UNMAPPED;
        /* Note: This may leave adjacent, mapped free blocks. */
      }
#   endif
    /* hbp may be on the wrong freelist; the parameter index is important. */
    hbp = GC_get_first_part(hbp, hhdr, (size_t)size_needed, index);
    if (EXPECT(NULL == hbp, FALSE)) return NULL;

    /* Add it to map of valid blocks.   */
    if (EXPECT(!GC_install_counts(hbp, (size_t)size_needed), FALSE))
      return NULL; /* This leaks memory under very rare conditions. */

    /* Set up the header.       */
    GC_ASSERT(HDR(hbp) == hhdr);
#   ifdef MARK_BIT_PER_OBJ
      (void)setup_header(hhdr, hbp, lb_adjusted, k, flags);
      /* Result is always true, not checked to avoid a cppcheck warning. */
#   else
      if (EXPECT(!setup_header(hhdr, hbp, lb_adjusted, k, flags), FALSE)) {
        GC_remove_counts(hbp, (size_t)size_needed);
        return NULL; /* ditto */
      }
#   endif

#   ifndef GC_DISABLE_INCREMENTAL
      /* Notify virtual dirty bit implementation that we are about to   */
      /* write.  Ensure that pointer-free objects are not protected     */
      /* if it is avoidable.  This also ensures that newly allocated    */
      /* blocks are treated as dirty - it is necessary since we do not  */
      /* protect free blocks.                                           */
      GC_ASSERT(modHBLKSZ(size_needed) == 0);
      GC_remove_protection(hbp, divHBLKSZ(size_needed), IS_PTRFREE(hhdr));
#   endif
    /* We just successfully allocated a block.  Restart count of        */
    /* consecutive failures.                                            */
    GC_fail_count = 0;

    GC_large_free_bytes -= size_needed;
    GC_ASSERT(IS_MAPPED(hhdr));
    return hbp;
}

#ifdef VALGRIND_TRACKING
  /* Note: this is intentionally defined in a file other than malloc.c  */
  /* and reclaim.c ones.                                                */
  GC_ATTR_NOINLINE
  GC_API void GC_CALLBACK GC_free_profiler_hook(void *p)
  {
#   ifndef PARALLEL_MARK
      GC_ASSERT(I_HOLD_LOCK());
#   endif
    /* Prevent treating this function by the compiler as a no-op one.   */
    NOOP1_PTR(p);
  }
#endif /* VALGRIND_TRACKING */

/*
 * Free a heap block.
 *
 * Coalesce the block with its neighbors if possible.
 *
 * All mark words are assumed to be cleared.
 */
GC_INNER void GC_freehblk(struct hblk *hbp)
{
    struct hblk *next, *prev;
    hdr *hhdr, *prevhdr, *nexthdr;
    word size;

    GET_HDR(hbp, hhdr);
    size = HBLKSIZE * OBJ_SZ_TO_BLOCKS(hhdr -> hb_sz);
    if ((size & SIGNB) != 0)
      ABORT("Deallocating excessively large block.  Too large an allocation?");
      /* Probably possible if we try to allocate more than half the address */
      /* space at once.  If we don't catch it here, strange things happen   */
      /* later.                                                             */

    GC_remove_counts(hbp, (size_t)size);
    hhdr -> hb_sz = size;
#   ifdef USE_MUNMAP
      hhdr -> hb_last_reclaimed = (unsigned short)GC_gc_no;
#   endif

    /* Check for duplicate deallocation in the easy case. */
    if (HBLK_IS_FREE(hhdr)) {
        ABORT_ARG1("Duplicate large block deallocation",
                   " of %p", (void *)hbp);
    }

    GC_ASSERT(IS_MAPPED(hhdr));
    hhdr -> hb_flags |= FREE_BLK;
    next = (struct hblk *)((ptr_t)hbp + size);
    GET_HDR(next, nexthdr);
    prev = GC_free_block_ending_at(hbp);
    /* Coalesce with successor, if possible.    */
    if (nexthdr != NULL && HBLK_IS_FREE(nexthdr) && IS_MAPPED(nexthdr)
          && !((hhdr -> hb_sz + nexthdr -> hb_sz) & SIGNB) /* no overflow */) {
        GC_remove_from_fl(nexthdr);
        hhdr -> hb_sz += nexthdr -> hb_sz;
        GC_remove_header(next);
    }

    /* Coalesce with predecessor, if possible. */
    if (prev /* != NULL */) { /* CPPCHECK */
        prevhdr = HDR(prev);
        if (IS_MAPPED(prevhdr)
            && !((hhdr -> hb_sz + prevhdr -> hb_sz) & SIGNB)) {
          GC_remove_from_fl(prevhdr);
          prevhdr -> hb_sz += hhdr -> hb_sz;
#         ifdef USE_MUNMAP
            prevhdr -> hb_last_reclaimed = (unsigned short)GC_gc_no;
#         endif
          GC_remove_header(hbp);
          hbp = prev;
          hhdr = prevhdr;
        }
    }
    /* FIXME: It is not clear we really always want to do these merges  */
    /* with USE_MUNMAP, since it updates ages and hence prevents        */
    /* unmapping.                                                       */

    GC_large_free_bytes += size;
    GC_add_to_fl(hbp, hhdr);
}
