/*  This file is part of Jellyfish.

    Jellyfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Jellyfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Jellyfish.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Specialized implementation of a concurrent hash table. Assume that
 * n threads are doing 100% insertion operations.
 */

#ifndef __JELLYFISH_PACKED_CONCURRENT_HASH_COUNTERS__
#define __JELLYFISH_PACKED_CONCURRENT_HASH_COUNTERS__

#include <new>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <stdint.h>
#include <jellyfish/small_packed_array.hpp>

struct stats {
  unsigned int  key_conflicts;
  unsigned int  val_conflicts;
  unsigned int  destroyed_key;
  unsigned int  destroyed_val;
  unsigned int  maxed_out_val;
  unsigned int  maxed_reprobe;
  unsigned int  resized_arys;
};
#ifdef STATS
#define HAS_STAT 1
#define STAT_INC(var) __sync_fetch_and_add(&stats->var, 1);
#else
#define HAS_STAT 0
#define STAT_INC(var)
#endif

template <class Key, class Val, class A>
class arys_iterator {
  A                     *ary;

public:
  uint64_t              pos;
  Key                   key;
  Val                   val;

  arys_iterator(A *_ary) : ary(_ary), pos(0) {
  }

  ~arys_iterator() { ary->ref_dec(); }

  void rewind() { pos = 0; }

  bool next() {
    while(pos < ary->size) {
      if(ary->get(pos++, &key, &val))
        return true;
    }
    return false;
  }
};

class bad_size_exception: public exception
{
  virtual const char* what() const throw() {
    return "Size must be a power of 2";
  }
};


typedef void *(*hash_allocator)(size_t size);
typedef void (*hash_deallocator)(void *, size_t size);

void *malloc_alloc(size_t size);
void malloc_dealloc(void *, size_t);
void *mmap_alloc(size_t);
void mmap_dealloc(void *, size_t);


template <class Key, class Val, 
          hash_allocator alloc = mmap_alloc,
          hash_deallocator dealloc = mmap_dealloc>
class arys_t {
public:
  typedef Val val_t;
  small_packed_array *keys;
  Val *vals;

  unsigned long size;
  unsigned long mod_mask;
  unsigned long volatile nb_elt;
  unsigned int  volatile count;
  arys_t        *next;
  bool          allocated;
  unsigned int  volatile copy_chunck;

  /* arys are in a linked list. They are freed in reverse order of
   * creation. The link from previous arys counts as 1 reference, to
   * ensure proper order of deletion.
   * The constructor is not thread safe (protect by mutex)
   */
  arys_t(uint32_t _item_len, unsigned long _size, arys_t *prev) : 
    nb_elt(0), count(0), next(NULL), allocated(true), copy_chunck(0) {
    size = 1;
    while(_size > size) { size <<= 1; }
    mod_mask = size - 1;
    if(prev) {
      ref_inc();
      prev->next = this;
    }

    keys = new small_packed_array(_item_len, size);
    vals = (Val *)alloc(sizeof(Val) * size);
    if(!vals || !keys) {
      perror("Allocation error");
      exit(1);
    }
  }

  arys_t(uint32_t _item_len, unsigned long _size, char *map) :
    nb_elt(0), count(0), next(NULL), allocated(false), copy_chunck(0) {
    size = 1;
    while(_size > size) { size <<= 1; }
    if(_size != size)
      throw new bad_size_exception;
    mod_mask = size - 1;
    keys = new small_packed_array(map, _item_len, _size);
    vals = (Val *)(map + keys->get_data_len());
  }

  ~arys_t() {
    if(allocated) {
      dealloc(vals, sizeof(Val) * size);
      delete keys;
    }
  }

  void write(ostream &out) {
    keys->write(out);
    out.write((char *)vals, sizeof(Val) * size);
  }

