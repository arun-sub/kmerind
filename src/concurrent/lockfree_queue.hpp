/**
 * @file		lockfree_queue.hpp
 * @ingroup bliss::concurrent
 * @author	tpan
 * @brief   Thread Safe Queue Implementation
 * @details this header file contains the templated implementation of a thread safe queue.  this class is used for MPI buffer management.
 *
 *    // can't use boost's lockfree queue:  expect T to have copy constuctor, tribial assignment operator, and trivial destuctor.
 *    // using moodycamel's concurrent queue
 *
 * Copyright (c) 2014 Georgia Institute of Technology.  All Rights Reserved.
 *
 * TODO add License
 */
#ifndef THREADSAFE_QUEUE_HPP_
#define THREADSAFE_QUEUE_HPP_

#include <thread>
#include <mutex>
#include <limits>
#include <atomic>
#include <stdexcept>
#include <xmmintrin.h>
#include <concurrentqueue/concurrentqueue.h>



namespace bliss
{
  namespace concurrent
  {
    /**
     * @class     bliss::concurrent::ThreadSafeQueue
     * @brief     a multi-producer, multi-consumer thread safe queue with an optional capacity
     * @details   adapted from http://www.justsoftwaresolutions.co.uk/threading/implementing-a-thread-safe-queue-using-condition-variables.html
     *            with SIGNIFICANT modifications
     *            1. use c++11 std::thread and atomic constructs
     *            2. incorporating move semantics.
     *            3. support a capacity limit.
     *            4. supports multiple producer, multiple consumer.
     *            5. allows blocking of the enqueue function, (useful when draining queue or when finishing the use of the queue).
     *
     *          note that this is NOT truly concurrent.  the class serializes parallel access.  move semantic minimizes the copy operations needed.
     *
     *          DUE TO LOCKS, this is NOT fast.  but may be fast enough for MPI buffer management.
     */
    template <typename T>
    class ThreadSafeQueue
    {
      protected:

        /// mutex for locking access to the queue
        mutable std::mutex mutex;

        /// underlying queue that is not thread safe.
        moodycamel::ConcurrentQueue<T> q;

        /// capacity of the queue.  if set to std::numeric_limits<size_t>::max() indicates unlimited size queue
        mutable int64_t capacity;

        /// size encodes 2 things:  sign bit encodes whether a calling thread can push into this queue.  use when suspending or terminating a queue.  rest is size of current queue.
        std::atomic<int64_t> size;

      private:
        /**
         * private move constructor that requires a lock during the call, so that the source of the move is locked.   content of other is moved back.
         * @param other   the soruce ThreadSafeQueue object from which data will be moved.
         * @param l       a lock that uses the mutex of the source ThreadSafeQueue.
         */
        ThreadSafeQueue(ThreadSafeQueue<T>&& other, const std::lock_guard<std::mutex>& l) :
          q(std::move(other.q)), capacity(other.capacity) {
          other.capacity = 0;
          size.exchange(other.size.exchange(std::numeric_limits<int64_t>::lowest()));
        };


        /**
         * copy constructor, DISABLED
         * @param other   the source ThreadSafeQueue from which to copy.
         */
        explicit ThreadSafeQueue(const ThreadSafeQueue<T>& other) = delete;

        /**
         * copy assignment operator
         * @param other   the source ThreadSafeQueue from which to copy.
         * @return
         */
        ThreadSafeQueue<T>& operator=(const ThreadSafeQueue<T>& other) = delete;

      public:
        /// maximum possible size of a thread safe queue.  initialized to maximum size_t value.
        static constexpr int64_t MAX_SIZE  = std::numeric_limits<int64_t>::max();

        /**
         * normal constructor allowing the caller to specify an optional capacity parameter.
         * @param _capacity   The maximum capacity for the thread safe queue.
         */
        explicit ThreadSafeQueue(const int64_t &_capacity = MAX_SIZE) :
              q( _capacity == MAX_SIZE ? 128 : _capacity), capacity(_capacity), size(0)
        {
          assert(_capacity <= MAX_SIZE);

          if (capacity == 0)
            throw std::invalid_argument("ThreadSafeQueue constructor parameter capacity is given as 0");
        };

        /**
         * move constructor.  mutex locks the src ThreadSafeQueue first before delegating to the private constructor.
         * @param other   the source ThreadSafeQueue from which to move.
         */
        explicit ThreadSafeQueue(ThreadSafeQueue<T>&& other) :
            ThreadSafeQueue<T>(std::move(other), std::lock_guard<std::mutex>(other.mutex)) {};

