/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2007-2008 H. Peter Anvin - All Rights Reserved
 *
 *   Permission is hereby granted, free of charge, to any person
 *   obtaining a copy of this software and associated documentation
 *   files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use,
 *   copy, modify, merge, publish, distribute, sublicense, and/or
 *   sell copies of the Software, and to permit persons to whom
 *   the Software is furnished to do so, subject to the following
 *   conditions:
 *
 *   The above copyright notice and this permission notice shall
 *   be included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *   OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *   OTHER DEALINGS IN THE SOFTWARE.
 *
 * ----------------------------------------------------------------------- */

/*
 * movebits.c
 *
 * Utility function to take a list of memory areas to shuffle and
 * convert it to a set of shuffle operations.
 *
 * Note: a lot of the functions in this file deal with "parent pointers",
 * which are pointers to a pointer to a list, or part of the list.
 * They can be pointers to a variable holding the list root pointer,
 * or pointers to a next field of a previous entry.
 */

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <inttypes.h>
#include <setjmp.h>

#include <syslinux/movebits.h>

#ifndef DEBUG
# ifdef TEST
#  define DEBUG 1
# else
#  define DEBUG 0
# endif
#endif

#if DEBUG
# include <stdio.h>
# define dprintf printf
#else
# define dprintf(...) ((void)0)
#endif

#define min(x,y) ((x) < (y) ? (x) : (y))
#define max(x,y) ((x) > (y) ? (x) : (y))

static jmp_buf new_movelist_bail;

static struct syslinux_movelist *
new_movelist(addr_t dst, addr_t src, addr_t len)
{
  struct syslinux_movelist *ml = malloc(sizeof(struct syslinux_movelist));

  if ( !ml )
    longjmp(new_movelist_bail, 1);

  ml->dst = dst;
  ml->src = src;
  ml->len = len;
  ml->next = NULL;

  return ml;
}

static struct syslinux_movelist *
dup_movelist(struct syslinux_movelist *src)
{
  struct syslinux_movelist *dst = NULL, **dstp = &dst, *ml;

  while (src) {
    ml = new_movelist(src->dst, src->src, src->len);
    *dstp = ml;
    dstp = &ml->next;
    src = src->next;
  }

  return dst;
}

static void
add_freelist(struct syslinux_memmap **mmap, addr_t start,
	     addr_t len, enum syslinux_memmap_types type)
{
  if (syslinux_add_memmap(mmap, start, len, type))
    longjmp(new_movelist_bail, 1);
}

/*
 * Take a chunk, entirely confined in **parentptr, and split it off so that
 * it has its own structure.
 */
static struct syslinux_movelist **
split_movelist(addr_t start, addr_t len, struct syslinux_movelist **parentptr)
{
  struct syslinux_movelist *m, *ml = *parentptr;

  assert(start >= ml->src);
  assert(start < ml->src+ml->len);

  /* Split off the beginning */
  if ( start > ml->src ) {
    addr_t l = start - ml->src;

    m = new_movelist(ml->dst+l, start, ml->len-l);
    m->next = ml->next;
    ml->len = l;
    ml->next = m;

    parentptr = &ml->next;
    ml = m;			/* Continue processing the new node */
  }

  /* Split off the end */
  if ( ml->len > len ) {
    addr_t l = ml->len - len;

    m = new_movelist(ml->dst+len, ml->src+len, l);
    m->next = ml->next;
    ml->len = len;
    ml->next = m;
  }

  return parentptr;
}

static void
delete_movelist(struct syslinux_movelist **parentptr)
{
  struct syslinux_movelist *o = *parentptr;
  *parentptr = o->next;
  free(o);
}

static void
free_movelist(struct syslinux_movelist **parentptr)
{
  while (*parentptr)
    delete_movelist(parentptr);
}

/*
 * Scan the freelist looking for a particular chunk of memory
 */
static const struct syslinux_memmap *
is_free_zone(const struct syslinux_memmap *mmap, addr_t start, addr_t len)
{
  while (mmap->next->type != SMT_END && mmap->next->start < start)
    mmap = mmap->next;

  if (mmap->start > start)
    return NULL;
  if (mmap->next->start - mmap->start < len)
    return NULL;

  return mmap->type == SMT_FREE ? mmap : NULL;
}

/*
 * Scan the freelist looking for the smallest chunk of memory which
 * can fit X bytes; returns the length of the block on success.
 */
static addr_t free_area(const struct syslinux_memmap *mmap,
			addr_t len, addr_t *start)
{
  const struct syslinux_memmap *best = NULL;
  const struct syslinux_memmap *s;
  addr_t slen, best_len = -1;

  for (s = mmap; s->type != SMT_END; s = s->next) {
    slen = s->next->start - s->start;
    if (slen >= len) {
      if (!best || best_len > slen) {
	best = s;
	best_len = slen;
      }
    }
  }

  if (best) {
    *start = best->start;
    return best_len;
  } else {
    return 0;
  }
}

