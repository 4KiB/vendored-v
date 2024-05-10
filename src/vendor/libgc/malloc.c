/*
 * Copyright 1988, 1989 Hans-J. Boehm, Alan J. Demers
 * Copyright (c) 1991-1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1999-2004 Hewlett-Packard Development Company, L.P.
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

#include <string.h>

/* Allocate reclaim list for the kind.  Returns TRUE on success.        */
STATIC GC_bool GC_alloc_reclaim_list(struct obj_kind *ok)
{
    struct hblk ** result;

    GC_ASSERT(I_HOLD_LOCK());
    result = (struct hblk **)GC_scratch_alloc(
                                (MAXOBJGRANULES+1) * sizeof(struct hblk *));
    if (EXPECT(NULL == result, FALSE)) return FALSE;

    BZERO(result, (MAXOBJGRANULES+1)*sizeof(struct hblk *));
    ok -> ok_reclaim_list = result;
    return TRUE;
}

/* Allocate a large block of size lb_adjusted bytes with the requested  */
/* alignment (align_m1 plus one).  The block is not cleared.  We assume */
/* that the size is non-zero and a multiple of GC_GRANULE_BYTES, and    */
/* that it already includes EXTRA_BYTES value.  The flags argument      */
/* should be IGNORE_OFF_PAGE or 0.  Calls GC_allochblk() to do the      */
/* actual allocation, but also triggers collection and/or heap          */
/* expansion as appropriate.  Updates value of GC_bytes_allocd; does    */
/* also other accounting.                                               */
STATIC ptr_t GC_alloc_large(size_t lb_adjusted, int k, unsigned flags,
                            size_t align_m1)
{
    struct hblk *h;
    size_t n_blocks; /* includes alignment */
    ptr_t result = NULL;
    GC_bool retry = FALSE;

    GC_ASSERT(I_HOLD_LOCK());
    GC_ASSERT(lb_adjusted != 0 && (lb_adjusted & (GC_GRANULE_BYTES-1)) == 0);
    n_blocks = OBJ_SZ_TO_BLOCKS_CHECKED(SIZET_SAT_ADD(lb_adjusted, align_m1));
    if (!EXPECT(GC_is_initialized, TRUE)) {
      UNLOCK(); /* just to unset GC_lock_holder */
      GC_init();
      LOCK();
    }
    /* Do our share of marking work.    */
    if (GC_incremental && !GC_dont_gc) {
            ENTER_GC();
            GC_collect_a_little_inner((int)n_blocks);
            EXIT_GC();
    }

    h = GC_allochblk(lb_adjusted, k, flags, align_m1);
#   ifdef USE_MUNMAP
        if (NULL == h) {
            GC_merge_unmapped();
            h = GC_allochblk(lb_adjusted, k, flags, align_m1);
        }
#   endif
    while (NULL == h && GC_collect_or_expand(n_blocks, flags, retry)) {
        h = GC_allochblk(lb_adjusted, k, flags, align_m1);
        retry = TRUE;
    }
    if (EXPECT(h != NULL, TRUE)) {
        GC_bytes_allocd += lb_adjusted;
        if (lb_adjusted > HBLKSIZE) {
            GC_large_allocd_bytes += HBLKSIZE * OBJ_SZ_TO_BLOCKS(lb_adjusted);
            if (GC_large_allocd_bytes > GC_max_large_allocd_bytes)
                GC_max_large_allocd_bytes = GC_large_allocd_bytes;
        }
        /* FIXME: Do we need some way to reset GC_max_large_allocd_bytes? */
        result = h -> hb_body;
        GC_ASSERT((ADDR(result) & align_m1) == 0);
    }
    return result;
}

/* Allocate a large block of given size in bytes, clear it if   */
/* appropriate.  We assume that the size is non-zero and        */
/* a multiple of GC_GRANULE_BYTES, and that it already includes */
/* EXTRA_BYTES value.  Update value of GC_bytes_allocd.         */
STATIC ptr_t GC_alloc_large_and_clear(size_t lb_adjusted, int k,
                                      unsigned flags)
{
    ptr_t result;

    GC_ASSERT(I_HOLD_LOCK());
    result = GC_alloc_large(lb_adjusted, k, flags, 0 /* align_m1 */);
    if (EXPECT(result != NULL, TRUE)
          && (GC_debugging_started || GC_obj_kinds[k].ok_init)) {
        /* Clear the whole block, in case of GC_realloc call. */
        BZERO(result, HBLKSIZE * OBJ_SZ_TO_BLOCKS(lb_adjusted));
    }
    return result;
}

