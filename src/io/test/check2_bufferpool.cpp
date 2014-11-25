/**
 * @file		check_bufferpool.cpp
 * @ingroup
 * @author	tpan
 * @brief
 * @details
 *
 * Copyright (c) 2014 Georgia Institute of Technology.  All Rights Reserved.
 *
 * TODO add License
 */

#include <unistd.h>  // for usleep

#include "omp.h"
#include <cassert>
#include <chrono>
#include <vector>
#include <cstdlib>   // for rand
#include <atomic>
#include <memory>

#include "utils/test_utils.hpp"

#include "io/locking_buffer.hpp"
#include "io/locking_object_pool.hpp"



template<typename PoolType>
void testAppendMultipleBuffers(const int NumThreads, const int total_count, bliss::concurrent::LockType poollt, bliss::concurrent::LockType bufferlt, const int64_t buffer_cap) {
  omp_lock_t writelock;
  omp_init_lock(&writelock);
  omp_lock_t writelock2;
  omp_init_lock(&writelock2);
  omp_lock_t writelock3;
  omp_init_lock(&writelock3);
//  std::atomic_flag writelock = ATOMIC_FLAG_INIT;
//  std::atomic_flag writelock2 = ATOMIC_FLAG_INIT;
//  std::atomic_flag writelock3 = ATOMIC_FLAG_INIT;


  printf("TESTING: %d threads, pool lock %d buffer lock %d append with %ld bufferSize and %d total counts from unlimited pool\n",
         NumThreads, poollt, bufferlt, buffer_cap, total_count);


  PoolType pool;

  std::vector<int> gold;
  std::vector<int> stored;

  int data = 0;
  unsigned int result = 0;

  int success = 0;
  int failure = 0;
  int swap = 0;
  int i = 0;

  auto buf_ptr = pool.tryAcquireObject();
  buf_ptr->clear_and_unblock_writes();

#pragma omp parallel for num_threads(NumThreads) default(none) shared(buf_ptr, gold, stored, writelock, writelock2, writelock3, pool) private(i, data, result) reduction(+:success, failure, swap)
  for (i = 0; i < total_count; ++i) {

//    while (writelock2.test_and_set());
    omp_set_lock(&writelock2);
    auto sptr = buf_ptr;
    auto ptr = sptr;
    omp_unset_lock(&writelock2);
//    writelock2.clear();

    if (sptr) {  // valid ptr

      data = static_cast<int>(i);
      result = ptr->append(&data, sizeof(int));
    } else {  // expired ptr
      result = 0x0;
    }

    if (result & 0x1) {
      ++success;
//      while (writelock.test_and_set());
      omp_set_lock(&writelock);
      gold.push_back(data);
      omp_unset_lock(&writelock);
//      writelock.clear();
    }
    else ++failure;

    if (result & 0x2) {
      ++swap;

      // swap in a new one.
      auto new_buf_ptr = pool.tryAcquireObject();
      new_buf_ptr->clear_and_unblock_writes();

//      while (writelock2.test_and_set());
      omp_set_lock(&writelock2);
      auto tmp = buf_ptr;
      buf_ptr = new_buf_ptr;
      new_buf_ptr = tmp;
#pragma omp flush(buf_ptr)
      omp_unset_lock(&writelock2);
//      writelock2.clear();

      // process the old buffer
//      while (writelock3.test_and_set());
      omp_set_lock(&writelock3);
      sptr = new_buf_ptr;
      stored.insert(stored.end(), sptr->operator int*(), sptr->operator int*() + sptr->getSize() / sizeof(int));
      omp_unset_lock(&writelock3);
//      writelock3.clear();

      // and release - few threads doing this, and full.

      pool.releaseObject(new_buf_ptr);

    }

  }
  int64_t buffer_capacity = buf_ptr->getCapacity();

  auto sptr = buf_ptr;
  sptr->block_and_flush();

  // compare unordered buffer content.
  stored.insert(stored.end(), sptr->operator int*(), sptr->operator int*() + sptr->getSize() / sizeof(int));
  pool.releaseObject(buf_ptr);
  int stored_count = stored.size();


  if ( swap != success / (buffer_capacity / sizeof(int)) || success != stored_count)
      printf("FAIL: (actual/expected)  success (%d/%d), failure (%d/?), swap(%ld/%d).\n", success, stored_count, failure, success / (buffer_capacity / sizeof(int)), swap);
  else {
    printf("INFO: success %d, failure %d, swap %d, total %d\n", success, failure, swap, total_count);

    if (compareUnorderedSequences(stored.begin(), gold.begin(), stored_count)) {
      printf("PASS\n");
    } else {
      printf("FAIL: content not matching\n");
    }
  }
  omp_destroy_lock(&writelock);
  omp_destroy_lock(&writelock2);
  omp_destroy_lock(&writelock3);
}