  bool add(uint64_t idx, Key key, Val val)
  {
    if(!keys->set(idx, key))
      return false;
    
    Val *cvals = vals + idx;
    Val cval = *cvals;
    Val ncval, nval;

    while(1) {
      ncval = ~cval;
      if(ncval == 0x0)
        return true;
      if(ncval < val) { // Key reaches its max, truncate
        nval = 0x0;
      } else {
        nval = cval + val;
      }
      nval = __sync_val_compare_and_swap(cvals, cval, nval);
      if(nval == cval)
        return true;
      cval = nval;
    }
  }

  inline bool inc(uint64_t idx, Key key)
  { 
    return add(idx, key, 1);
  }

  bool get(uint64_t idx, Key *key, Val *val)
  {
    if(!keys->get(idx, key))
      return false;

    //    printf("Got key %lx\n", *key);
    //    printf("size %ld idx %ld vals %p val %p val %u\n", size, idx, vals, val,
    *val = vals[idx];
    return true;
  }

  /* Return a chuck of indices of elements to copy over.
   * Return false if nothing to copy anymore.
   */
  void rewind_chunck()
  {
    copy_chunck = 0;
  }

  bool get_chunck(unsigned long *start, unsigned long *end)
  {
    unsigned int i = __sync_fetch_and_add(&copy_chunck, 1);
    if(i >= 128)
      return false;
    unsigned long size_chunck = size >> 7;
    *start = i * size_chunck;
    *end = *start + size_chunck;
    if(*end > size)
      *end = size;
    return true;
  }

  typedef arys_iterator<Key,Val,arys_t> iterator;
  arys_iterator<Key,Val,arys_t> * new_iterator() {
    ref_inc();
    return new arys_iterator<Key,Val,arys_t>(this);
  }
  
  unsigned int ref_inc() {
    unsigned int c = __sync_add_and_fetch(&count, 1);
    return c;
  }
  unsigned int ref_dec() {
    unsigned int c = __sync_sub_and_fetch(&count, 1);
    return c;
  }
};

template <class Key, class Val, class arys_t>
class thread_hash_counter {
  uint32_t                      key_len;
  arys_t                        * volatile *key_val_arys_ptr;
  arys_t                        *key_val_arys;
  unsigned int                  max_reprobe, emax_reprobe;
  pthread_mutex_t               *resize_lock;
  //CLKBAR: pthread_barrier_t             *resize_barrier;
  barrier                       *resize_barrier;
  struct stats                  *stats;

  bool resize(arys_t *cary, bool lock);
  inline void release(arys_t *cary) {
    while(!cary->ref_dec()) {
      arys_t *next = cary->next;
      delete cary;
      cary = next;
    }
  }
  void copy_over(arys_t *carys);
  uint64_t MurmurHash64A(const void * key, int len, unsigned int seed);

public:
  thread_hash_counter(uint32_t _key_len, arys_t * volatile * _arys,
                      unsigned int _max_reprobe,
                      pthread_mutex_t *_resize_lock, 
                      //pthread_barrier_t *_resize_barrier,
                      barrier *_resize_barrier,
                      struct stats *_stats) : 
    key_len(_key_len), key_val_arys_ptr(_arys), max_reprobe(_max_reprobe), 
    emax_reprobe(_max_reprobe),
    resize_lock(_resize_lock), resize_barrier(_resize_barrier), stats(_stats) {
    key_val_arys = *key_val_arys_ptr;
    key_val_arys->ref_inc();
  }
  
  ~thread_hash_counter() {
    release(key_val_arys);
  }
  
  void add(Key k, Val v);
  inline void inc(Key k) { add(k, 1); }
};

template <class Key, class Val, class arys_t>
class concurrent_hash_counter {
  uint32_t                      key_len;
  arys_t                        * volatile key_val_arys;
  struct stats                  stats;
  unsigned int                  max_reprobe;
  pthread_mutex_t               resize_lock;
  barrier                       resize_barrier;
  //CLKBAR: pthread_barrier_t             resize_barrier;

public:
  concurrent_hash_counter(uint32_t _key_len, unsigned long _size, 
                          unsigned int _max_reprobe,
                          unsigned int nb_threads);
  ~concurrent_hash_counter();

