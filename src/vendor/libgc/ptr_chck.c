/*
 * Copyright (c) 1991-1994 by Xerox Corporation.  All rights reserved.
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

#include "private/gc_pmark.h"

/*
 * These are checking routines calls to which could be inserted by a
 * preprocessor to validate C pointer arithmetic.
 */

STATIC void GC_CALLBACK GC_default_same_obj_print_proc(void *p, void *q)
{
    ABORT_ARG2("GC_same_obj test failed",
               ": %p and %p are not in the same object", p, q);
}

GC_same_obj_print_proc_t GC_same_obj_print_proc =
                GC_default_same_obj_print_proc;

GC_API void * GC_CALL GC_same_obj(void *p, void *q)
{
    hdr *hhdr;
    ptr_t base, limit;
    word sz;

    if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
    hhdr = HDR(p);
    if (NULL == hhdr) {
        if (divHBLKSZ(ADDR(p)) != divHBLKSZ(ADDR(q)) && HDR(q) != NULL) {
          GC_same_obj_print_proc((ptr_t)p, (ptr_t)q);
        }
        return p;
    }
    /* If it's a pointer to the middle of a large object, move it       */
    /* to the beginning.                                                */
    if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
        struct hblk *h = GC_find_starting_hblk(HBLKPTR(p), &hhdr);

        limit = (ptr_t)h + hhdr -> hb_sz;
        if (ADDR_GE((ptr_t)p, limit) || ADDR_GE((ptr_t)q, limit)
            || ADDR_LT((ptr_t)q, (ptr_t)h)) {
          GC_same_obj_print_proc((ptr_t)p, (ptr_t)q);
        }
        return p;
    }
    sz = hhdr -> hb_sz;
    if (sz > MAXOBJBYTES) {
      base = (ptr_t)HBLKPTR(p);
      limit = base + sz;
      if (ADDR_GE((ptr_t)p, limit)) {
        GC_same_obj_print_proc((ptr_t)p, (ptr_t)q);
        return p;
      }
    } else {
      size_t offset;
      size_t pdispl = HBLKDISPL(p);

      offset = pdispl % sz;
      if (HBLKPTR(p) != HBLKPTR(q)) {
                /* W/o this check, we might miss an error if    */
                /* q points to the first object on a page, and  */
                /* points just before the page.                 */
        GC_same_obj_print_proc((ptr_t)p, (ptr_t)q);
        return p;
      }
      base = (ptr_t)p - offset;
      limit = base + sz;
    }
    /* [base, limit) delimits the object containing p, if any.  */
    /* If p is not inside a valid object, then either q is      */
    /* also outside any valid object, or it is outside          */
    /* [base, limit).                                           */
    if (!ADDR_INSIDE((ptr_t)q, base, limit)) {
      GC_same_obj_print_proc((ptr_t)p, (ptr_t)q);
    }
    return p;
}

STATIC void GC_CALLBACK GC_default_is_valid_displacement_print_proc(void *p)
{
    ABORT_ARG1("GC_is_valid_displacement test failed", ": %p not valid", p);
}

GC_valid_ptr_print_proc_t GC_is_valid_displacement_print_proc =
                GC_default_is_valid_displacement_print_proc;

GC_API void * GC_CALL GC_is_valid_displacement(void *p)
{
    hdr *hhdr;
    word pdispl;
    word offset;
    struct hblk *h;
    word sz;

    if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
    if (NULL == p) return NULL;
    hhdr = HDR(p);
    if (NULL == hhdr) return p;
    h = HBLKPTR(p);
    if (GC_all_interior_pointers) {
        h = GC_find_starting_hblk(h, &hhdr);
    } else if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
        GC_is_valid_displacement_print_proc((ptr_t)p);
        return p;
    }
    sz = hhdr -> hb_sz;
    pdispl = HBLKDISPL(p);
    offset = pdispl % sz;
    if ((sz > MAXOBJBYTES && ADDR_GE((ptr_t)p, (ptr_t)h + sz))
        || !GC_valid_offsets[offset]
        || (ADDR_LT((ptr_t)(h + 1), (ptr_t)p + sz - offset)
            && !IS_FORWARDING_ADDR_OR_NIL(HDR(h + 1)))) {
        GC_is_valid_displacement_print_proc((ptr_t)p);
    }
    return p;
}

STATIC void GC_CALLBACK GC_default_is_visible_print_proc(void *p)
{
    ABORT_ARG1("GC_is_visible test failed", ": %p not GC-visible", p);
}

GC_valid_ptr_print_proc_t GC_is_visible_print_proc =
                GC_default_is_visible_print_proc;

#ifndef THREADS
/* Could p be a stack address? */
  STATIC GC_bool GC_on_stack(ptr_t p)
  {
    return HOTTER_THAN(p, GC_stackbottom) && !HOTTER_THAN(p, GC_approx_sp());
  }