/* Fill in additional entries in GC_size_map, including the i-th one.   */
/* Note that a filled in section of the array ending at n always        */
/* has the length of at least n/4.                                      */
STATIC void GC_extend_size_map(size_t i)
{
  size_t original_lg = ALLOC_REQUEST_GRANS(i);
  size_t lg;
  size_t byte_sz = GRANULES_TO_BYTES(original_lg);
                        /* The size we try to preserve.         */
                        /* Close to i, unless this would        */
                        /* introduce too many distinct sizes.   */
  size_t smaller_than_i = byte_sz - (byte_sz >> 3);
  size_t low_limit; /* The lowest indexed entry we initialize.  */
  size_t number_of_objs;

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(0 == GC_size_map[i]);
  if (0 == GC_size_map[smaller_than_i]) {
    low_limit = byte_sz - (byte_sz >> 2); /* much smaller than i */
    lg = original_lg;
    while (GC_size_map[low_limit] != 0)
      low_limit++;
  } else {
    low_limit = smaller_than_i + 1;
    while (GC_size_map[low_limit] != 0)
      low_limit++;

    lg = ALLOC_REQUEST_GRANS(low_limit);
    lg += lg >> 3;
    if (lg < original_lg) lg = original_lg;
  }

  /* For these larger sizes, we use an even number of granules.         */
  /* This makes it easier to, e.g., construct a 16-byte-aligned         */
  /* allocator even if GC_GRANULE_BYTES is 8.                           */
  lg = (lg + 1) & ~(size_t)1;
  if (lg > MAXOBJGRANULES) lg = MAXOBJGRANULES;

  /* If we can fit the same number of larger objects in a block, do so. */
  GC_ASSERT(lg != 0);
  number_of_objs = HBLK_GRANULES / lg;
  GC_ASSERT(number_of_objs != 0);
  lg = (HBLK_GRANULES / number_of_objs) & ~(size_t)1;

  byte_sz = GRANULES_TO_BYTES(lg) - EXTRA_BYTES;
                        /* We may need one extra byte; do not always    */
                        /* fill in GC_size_map[byte_sz].                */

  for (; low_limit <= byte_sz; low_limit++)
    GC_size_map[low_limit] = lg;
}

STATIC void * GC_generic_malloc_inner_small(size_t lb, int k)
{
  struct obj_kind *ok = &GC_obj_kinds[k];
  size_t lg = GC_size_map[lb];
  void **opp = &(ok -> ok_freelist[lg]);
  void *op = *opp;

  GC_ASSERT(I_HOLD_LOCK());
  if (EXPECT(NULL == op, FALSE)) {
    if (0 == lg) {
      if (!EXPECT(GC_is_initialized, TRUE)) {
        UNLOCK(); /* just to unset GC_lock_holder */
        GC_init();
        LOCK();
        lg = GC_size_map[lb];
      }
      if (0 == lg) {
        GC_extend_size_map(lb);
        lg = GC_size_map[lb];
        GC_ASSERT(lg != 0);
      }
      /* Retry */
      opp = &(ok -> ok_freelist[lg]);
      op = *opp;
    }
    if (NULL == op) {
      if (NULL == ok -> ok_reclaim_list
          && !GC_alloc_reclaim_list(ok))
        return NULL;
      op = GC_allocobj(lg, k);
      if (NULL == op) return NULL;
    }
  }
  *opp = obj_link(op);
  obj_link(op) = NULL;
  GC_bytes_allocd += GRANULES_TO_BYTES((word)lg);
  return op;
}