/*
 * Remove a chunk from the freelist
 */
static inline void
allocate_from(struct syslinux_memmap **mmap, addr_t start, addr_t len)
{
  syslinux_add_memmap(mmap, start, len, SMT_ALLOC);
}

/*
 * moves is computed from "frags" and "freemem".  "space" lists
 * free memory areas at our disposal, and is (src, cnt) only.
 */

int
syslinux_compute_movelist(struct syslinux_movelist **moves,
			  struct syslinux_movelist *ifrags,
			  struct syslinux_memmap *memmap)
{
  struct syslinux_memmap *mmap = NULL;
  const struct syslinux_memmap *mm, *ep;
  struct syslinux_movelist *frags = NULL;
  struct syslinux_movelist *mv;
  struct syslinux_movelist *f, **fp;
  struct syslinux_movelist *o, **op;
  addr_t needbase, needlen, copysrc, copydst, copylen;
  addr_t freebase, freelen;
  addr_t avail;
  addr_t fstart, flen;
  addr_t cbyte;
  addr_t ep_len;
  int rv = -1;
  int reverse;
  int again;

  dprintf("entering syslinux_compute_movelist()...\n");

  if (setjmp(new_movelist_bail)) {
  nomem:
    dprintf("Out of working memory!\n");
    goto bail;
  }

  *moves = NULL;

  /* Create our memory map.  Anything that is SMT_FREE or SMT_ZERO is
     fair game, but mark anything used by source material as SMT_ALLOC. */
  mmap = syslinux_init_memmap();
  if (!mmap)
    goto nomem;

  frags = dup_movelist(ifrags);

  for (mm = memmap; mm->type != SMT_END; mm = mm->next)
    add_freelist(&mmap, mm->start, mm->next->start - mm->start,
		 mm->type == SMT_ZERO ? SMT_FREE : mm->type);

  for (f = frags; f; f = f->next)
    add_freelist(&mmap, f->src, f->len, SMT_ALLOC);

