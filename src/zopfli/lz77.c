/*
Copyright 2011 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

/*Modified by Felix Hanau*/

#include "lz77.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void ZopfliInitLZ77Store(ZopfliLZ77Store* store) {
  store->size = 0;
  store->litlens = 0;
  store->dists = 0;
}

void ZopfliCleanLZ77Store(ZopfliLZ77Store* store) {
  free(store->litlens);
  free(store->dists);
}

void ZopfliCopyLZ77Store(const ZopfliLZ77Store* source, ZopfliLZ77Store* dest) {
  size_t i;
  ZopfliCleanLZ77Store(dest);
  dest->litlens = (unsigned short*)malloc(sizeof(*dest->litlens) * source->size);
  dest->dists = (unsigned short*)malloc(sizeof(*dest->dists) * source->size);

  if (!dest->litlens || !dest->dists) exit(-1); /* Allocation failed. */

  dest->size = source->size;
  for (i = 0; i < source->size; i++) {
    dest->litlens[i] = source->litlens[i];
    dest->dists[i] = source->dists[i];
  }
}

/*
Appends the length and distance to the LZ77 arrays of the ZopfliLZ77Store.
context must be a ZopfliLZ77Store*.
*/
void ZopfliStoreLitLenDist(unsigned short length, unsigned short dist,
                           ZopfliLZ77Store* store) {
  size_t size2 = store->size;  /* Needed for using ZOPFLI_APPEND_DATA twice. */
  ZOPFLI_APPEND_DATA(length, &store->litlens, &store->size);
  ZOPFLI_APPEND_DATA(dist, &store->dists, &size2);
}

#ifndef NDEBUG
void ZopfliVerifyLenDist(const unsigned char* data, size_t datasize, size_t pos,
                         unsigned short dist, unsigned short length) {

  size_t i;

  assert(pos + length <= datasize);
  for (i = 0; i < length; i++) {
    if (data[pos - dist + i] != data[pos + i]) {
      assert(data[pos - dist + i] == data[pos + i]);
      break;
    }
  }
}
#endif

/*
Finds how long the match of scan and match is. Can be used to find how many
bytes starting from scan, and from match, are equal. Returns the last byte
after scan, which is still equal to the correspondinb byte after match.
scan is the position to compare
match is the earlier position to compare.
end is the last possible byte, beyond which to stop looking.
safe_end is a few (8) bytes before end, for comparing multiple bytes at once.
*/
static const unsigned char* GetMatch(const unsigned char* scan,
                                     const unsigned char* match,
                                     const unsigned char* end
#ifndef __GNUC__
, const unsigned char* safe_end
#endif
) {
#ifdef __GNUC__

    /* Optimized Function based on cloudflare's zlib fork. Using AVX for 32 Checks at once may be even faster but currently there is no ctz function for vectors so the old approach would be neccesary again. */
  do {
    unsigned long sv = *(unsigned long*)(void*)scan;
    unsigned long mv = *(unsigned long*)(void*)match;
    unsigned long xor = sv ^ mv;
    if (xor) {
      scan += __builtin_ctzl(xor) / 8;
      break;
    } else {
      scan += 8;
      match += 8;
    }
  } while (scan < end);

  if (unlikely(scan > end))
    scan = end;
  return scan;

#else

    if (sizeof(size_t) == 8) {
        /* 8 checks at once per array bounds check (size_t is 64-bit). */
        while (scan < safe_end && *((size_t*)scan) == *((size_t*)match)) {
            scan += 8;
            match += 8;
        }
    } else if (sizeof(unsigned) == 4) {
        /* 4 checks at once per array bounds check (unsigned is 32-bit). */
        while (scan < safe_end
               && *((unsigned*)scan) == *((unsigned*)match)) {
            scan += 4;
            match += 4;
        }
    } else {
        /* do 8 checks at once per array bounds check. */
        while (scan < safe_end && *scan == *match && *++scan == *++match
               && *++scan == *++match && *++scan == *++match
               && *++scan == *++match && *++scan == *++match
               && *++scan == *++match && *++scan == *++match) {
            scan++; match++;
        }
    }
    /* The remaining few bytes. */
    while (scan != end && *scan == *match) {
        scan++; match++;
    }
    return scan;
#endif
}

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
/*
Gets distance, length and sublen values from the cache if possible.
Returns 1 if it got the values from the cache, 0 if not.
Updates the limit value to a smaller one if possible with more limited
information from the cache.
*/
static int TryGetFromLongestMatchCache(ZopfliBlockState* s,
    size_t pos, unsigned short* limit,
    unsigned short* distance, unsigned short* length) {
  /* The LMC cache starts at the beginning of the block rather than the
     beginning of the whole array. */
  size_t lmcpos = pos - s->blockstart;

  /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
     that this cache value is not filled in yet. */
  unsigned char cache_available = s->lmc->length[lmcpos] == 0 ||
      s->lmc->dist[lmcpos] != 0;
  unsigned char limit_ok_for_cache = cache_available && s->lmc->length[lmcpos] <= *limit;

  if (limit_ok_for_cache) {
      *length = s->lmc->length[lmcpos];
      if (*length > *limit) *length = *limit;
      *distance = s->lmc->dist[lmcpos];
      return 1;
  }

  return 0;
}