template<typename PoolType>
void testPool(PoolType && pool, bliss::concurrent::LockType poollt, bliss::concurrent::LockType bufferlt, int pool_threads, int buffer_threads) {


  printf("TESTING pool lock %d buffer lock %d %s: pool threads %d, buffer threads %d\n", poollt, bufferlt, (pool.isUnlimited() ? "GROW" : "FIXED"),  pool_threads, buffer_threads);

  printf("TEST acquire\n");
  int expected;
  int i = 0;
  int count = 0;
  int mx = pool.isUnlimited() ? 100 : pool.getCapacity();
#pragma omp parallel for num_threads(pool_threads) default(none) private(i) shared(pool, mx) reduction(+ : count)
  for (i = 0; i < mx; ++i) {
	  auto ptr = pool.tryAcquireObject();
    if (! ptr) {
      ++count;
    }
  }
  expected = 0;
  if (count != expected) printf("FAIL: number of failed attempt to acquire buffer should be %d, actual %d.  pool capacity %lu, remaining: %lu \n", expected, count, pool.getCapacity(), pool.getAvailableCount());
  else printf("PASSED.\n");
  pool.reset();

  printf("TEST acquire with growth\n");
  i = 0;
  count = 0;
  mx = pool.isUnlimited() ? 100 : pool.getCapacity();
#pragma omp parallel for num_threads(pool_threads) default(none) private(i) shared(pool, mx) reduction(+ : count)
  for (i = 0; i <= mx; ++i) {  // <= so we get 1 extra
		auto ptr = pool.tryAcquireObject();
	    if (! ptr) {
	      ++count;
	    }
  }
  expected = pool.isUnlimited() ? 0 : 1;
  if (count != expected) printf("FAIL: number of failed attempt to acquire buffer should be %d, actual %d.  pool remaining: %lu \n", expected, count, pool.getAvailableCount());
  else printf("PASSED.\n");


  pool.reset();

  printf("TEST release\n");
  count = 0;
  mx = pool.isUnlimited() ? 100 : pool.getCapacity();
  // and create some dummy buffers to insert
  std::vector<typename PoolType::ObjectPtrType> temp;
  // first drain the pool
  for (i = 0; i < mx; ++i) {
    typename PoolType::ObjectPtrType ptr = pool.tryAcquireObject();
    temp.push_back(ptr);
    temp.push_back(ptr);
  }
#pragma omp parallel for num_threads(pool_threads) default(none) shared(pool, mx, temp) private(i) reduction(+ : count)
  for (i = 0; i < mx * 2; ++i) {

    typename PoolType::ObjectPtrType ptr = temp[i];
    if (ptr) {
      ptr->block_and_flush();
      if (! pool.releaseObject(ptr)) {
        ++count; // failed release
      }
    }
  }
  expected = mx;  // unlimited or not, can only push back in as much as taken out.
  if (count != expected) printf("FAIL: number of failed attempt to release buffer should be %d, actual %d. pool remaining: %lu \n", expected, count, pool.getAvailableCount());
  else printf("PASSED.\n");
  pool.reset();
  temp.clear();

  printf("TEST access by multiple threads, each a separate buffer.\n");


  count = 0;
  int count1 = 0;
  int count2 = 0;
#pragma omp parallel num_threads(pool_threads) default(none) shared(pool, std::cout) reduction(+ : count, count1, count2)
  {
    int v = omp_get_thread_num() + 5;
    auto ptr = pool.tryAcquireObject();

    if (! ptr) ++count2;
    else {
      ptr->clear_and_unblock_writes();

      int res = ptr->append(&v, sizeof(int));


    if (! (res & 0x1)) {
      ++count;
    }

    int u = ptr->operator int*()[0];
    if (v != u) {
      ++count1;
    }

    ptr->block_and_flush();
    pool.releaseObject(ptr);
    }
  }
  if (count2 != 0) printf("FAIL: acquire failed\n");
  else if (count != 0) printf("FAIL: append failed\n");
  else if (count1 != 0) printf("FAIL: inserted and got back wrong values\n");
  else printf("PASSED.\n");
  pool.reset();

  printf("TEST access by multiple threads, all to same buffer.\n");


  auto ptr = pool.tryAcquireObject();
  ptr->clear_and_unblock_writes();

#pragma omp parallel num_threads(buffer_threads) default(none) shared(pool, ptr)
  {
    int v = 7;
    ptr->append(&v, sizeof(int));
  }

  bool same = true;
  for (int i = 0; i < buffer_threads ; ++i) {
    same &= ptr->operator int*()[i] == 7;
  }
  if (!same) printf("FAIL: inserted not same\n");
  else printf("PASSED.\n");

  pool.reset();


  omp_set_nested(1);

  printf("TEST all operations together\n");
#pragma omp parallel num_threads(pool_threads) default(none) shared(pool, pool_threads, buffer_threads, std::cout)
  {
    // Id range is 0 to 100
    int iter;
    int j = 0;
    for (int i = 0; i < 100; ++i) {
      // acquire
      auto buf = pool.tryAcquireObject();
//      printf("acquiring ");
      while (!buf) {
        _mm_pause();
//        printf(".");
        buf = pool.tryAcquireObject();
      }
//      printf(" done.\n");
      buf->clear_and_unblock_writes();

      // access
      iter = rand() % 100;
      int count = 0;
#pragma omp parallel for num_threads(buffer_threads) default(none) shared(buf, iter) private(j) reduction(+:count)
      for (j = 0; j < iter; ++j) {
        bool res = buf->append(&j, sizeof(int));
        if (! (res & 0x1)) {
          count++;
        }
      }

      // random sleep
      for (int i = 0; i < rand() % 1000; ++i) {
        _mm_pause();
      }
      // clear buffer
//      std::cout << "before block and flush: " << *buf << std::endl << std::flush;
      buf->block_and_flush();
//      std::cout << "after block and flush: " << *buf << std::endl << std::flush;

//      printf("count = %d\n", count);

      if (buf->getSize() != sizeof(int) * iter  || count != 0)
        printf("FAIL: thread %d/%d buffer size is %ld, expected %lu\n", omp_get_thread_num() + 1, pool_threads, buf->getSize(), sizeof(int) * iter);
  else printf("PASSED.\n");

      //release
      pool.releaseObject(buf);
      //if (i % 25 == 0)
//      printf("thread %d released buffer %d\n", omp_get_thread_num(), id);

    }
  }




};