GC_INNER void * GC_generic_malloc_inner(size_t lb, int k, unsigned flags)
{
    size_t lb_adjusted;

    GC_ASSERT(I_HOLD_LOCK());
    GC_ASSERT(k < MAXOBJKINDS);
    if (SMALL_OBJ(lb)) {
        return GC_generic_malloc_inner_small(lb, k);
    }

#   if MAX_EXTRA_BYTES > 0
      if ((flags & IGNORE_OFF_PAGE) != 0 && lb >= HBLKSIZE) {
        /* No need to add EXTRA_BYTES.  */
        lb_adjusted = lb;
      } else
#   endif
    /* else */ {
      lb_adjusted = ADD_EXTRA_BYTES(lb);
    }
    return GC_alloc_large_and_clear(ROUNDUP_GRANULE_SIZE(lb_adjusted),
                                    k, flags);
}

#ifdef GC_COLLECT_AT_MALLOC
  /* Parameter to force GC at every malloc of size greater or equal to  */
  /* the given value.  This might be handy during debugging.            */
# if defined(CPPCHECK)
    size_t GC_dbg_collect_at_malloc_min_lb = 16*1024; /* e.g. */
# else
    size_t GC_dbg_collect_at_malloc_min_lb = (GC_COLLECT_AT_MALLOC);
# endif
#endif

GC_INNER void * GC_generic_malloc_aligned(size_t lb, int k, unsigned flags,
                                          size_t align_m1)
{
    void * result;

    GC_ASSERT(k < MAXOBJKINDS);
    if (EXPECT(get_have_errors(), FALSE))
      GC_print_all_errors();
    GC_INVOKE_FINALIZERS();
    GC_DBG_COLLECT_AT_MALLOC(lb);
    if (SMALL_OBJ(lb) && EXPECT(align_m1 < GC_GRANULE_BYTES, TRUE)) {
        LOCK();
        result = GC_generic_malloc_inner_small(lb, k);
        UNLOCK();
    } else {
#       ifdef THREADS
          size_t lg;
#       endif
        size_t lb_adjusted;
        GC_bool init;

#       if MAX_EXTRA_BYTES > 0
          if ((flags & IGNORE_OFF_PAGE) != 0 && lb >= HBLKSIZE) {
            /* No need to add EXTRA_BYTES.      */
            lb_adjusted = ROUNDUP_GRANULE_SIZE(lb);
#           ifdef THREADS
              lg = BYTES_TO_GRANULES(lb_adjusted);
#           endif
          } else
#       endif
        /* else */ {
#         ifndef THREADS
            size_t lg; /* CPPCHECK */
#         endif

          if (EXPECT(0 == lb, FALSE)) lb = 1;
          lg = ALLOC_REQUEST_GRANS(lb);
          lb_adjusted = GRANULES_TO_BYTES(lg);
        }

        init = GC_obj_kinds[k].ok_init;
        if (EXPECT(align_m1 < GC_GRANULE_BYTES, TRUE)) {
          align_m1 = 0;
        } else if (align_m1 < HBLKSIZE) {
          align_m1 = HBLKSIZE - 1;
        }
        LOCK();
        result = GC_alloc_large(lb_adjusted, k, flags, align_m1);
        if (EXPECT(result != NULL, TRUE)) {
          if (GC_debugging_started
#             ifndef THREADS
                || init
#             endif
             ) {
            BZERO(result, HBLKSIZE * OBJ_SZ_TO_BLOCKS(lb_adjusted));
          } else {
#           ifdef THREADS
              GC_ASSERT(GRANULES_TO_WORDS(lg) >= 2);
              /* Clear any memory that might be used for GC descriptors */
              /* before we release the allocator lock.                  */
                ((word *)result)[0] = 0;
                ((word *)result)[1] = 0;
                ((word *)result)[GRANULES_TO_WORDS(lg)-1] = 0;
                ((word *)result)[GRANULES_TO_WORDS(lg)-2] = 0;
#           endif
          }
        }
        UNLOCK();
#       ifdef THREADS
          if (init && !GC_debugging_started && result != NULL) {
            /* Clear the rest (i.e. excluding the initial 2 words). */
            BZERO((word *)result + 2,
                  HBLKSIZE * OBJ_SZ_TO_BLOCKS(lb_adjusted) - 2 * sizeof(word));
          }
#       endif
    }
    if (EXPECT(NULL == result, FALSE))
      result = (*GC_get_oom_fn())(lb); /* might be misaligned */
    return result;
}

