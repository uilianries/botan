/*
* (C) 2019,2021 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/internal/thread_pool.h>

#include <botan/exceptn.h>
#include <botan/internal/os_utils.h>
#include <thread>

namespace Botan {

namespace {

std::optional<size_t> global_thread_pool_size() {
   std::string var;
   if(OS::read_env_variable(var, "BOTAN_THREAD_POOL_SIZE")) {
      try {
         return std::optional<size_t>(std::stoul(var, nullptr));
      } catch(std::exception&) { /* ignore it */
      }

      if(var == "none") {
         return std::nullopt;
      }
   }

   // If it was neither a number nor a special value, then ignore it.
   return std::optional<size_t>(0);
}

}  // namespace

//static
Thread_Pool& Thread_Pool::global_instance() {
   static Thread_Pool g_thread_pool(global_thread_pool_size());
   return g_thread_pool;
}

Thread_Pool::Thread_Pool(std::optional<size_t> opt_pool_size) {
   m_shutdown = false;

   if(!opt_pool_size.has_value()) {
      return;
   }

   size_t pool_size = opt_pool_size.value();

   if(pool_size == 0) {
      pool_size = OS::get_cpu_available();

      // Unclear if this can happen, but be defensive
      if(pool_size == 0) {
         pool_size = 2;
      }

      /*
      * For large machines don't create too many threads, unless
      * explicitly asked to by the caller.
      */
      if(pool_size > 16) {
         pool_size = 16;
      }
   }

   for(size_t i = 0; i != pool_size; ++i) {
      m_workers.push_back(std::thread(&Thread_Pool::worker_thread, this));
   }
}

void Thread_Pool::shutdown() {
   {
      std::unique_lock<std::mutex> lock(m_mutex);

      if(m_shutdown == true) {
         return;
      }

      m_shutdown = true;

      m_more_tasks.notify_all();
   }

   for(auto&& thread : m_workers) {
      thread.join();
   }
   m_workers.clear();
}

void Thread_Pool::queue_thunk(const std::function<void()>& fn) {
   std::unique_lock<std::mutex> lock(m_mutex);

   if(m_shutdown) {
      throw Invalid_State("Cannot add work after thread pool has shut down");
   }

   if(m_workers.empty()) {
      return fn();
   }

   m_tasks.push_back(fn);
   m_more_tasks.notify_one();
}

void Thread_Pool::worker_thread() {
   for(;;) {
      std::function<void()> task;

      {
         std::unique_lock<std::mutex> lock(m_mutex);
         m_more_tasks.wait(lock, [this] { return m_shutdown || !m_tasks.empty(); });

         if(m_tasks.empty()) {
            if(m_shutdown) {
               return;
            } else {
               continue;
            }
         }

         task = m_tasks.front();
         m_tasks.pop_front();
      }

      task();
   }
}

}  // namespace Botan