int main(int argc, char** argv) {

  // construct, acquire, access, release
#ifdef BLISS_MUTEX
  constexpr bliss::concurrent::LockType lt = bliss::concurrent::LockType::MUTEX;
#endif
#ifdef BLISS_SPINLOCK
  constexpr bliss::concurrent::LockType lt = bliss::concurrent::LockType::SPINLOCK;
#endif

  //////////////  unbounded version

  /// thread unsafe.  test in single thread way.


  testPool(std::move(bliss::io::ObjectPool< bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::NONE, 8192> >()), bliss::concurrent::LockType::NONE,bliss::concurrent::LockType::NONE, 1, 1);
  testPool(std::move(bliss::io::ObjectPool< bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::NONE, 8192> >(16)), bliss::concurrent::LockType::NONE,bliss::concurrent::LockType::NONE, 1, 1);

  for (int i = 1; i <= 8; ++i) {
	if (i == 5 || i == 6 || i == 7) continue;

    // okay to test.  in real life, pools would not be single threaded.
    testPool(std::move(bliss::io::ObjectPool<bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::MUTEX, 8192> >()),    bliss::concurrent::LockType::NONE, bliss::concurrent::LockType::MUTEX, 1, i);
    testPool(std::move(bliss::io::ObjectPool<bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::SPINLOCK, 8192> >()), bliss::concurrent::LockType::NONE, bliss::concurrent::LockType::SPINLOCK, 1, i);
    testPool(std::move(bliss::io::ObjectPool<bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::LOCKFREE, 8192> >()), bliss::concurrent::LockType::NONE, bliss::concurrent::LockType::LOCKFREE, 1, i);

    // okay to test.  in real life, pools would not be single threaded.
    testPool(std::move(bliss::io::ObjectPool<bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::MUTEX, 8192> >(16)),    bliss::concurrent::LockType::NONE, bliss::concurrent::LockType::MUTEX, 1, i);
    testPool(std::move(bliss::io::ObjectPool<bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::SPINLOCK, 8192> >(16)), bliss::concurrent::LockType::NONE, bliss::concurrent::LockType::SPINLOCK, 1, i);
    testPool(std::move(bliss::io::ObjectPool<bliss::concurrent::LockType::NONE, bliss::io::Buffer<bliss::concurrent::LockType::LOCKFREE, 8192> >(16)), bliss::concurrent::LockType::NONE, bliss::concurrent::LockType::LOCKFREE, 1, i);


    for (int j = 1; j <= 4; ++j) {
	if (i * j > 16) continue;
      testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::MUTEX, 8192> >()),    lt, bliss::concurrent::LockType::MUTEX, j, i);
      testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::SPINLOCK, 8192> >()), lt, bliss::concurrent::LockType::SPINLOCK, j, i);
      testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::LOCKFREE, 8192> >()), lt, bliss::concurrent::LockType::LOCKFREE, j, i);

      testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::MUTEX, 8192> >(16)),    lt, bliss::concurrent::LockType::MUTEX, j, i);
      testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::SPINLOCK, 8192> >(16)), lt, bliss::concurrent::LockType::SPINLOCK, j, i);
      testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::LOCKFREE, 8192> >(16)), lt, bliss::concurrent::LockType::LOCKFREE, j, i);

    }

    testAppendMultipleBuffers<bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::LOCKFREE, 8192> > >(i, 1000000, lt, bliss::concurrent::LockType::LOCKFREE, 8192);


    // no multithread pool single thread buffer test right now.
    //testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::NONE, 8192> >()), lt, bliss::concurrent::LockType::NONE, i, 1);
    //testPool(std::move(bliss::io::ObjectPool<lt, bliss::io::Buffer<bliss::concurrent::LockType::NONE, 8192> >(16)), lt, bliss::concurrent::LockType::NONE, i, 1);
  }


}