GC_API GC_ATTR_MALLOC void * GC_CALL GC_generic_malloc(size_t lb, int k)
{
    return GC_generic_malloc_aligned(lb, k, 0 /* flags */, 0 /* align_m1 */);
}

GC_API GC_ATTR_MALLOC void * GC_CALL GC_malloc_kind_global(size_t lb, int k)
{
    return GC_malloc_kind_aligned_global(lb, k, 0 /* align_m1 */);
}

GC_INNER void * GC_malloc_kind_aligned_global(size_t lb, int k,
                                              size_t align_m1)
{
    GC_ASSERT(k < MAXOBJKINDS);
    if (SMALL_OBJ(lb) && EXPECT(align_m1 < HBLKSIZE / 2, TRUE)) {
        void *op;
        void **opp;
        size_t lg;

        GC_DBG_COLLECT_AT_MALLOC(lb);
        LOCK();
        lg = GC_size_map[lb];
        opp = &GC_obj_kinds[k].ok_freelist[lg];
        op = *opp;
        if (EXPECT(align_m1 >= GC_GRANULE_BYTES, FALSE)) {
            /* TODO: Avoid linear search. */
            for (; (ADDR(op) & align_m1) != 0; op = *opp) {
                opp = &obj_link(op);
            }
        }
        if (EXPECT(op != NULL, TRUE)) {
            GC_ASSERT(PTRFREE == k || NULL == obj_link(op)
                      || (ADDR(obj_link(op)) < GC_greatest_real_heap_addr
                          && GC_least_real_heap_addr < ADDR(obj_link(op))));
            *opp = obj_link(op);
            if (k != PTRFREE)
                obj_link(op) = NULL;
            GC_bytes_allocd += GRANULES_TO_BYTES((word)lg);
            UNLOCK();
            GC_ASSERT((ADDR(op) & align_m1) == 0);
            return op;
        }
        UNLOCK();
    }

    /* We make the GC_clear_stack() call a tail one, hoping to get more */
    /* of the stack.                                                    */
    return GC_clear_stack(GC_generic_malloc_aligned(lb, k, 0 /* flags */,
                                                    align_m1));
}

#if defined(THREADS) && !defined(THREAD_LOCAL_ALLOC)
  GC_API GC_ATTR_MALLOC void * GC_CALL GC_malloc_kind(size_t lb, int k)
  {
    return GC_malloc_kind_global(lb, k);
  }
#endif

/* Allocate lb bytes of atomic (pointer-free) data.     */
GC_API GC_ATTR_MALLOC void * GC_CALL GC_malloc_atomic(size_t lb)
{
    return GC_malloc_kind(lb, PTRFREE);
}

/* Allocate lb bytes of composite (pointerful) data.    */
GC_API GC_ATTR_MALLOC void * GC_CALL GC_malloc(size_t lb)
{
    return GC_malloc_kind(lb, NORMAL);
}