  typedef thread_hash_counter<Key, Val, arys_t> thread;

  thread *new_hash_counter() {
    return new thread_hash_counter<Key,Val,arys_t>(key_len, &key_val_arys, 
                                                   max_reprobe, &resize_lock,
                                                   &resize_barrier,
                                                   &stats);
  }

  unsigned long size() { return key_val_arys->size; }
  arys_t *get_arys() { return key_val_arys; }

  // Not thread safe
  void print(ostream &out);
  void print_debug(ostream &out);
  void write_keys_vals(ostream &out);

  int has_stats() { return HAS_STAT; };
  void print_stats(ostream &out);
};

/*
 * Usefull sizes
 */
typedef arys_t<uint64_t,uint32_t> arys_64_32_t;
typedef concurrent_hash_counter<uint64_t,uint32_t,arys_64_32_t> chc_64_32_t;

/*
 * concurrent_hash_counter
 */
template <class Key, class Val, class arys_t>
concurrent_hash_counter<Key,Val,arys_t>::concurrent_hash_counter(uint32_t _key_len, unsigned long _size, unsigned int _max_reprobe, unsigned int nb_threads) :
  key_len(_key_len), max_reprobe(_max_reprobe), resize_barrier(nb_threads) { //CLKBAR: added call
  key_val_arys = new arys_t(key_len, _size, (arys_t *)NULL);
  key_val_arys->ref_inc();      // Head of arys
  pthread_mutex_init(&resize_lock, NULL);
  //CLKBAR: pthread_barrier_init(&resize_barrier, NULL, nb_threads);
  memset(&stats, 0, sizeof(stats));
}

template<class Key, class Val, class arys_t>
concurrent_hash_counter<Key,Val,arys_t>::~concurrent_hash_counter() {
  pthread_mutex_destroy(&resize_lock);
  delete key_val_arys;
}

template<class Key, class Val, class arys_t>
void concurrent_hash_counter<Key,Val,arys_t>::print_debug(ostream &out) {
//   unsigned long i;
//   arys_t *carys = key_val_arys;
//   Key key;

//   for(i = 0; i < carys->size; i++) {
//     key = carys->keys[i];
//     if(key & 0x1)
//       out << i << " " << (key >> 2) << " " << (key & 0x3) << " " << 
//         (carys->vals[i] >> 1) << endl;
//     else
//       out << i << " Empty" << endl;
//   }
}

template<class Key, class Val, class arys_t>
void concurrent_hash_counter<Key,Val,arys_t>::print(ostream &out) {
  unsigned long i;
  arys_t *carys = key_val_arys;
  Key *ckey, key;
  Val *cval;

  for(i = 0; i < carys->size; i++) {
    carys->get(i, &ckey, &cval);
    key = *ckey;
    if(key & 0x1)
      out << (key >> 2) << " " << (*cval >> 1) << endl;
  }
}

template<class Key, class Val, class arys_t>
void concurrent_hash_counter<Key,Val,arys_t>::write_keys_vals(ostream &out) {
  arys_t *carys = key_val_arys;

  carys->write(out);
}

template<class Key, class Val, class arys_t>
void concurrent_hash_counter<Key,Val,arys_t>::print_stats(ostream &out) {
#ifdef STATS
#define PRINT_STAT(var) { out << #var ": " << stats.var << endl; }
#else
#define PRINT_STAT(var) { out << #var ": -" << endl; }
#endif

  PRINT_STAT(key_conflicts);
  PRINT_STAT(val_conflicts);
  PRINT_STAT(destroyed_key);
  PRINT_STAT(destroyed_val);
  PRINT_STAT(maxed_out_val);
  PRINT_STAT(maxed_reprobe);
  PRINT_STAT(resized_arys);
}

/*
 * thread_hash_counter
 */