  do {
    again = 0;

#if DEBUG
    dprintf("Current free list:\n");
    syslinux_dump_memmap(stdout, mmap);
    dprintf("Current frag list:\n");
    syslinux_dump_movelist(stdout, frags);
#endif

    fp = &frags;
    while ( (f = *fp) ) {
      dprintf("@: 0x%08x bytes at 0x%08x -> 0x%08x\n",
	      f->len, f->src, f->dst);

      if ( f->src == f->dst ) {
	delete_movelist(fp);
	continue;
      }

      /* See if we can move this chunk into place by claiming
	 the destination, or in the case of partial overlap, the
	 missing portion. */

      needbase = f->dst;
      needlen  = f->len;

      dprintf("need: base = 0x%08x, len = 0x%08x\n", needbase, needlen);

      reverse = 0;
      cbyte = f->dst;		/* "Critical byte" */
      if ( f->src < f->dst && (f->dst - f->src) < f->len ) {
	/* "Shift up" type overlap */
	needlen  = f->dst - f->src;
	needbase = f->dst + (f->len - needlen);
	cbyte = f->dst + f->len - 1;
	reverse = 1;
      } else if ( f->src > f->dst && (f->src - f->dst) < f->len ) {
	/* "Shift down" type overlap */
	needbase = f->dst;
	needlen  = f->src - f->dst;
      }

      dprintf("need: base = 0x%08x, len = 0x%08x, "
	      "reverse = %d, cbyte = 0x%08x\n",
	      needbase, needlen, reverse, cbyte);

      ep = is_free_zone(mmap, cbyte, 1);
      if (ep) {
	ep_len = ep->next->start - ep->start;
	if (reverse)
	  avail = needbase+needlen - ep->start;
	else
	  avail = ep_len - (needbase - ep->start);
      } else {
	avail = 0;
      }

      if (avail) {
	/* We can move at least part of this chunk into place without
	   further ado */
	dprintf("space: start 0x%08x, len 0x%08x, free 0x%08x\n",
		ep->start, ep_len, avail);
	copylen = min(needlen, avail);

	if (reverse)
	  allocate_from(&mmap, needbase+needlen-copylen, copylen);
	else
	  allocate_from(&mmap, needbase, copylen);

	goto move_chunk;
      }

      /* At this point, we need to evict something out of our space.
	 Find the object occupying the critical byte of our target space,
	 and move it out (the whole object if we can, otherwise a subset.)
	 Then move a chunk of ourselves into place. */
      for ( op = &f->next, o = *op ; o ; op = &o->next, o = *op ) {

	dprintf("O: 0x%08x bytes at 0x%08x -> 0x%08x\n",
		o->len, o->src, o->dst);

	if ( !(o->src <= cbyte && o->src+o->len > cbyte) )
	  continue;		/* Not what we're looking for... */

	/* Find somewhere to put it... */

	if ( is_free_zone(mmap, o->dst, o->len) ) {
	  /* Score!  We can move it into place directly... */
	  copydst = o->dst;
	  copylen = o->len;
	} else if ( free_area(mmap, o->len, &fstart) ) {
	  /* We can move the whole chunk */
	  copydst = fstart;
	  copylen = o->len;
	} else {
	  /* Well, copy as much as we can... */
	  if (syslinux_memmap_largest(mmap, SMT_FREE, &fstart, &flen)) {
	    dprintf("No free memory at all!\n");
	    goto bail;		/* Stuck! */
	  }

	  /* Make sure we include the critical byte */
	  copydst = fstart;
	  if (reverse) {
	    copysrc = max(o->src, cbyte+1 - flen);
	    copylen = cbyte+1 - copysrc;
	  } else {
	    copysrc = cbyte;
	    copylen = min(flen, o->len - (cbyte-o->src));
	  }
	}
	allocate_from(&mmap, copydst, copylen);

	if ( copylen < o->len ) {
	  op = split_movelist(copysrc, copylen, op);
	  o = *op;
	}

	mv = new_movelist(copydst, copysrc, copylen);
	dprintf("C: 0x%08x bytes at 0x%08x -> 0x%08x\n",
		mv->len, mv->src, mv->dst);
	*moves = mv;
	moves = &mv->next;

	o->src = copydst;

	if ( copylen > needlen ) {
	  /* We don't need all the memory we freed up.  Mark it free. */
	  if ( copysrc < needbase ) {
	    add_freelist(&mmap, copysrc, needbase-copysrc, SMT_FREE);
	    copylen -= (needbase-copysrc);
	  }
	  if ( copylen > needlen ) {
	    add_freelist(&mmap, copysrc+needlen, copylen-needlen, SMT_FREE);
	    copylen = needlen;
	  }
	}
	reverse = 0;
	goto move_chunk;
      }
      dprintf("Cannot find the chunk containing the critical byte\n");
      goto bail;			/* Stuck! */

    move_chunk:
      /* We're allowed to move the chunk into place now. */

      copydst = f->dst;
      copysrc = f->src;

      dprintf("Q: copylen = 0x%08x, needlen = 0x%08x\n", copylen, needlen);

      if ( copylen < needlen ) {
	if (reverse) {
	  copydst += (f->len-copylen);
	  copysrc += (f->len-copylen);
	}

	dprintf("X: 0x%08x bytes at 0x%08x -> 0x%08x\n",
		copylen, copysrc, copydst);

	/* Didn't get all we wanted, so we have to split the chunk */
	fp = split_movelist(copysrc, copylen, fp); /* Is this right? */
	f = *fp;
      }

      mv = new_movelist(f->dst, f->src, f->len);
      dprintf("A: 0x%08x bytes at 0x%08x -> 0x%08x\n",
	      mv->len, mv->src, mv->dst);
      *moves = mv;
      moves = &mv->next;

      /* Mark the new memory range occupied */
      add_freelist(&mmap, f->dst, f->len, SMT_ALLOC);

      /* Figure out what memory we just freed up */
      if ( f->dst > f->src ) {
	freebase = f->src;
	freelen  = min(f->len, f->dst-f->src);
      } else if ( f->src >= f->dst+f->len ) {
	freebase = f->src;
	freelen  = f->len;
      } else {
	freelen  = f->src-f->dst;
	freebase = f->dst+f->len;
      }

      dprintf("F: 0x%08x bytes at 0x%08x\n", freelen, freebase);

      add_freelist(&mmap, freebase, freelen, SMT_FREE);

      delete_movelist(fp);
      again = 1;		/* At least one chunk was moved */
    }
  } while (again);

  rv = 0;
 bail:
  if (mmap)
    syslinux_free_memmap(mmap);
  if (frags)
    free_movelist(&frags);
  return rv;
}

#ifdef TEST

#include <stdio.h>

int main(int argc, char *argv[])
{
  FILE *f;
  unsigned long d, s, l;
  struct syslinux_movelist *frags;
  struct syslinux_movelist **fep = &frags;
  struct syslinux_movelist *space;
  struct syslinux_movelist **sep = &space;
  struct syslinux_movelist *mv, *moves;

  f = fopen(argv[1], "r");
  while ( fscanf(f, "%lx %lx %lx", &d, &s, &l) == 3 ) {
    if ( d ) {
      mv = new_movelist(d, s, l);
      *fep = mv;
      fep = &mv->next;
    } else {
      mv = new_movelist(0, s, l);
      *sep = mv;
      sep = &mv->next;
    }
  }
  fclose(f);

  if ( syslinux_compute_movelist(&moves, frags, space) ) {
    printf("Failed to compute a move sequence\n");
    return 1;
  } else {
    for ( mv = moves ; mv ; mv = mv->next ) {
      printf("0x%08x bytes at 0x%08x -> 0x%08x\n",
	     mv->len, mv->src, mv->dst);
    }
    return 0;
  }
 }

#endif /* TEST */