GC_API GC_ATTR_MALLOC void * GC_CALL GC_generic_malloc_uncollectable(
                                                        size_t lb, int k)
{
    void *op;
    size_t lb_orig = lb;

    GC_ASSERT(k < MAXOBJKINDS);
    if (EXTRA_BYTES != 0 && EXPECT(lb != 0, TRUE)) lb--;
                /* We do not need the extra byte, since this will   */
                /* not be collected anyway.                         */

    if (SMALL_OBJ(lb)) {
        void **opp;
        size_t lg;

        if (EXPECT(get_have_errors(), FALSE))
          GC_print_all_errors();
        GC_INVOKE_FINALIZERS();
        GC_DBG_COLLECT_AT_MALLOC(lb_orig);
        LOCK();
        lg = GC_size_map[lb];
        opp = &GC_obj_kinds[k].ok_freelist[lg];
        op = *opp;
        if (EXPECT(op != NULL, TRUE)) {
            *opp = obj_link(op);
            obj_link(op) = 0;
            GC_bytes_allocd += GRANULES_TO_BYTES((word)lg);
            /* Mark bit was already set on free list.  It will be       */
            /* cleared only temporarily during a collection, as a       */
            /* result of the normal free list mark bit clearing.        */
            GC_non_gc_bytes += GRANULES_TO_BYTES((word)lg);
        } else {
            op = GC_generic_malloc_inner_small(lb, k);
            if (NULL == op) {
              GC_oom_func oom_fn = GC_oom_fn;
              UNLOCK();
              return (*oom_fn)(lb_orig);
            }
            /* For small objects, the free lists are completely marked. */
        }
        GC_ASSERT(GC_is_marked(op));
        UNLOCK();
    } else {
      op = GC_generic_malloc_aligned(lb, k, 0 /* flags */, 0 /* align_m1 */);
      if (op /* != NULL */) { /* CPPCHECK */
        hdr * hhdr = HDR(op);

        GC_ASSERT(HBLKDISPL(op) == 0); /* large block */
        /* We do not need to acquire the allocator lock before HDR(op), */
        /* since we have an undisguised pointer, but we need it while   */
        /* we adjust the mark bits.                                     */
        LOCK();
        set_mark_bit_from_hdr(hhdr, 0); /* Only object. */
#       ifndef THREADS
          GC_ASSERT(hhdr -> hb_n_marks == 0);
                /* This is not guaranteed in the multi-threaded case    */
                /* because the counter could be updated before locking. */
#       endif
        hhdr -> hb_n_marks = 1;
        UNLOCK();
      }
    }
    return op;
}

/* Allocate lb bytes of pointerful, traced, but not collectible data.   */
GC_API GC_ATTR_MALLOC void * GC_CALL GC_malloc_uncollectable(size_t lb)
{
  return GC_generic_malloc_uncollectable(lb, UNCOLLECTABLE);
}

#ifdef GC_ATOMIC_UNCOLLECTABLE
  /* Allocate lb bytes of pointer-free, untraced, uncollectible data    */
  /* This is normally roughly equivalent to the system malloc.          */
  /* But it may be useful if malloc is redefined.                       */
  GC_API GC_ATTR_MALLOC void * GC_CALL
        GC_malloc_atomic_uncollectable(size_t lb)
  {
    return GC_generic_malloc_uncollectable(lb, AUNCOLLECTABLE);
  }
#endif /* GC_ATOMIC_UNCOLLECTABLE */

#if defined(REDIRECT_MALLOC) && !defined(REDIRECT_MALLOC_IN_HEADER)

# ifndef MSWINCE
#   include <errno.h>
# endif

  /* Avoid unnecessary nested procedure calls here, by #defining some   */
  /* malloc replacements.  Otherwise we end up saving a meaningless     */
  /* return address in the object.  It also speeds things up, but it is */
  /* admittedly quite ugly.                                             */
# define GC_debug_malloc_replacement(lb) GC_debug_malloc(lb, GC_DBG_EXTRAS)

# if defined(CPPCHECK)
#   define REDIRECT_MALLOC_F GC_malloc /* e.g. */
# else
#   define REDIRECT_MALLOC_F REDIRECT_MALLOC
# endif

  void * malloc(size_t lb)
  {
    /* It might help to manually inline the GC_malloc call here.        */
    /* But any decent compiler should reduce the extra procedure call   */
    /* to at most a jump instruction in this case.                      */
#   if defined(I386) && defined(GC_SOLARIS_THREADS)
      /* Thread initialization can call malloc before we are ready for. */
      /* It is not clear that this is enough to help matters.           */
      /* The thread implementation may well call malloc at other        */
      /* inopportune times.                                             */
      if (!EXPECT(GC_is_initialized, TRUE)) return sbrk(lb);
#   endif
    return (void *)REDIRECT_MALLOC_F(lb);
  }

# if defined(GC_LINUX_THREADS)
#   ifdef HAVE_LIBPTHREAD_SO
      STATIC ptr_t GC_libpthread_start = NULL;
      STATIC ptr_t GC_libpthread_end = NULL;
