// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_H_

// DESCRIPTION
// PartitionRoot::Alloc() and PartitionRoot::Free() are approximately analagous
// to malloc() and free().
//
// The main difference is that a PartitionRoot object must be supplied to these
// functions, representing a specific "heap partition" that will be used to
// satisfy the allocation. Different partitions are guaranteed to exist in
// separate address spaces, including being separate from the main system
// heap. If the contained objects are all freed, physical memory is returned to
// the system but the address space remains reserved.  See PartitionAlloc.md for
// other security properties PartitionAlloc provides.
//
// THE ONLY LEGITIMATE WAY TO OBTAIN A PartitionRoot IS THROUGH THE
// PartitionAllocator classes. To minimize the instruction count to the fullest
// extent possible, the PartitionRoot is really just a header adjacent to other
// data areas provided by the allocator class.
//
// The constraints for PartitionRoot::Alloc() are:
// - Multi-threaded use against a single partition is ok; locking is handled.
// - Allocations of any arbitrary size can be handled (subject to a limit of
//   INT_MAX bytes for security reasons).
// - Bucketing is by approximate size, for example an allocation of 4000 bytes
//   might be placed into a 4096-byte bucket. Bucket sizes are chosen to try and
//   keep worst-case waste to ~10%.
//
// The allocators are designed to be extremely fast, thanks to the following
// properties and design:
// - Just two single (reasonably predicatable) branches in the hot / fast path
//   for both allocating and (significantly) freeing.
// - A minimal number of operations in the hot / fast path, with the slow paths
//   in separate functions, leading to the possibility of inlining.
// - Each partition page (which is usually multiple physical pages) has a
//   metadata structure which allows fast mapping of free() address to an
//   underlying bucket.
// - Supports a lock-free API for fast performance in single-threaded cases.
// - The freelist for a given bucket is split across a number of partition
//   pages, enabling various simple tricks to try and minimize fragmentation.
// - Fine-grained bucket sizes leading to less waste and better packing.
//
// The following security properties could be investigated in the future:
// - Per-object bucketing (instead of per-size) is mostly available at the API,
// but not used yet.
// - No randomness of freelist entries or bucket position.
// - Better checking for wild pointers in free().
// - Better freelist masking function to guarantee fault on 32-bit.

#include <limits.h>
#include <string.h>
#include <memory>

#include <atomic>

#include "base/allocator/buildflags.h"
#include "base/allocator/partition_allocator/checked_ptr_support.h"
#include "base/allocator/partition_allocator/page_allocator.h"
#include "base/allocator/partition_allocator/partition_alloc_check.h"
#include "base/allocator/partition_allocator/partition_alloc_constants.h"
#include "base/allocator/partition_allocator/partition_alloc_features.h"
#include "base/allocator/partition_allocator/partition_alloc_forward.h"
#include "base/allocator/partition_allocator/partition_bucket.h"
#include "base/allocator/partition_allocator/partition_cookie.h"
#include "base/allocator/partition_allocator/partition_lock.h"
#include "base/allocator/partition_allocator/partition_oom.h"
#include "base/allocator/partition_allocator/partition_page.h"
#include "base/allocator/partition_allocator/partition_ref_count.h"
#include "base/allocator/partition_allocator/partition_root.h"
#include "base/allocator/partition_allocator/partition_tag.h"
#include "base/allocator/partition_allocator/pcscan.h"
#include "base/allocator/partition_allocator/thread_cache.h"
#include "base/base_export.h"
#include "base/bits.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/gtest_prod_util.h"
#include "base/notreached.h"
#include "base/optional.h"
#include "base/partition_alloc_buildflags.h"
#include "base/stl_util.h"
#include "base/synchronization/lock.h"
#include "base/sys_byteorder.h"
#include "build/build_config.h"
#include "build/buildflag.h"

#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
#include <stdlib.h>
#endif

#if defined(ADDRESS_SANITIZER)
#include <sanitizer/asan_interface.h>
#endif  // defined(ADDRESS_SANITIZER)

// We use this to make MEMORY_TOOL_REPLACES_ALLOCATOR behave the same for max
// size as other alloc code.
#define CHECK_MAX_SIZE_OR_RETURN_NULLPTR(size, flags) \
  if (size > MaxDirectMapped()) {                     \
    if (flags & PartitionAllocReturnNull) {           \
      return nullptr;                                 \
    }                                                 \
    PA_CHECK(false);                                  \
  }

namespace base {

typedef void (*OomFunction)(size_t);

// PartitionAlloc supports setting hooks to observe allocations/frees as they
// occur as well as 'override' hooks that allow overriding those operations.
class BASE_EXPORT PartitionAllocHooks {
 public:
  // Log allocation and free events.
  typedef void AllocationObserverHook(void* address,
                                      size_t size,
                                      const char* type_name);
  typedef void FreeObserverHook(void* address);

  // If it returns true, the allocation has been overridden with the pointer in
  // *out.
  typedef bool AllocationOverrideHook(void** out,
                                      int flags,
                                      size_t size,
                                      const char* type_name);
  // If it returns true, then the allocation was overridden and has been freed.
  typedef bool FreeOverrideHook(void* address);
  // If it returns true, the underlying allocation is overridden and *out holds
  // the size of the underlying allocation.
  typedef bool ReallocOverrideHook(size_t* out, void* address);