        /**
         * move assignment operator.  locks both the src and destination ThreadSafeQueue before performing the move.
         * @param other   the source ThreadSafeQueue from which to move.
         * @return
         */
        ThreadSafeQueue<T>& operator=(ThreadSafeQueue<T>&& other) {
          std::unique_lock<std::mutex> mylock(mutex, std::defer_lock), otherlock(other.mutex, std::defer_lock);
          std::lock(mylock, otherlock);
          q = std::move(other.q);
          capacity = other.capacity; other.capacity = 0;
          size.exchange(other.size.exchange(std::numeric_limits<int64_t>::lowest()));
          return *this;
        }

        /**
         * get the capacity of the thread safe queue
         * @return    capacity of the queue
         */
        inline const int64_t& getCapacity() const {
          return capacity;
        }

        /**
         * check if the thread safe queue is full.
         * @return    boolean - whether the queue is full.
         */
        inline bool isFull() const {
          return (capacity < MAX_SIZE) && (getSize() >= capacity);
        }

        /**
         * check if the thread safe queue is empty
         * @return    boolean - whether the queue is empty.
         */
        inline bool isEmpty() const
        {
          return getSize() == 0;
        }

        /**
         * get the current size of the queue
         * @return    the current size of the queue
         */
        inline size_t getSize() const
        {
          return size.load() & std::numeric_limits<int64_t>::max();
        }

        /**
         * clears the queue of all contents (discarding).
         */
        void clear()
        {
          std::lock_guard<std::mutex> lock(mutex);
          T val;
          while (q.try_dequeue(val)) ;

          size.fetch_and(std::numeric_limits<int64_t>::lowest());  // keep the push bit, and set size to 0

        }

        /**
         * set the queue to accept new elements
         */
        void enablePush() {
          size.fetch_and(std::numeric_limits<int64_t>::max());  // clear the push bit, and leave size as is
        }

        /**
         * set the queue to disallow insertion of new elements.
         */
        void disablePush() {
          size.fetch_or(std::numeric_limits<int64_t>::lowest());   // set the push bit, and leave size as is.
        }

        /**
         * check if the thread safe queue is accepting new elements. (full or not)
         * @return    boolean - queue insertion allowed or not.
         */
        bool canPush() {
          return size.load(std::memory_order_seq_cst) >= 0;   // int highest bit set means negative, and means cannot push
        }

        /**
         * check if the thread safe queue can produce an element now or in near future.  ThreadSafeQueue can pop only if it has elements in the base queue. or
         * if additional items can be pushed in.
         * @return    boolean - queue pop is allowed or not.
         */
        bool canPop() {
          // canPush == first bit is 0, OR has some elements (not 0 for remaining bits).  so basically, not 1000000000...
          return size.load() != std::numeric_limits<int64_t>::lowest();
          //return canPush() || !isEmpty();
        }

      protected:
        inline bool canPushAndHasRoom() {
          // canPush() && !full() :  positive number less than capacity.
          //return size.load(std::memory_order_seq_cst) >= 0 &&
          //    ((capacity >= MAX_SIZE) || ((size.load() & std::numeric_limits<int64_t>::max()) < capacity));

          // if we reinterpret this number as a uint64_t, then we only need to check less than capacity, since now highest bits are all way higher.
          int64_t v = size.load();
          return reinterpret_cast<uint64_t&>(v) < static_cast<uint64_t>(capacity);
        }

      public:

        /**
         * Non-blocking - pushes an element by constant reference (copy).
         * Returns true only if push was successful.
         * If queue is full, or if queue is not accepting new element inserts, return false.
         *    Does not modify the element if cannot push onto queue.
         *
         * @param data    data element to be pushed onto the thread safe queue
         * @return        whether push was successful.
         */
        bool tryPush (T const& data) {

          int64_t v = size.load();
          bool res = false;
          if (reinterpret_cast<uint64_t&>(v) < static_cast<uint64_t>(capacity)) {
            if ((res = q.enqueue(data)) == true) size++;
          }

          return res;
        }

        /**
         * Non-blocking - pushes an element by constant reference (move).
         * Returns true only if push was successful.
         * If queue is full, or if queue is not accepting new element inserts, return false.
         *    Does not modify the element if cannot push onto queue.
         *
         * @details  if move fails, concurrentqueue does NOT touch data (also uses std::forward, not move),
         *          so can return the results.
         * @note     requires move constuctor CLEAR OUT OLD, since that is semantically correct.  else destuction
         *            of old object could invalidate the moved to object.
         *
         * @param data    data element to be pushed onto the thread safe queue
         * @return        whether push was successful.
         */
        std::pair<bool,T> tryPush (T && data) {
          int64_t v = size.load();
          bool res = false;
          if (reinterpret_cast<uint64_t&>(v) < static_cast<uint64_t>(capacity)) {
            if ((res = q.enqueue(std::forward<T>(data))) == true) size++;
          }

          // at this point, if success, data should be empty.  else data should be untouched.
          return std::move(std::make_pair(res, std::forward<T>(data)));;
        }