#   endif
    STATIC ptr_t GC_libld_start = NULL;
    STATIC ptr_t GC_libld_end = NULL;
    static GC_bool lib_bounds_set = FALSE;

    GC_INNER void GC_init_lib_bounds(void)
    {
      IF_CANCEL(int cancel_state;)

      /* This test does not need to ensure memory visibility, since     */
      /* the bounds will be set when/if we create another thread.       */
      if (EXPECT(lib_bounds_set, TRUE)) return;

      DISABLE_CANCEL(cancel_state);
      GC_init(); /* if not called yet */
#     if defined(GC_ASSERTIONS) && defined(GC_ALWAYS_MULTITHREADED)
        LOCK(); /* just to set GC_lock_holder */
#     endif
#     ifdef HAVE_LIBPTHREAD_SO
        if (!GC_text_mapping("libpthread-",
                             &GC_libpthread_start, &GC_libpthread_end)) {
          WARN("Failed to find libpthread.so text mapping: Expect crash\n", 0);
          /* This might still work with some versions of libpthread,    */
          /* so we do not abort.                                        */
        }
#     endif
      if (!GC_text_mapping("ld-", &GC_libld_start, &GC_libld_end)) {
          WARN("Failed to find ld.so text mapping: Expect crash\n", 0);
      }
#     if defined(GC_ASSERTIONS) && defined(GC_ALWAYS_MULTITHREADED)
        UNLOCK();
#     endif
      RESTORE_CANCEL(cancel_state);
      lib_bounds_set = TRUE;
    }
# endif /* GC_LINUX_THREADS */

  void * calloc(size_t n, size_t lb)
  {
    if (EXPECT((lb | n) > GC_SQRT_SIZE_MAX, FALSE) /* fast initial test */
        && lb && n > GC_SIZE_MAX / lb)
      return (*GC_get_oom_fn())(GC_SIZE_MAX); /* n*lb overflow */
#   if defined(GC_LINUX_THREADS)
      /* The linker may allocate some memory that is only pointed to by */
      /* mmapped thread stacks.  Make sure it is not collectible.       */
      {
        ptr_t caller = (ptr_t)__builtin_return_address(0);

        GC_init_lib_bounds();
        if (ADDR_INSIDE(caller, GC_libld_start, GC_libld_end)
#           ifdef HAVE_LIBPTHREAD_SO
              || ADDR_INSIDE(caller, GC_libpthread_start, GC_libpthread_end)
                    /* The two ranges are actually usually adjacent,    */
                    /* so there may be a way to speed this up.          */
#           endif
           ) {
          return GC_generic_malloc_uncollectable(n * lb, UNCOLLECTABLE);
        }
      }
#   endif
    return (void *)REDIRECT_MALLOC_F(n * lb);
  }

# ifndef strdup
    char *strdup(const char *s)
    {
      size_t lb = strlen(s) + 1;
      char *result = (char *)REDIRECT_MALLOC_F(lb);

      if (EXPECT(NULL == result, FALSE)) {
        errno = ENOMEM;
        return NULL;
      }
      BCOPY(s, result, lb);
      return result;
    }
# endif /* !defined(strdup) */
 /* If strdup is macro defined, we assume that it actually calls malloc, */
 /* and thus the right thing will happen even without overriding it.     */
 /* This seems to be true on most Linux systems.                         */

# ifndef strndup
    /* This is similar to strdup().     */
    char *strndup(const char *str, size_t size)
    {
      char *copy;
      size_t len = strlen(str);
      if (EXPECT(len > size, FALSE))
        len = size;
      copy = (char *)REDIRECT_MALLOC_F(len + 1);
      if (EXPECT(NULL == copy, FALSE)) {
        errno = ENOMEM;
        return NULL;
      }
      if (EXPECT(len > 0, TRUE))
        BCOPY(str, copy, len);
      copy[len] = '\0';
      return copy;
    }
# endif /* !strndup */

# undef GC_debug_malloc_replacement

#endif /* REDIRECT_MALLOC */