  // To unhook, call Set*Hooks with nullptrs.
  static void SetObserverHooks(AllocationObserverHook* alloc_hook,
                               FreeObserverHook* free_hook);
  static void SetOverrideHooks(AllocationOverrideHook* alloc_hook,
                               FreeOverrideHook* free_hook,
                               ReallocOverrideHook realloc_hook);

  // Helper method to check whether hooks are enabled. This is an optimization
  // so that if a function needs to call observer and override hooks in two
  // different places this value can be cached and only loaded once.
  static bool AreHooksEnabled() {
    return hooks_enabled_.load(std::memory_order_relaxed);
  }

  static void AllocationObserverHookIfEnabled(void* address,
                                              size_t size,
                                              const char* type_name);
  static bool AllocationOverrideHookIfEnabled(void** out,
                                              int flags,
                                              size_t size,
                                              const char* type_name);

  static void FreeObserverHookIfEnabled(void* address);
  static bool FreeOverrideHookIfEnabled(void* address);

  static void ReallocObserverHookIfEnabled(void* old_address,
                                           void* new_address,
                                           size_t size,
                                           const char* type_name);
  static bool ReallocOverrideHookIfEnabled(size_t* out, void* address);

 private:
  // Single bool that is used to indicate whether observer or allocation hooks
  // are set to reduce the numbers of loads required to check whether hooking is
  // enabled.
  static std::atomic<bool> hooks_enabled_;

  // Lock used to synchronize Set*Hooks calls.
  static std::atomic<AllocationObserverHook*> allocation_observer_hook_;
  static std::atomic<FreeObserverHook*> free_observer_hook_;

  static std::atomic<AllocationOverrideHook*> allocation_override_hook_;
  static std::atomic<FreeOverrideHook*> free_override_hook_;
  static std::atomic<ReallocOverrideHook*> realloc_override_hook_;
};

namespace internal {

ALWAYS_INLINE void* PartitionPointerAdjustSubtract(bool allow_extras,
                                                   void* ptr) {
  if (allow_extras) {
    ptr = PartitionTagPointerAdjustSubtract(ptr);
    ptr = PartitionCookiePointerAdjustSubtract(ptr);
    ptr = PartitionRefCountPointerAdjustSubtract(ptr);
  }
  return ptr;
}

ALWAYS_INLINE void* PartitionPointerAdjustAdd(bool allow_extras, void* ptr) {
  if (allow_extras) {
    ptr = PartitionTagPointerAdjustAdd(ptr);
    ptr = PartitionCookiePointerAdjustAdd(ptr);
    ptr = PartitionRefCountPointerAdjustAdd(ptr);
  }
  return ptr;
}

ALWAYS_INLINE size_t PartitionSizeAdjustAdd(bool allow_extras, size_t size) {
  if (allow_extras) {
    size = PartitionTagSizeAdjustAdd(size);
    size = PartitionCookieSizeAdjustAdd(size);
    size = PartitionRefCountSizeAdjustAdd(size);
  }
  return size;
}

ALWAYS_INLINE size_t PartitionSizeAdjustSubtract(bool allow_extras,
                                                 size_t size) {
  if (allow_extras) {
    size = PartitionTagSizeAdjustSubtract(size);
    size = PartitionCookieSizeAdjustSubtract(size);
    size = PartitionRefCountSizeAdjustSubtract(size);
  }
  return size;
}

// g_oom_handling_function is invoked when PartitionAlloc hits OutOfMemory.
static OomFunction g_oom_handling_function = nullptr;

}  // namespace internal

class PartitionStatsDumper;

enum PartitionPurgeFlags {
  // Decommitting the ring list of empty pages is reasonably fast.
  PartitionPurgeDecommitEmptyPages = 1 << 0,
  // Discarding unused system pages is slower, because it involves walking all
  // freelists in all active partition pages of all buckets >= system page
  // size. It often frees a similar amount of memory to decommitting the empty
  // pages, though.
  PartitionPurgeDiscardUnusedSystemPages = 1 << 1,
};

// Struct used to retrieve total memory usage of a partition. Used by
// PartitionStatsDumper implementation.
struct PartitionMemoryStats {
  size_t total_mmapped_bytes;    // Total bytes mmaped from the system.
  size_t total_committed_bytes;  // Total size of commmitted pages.
  size_t total_resident_bytes;   // Total bytes provisioned by the partition.
  size_t total_active_bytes;     // Total active bytes in the partition.
  size_t total_decommittable_bytes;  // Total bytes that could be decommitted.
  size_t total_discardable_bytes;    // Total bytes that could be discarded.

  bool has_thread_cache;
  internal::ThreadCacheStats current_thread_cache_stats;
  internal::ThreadCacheStats all_thread_caches_stats;
};

// Struct used to retrieve memory statistics about a partition bucket. Used by
// PartitionStatsDumper implementation.
struct PartitionBucketMemoryStats {
  bool is_valid;       // Used to check if the stats is valid.
  bool is_direct_map;  // True if this is a direct mapping; size will not be
                       // unique.
  uint32_t bucket_slot_size;     // The size of the slot in bytes.
  uint32_t allocated_page_size;  // Total size the partition page allocated from
                                 // the system.
  uint32_t active_bytes;         // Total active bytes used in the bucket.
  uint32_t resident_bytes;       // Total bytes provisioned in the bucket.
  uint32_t decommittable_bytes;  // Total bytes that could be decommitted.
  uint32_t discardable_bytes;    // Total bytes that could be discarded.
  uint32_t num_full_pages;       // Number of pages with all slots allocated.
  uint32_t num_active_pages;     // Number of pages that have at least one
                                 // provisioned slot.
  uint32_t num_empty_pages;      // Number of pages that are empty
                                 // but not decommitted.
  uint32_t num_decommitted_pages;  // Number of pages that are empty
                                   // and decommitted.
};

// Interface that is passed to PartitionDumpStats and
// PartitionDumpStats for using the memory statistics.
class BASE_EXPORT PartitionStatsDumper {
 public:
  // Called to dump total memory used by partition, once per partition.
  virtual void PartitionDumpTotals(const char* partition_name,
                                   const PartitionMemoryStats*) = 0;