#endif /* !THREADS */

GC_API void * GC_CALL GC_is_visible(void *p)
{
    const hdr *hhdr;

    if ((ADDR(p) & (ALIGNMENT-1)) != 0) goto fail;
    if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
#   ifdef THREADS
        hhdr = HDR(p);
        if (hhdr != NULL && NULL == GC_base(p)) {
            goto fail;
        } else {
            /* May be inside thread stack.  We can't do much. */
            return p;
        }
#   else
        /* Check stack first: */
        if (GC_on_stack((ptr_t)p)) return p;

        hhdr = HDR(p);
        if (NULL == hhdr) {
            if (GC_is_static_root((ptr_t)p)) return p;
            /* Else do it again correctly:      */
#           if defined(DYNAMIC_LOADING) || defined(ANY_MSWIN) || defined(PCR)
              if (!GC_no_dls) {
                GC_register_dynamic_libraries();
                if (GC_is_static_root((ptr_t)p)) return p;
              }
#           endif
            goto fail;
        } else {
            /* p points to the heap. */
            word descr;
            ptr_t base = (ptr_t)GC_base(p);
                        /* TODO: should GC_base be manually inlined? */

            if (NULL == base) goto fail;
            if (HBLKPTR(base) != HBLKPTR(p))
                hhdr = HDR(base);
            descr = hhdr -> hb_descr;
        retry:
            switch (descr & GC_DS_TAGS) {
                case GC_DS_LENGTH:
                    if ((word)((ptr_t)p - base) > descr) goto fail;
                    break;
                case GC_DS_BITMAP:
                    if ((ptr_t)p - base >= WORDS_TO_BYTES(BITMAP_BITS)
                        || (ADDR(p) & (sizeof(word)-1)) != 0) goto fail;
                    if (!(((word)1 << (CPP_WORDSZ-1 - (word)((ptr_t)p - base)))
                          & descr)) goto fail;
                    break;
                case GC_DS_PROC:
                    /* We could try to decipher this partially.         */
                    /* For now we just punt.                            */
                    break;
                case GC_DS_PER_OBJECT:
                    if (!(descr & SIGNB)) {
                      descr = *(word *)((ptr_t)base
                                        + (descr & ~(word)GC_DS_TAGS));
                    } else {
                      ptr_t type_descr = *(ptr_t *)base;
                      descr = *(word *)(type_descr
                                        - (descr - (word)(GC_DS_PER_OBJECT
                                           - GC_INDIR_PER_OBJ_BIAS)));
                    }
                    goto retry;
            }
            return p;
        }
#   endif
fail:
    GC_is_visible_print_proc((ptr_t)p);
    return p;
}

GC_API void * GC_CALL GC_pre_incr(void **p, ptrdiff_t how_much)
{
    void *initial = *p;
    void *result = GC_same_obj((ptr_t)initial + how_much, initial);

    if (!GC_all_interior_pointers) {
        (void)GC_is_valid_displacement(result);
    }
    *p = result;
    return result; /* updated pointer */
}

GC_API void * GC_CALL GC_post_incr(void **p, ptrdiff_t how_much)
{
    void *initial = *p;
    void *result = GC_same_obj((ptr_t)initial + how_much, initial);

    if (!GC_all_interior_pointers) {
        (void)GC_is_valid_displacement(result);
    }
    *p = result;
    return initial; /* original *p */
}

GC_API void GC_CALL GC_set_same_obj_print_proc(GC_same_obj_print_proc_t fn)
{
    GC_ASSERT(NONNULL_ARG_NOT_NULL(fn));
    GC_same_obj_print_proc = fn;
}

GC_API GC_same_obj_print_proc_t GC_CALL GC_get_same_obj_print_proc(void)
{
    return GC_same_obj_print_proc;
}

GC_API void GC_CALL GC_set_is_valid_displacement_print_proc(
                                        GC_valid_ptr_print_proc_t fn)
{
    GC_ASSERT(NONNULL_ARG_NOT_NULL(fn));
    GC_is_valid_displacement_print_proc = fn;
}

GC_API GC_valid_ptr_print_proc_t GC_CALL
GC_get_is_valid_displacement_print_proc(void)
{
    return GC_is_valid_displacement_print_proc;
}

GC_API void GC_CALL GC_set_is_visible_print_proc(GC_valid_ptr_print_proc_t fn)
{
    GC_ASSERT(NONNULL_ARG_NOT_NULL(fn));
    GC_is_visible_print_proc = fn;
}

GC_API GC_valid_ptr_print_proc_t GC_CALL GC_get_is_visible_print_proc(void)
{
    return GC_is_visible_print_proc;
}