/* Explicitly deallocate the object.  hhdr should correspond to p.      */
static void free_internal(void *p, const hdr *hhdr)
{
  size_t lb = (size_t)(hhdr -> hb_sz);  /* size in bytes */
  size_t lg = BYTES_TO_GRANULES(lb);    /* size in granules */
  int k = hhdr -> hb_obj_kind;

  GC_bytes_freed += lb;
  if (IS_UNCOLLECTABLE(k)) GC_non_gc_bytes -= lb;
  if (EXPECT(lg <= MAXOBJGRANULES, TRUE)) {
    struct obj_kind *ok = &GC_obj_kinds[k];
    void **flh;

    /* It is unnecessary to clear the mark bit.  If the object is       */
    /* reallocated, it does not matter.  Otherwise, the collector will  */
    /* do it, since it is on a free list.                               */
    if (ok -> ok_init && EXPECT(lb > sizeof(word), TRUE)) {
      BZERO((word *)p + 1, lb - sizeof(word));
    }

    flh = &(ok -> ok_freelist[lg]);
    obj_link(p) = *flh;
    *flh = (ptr_t)p;
  } else {
    if (lb > HBLKSIZE) {
      GC_large_allocd_bytes -= HBLKSIZE * OBJ_SZ_TO_BLOCKS(lb);
    }
    GC_freehblk(HBLKPTR(p));
  }
}

GC_API void GC_CALL GC_free(void * p)
{
    const hdr *hhdr;

    if (p /* != NULL */) {
        /* CPPCHECK */
    } else {
        /* Required by ANSI.  It's not my fault ...     */
        return;
    }

#   ifdef LOG_ALLOCS
      GC_log_printf("GC_free(%p) after GC #%lu\n",
                    p, (unsigned long)GC_gc_no);
#   endif
    hhdr = HDR(p);
#   if defined(REDIRECT_MALLOC) && \
        ((defined(NEED_CALLINFO) && defined(GC_HAVE_BUILTIN_BACKTRACE)) \
         || defined(GC_SOLARIS_THREADS) || defined(GC_LINUX_THREADS) \
         || defined(MSWIN32))
        /* This might be called indirectly by GC_print_callers to free  */
        /* the result of backtrace_symbols.                             */
        /* For Solaris, we have to redirect malloc calls during         */
        /* initialization.  For the others, this seems to happen        */
        /* implicitly.                                                  */
        /* Don't try to deallocate that memory.                         */
        if (EXPECT(NULL == hhdr, FALSE)) return;
#   endif
    GC_ASSERT(GC_base(p) == p);
    LOCK();
    free_internal(p, hhdr);
    FREE_PROFILER_HOOK(p);
    UNLOCK();
}

#ifdef THREADS
  GC_INNER void GC_free_inner(void * p)
  {
    GC_ASSERT(I_HOLD_LOCK());
    free_internal(p, HDR(p));
  }
#endif /* THREADS */

#if defined(REDIRECT_MALLOC) && !defined(REDIRECT_FREE)
# define REDIRECT_FREE GC_free
#endif

#if defined(REDIRECT_FREE) && !defined(REDIRECT_MALLOC_IN_HEADER)

# if defined(CPPCHECK)
#   define REDIRECT_FREE_F GC_free /* e.g. */
# else
#   define REDIRECT_FREE_F REDIRECT_FREE
# endif

  void free(void * p)
  {
#   ifdef IGNORE_FREE
      UNUSED_ARG(p);
#   else
#     if defined(GC_LINUX_THREADS) && !defined(USE_PROC_FOR_LIBRARIES)
        /* Don't bother with initialization checks.  If nothing         */
        /* has been initialized, the check fails, and that's safe,      */
        /* since we have not allocated uncollectible objects neither.   */
        ptr_t caller = (ptr_t)__builtin_return_address(0);
        /* This test does not need to ensure memory visibility, since   */
        /* the bounds will be set when/if we create another thread.     */
        if (ADDR_INSIDE(caller, GC_libld_start, GC_libld_end)
#           ifdef HAVE_LIBPTHREAD_SO
              || ADDR_INSIDE(caller, GC_libpthread_start, GC_libpthread_end)
#           endif
           ) {
          GC_free(p);
          return;
        }
#     endif
      REDIRECT_FREE_F(p);
#   endif
  }
#endif /* REDIRECT_FREE */