  // Called to dump stats about buckets, for each bucket.
  virtual void PartitionsDumpBucketStats(const char* partition_name,
                                         const PartitionBucketMemoryStats*) = 0;
};

namespace {
// Precalculate some shift and mask constants used in the hot path.
// Example: malloc(41) == 101001 binary.
// Order is 6 (1 << 6-1) == 32 is highest bit set.
// order_index is the next three MSB == 010 == 2.
// sub_order_index_mask is a mask for the remaining bits == 11 (masking to 01
// for the sub_order_index).
constexpr size_t OrderIndexShift(size_t order) {
  if (order < kNumBucketsPerOrderBits + 1)
    return 0;

  return order - (kNumBucketsPerOrderBits + 1);
}

constexpr size_t OrderSubIndexMask(size_t order) {
  if (order == kBitsPerSizeT)
    return static_cast<size_t>(-1) >> (kNumBucketsPerOrderBits + 1);

  return ((static_cast<size_t>(1) << order) - 1) >>
         (kNumBucketsPerOrderBits + 1);
}

#if defined(ARCH_CPU_64_BITS) && !defined(OS_NACL)
#define BITS_PER_SIZE_T 64
static_assert(kBitsPerSizeT == 64, "");
#else
#define BITS_PER_SIZE_T 32
static_assert(kBitsPerSizeT == 32, "");
#endif

constexpr size_t kOrderIndexShift[BITS_PER_SIZE_T + 1] = {
    OrderIndexShift(0),  OrderIndexShift(1),  OrderIndexShift(2),
    OrderIndexShift(3),  OrderIndexShift(4),  OrderIndexShift(5),
    OrderIndexShift(6),  OrderIndexShift(7),  OrderIndexShift(8),
    OrderIndexShift(9),  OrderIndexShift(10), OrderIndexShift(11),
    OrderIndexShift(12), OrderIndexShift(13), OrderIndexShift(14),
    OrderIndexShift(15), OrderIndexShift(16), OrderIndexShift(17),
    OrderIndexShift(18), OrderIndexShift(19), OrderIndexShift(20),
    OrderIndexShift(21), OrderIndexShift(22), OrderIndexShift(23),
    OrderIndexShift(24), OrderIndexShift(25), OrderIndexShift(26),
    OrderIndexShift(27), OrderIndexShift(28), OrderIndexShift(29),
    OrderIndexShift(30), OrderIndexShift(31), OrderIndexShift(32),
#if BITS_PER_SIZE_T == 64
    OrderIndexShift(33), OrderIndexShift(34), OrderIndexShift(35),
    OrderIndexShift(36), OrderIndexShift(37), OrderIndexShift(38),
    OrderIndexShift(39), OrderIndexShift(40), OrderIndexShift(41),
    OrderIndexShift(42), OrderIndexShift(43), OrderIndexShift(44),
    OrderIndexShift(45), OrderIndexShift(46), OrderIndexShift(47),
    OrderIndexShift(48), OrderIndexShift(49), OrderIndexShift(50),
    OrderIndexShift(51), OrderIndexShift(52), OrderIndexShift(53),
    OrderIndexShift(54), OrderIndexShift(55), OrderIndexShift(56),
    OrderIndexShift(57), OrderIndexShift(58), OrderIndexShift(59),
    OrderIndexShift(60), OrderIndexShift(61), OrderIndexShift(62),
    OrderIndexShift(63), OrderIndexShift(64)
#endif
};

constexpr size_t kOrderSubIndexMask[BITS_PER_SIZE_T + 1] = {
    OrderSubIndexMask(0),  OrderSubIndexMask(1),  OrderSubIndexMask(2),
    OrderSubIndexMask(3),  OrderSubIndexMask(4),  OrderSubIndexMask(5),
    OrderSubIndexMask(6),  OrderSubIndexMask(7),  OrderSubIndexMask(8),
    OrderSubIndexMask(9),  OrderSubIndexMask(10), OrderSubIndexMask(11),
    OrderSubIndexMask(12), OrderSubIndexMask(13), OrderSubIndexMask(14),
    OrderSubIndexMask(15), OrderSubIndexMask(16), OrderSubIndexMask(17),
    OrderSubIndexMask(18), OrderSubIndexMask(19), OrderSubIndexMask(20),
    OrderSubIndexMask(21), OrderSubIndexMask(22), OrderSubIndexMask(23),
    OrderSubIndexMask(24), OrderSubIndexMask(25), OrderSubIndexMask(26),
    OrderSubIndexMask(27), OrderSubIndexMask(28), OrderSubIndexMask(29),
    OrderSubIndexMask(30), OrderSubIndexMask(31), OrderSubIndexMask(32),
#if BITS_PER_SIZE_T == 64
    OrderSubIndexMask(33), OrderSubIndexMask(34), OrderSubIndexMask(35),
    OrderSubIndexMask(36), OrderSubIndexMask(37), OrderSubIndexMask(38),
    OrderSubIndexMask(39), OrderSubIndexMask(40), OrderSubIndexMask(41),
    OrderSubIndexMask(42), OrderSubIndexMask(43), OrderSubIndexMask(44),
    OrderSubIndexMask(45), OrderSubIndexMask(46), OrderSubIndexMask(47),
    OrderSubIndexMask(48), OrderSubIndexMask(49), OrderSubIndexMask(50),
    OrderSubIndexMask(51), OrderSubIndexMask(52), OrderSubIndexMask(53),
    OrderSubIndexMask(54), OrderSubIndexMask(55), OrderSubIndexMask(56),
    OrderSubIndexMask(57), OrderSubIndexMask(58), OrderSubIndexMask(59),
    OrderSubIndexMask(60), OrderSubIndexMask(61), OrderSubIndexMask(62),
    OrderSubIndexMask(63), OrderSubIndexMask(64)
#endif
};

}  // namespace

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::AllocFromBucket(
    Bucket* bucket,
    int flags,
    size_t raw_size,
    size_t* utilized_slot_size,
    bool* is_already_zeroed) {
  *is_already_zeroed = false;

  Page* page = bucket->active_pages_head;
  // Check that this page is neither full nor freed.
  PA_DCHECK(page);
  PA_DCHECK(page->num_allocated_slots >= 0);
  *utilized_slot_size = bucket->slot_size;

  void* ret = page->freelist_head;
  if (LIKELY(ret)) {
    // If these DCHECKs fire, you probably corrupted memory. TODO(palmer): See
    // if we can afford to make these CHECKs.
    PA_DCHECK(IsValidPage(page));

    // All large allocations must go through the slow path to correctly update
    // the size metadata.
    PA_DCHECK(!page->CanStoreRawSize());
    internal::PartitionFreelistEntry* new_head =
        internal::EncodedPartitionFreelistEntry::Decode(
            page->freelist_head->next);
    page->freelist_head = new_head;
    page->num_allocated_slots++;

    PA_DCHECK(page->bucket == bucket);
  } else {
    ret = bucket->SlowPathAlloc(this, flags, raw_size, is_already_zeroed);
    // TODO(palmer): See if we can afford to make this a CHECK.
    PA_DCHECK(!ret || IsValidPage(Page::FromPointer(ret)));

    if (UNLIKELY(!ret))
      return nullptr;

    page = Page::FromPointer(ret);
    // For direct mapped allocations, |bucket| is the sentinel.
    PA_DCHECK((page->bucket == bucket) || (page->bucket->is_direct_mapped() &&
                                           (bucket == &sentinel_bucket)));

    *utilized_slot_size = page->GetUtilizedSlotSize();
  }

  return ret;
}

// static
template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::Free(void* ptr) {
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  free(ptr);
#else
  if (UNLIKELY(!ptr))
    return;

  if (PartitionAllocHooks::AreHooksEnabled()) {
    PartitionAllocHooks::FreeObserverHookIfEnabled(ptr);
    if (PartitionAllocHooks::FreeOverrideHookIfEnabled(ptr))
      return;
  }

  FreeNoHooks(ptr);
#endif  // defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
}

// static
template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::FreeNoHooks(void* ptr) {
  if (UNLIKELY(!ptr))
    return;

  // No check as the pointer hasn't been adjusted yet.
  Page* page = Page::FromPointerNoAlignmentCheck(ptr);
  // TODO(palmer): See if we can afford to make this a CHECK.
  PA_DCHECK(IsValidPage(page));
  auto* root = PartitionRoot<thread_safe>::FromPage(page);

  // TODO(bikineev): Change the first condition to LIKELY once PCScan is enabled
  // by default.
  if (UNLIKELY(root->pcscan) && LIKELY(!page->bucket->is_direct_mapped())) {
    root->pcscan->MoveToQuarantine(ptr, page);
    return;
  }

  root->FreeNoHooksImmediate(ptr, page);
}

template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::FreeNoHooksImmediate(
    void* ptr,
    Page* page) {
  // The thread cache is added "in the middle" of the main allocator, that is:
  // - After all the cookie/tag/ref-count management
  // - Before the "raw" allocator.
  //
  // On the deallocation side:
  // 1. Check cookies/tags/ref-count, adjust the pointer
  // 2. Deallocation
  //   a. Return to the thread cache if possible. If it succeeds, return.
  //   b. Otherwise, call the "raw" allocator <-- Locking
  PA_DCHECK(ptr);
  PA_DCHECK(page);
  PA_DCHECK(IsValidPage(page));

#if DCHECK_IS_ON()
  size_t utilized_slot_size = page->GetUtilizedSlotSize();
#endif

  if (allow_extras) {
    // |ptr| points after the tag and the cookie.
    //
    // Layout inside the slot:
    //  <--------extras------->                  <-extras->
    //  <----------------utilized_slot_size--------------->
    //  |[tag/refcnt]|[cookie]|...data...|[empty]|[cookie]|[unused]|
    //  ^                     ^
    //  |                     |
    //  allocation_start_ptr  ptr
    //
    // Note: tag, ref-count and cookie can be 0-sized.
    //
    // For more context, see the other "Layout inside the slot" comment below.
    void* allocation_start_ptr =
        internal::PartitionPointerAdjustSubtract(true /* allow_extras */, ptr);

#if DCHECK_IS_ON()
    void* start_cookie_ptr =
        internal::PartitionCookiePointerAdjustSubtract(ptr);
    void* end_cookie_ptr = internal::PartitionCookiePointerAdjustSubtract(
        reinterpret_cast<char*>(allocation_start_ptr) + utilized_slot_size);

    // If these asserts fire, you probably corrupted memory.
    internal::PartitionCookieCheckValue(start_cookie_ptr);
    internal::PartitionCookieCheckValue(end_cookie_ptr);
#endif

    if (!page->bucket->is_direct_mapped()) {
      // PartitionTagIncrementValue and PartitionTagClearValue require that the
      // size is tag_bitmap::kBytesPerPartitionTag-aligned (currently 16
      // bytes-aligned) when MTECheckedPtr is enabled. However,
      // utilized_slot_size may not be aligned for single-slot slot spans. So we
      // need the bucket's slot_size.
      size_t slot_size_with_no_extras =
          internal::PartitionSizeAdjustSubtract(true, page->bucket->slot_size);
#if ENABLE_TAG_FOR_MTE_CHECKED_PTR && MTE_CHECKED_PTR_SET_TAG_AT_FREE
      internal::PartitionTagIncrementValue(ptr, slot_size_with_no_extras);
#else
      internal::PartitionTagClearValue(ptr, slot_size_with_no_extras);
#endif

#if ENABLE_REF_COUNT_FOR_BACKUP_REF_PTR
      internal::PartitionRefCount* ref_count =
          internal::PartitionRefCountPointerNoOffset(ptr);
      // If we are holding the last reference to the allocation, it can be freed
      // immediately. Otherwise, defer the operation and zap the memory to turn
      // potential use-after-free issues into unexploitable crashes.
      if (UNLIKELY(!ref_count->HasOneRef())) {
#ifdef ADDRESS_SANITIZER
        ASAN_POISON_MEMORY_REGION(ptr, usable_size);
#else
        memset(ptr, kFreedByte, usable_size);
#endif
        ref_count->Release();
        return;
      }
#endif
    }

    ptr = allocation_start_ptr;
  }

#if DCHECK_IS_ON()
  memset(ptr, kFreedByte, utilized_slot_size);
#endif

  // TLS access can be expensive, do a cheap local check first.
  //
  // Also the thread-unsafe variant doesn't have a use for a thread cache, so
  // make it statically known to the compiler.
  if (thread_safe && with_thread_cache && !page->bucket->is_direct_mapped()) {
    PA_DCHECK(page->bucket >= this->buckets &&
              page->bucket <= &this->sentinel_bucket);
    size_t bucket_index = page->bucket - this->buckets;
    auto* thread_cache = internal::ThreadCache::Get();
    if (thread_cache && thread_cache->MaybePutInCache(ptr, bucket_index))
      return;
  }

  RawFree(ptr, page);
}

template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::RawFree(void* ptr, Page* page) {
  internal::DeferredUnmap deferred_unmap;
  {
    ScopedGuard guard{lock_};
    deferred_unmap = page->Free(ptr);
  }
  deferred_unmap.Run();
}

// static
template <bool thread_safe>
void PartitionRoot<thread_safe>::RawFreeStatic(void* ptr) {
  Page* page = Page::FromPointerNoAlignmentCheck(ptr);
  auto* root = PartitionRoot<thread_safe>::FromPage(page);
  root->RawFree(ptr, page);
}

// static
template <bool thread_safe>
ALWAYS_INLINE bool PartitionRoot<thread_safe>::IsValidPage(Page* page) {
  PartitionRoot* root = FromPage(page);
  return root->inverted_self == ~reinterpret_cast<uintptr_t>(root);
}

template <bool thread_safe>
ALWAYS_INLINE PartitionRoot<thread_safe>* PartitionRoot<thread_safe>::FromPage(
    Page* page) {
  auto* extent_entry = reinterpret_cast<SuperPageExtentEntry*>(
      reinterpret_cast<uintptr_t>(page) & SystemPageBaseMask());
  return extent_entry->root;
}

template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::IncreaseCommittedPages(
    size_t len) {
  total_size_of_committed_pages += len;
  PA_DCHECK(total_size_of_committed_pages <=
            total_size_of_super_pages + total_size_of_direct_mapped_pages);
}

template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::DecreaseCommittedPages(
    size_t len) {
  total_size_of_committed_pages -= len;
  PA_DCHECK(total_size_of_committed_pages <=
            total_size_of_super_pages + total_size_of_direct_mapped_pages);
}

template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::DecommitSystemPages(
    void* address,
    size_t length) {
  ::base::DecommitSystemPages(address, length);
  DecreaseCommittedPages(length);
}

template <bool thread_safe>
ALWAYS_INLINE void PartitionRoot<thread_safe>::RecommitSystemPages(
    void* address,
    size_t length) {
  PA_CHECK(::base::RecommitSystemPages(address, length, PageReadWrite));
  IncreaseCommittedPages(length);
}

BASE_EXPORT void PartitionAllocGlobalInit(OomFunction on_out_of_memory);
BASE_EXPORT void PartitionAllocGlobalUninitForTesting();

namespace internal {
// Gets the PartitionPage object for the first partition page of the slot span
// that contains |ptr|. It's used with intention to do obtain the slot size.
// CAUTION! It works well for normal buckets, but for direct-mapped allocations
// it'll only work if |ptr| is in the first partition page of the allocation.
template <bool thread_safe>
ALWAYS_INLINE internal::PartitionPage<thread_safe>*
PartitionAllocGetPageForSize(void* ptr) {
  // No need to lock here. Only |ptr| being freed by another thread could
  // cause trouble, and the caller is responsible for that not happening.
  auto* page =
      internal::PartitionPage<thread_safe>::FromPointerNoAlignmentCheck(ptr);
  // TODO(palmer): See if we can afford to make this a CHECK.
  PA_DCHECK(PartitionRoot<thread_safe>::IsValidPage(page));
  return page;
}
}  // namespace internal

// static
// Gets the allocated size of the |ptr|, from the point of view of the app, not
// PartitionAlloc. It can be equal or higher than the requested size. If higher,
// the overage won't exceed what's actually usable by the app without a risk of
// running out of an allocated region or into PartitionAlloc's internal data.
// Used as malloc_usable_size.
template <bool thread_safe>
ALWAYS_INLINE size_t PartitionRoot<thread_safe>::GetUsableSize(void* ptr) {
  Page* page = Page::FromPointerNoAlignmentCheck(ptr);
  auto* root = PartitionRoot<thread_safe>::FromPage(page);

  size_t size = page->GetUtilizedSlotSize();
  // Adjust back by subtracing extras (if any).
  size = internal::PartitionSizeAdjustSubtract(root->allow_extras, size);
  return size;
}

// Gets the size of the allocated slot that contains |ptr|, adjusted for cookie
// and tag (if any). CAUTION! For direct-mapped allocation, |ptr| has to be
// within the first partition page.
template <bool thread_safe>
ALWAYS_INLINE size_t PartitionRoot<thread_safe>::GetSize(void* ptr) const {
  ptr = internal::PartitionPointerAdjustSubtract(allow_extras, ptr);
  auto* page = internal::PartitionAllocGetPageForSize<thread_safe>(ptr);
  size_t size = internal::PartitionSizeAdjustSubtract(allow_extras,
                                                      page->bucket->slot_size);
  return size;
}

// This file may end up getting included even when PartitionAlloc isn't used,
// but the .cc file won't be linked. Exclude the code that relies on it.
#if BUILDFLAG(USE_PARTITION_ALLOC)

namespace internal {
// Avoid including partition_address_space.h from this .h file, by moving the
// call to IfManagedByPartitionAllocNormalBuckets into the .cc file.
#if DCHECK_IS_ON()
BASE_EXPORT void DCheckIfManagedByPartitionAllocNormalBuckets(const void* ptr);
#else
ALWAYS_INLINE void DCheckIfManagedByPartitionAllocNormalBuckets(const void*) {}
#endif
}  // namespace internal

#endif  // BUILDFLAG(USE_PARTITION_ALLOC)

// static
template <bool thread_safe>
ALWAYS_INLINE uint16_t
PartitionRoot<thread_safe>::SizeToBucketIndex(size_t size) {
  size_t order = kBitsPerSizeT - bits::CountLeadingZeroBitsSizeT(size);
  // The order index is simply the next few bits after the most significant bit.
  size_t order_index =
      (size >> kOrderIndexShift[order]) & (kNumBucketsPerOrder - 1);
  // And if the remaining bits are non-zero we must bump the bucket up.
  size_t sub_order_index = size & kOrderSubIndexMask[order];
  uint16_t index = bucket_index_lookup[(order << kNumBucketsPerOrderBits) +
                                       order_index + !!sub_order_index];
  PA_DCHECK(index <= kNumBuckets);  // Last one is the sentinetl bucket.
  return index;
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::AllocFlags(
    int flags,
    size_t requested_size,
    const char* type_name) {
  PA_DCHECK(flags < PartitionAllocLastFlag << 1);
  PA_DCHECK((flags & PartitionAllocNoHooks) == 0);  // Internal only.
  PA_DCHECK(initialized);

#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  CHECK_MAX_SIZE_OR_RETURN_NULLPTR(requested_size, flags);
  const bool zero_fill = flags & PartitionAllocZeroFill;
  void* result = zero_fill ? calloc(1, requested_size) : malloc(requested_size);
  PA_CHECK(result || flags & PartitionAllocReturnNull);
  return result;
#else
  PA_DCHECK(initialized);
  void* ret = nullptr;
  const bool hooks_enabled = PartitionAllocHooks::AreHooksEnabled();
  if (UNLIKELY(hooks_enabled)) {
    if (PartitionAllocHooks::AllocationOverrideHookIfEnabled(
            &ret, flags, requested_size, type_name)) {
      PartitionAllocHooks::AllocationObserverHookIfEnabled(ret, requested_size,
                                                           type_name);
      return ret;
    }
  }

  ret = AllocFlagsNoHooks(flags, requested_size);

  if (UNLIKELY(hooks_enabled)) {
    PartitionAllocHooks::AllocationObserverHookIfEnabled(ret, requested_size,
                                                         type_name);
  }

  return ret;
#endif  // defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::AllocFlagsNoHooks(
    int flags,
    size_t requested_size) {
  // The thread cache is added "in the middle" of the main allocator, that is:
  // - After all the cookie/tag management
  // - Before the "raw" allocator.
  //
  // That is, the general allocation flow is:
  // 1. Adjustment of requested size to make room for extras
  // 2. Allocation:
  //   a. Call to the thread cache, if it succeeds, go to step 3.
  //   b. Otherwise, call the "raw" allocator <-- Locking
  // 3. Handle cookies/tags, zero allocation if required
  size_t raw_size =
      internal::PartitionSizeAdjustAdd(allow_extras, requested_size);
  PA_CHECK(raw_size >= requested_size);  // check for overflows

  uint16_t bucket_index = SizeToBucketIndex(raw_size);
  size_t utilized_slot_size;
  bool is_already_zeroed;
  void* ret = nullptr;

  // !thread_safe => !with_thread_cache, but adding the condition allows the
  // compiler to statically remove this branch for the thread-unsafe variant.
  if (thread_safe && with_thread_cache) {
    auto* tcache = internal::ThreadCache::Get();
    if (UNLIKELY(!tcache)) {
      // There is no per-thread ThreadCache allocated here yet, and this
      // partition has a thread cache, allocate a new one.
      //
      // The thread cache allocation itself will not reenter here, as it
      // sidesteps the thread cache by using placement new and
      // |RawAlloc()|. However, internally to libc, allocations may happen to
      // create a new TLS variable. This would end up here again, which is not
      // what we want (and likely is not supported by libc).
      //
      // To avoid this sort of reentrancy, temporarily set this partition as not
      // supporting a thread cache. so that reentering allocations will not end
      // up allocating a thread cache. This value may be seen by other threads
      // as well, in which case a few allocations will not use the thread
      // cache. As it is purely an optimization, this is not a correctness
      // issue.
      //
      // Note that there is no deadlock or data inconsistency concern, since we
      // do not hold the lock, and has such haven't touched any internal data.
      with_thread_cache = false;
      tcache = internal::ThreadCache::Create(this);
      with_thread_cache = true;
    }
    // bucket->slot_size is 0 for direct-mapped allocations, as their bucket is
    // the sentinel one. Since |bucket_index| is going to be kNumBuckets + 1,
    // the thread cache allocation will return nullptr.
    ret = tcache->GetFromCache(bucket_index);
    is_already_zeroed = false;
    utilized_slot_size = buckets[bucket_index].slot_size;

#if DCHECK_IS_ON()
    // Make sure that the allocated pointer comes from the same place it would
    // for a non-thread cache allocation.
    if (ret) {
      Page* page = Page::FromPointerNoAlignmentCheck(ret);
      // All large allocations must go through the RawAlloc path to correctly
      // set |utilized_slot_size|.
      PA_DCHECK(!page->CanStoreRawSize());
      PA_DCHECK(IsValidPage(page));
      PA_DCHECK(page->bucket == &buckets[bucket_index]);
    }
#endif
  }

  if (!ret)
    ret = RawAlloc(buckets + bucket_index, flags, raw_size, &utilized_slot_size,
                   &is_already_zeroed);

  if (UNLIKELY(!ret))
    return nullptr;

  // Layout inside the slot:
  //  |[tag/refcnt]|[cookie]|...data...|[empty]|[cookie]|[unused]|
  //                        <----a----->
  //                        <--------b--------->
  //  <----------c---------->                  <---c---->
  //  <---------------d--------------->   +    <---d---->
  //  <------------------------e------------------------>
  //  <----------------------------f----------------------------->
  //   a: requested_size
  //   b: usable_size
  //   c: extras
  //   d: raw_size
  //   e: utilized_slot_size
  //   f: slot_size
  //
  // - The tag/ref-count may or may not exist in the slot, depending on
  //   CheckedPtr implementation.
  // - Cookies exist only when DCHECK is on.
  // - Think of raw_size as the minimum size required internally to satisfy
  //   the allocation request (i.e. requested_size + extras)
  // - Note, at most one "empty" or "unused" space can occur at a time. It
  //   occurs when slot_size is larger than raw_size. "unused" applies only to
  //   large allocations (direct-mapped and single-slot slot spans) and "empty"
  //   only to small allocations.
  //   Why either-or, one might ask? We make an effort to put the trailing
  //   cookie as close to data as possible to catch overflows (often
  //   off-by-one), but that's possible only if we have enough space in metadata
  //   to save raw_size, i.e. only for large allocations. For small allocations,
  //   we have no other choice than putting the cookie at the very end of the
  //   slot, thus creating the "empty" space.
  size_t usable_size =
      internal::PartitionSizeAdjustSubtract(allow_extras, utilized_slot_size);
  // The value given to the application is just after the tag and cookie.
  ret = internal::PartitionPointerAdjustAdd(allow_extras, ret);

#if DCHECK_IS_ON()
  // Surround the region with 2 cookies.
  if (allow_extras) {
    char* char_ret = static_cast<char*>(ret);
    internal::PartitionCookieWriteValue(char_ret - internal::kCookieSize);
    internal::PartitionCookieWriteValue(char_ret + usable_size);
  }
#endif

  // Fill the region kUninitializedByte (on debug builds, if not requested to 0)
  // or 0 (if requested and not 0 already).
  bool zero_fill = flags & PartitionAllocZeroFill;
  if (!zero_fill) {
#if DCHECK_IS_ON()
    memset(ret, kUninitializedByte, usable_size);
#endif
  } else if (!is_already_zeroed) {
    memset(ret, 0, usable_size);
  }

  bool is_direct_mapped = raw_size > kMaxBucketed;
  if (allow_extras && !is_direct_mapped) {
    // Do not set tag for MTECheckedPtr in the set-tag-at-free case.
    // It is set only at Free() time and at slot span allocation time.
#if !ENABLE_TAG_FOR_MTE_CHECKED_PTR || !MTE_CHECKED_PTR_SET_TAG_AT_FREE
    // PartitionTagSetValue requires that the size is
    // tag_bitmap::kBytesPerPartitionTag-aligned (currently 16 bytes-aligned)
    // when MTECheckedPtr is enabled. However, utilized_slot_size may not be
    // aligned for single-slot slot spans. So we need the bucket's slot_size.
    size_t slot_size_with_no_extras = internal::PartitionSizeAdjustSubtract(
        allow_extras, buckets[bucket_index].slot_size);
    internal::PartitionTagSetValue(ret, slot_size_with_no_extras,
                                   GetNewPartitionTag());
#endif  // !ENABLE_TAG_FOR_MTE_CHECKED_PTR || !MTE_CHECKED_PTR_SET_TAG_AT_FREE

#if ENABLE_REF_COUNT_FOR_BACKUP_REF_PTR
    internal::PartitionRefCountPointerNoOffset(ret)->Init();
#endif  // ENABLE_REF_COUNT_FOR_BACKUP_REF_PTR
  }
  return ret;
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::RawAlloc(
    Bucket* bucket,
    int flags,
    size_t raw_size,
    size_t* utilized_slot_size,
    bool* is_already_zeroed) {
  internal::ScopedGuard<thread_safe> guard{lock_};
  return AllocFromBucket(bucket, flags, raw_size, utilized_slot_size,
                         is_already_zeroed);
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::AlignedAllocFlags(
    int flags,
    size_t alignment,
    size_t size) {
  // Aligned allocation support relies on the natural alignment guarantees of
  // PartitionAlloc. Since cookies and tags are layered on top of
  // PartitionAlloc, they change the guarantees. As a consequence, forbid both.
  PA_DCHECK(!allow_extras);

  // This is mandated by |posix_memalign()|, so should never fire.
  PA_CHECK(base::bits::IsPowerOfTwo(alignment));

  size_t requested_size;
  // Handle cases such as size = 16, alignment = 64.
  // Wastes memory when a large alignment is requested with a small size, but
  // this is hard to avoid, and should not be too common.
  if (UNLIKELY(size < alignment)) {
    requested_size = alignment;
  } else {
    // PartitionAlloc only guarantees alignment for power-of-two sized
    // allocations. To make sure this applies here, round up the allocation
    // size.
    requested_size =
        static_cast<size_t>(1)
        << (sizeof(size_t) * 8 - base::bits::CountLeadingZeroBits(size - 1));
  }

  // Overflow check. requested_size must be larger or equal to size.
  if (requested_size < size) {
    if (flags & PartitionAllocReturnNull)
      return nullptr;
    // OutOfMemoryDeathTest.AlignedAlloc requires base::OnNoMemoryInternal
    // (invoked by PartitionExcessiveAllocationSize).
    internal::PartitionExcessiveAllocationSize(size);
    // internal::PartitionExcessiveAllocationSize(size) causes OOM_CRASH.
    NOTREACHED();
  }

  bool no_hooks = flags & PartitionAllocNoHooks;
  void* ptr = no_hooks ? AllocFlagsNoHooks(0, requested_size)
                       : Alloc(requested_size, "");

  // |alignment| is a power of two, but the compiler doesn't necessarily know
  // that. A regular % operation is very slow, make sure to use the equivalent,
  // faster form.
  PA_CHECK(!(reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)));

  return ptr;
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::Alloc(size_t requested_size,
                                                      const char* type_name) {
  return AllocFlags(0, requested_size, type_name);
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::Realloc(void* ptr,
                                                        size_t new_size,
                                                        const char* type_name) {
  return ReallocFlags(0, ptr, new_size, type_name);
}

template <bool thread_safe>
ALWAYS_INLINE void* PartitionRoot<thread_safe>::TryRealloc(
    void* ptr,
    size_t new_size,
    const char* type_name) {
  return ReallocFlags(PartitionAllocReturnNull, ptr, new_size, type_name);
}

template <bool thread_safe>
ALWAYS_INLINE size_t PartitionRoot<thread_safe>::ActualSize(size_t size) {
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  return size;
#else
  PA_DCHECK(PartitionRoot<thread_safe>::initialized);
  size = internal::PartitionSizeAdjustAdd(allow_extras, size);
  auto& bucket = buckets[SizeToBucketIndex(size)];
  PA_DCHECK(!bucket.slot_size || bucket.slot_size >= size);
  PA_DCHECK(!(bucket.slot_size % kSmallestBucket));

  if (LIKELY(!bucket.is_direct_mapped())) {
    size = bucket.slot_size;
  } else if (size > MaxDirectMapped()) {
    // Too large to allocate => return the size unchanged.
  } else {
    size = Bucket::get_direct_map_size(size);
  }
  size = internal::PartitionSizeAdjustSubtract(allow_extras, size);
  return size;
#endif
}

namespace internal {
template <bool thread_safe>
struct BASE_EXPORT PartitionAllocator {
  PartitionAllocator() = default;
  ~PartitionAllocator();

  void init(PartitionOptions = {});

  ALWAYS_INLINE PartitionRoot<thread_safe>* root() { return &partition_root_; }
  ALWAYS_INLINE const PartitionRoot<thread_safe>* root() const {
    return &partition_root_;
  }

 private:
  PartitionRoot<thread_safe> partition_root_;
};

}  // namespace internal

using PartitionAllocator = internal::PartitionAllocator<internal::ThreadSafe>;
using ThreadUnsafePartitionAllocator =
    internal::PartitionAllocator<internal::NotThreadSafe>;

using ThreadSafePartitionRoot = PartitionRoot<internal::ThreadSafe>;
using ThreadUnsafePartitionRoot = PartitionRoot<internal::NotThreadSafe>;

}  // namespace base

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_H_