static int TryGetFromLongestMatchCache2(ZopfliBlockState* s,
                                        size_t pos, unsigned* limit,
                                        unsigned short* sublen, unsigned short* length) {
    /* The LMC cache starts at the beginning of the block rather than the
     beginning of the whole array. */
    size_t lmcpos = pos - s->blockstart;

    /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
     that this cache value is not filled in yet. */
    if (s->lmc->length[lmcpos] == 0 ||
        s->lmc->dist[lmcpos] != 0) {
        if (s->lmc->length[lmcpos] <= ZopfliMaxCachedSublen(s->lmc, lmcpos)) {
            *length = s->lmc->length[lmcpos];
            if (*length > ZOPFLI_MAX_MATCH) *length = ZOPFLI_MAX_MATCH;
            ZopfliCacheToSublen(s->lmc, lmcpos, sublen);
            return 1;
        }
        /* Can't use much of the cache, since the "sublens" need to be calculated,
         but at least we already know when to stop. */
        *limit = s->lmc->length[lmcpos];
    }
    
    return 0;
}

/*
Stores the found sublen, distance and length in the longest match cache, if
possible.
*/
static void StoreInLongestMatchCache(ZopfliBlockState* s,
    size_t pos, size_t limit,
    const unsigned short* sublen,
    unsigned short distance, unsigned short length) {
  /* The LMC cache starts at the beginning of the block rather than the
   beginning of the whole array. */
  size_t lmcpos = pos - s->blockstart;

  /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
   that this cache value is not filled in yet. */
  if (s->lmc && limit == ZOPFLI_MAX_MATCH && sublen && s->lmc->length[lmcpos] == 1 && s->lmc->dist[lmcpos] == 0){
    if (length < ZOPFLI_MIN_MATCH){
      s->lmc->dist[lmcpos] = 0;
      s->lmc->length[lmcpos] = 0;
    }
    else{
      s->lmc->dist[lmcpos] = distance;
      s->lmc->length[lmcpos] = length;
      assert(!(s->lmc->length[lmcpos] == 1 && s->lmc->dist[lmcpos] == 0));
      ZopfliSublenToCache(sublen, lmcpos, length, s->lmc);
    }
  }
}
#endif

void ZopfliFindLongestMatch(ZopfliBlockState* s, const ZopfliHash* h,
                            const unsigned char* array,
                            size_t pos, size_t size, unsigned short limit,
                            unsigned short* sublen, unsigned short* distance, unsigned short* length, unsigned char try) {
#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  if (try){
      if (TryGetFromLongestMatchCache(s, pos, &limit, distance, length)) {
          return;
      }
  }
#endif
  if (size - pos < ZOPFLI_MIN_MATCH) {
    /* The rest of the code assumes there are at least ZOPFLI_MIN_MATCH bytes to
     try. */
    *length = 0;
    *distance = 0;
    return;
  }
  unsigned short chain_counter = s->options->chain_length;   /*For quitting early. */

  if (pos + limit > size) {
    limit = size - pos;
  }
  const unsigned char* arrayend = &array[pos] + limit;
#ifndef __GNUC__
  const unsigned char* arrayend_safe = arrayend - 8;