        /**
         * Semi-blocking - pushes an element by constant reference (copy).
         * Returns true only if push was successful.
         * If queue is full, wait until space becomes available.
         * If queue is not accepting new element inserts, return false.
         *    Does not modify the element if cannot push onto queue.
         *
         * this function signature is useful where the application would like to have guaranteed insertion without busy-waiting,
         * yet needs a way to lift the block when the queue has been marked by another thread as terminating (not accepting new elements)
         *
         * @param data    data element to be pushed onto the thread safe queue
         * @return        whether push was successful.
         */
        bool waitAndPush (T const& data) {

          // wait while size is greater than capacity.
          volatile int64_t v = 0;
          if ((v = size.load()) >= capacity) {
            _mm_pause();              // full q.  wait for someone to signal (not full && canPush, or !canPush).

          }
          // to get here, have to have one of these conditions changed:  pushEnabled, !full

          // after the loop, if size is less than 0, then push disabled, return false.  else push and increment.
          bool res = false;
          if (v >= 0 ) {
            if ((res = q.enqueue(data))  == true) size++;
          } // else push disabled. return false.

          return res;

        }

        /**
         * Semi-blocking - pushes an element by constant reference (move).
         * Returns true only if push was successful.
         * If queue is full, wait until space becomes available.
         * If queue is not accepting new element inserts, return false.
         *    Does not modify the element if cannot push onto queue.
         *
         * this function signature is useful where the application would like to have guaranteed insertion without busy-waiting,
         * yet needs a way to lift the block when the queue has been marked by another thread as terminating (not accepting new elements)
         *
         * @param data    data element to be pushed onto the thread safe queue
         * @return        whether push was successful.
         */
        std::pair<bool,T> waitAndPush (T && data) {
          // wait while size is greater than capacity.
          volatile int64_t v = 0;
          if ((v = size.load()) >= capacity) {
            _mm_pause();              // full q.  wait for someone to signal (not full && canPush, or !canPush).

          }
          // to get here, have to have one of these conditions changed:  pushEnabled, !full

          // after the loop, if size is less than 0, then push disabled, return false.  else push and increment.
          bool res = false;
          if (v >= 0 ) {  // == canPush
            if ((res = q.enqueue(std::forward<T>(data)))  == true) size++;
          } // else push disabled. return false.

          // at this point, if success, data should be empty.  else data should be untouched.
          return std::move(std::make_pair(res, std::forward<T>(data)));

        }



        /**
         * Non-blocking - remove the first element in the queue and return it to the calling thread.
         * returns a std::pair with first element indicating the success/failure of the Pop operation,
         * and second is the popped queue element, if pop were successful.
         *
         * Function fails when the queue is empty, returning false.
         * Function returns success and retrieved element, regardless of whether the thread safe queue is accepting new inserts.
         *
         * @details uses move assignment operator internally.  requires Move assignment operator to clear the source, else destructor could invalidate current one.
         *
         * @return    std::pair with boolean (successful pop?) and an element from the queue (if successful)
         */
        std::pair<bool, T> tryPop() {
          std::pair<bool, T> output;
          output.first = false;

          if ((output.first = q.try_dequeue(output.second)) == true) {
            size--;
          }

          return output;
        }

        /**
         * Semi-blocking - remove the first element in the queue and return it to the calling thread.
         * Returns a std::pair with first element indicating the success/failure of the Pop operation,
         *  and second is the popped queue element, if pop were successful.
         *
         * Function will wait for some element to be available for pop from the queue.
         * Function will return when it has retrieved some data from queue, or when if it is notified that the thread safe queue has terminated.
         * Returns false if queue is terminated (no new elements) and flushed.
         *
         * This function signature is useful where the application would like to have guaranteed data retrieval from the queue without busy-waiting,
         * yet needs a way to lift the block when the queue has been marked by another thread as terminated and flushed
         *   (no more new elements to be inserted and queue is empty.)
         *
         * @return    std::pair with boolean (successful pop?) and an element from the queue (if successful)
         */
        std::pair<bool, T> waitAndPop() {
          std::pair<bool, T> output;
          output.first = false;

          // loop while pop fails, and canPop()
          if (canPop() && ((output.first = q.try_dequeue(output.second)) == false)) {
            _mm_pause();
          }

          // get here if !canPop, OR dequeue succeeds
          if (output.first) --size;   // successful dequeue, so decrement size.

          return output;
        }




    };

    /**
     * static templated MAX_SIZE definition.
     */
    template<typename T> constexpr size_t ThreadSafeQueue<T>::MAX_SIZE;



  } /* namespace concurrent */
} /* namespace bliss */

#endif /* THREADSAFE_QUEUE_HPP_ */