template <class Key, class Val, class arys_t>
uint64_t  thread_hash_counter<Key,Val,arys_t>::MurmurHash64A(const void * key, int len, unsigned int seed)
{
  const uint64_t m = 0xc6a4a7935bd1e995;
  const int r = 47;

  uint64_t h = seed ^ (len * m);

  const uint64_t * data = (const uint64_t *)key;
  const uint64_t * end = data + (len/8);

  while(data != end)
    {
      uint64_t k = *data++;

      k *= m; 
      k ^= k >> r; 
      k *= m; 
		
      h ^= k;
      h *= m; 
    }

  const unsigned char * data2 = (const unsigned char*)data;

  switch(len & 7)
    {
    case 7: h ^= uint64_t(data2[6]) << 48;
    case 6: h ^= uint64_t(data2[5]) << 40;
    case 5: h ^= uint64_t(data2[4]) << 32;
    case 4: h ^= uint64_t(data2[3]) << 24;
    case 3: h ^= uint64_t(data2[2]) << 16;
    case 2: h ^= uint64_t(data2[1]) << 8;
    case 1: h ^= uint64_t(data2[0]);
      h *= m;
    };
 
  h ^= h >> r;
  h *= m;
  h ^= h >> r;

  return h;
} 

template <class Key, class Val, class arys_t>
void thread_hash_counter<Key,Val,arys_t>::add(Key k, Val v) {
  unsigned long hash, idx;
  arys_t *carys = key_val_arys;
  bool done = false;
  unsigned int reprobe = 0;
  unsigned int emax_reprobe = max_reprobe;

  hash = MurmurHash64A(&k, sizeof(Key), 0x818c4070);
  //  rhash = MurmurHash64A(&k, sizeof(Key), 0xb756dc39);

  while(!done) {
    carys = *key_val_arys_ptr;
    if(carys != key_val_arys) {
      // If arys has changed (resized), help with copying over and
      // release old one
      arys_t *oarys = key_val_arys;
      carys->ref_inc();
      key_val_arys = carys;
      copy_over(oarys);
      release(oarys);
      reprobe = 0;
      continue;
    }

    idx = hash & carys->mod_mask;
      
    while(1) {
      if(carys->add(idx, k, v)) {
        done = true;
        break;
      }
      
      if(++reprobe > emax_reprobe) { // Do we need to resize?
        if(resize(carys, false))
          break; // Successfull resize
        
        if(emax_reprobe > max_reprobe) {
          resize(carys, true); // Already increased emax_reprobe -> block
          break;
        }
        emax_reprobe = 4 * max_reprobe;
      }

      idx = (idx + reprobe) & carys->mod_mask; // reprobe
    }
  }
}

template<class Key, class Val, class arys_t>
bool thread_hash_counter<Key,Val,arys_t>::resize(arys_t *carys, bool block) {
  arys_t *narys;

  if(block) {
    pthread_mutex_lock(resize_lock);
  } else if(pthread_mutex_trylock(resize_lock)) {
    return false;
  }
  if(*key_val_arys_ptr != carys) { // Another thread already resized
    pthread_mutex_unlock(resize_lock);
    return true;
  }
  printf("Resize %lx %lx %e\n", carys->size, carys->nb_elt,
         (double)carys->nb_elt / (double)carys->size);
  narys = new arys_t(key_len, carys->size << 1, carys);
  narys->ref_inc();     // New head
  carys->ref_dec();     // Not the head anymore
  *key_val_arys_ptr = narys;  
  pthread_mutex_unlock(resize_lock);

  STAT_INC(resized_arys);
  return true;
}

template<class Key, class Val, class arys_t>
void thread_hash_counter<Key,Val,arys_t>::copy_over(arys_t *carys)
{
  unsigned long start, end, i;
  Key ckey;
  Val cval;

  //CLKBAR: pthread_barrier_wait(resize_barrier);
  resize_barrier->wait();
  while(carys->get_chunck(&start, &end)) {
    // Copy over and free old data
    for(i = start; i < end; i++) {
      if(carys->get(i, &ckey, &cval))
        add(ckey, cval);
    }
  }
}

#endif // __PACKED_CONCURRENT_HASH_COUNTERS__