#endif
  unsigned short hpos = pos & ZOPFLI_WINDOW_MASK, p, pp;
  unsigned short* hprev = h->prev;
  int* hhead = h->head;
  pp = hpos;  /* During the whole loop, p == hprev[pp]. */
  p = hprev[pp];

  unsigned dist = p < pp ? pp - p : ((ZOPFLI_WINDOW_SIZE - p) + pp); /* Not unsigned short on purpose. */

  unsigned short bestlength = 1;
  unsigned short bestdist = 0;
  const unsigned char* scan;
  const unsigned char* match;
  const unsigned char* new = &array[pos];
  /* Go through all distances. */
  while (dist < ZOPFLI_WINDOW_SIZE) {
    if (dist > 0) {
      scan = new;
      match = new - dist;

      /* Testing the byte at position bestlength first, goes slightly faster. */
      if (pos + bestlength >= size
          || *(scan + bestlength) == *(match + bestlength)) {

#ifdef ZOPFLI_HASH_SAME
        unsigned short same0 = h->same[hpos];
        if (same0 > 2) {
          unsigned short same1 = h->same[(pos - dist) & ZOPFLI_WINDOW_MASK];
          unsigned short same = same0 < same1 ? same0 : same1;
          if (same > limit) same = limit;
          scan += same;
          match += same;
        }
#endif
        scan = GetMatch(scan, match, arrayend
#ifndef __GNUC__
                        , arrayend_safe
#endif
                        );
        unsigned short currentlength = scan - new;  /* The found length. */
        if (currentlength > bestlength) {
          if (sublen) {
            for (unsigned short j = bestlength + 1; j <= currentlength; j++) {
              sublen[j] = dist;
            }
          }
          bestdist = dist;
          bestlength = currentlength;
          if (currentlength >= limit) break;
        }
      }
    }

#ifdef ZOPFLI_HASH_SAME_HASH
    /* Switch to the other hash once this will be more efficient. */
    if (hhead != h->head2 && bestlength >= h->same[hpos] &&
        h->val2 == h->hashval2[p]) {
      /* Now use the hash that encodes the length and first byte. */
      hhead = h->head2;
      hprev = h->prev2;
    }
#endif

    pp = p;
    p = hprev[p];

    dist += p < pp ? pp - p : ((ZOPFLI_WINDOW_SIZE - p) + pp);
    chain_counter--;
    if (!chain_counter) break;
  }

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  StoreInLongestMatchCache(s, pos, limit, sublen, bestdist, bestlength);
#endif

  *distance = bestdist;
  *length = bestlength;
}

void ZopfliFindLongestMatch2(ZopfliBlockState* s, const ZopfliHash* h,
                            const unsigned char* array,
                            size_t pos, size_t size, unsigned short* sublen, unsigned short* length, unsigned char storeincache) {
  unsigned limit = ZOPFLI_MAX_MATCH;

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  if (TryGetFromLongestMatchCache2(s, pos, &limit, sublen, length)) {
    return;
  }
#endif
  if (size - pos < ZOPFLI_MIN_MATCH) {
    /* The rest of the code assumes there are at least ZOPFLI_MIN_MATCH bytes to
     try. */
    *length = 0;
    return;
  }
  unsigned short chain_counter = s->options->chain_length;   /*For quitting early. */

  if (pos + limit > size) {
    limit = size - pos;
  }
  const unsigned char* arrayend = &array[pos] + limit;
#ifndef __GNUC__
  const unsigned char* arrayend_safe = arrayend - 8;
#endif
  unsigned short hpos = pos & ZOPFLI_WINDOW_MASK;
  unsigned short pp = hpos;  /* During the whole loop, p == hprev[pp]. */
  unsigned short* hprev = h->prev;
  unsigned char hash2 = 0;
  unsigned size2 = size - pos;
  unsigned short p = hprev[pp];

  unsigned dist = p < pp ? pp - p : ((ZOPFLI_WINDOW_SIZE - p) + pp); /* Not unsigned short on purpose. */

  unsigned short same0 = h->same[hpos];
    if (same0 >= limit) {
        same0 = limit;
        hash2 = 1;
    }

  unsigned short bestlength = 1;
  unsigned short bestdist = 0;

  const unsigned char* new = &array[pos];

  const unsigned char* scan;
  const unsigned char* match;
  /* Go through all distances. */
  if (dist == 0){
    pp = p;
    p = hprev[p];

    dist += p < pp ? pp - p : ((ZOPFLI_WINDOW_SIZE - p) + pp);
    chain_counter--;
  }
  while (dist < ZOPFLI_WINDOW_SIZE) {
    scan = new;
    match = scan - dist;

    if (unlikely(bestlength >= size2
                  || *(scan + bestlength) == *(match + bestlength))) {
#ifdef ZOPFLI_HASH_SAME
      if (same0 > 2) {//TUNE THE TWO!!!
        unsigned short same1 = h->same[(pos - dist) & ZOPFLI_WINDOW_MASK];
        unsigned short same = same0 < same1 ? same0 : same1;
        scan += same;
        match += same;
      }
#endif
      scan = GetMatch(scan, match, arrayend
#ifndef __GNUC__
                      , arrayend_safe
#endif
                      );
      unsigned short currentlength = scan - new;
      if (unlikely(currentlength > bestlength)) {
        for (unsigned short j = bestlength + 1; j <= currentlength; j++) {
          sublen[j] = dist;
        }
        bestdist = dist;
        bestlength = currentlength;
        if (currentlength >= limit) break;
      }
    }

#ifdef ZOPFLI_HASH_SAME_HASH
    /* Switch to the other hash once this will be more efficient. */
    if (unlikely(!hash2 && bestlength >= same0 &&
        h->val2 == h->hashval2[p])) {
      /* Now use the hash that encodes the length and first byte. */
      hash2 = 1;
      hprev = h->prev2;
    }
#endif

    pp = p;
    p = hprev[p];

    dist += p < pp ? pp - p : ((ZOPFLI_WINDOW_SIZE - p) + pp);
    chain_counter--;
    if (!chain_counter) break;
  }

#ifdef ZOPFLI_LONGEST_MATCH_CACHE
  if (storeincache){
    StoreInLongestMatchCache(s, pos, limit, sublen, bestdist, bestlength);
  }
#endif

  *length = bestlength;
}

void ZopfliLZ77Greedy(ZopfliBlockState* s, const unsigned char* in,
                      size_t instart, size_t inend,
                      ZopfliLZ77Store* store, unsigned char blocksplitting) {
  size_t i = 0, j;
  unsigned short leng;
  unsigned short dist;
  int lengthscore;
  size_t windowstart = instart > ZOPFLI_WINDOW_SIZE
      ? instart - ZOPFLI_WINDOW_SIZE : 0;
  unsigned short dummysublen[259];

  ZopfliHash hash;
  ZopfliHash* h = &hash;

  unsigned prev_length = 0;
  unsigned prev_match = 0;
  int prevlengthscore;
  int match_available = 0;

  if (instart == inend) return;

  ZopfliInitHash(h);
  ZopfliWarmupHash(in, windowstart, h);
  LoopedUpdateHash(in, windowstart, inend, h, instart - windowstart);

  for (i = instart; i < inend; i++) {
    ZopfliUpdateHash(in, i, inend, h);

    ZopfliFindLongestMatch(s, h, in, i, inend, ZOPFLI_MAX_MATCH, dummysublen,
                           &dist, &leng, 0);
    lengthscore = dist > s->options->lengthscoresearch ? leng - 1 : leng;

    /* Lazy matching. */
    prevlengthscore = prev_match > s->options->lengthscoresearch ? prev_length - 1 : prev_length;
    if (match_available) {
      match_available = 0;
      if (lengthscore > prevlengthscore + 1) {
        ZopfliStoreLitLenDist(in[i - 1], 0, store);
        if (lengthscore >= ZOPFLI_MIN_MATCH && leng < ZOPFLI_MAX_MATCH) {
          match_available = 1;
          prev_length = leng;
          prev_match = dist;
          continue;
        }
      } else {
        /* Add previous to output. */
        leng = prev_length;
        dist = prev_match;
        /* Add to output. */
#ifndef NDEBUG
        ZopfliVerifyLenDist(in, inend, i - 1, dist, leng);
#endif
        ZopfliStoreLitLenDist(leng, dist, store);
        for (j = 2; j < leng; j++) {
          i++;
          ZopfliUpdateHash(in, i, inend, h);
        }
        continue;
      }
    }
    else if (lengthscore >= ZOPFLI_MIN_MATCH && leng < ZOPFLI_MAX_MATCH) {
      match_available = 1;
      prev_length = leng;
      prev_match = dist;
      continue;
    }
    /* End of lazy matching. */

    /* Add to output. */
    if (lengthscore >= ZOPFLI_MIN_MATCH) {
#ifndef NDEBUG
        ZopfliVerifyLenDist(in, inend, i, dist, leng);
#endif
      ZopfliStoreLitLenDist(leng, dist, store);
    } else {
      leng = 1;
      ZopfliStoreLitLenDist(in[i], 0, store);
    }
    for (j = 1; j < leng; j++) {
      assert(i < inend);
      i++;
      ZopfliUpdateHash(in, i, inend, h);
    }
  }

  ZopfliCleanHash(h);
}

void ZopfliLZ77Counts(const unsigned short* litlens,
                      const unsigned short* dists,
                      size_t start, size_t end,
                      size_t* ll_count, size_t* d_count) {
  size_t i;

  for (i = 0; i < 288; i++) {
    ll_count[i] = 0;
  }
  for (i = 0; i < 32; i++) {
    d_count[i] = 0;
  }

  for (i = start; i < end; i++) {
    if (dists[i] == 0) {
      ll_count[litlens[i]]++;
    } else {
      ll_count[ZopfliGetLengthSymbol(litlens[i])]++;
      d_count[ZopfliGetDistSymbol(dists[i])]++;
    }
  }

  ll_count[256] = 1;  /* End symbol. */
}
