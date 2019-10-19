#pragma once
#include <immintrin.h>

#include <bitset>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../util/hash.h"
#include "../util/pair.h"
#include "../util/persist.h"
#include "Hash.h"
#include "allocator.h"

#ifdef PMEM
#include <libpmemobj.h>
#endif

//uint64_t merge_time;

namespace extendible {

#define _INVALID 0 /* we use 0 as the invalid key*/
#define SINGLE 1
#define COUNTING 1
#define EPOCH 1
#define CRASH 1
//#define PREALLOC 1

#define SIMD 1
#define SIMD_CMP8(src, key)                                         \
  do {                                                              \
    const __m256i key_data = _mm256_set1_epi8(key);                 \
    __m256i seg_data =                                              \
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src)); \
    __m256i rv_mask = _mm256_cmpeq_epi8(seg_data, key_data);        \
    mask = _mm256_movemask_epi8(rv_mask);                           \
  } while (0)

#define SSE_CMP8(src, key)                                       \
  do {                                                           \
    const __m128i key_data = _mm_set1_epi8(key);                 \
    __m128i seg_data =                                           \
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(src)); \
    __m128i rv_mask = _mm_cmpeq_epi8(seg_data, key_data);        \
    mask = _mm_movemask_epi8(rv_mask);                           \
  } while (0)

#define CHECK_BIT(var, pos) ((((var) & (1 << pos)) > 0) ? (1) : (0))

const uint32_t lockSet = ((uint32_t)1 << 31);      /*locking information*/
const uint32_t lockMask = ((uint32_t)1 << 31) - 1; /*locking mask*/
const int overflowSet = 1 << 15;
const int countMask = (1 << 4) - 1;

template <class T>
struct _Pair {
  T key;
  Value_t value;
};

constexpr size_t k_PairSize = 16;  // a k-v _Pair with a bit
constexpr size_t kNumPairPerBucket =
    14; /* it is determined by the usage of the fingerprint*/
constexpr size_t kFingerBits = 8;
constexpr size_t kMask = (1 << kFingerBits) - 1;
const constexpr size_t kNumBucket = 64;
constexpr size_t stashBucket = 2;
constexpr int allocMask = (1 << kNumPairPerBucket) - 1;
constexpr size_t bucketMask = ((1 << (int)log2(kNumBucket)) - 1);
constexpr size_t stashMask = (1 << (int)log2(stashBucket)) - 1;
constexpr uint8_t stashHighMask = ~((uint8_t)stashMask);
constexpr uint8_t preNeighborSet = 1 << 7;
constexpr uint8_t nextNeighborSet = 1 << 6;
const uint64_t recoverBit = 1UL << 63;
const uint64_t lockBit = 1UL << 62;
#define BUCKET_INDEX(hash) ((hash >> kFingerBits) & bucketMask)
#define GET_COUNT(var) ((var)&countMask)
#define GET_BITMAP(var) (((var) >> 4) & allocMask)
#define ORG_BITMAP(var) ((~((var)&allocMask)) & allocMask)
#define PROBE_BITMAP(var) ((var)&allocMask)

#ifdef CRASH
uint64_t split_num;
#endif

inline bool var_compare(char *str1, char *str2, int len1, int len2) {
  if (len1 != len2) return false;
  return !memcmp(str1, str2, len1);
}

template <class T>
struct Bucket {
  inline int find_empty_slot() {
    if (GET_COUNT(bitmap) == kNumPairPerBucket) {
      return -1;
    }
    auto mask = ~(GET_BITMAP(bitmap));  // Now the 1 bit should be the empty
                                        // slot
    return __builtin_ctz(mask);
  }

  /*true indicates overflow, needs extra check in the stash*/
  inline bool test_overflow() {
    // return (overflowCount != 0) ? true : false;
    return overflowCount;
  }

  inline bool test_stash_check() {
    int mask = *((int *)membership);
    // return ((mask & overflowSet) != 0) ? true : false;
    return (mask & overflowSet);
  }

  inline void clear_stash_check() {
    int mask = *((int *)membership);
    *((int *)membership) = (*((int *)membership)) & (~overflowSet);
  }

  inline void set_indicator(uint8_t meta_hash, Bucket<T> *neighbor,
                            uint8_t pos) {
    int mask = finger_array[14];
    mask = ~mask;
    auto index = __builtin_ctz(mask);

    if (index < 4) {
      finger_array[15 + index] = meta_hash;
      finger_array[14] =
          ((uint8_t)(1 << index) | finger_array[14]); /*may be optimized*/
      finger_array[19] =
          (finger_array[19] & (~(3 << (index * 2)))) | (pos << (index * 2));
    } else {
      mask = neighbor->finger_array[14];
      mask = ~mask;
      index = __builtin_ctz(mask);
      if (index < 4) {
        neighbor->finger_array[15 + index] = meta_hash;
        neighbor->finger_array[14] =
            ((uint8_t)(1 << index) | neighbor->finger_array[14]);
        neighbor->overflowMember =
            ((uint8_t)(1 << index) | neighbor->overflowMember);
        neighbor->finger_array[19] =
            (neighbor->finger_array[19] & (~(3 << (index * 2)))) |
            (pos << (index * 2));
      } else { /*overflow, increase count*/
        overflowCount++;
      }
    }
    *((int *)membership) = (*((int *)membership)) | overflowSet;
  }

  /*both clear this bucket and its neighbor bucket*/
  inline void unset_indicator(uint8_t meta_hash, Bucket<T> *neighbor, T key,
                              uint64_t pos) {
    /*also needs to ensure that this meta_hash must belongs to other bucket*/
    bool clear_success = false;
    int mask1 = finger_array[14];
    for (int i = 0; i < 4; ++i) {
      if (CHECK_BIT(mask1, i) && (finger_array[15 + i] == meta_hash) &&
          (((1 << i) & overflowMember) == 0) &&
          (((finger_array[19] >> (2 * i)) & stashMask) == pos)) {
        // printf("clear the indicator 1\n");
        finger_array[14] = finger_array[14] & ((uint8_t)(~(1 << i)));
        finger_array[19] = finger_array[19] & (~(3 << (i * 2)));
        assert(((finger_array[19] >> (i * 2)) & stashMask) == 0);
        clear_success = true;
        break;
      }
    }

    int mask2 = neighbor->finger_array[14];
    if (!clear_success) {
      for (int i = 0; i < 4; ++i) {
        if (CHECK_BIT(mask2, i) &&
            (neighbor->finger_array[15 + i] == meta_hash) &&
            (((1 << i) & neighbor->overflowMember) != 0) &&
            (((neighbor->finger_array[19] >> (2 * i)) & stashMask) == pos)) {
          // printf("clear the indicator 2\n");
          neighbor->finger_array[14] =
              neighbor->finger_array[14] & ((uint8_t)(~(1 << i)));
          neighbor->overflowMember =
              neighbor->overflowMember & ((uint8_t)(~(1 << i)));
          neighbor->finger_array[19] =
              neighbor->finger_array[19] & (~(3 << (i * 2)));
          assert(((neighbor->finger_array[19] >> (i * 2)) & stashMask) == 0);
          clear_success = true;
          break;
        }
      }
    }

    if (!clear_success) {
      // printf("decrease overflowCount\n");
      overflowCount--;
    }

    mask1 = finger_array[14];
    mask2 = neighbor->finger_array[14];
    if (((mask1 & (~overflowMember)) == 0) && (overflowCount == 0) &&
        ((mask2 & neighbor->overflowMember) == 0)) {
      // printf("clear the stash check\n");
      clear_stash_check();
    }
  }

  int unique_check(uint8_t meta_hash, T key, Bucket<T> *neighbor,
                   Bucket<T> *stash) {
    // return check_and_get(meta_hash, key) == NONE ? 0 : -1;
    if ((check_and_get(meta_hash, key, false) != NONE) ||
        (neighbor->check_and_get(meta_hash, key, true) != NONE)) {
      return -1;
    }

    if (test_stash_check()) {
      auto test_stash = false;
      if (test_overflow()) {
        test_stash = true;
      } else {
        int mask = finger_array[14];
        if (finger_array[14] != 0) {
          for (int i = 0; i < 4; ++i) {
            if (CHECK_BIT(mask, i) && (finger_array[15 + i] == meta_hash) &&
                (((1 << i) & overflowMember) == 0)) {
              test_stash = true;
              goto STASH_CHECK;
            }
          }
        }

        if (neighbor->finger_array[14] != 0) {
          mask = neighbor->finger_array[14];
          for (int i = 0; i < 4; ++i) {
            if (CHECK_BIT(mask, i) &&
                (neighbor->finger_array[15 + i] == meta_hash) &&
                (((1 << i) & neighbor->overflowMember) != 0)) {
              test_stash = true;
              break;
            }
          }
        }
      }
    STASH_CHECK:
      if (test_stash == true) {
        for (int i = 0; i < stashBucket; ++i) {
          Bucket *curr_bucket = stash + i;
          if (curr_bucket->check_and_get(meta_hash, key, false) != NONE) {
            return -1;
          }
        }
      }
    }
    /*
    for (int i = 0; i < stashBucket; ++i) {
          Bucket *curr_bucket = stash + i;
      if (curr_bucket->check_and_get(meta_hash, key, false) != NONE) {
        return -1;
      }
    }*/
    return 0;
  }

  inline int get_current_mask() {
    int mask = GET_BITMAP(bitmap) & ((~(*(int *)membership)) & allocMask);
    return mask;
  }

  Value_t check_and_get(uint8_t meta_hash, T key, bool probe) {
    int mask = 0;
    SSE_CMP8(finger_array, meta_hash);
    if (!probe) {
      mask = mask & GET_BITMAP(bitmap) & ((~(*(int *)membership)) & allocMask);
    } else {
      mask = mask & GET_BITMAP(bitmap) & ((*(int *)membership) & allocMask);
    }

    if (mask == 0) {
      return NONE;
    }

    if constexpr (std::is_pointer_v<T>) {
      string_key *_key = reinterpret_cast<string_key *>(key);
      for (int i = 0; i < 12; i += 4) {
        // if (CHECK_BIT(mask, i) && (strcmp(_[i].key, key) == 0)) {
        //  return _[i].value;
        //}
        if (CHECK_BIT(mask, i) &&
            (var_compare((reinterpret_cast<string_key *>(_[i].key))->key,
                         _key->key,
                         (reinterpret_cast<string_key *>(_[i].key))->length,
                         _key->length))) {
          return _[i].value;
        }

        if (CHECK_BIT(mask, i + 1) &&
            (var_compare((reinterpret_cast<string_key *>(_[i + 1].key))->key,
                         _key->key,
                         (reinterpret_cast<string_key *>(_[i + 1].key))->length,
                         _key->length))) {
          return _[i + 1].value;
        }

        if (CHECK_BIT(mask, i + 2) &&
            (var_compare((reinterpret_cast<string_key *>(_[i + 2].key))->key,
                         _key->key,
                         (reinterpret_cast<string_key *>(_[i + 2].key))->length,
                         _key->length))) {
          return _[i + 2].value;
        }

        if (CHECK_BIT(mask, i + 3) &&
            (var_compare((reinterpret_cast<string_key *>(_[i + 3].key))->key,
                         _key->key,
                         (reinterpret_cast<string_key *>(_[i + 3].key))->length,
                         _key->length))) {
          return _[i + 3].value;
        }
      }

      if (CHECK_BIT(mask, 12) &&
          (var_compare((reinterpret_cast<string_key *>(_[12].key))->key,
                       _key->key,
                       (reinterpret_cast<string_key *>(_[12].key))->length,
                       _key->length))) {
        return _[12].value;
      }

      if (CHECK_BIT(mask, 13) &&
          (var_compare((reinterpret_cast<string_key *>(_[13].key))->key,
                       _key->key,
                       (reinterpret_cast<string_key *>(_[13].key))->length,
                       _key->length))) {
        return _[13].value;
      }
    } else {
      /*loop unrolling*/
      for (int i = 0; i < 12; i += 4) {
        if (CHECK_BIT(mask, i) && (_[i].key == key)) {
          return _[i].value;
        }

        if (CHECK_BIT(mask, i + 1) && (_[i + 1].key == key)) {
          return _[i + 1].value;
        }

        if (CHECK_BIT(mask, i + 2) && (_[i + 2].key == key)) {
          return _[i + 2].value;
        }

        if (CHECK_BIT(mask, i + 3) && (_[i + 3].key == key)) {
          return _[i + 3].value;
        }
      }

      if (CHECK_BIT(mask, 12) && (_[12].key == key)) {
        return _[12].value;
      }

      if (CHECK_BIT(mask, 13) && (_[13].key == key)) {
        return _[13].value;
      }
    }
    return NONE;
  }

  inline void set_hash(int index, uint8_t meta_hash,
                       bool probe) /* Do I needs the atomic instruction????*/
  {
    finger_array[index] = meta_hash;
    auto new_bitmap = bitmap | (1 << (index + 4));
    assert(GET_COUNT(bitmap) < kNumPairPerBucket);
    new_bitmap += 1;
    bitmap = new_bitmap;
    // #ifdef PMEM
    // Allocator::NTWrite32(reinterpret_cast<uint32_t *>(&bitmap), new_bitmap);
    // #else
    // bitmap = new_bitmap;
    // #endif
    if (probe) {
      *((int *)membership) = (1 << index) | *((int *)membership);
    }
  }

  inline uint8_t get_hash(int index) { return finger_array[index]; }

  inline void unset_hash(int index, bool nt_flush = false) {
    auto new_bitmap = bitmap & (~(1 << (index + 4)));
    assert(GET_COUNT(bitmap) <= kNumPairPerBucket);
    assert(GET_COUNT(bitmap) > 0);
    new_bitmap -= 1;
#ifdef PMEM
    if (nt_flush) {
      Allocator::NTWrite32(reinterpret_cast<uint32_t *>(&bitmap), new_bitmap);
    } else {
      bitmap = new_bitmap;
    }
#else
    bitmap = new_bitmap;
#endif

    *((int *)membership) =
        (~(1 << index)) &
        (*((int *)membership)); /*since they are in the same cacheline,
                                   therefore no performance influence?*/
  }
  /*
    inline void get_lock() {
      auto old_value = version_lock & lockMask;
      auto new_value = version_lock | lockSet;
      while (!CAS(&version_lock, &old_value, new_value)) {
        old_value = version_lock & lockMask;
        new_value = version_lock | lockSet;
      }
    }

    inline bool try_get_lock() {
      auto old_value = version_lock & lockMask;
      auto new_value = version_lock | lockSet;
      return CAS(&version_lock, &old_value, new_value);
    }

    inline void release_lock() {
      auto old_value = version_lock;
      auto new_value = ((old_value & lockMask) + 1) & lockMask;

      while (!CAS(&version_lock, &old_value, new_value)) {
        old_value = version_lock;
        new_value = ((old_value & lockMask) + 1) & lockMask;
      }
    }
  */
  inline void get_lock() {
    uint32_t new_value = 0;
    uint32_t old_value = 0;
    do {
      while (true) {
        old_value = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
        if (!(old_value & lockSet)) {
          old_value &= lockMask;
          break;
        }
      }
      new_value = old_value | lockSet;
    } while (!CAS(&version_lock, &old_value, new_value));
  }

  inline bool try_get_lock() {
    uint32_t v = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    if (v & lockSet) {
      return false;
    }
    auto old_value = v & lockMask;
    auto new_value = v | lockSet;
    return CAS(&version_lock, &old_value, new_value);
  }

  inline void release_lock() {
    uint32_t v = version_lock;
    __atomic_store_n(&version_lock, v + 1 - lockSet, __ATOMIC_RELEASE);
  }

  /*if the lock is set, return true*/
  inline bool test_lock_set(uint32_t &version) {
    // auto value = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    // version = value & lockMask;
    // return (value & lockSet) != 0;
    version = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    return (version & lockSet) != 0;
  }

  // test whether the version has change, if change, return true
  inline bool test_lock_version_change(uint32_t old_version) {
    // auto value = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    // return ((value & lockSet) != 0) || ((value & lockMask) != old_version);
    auto value = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    return (old_version != value);
  }

  int Insert(T key, Value_t value, uint8_t meta_hash, bool probe) {
    auto slot = find_empty_slot();
    /* this branch can be optimized out*/
    assert(slot < kNumPairPerBucket);
    if (slot == -1) {
      printf("cannot find the empty slot, for key %llu\n", key);
      return -1;
    }
    _[slot].value = value;
    _[slot].key = key;
#ifdef PMEM
    Allocator::Persist(&_[slot], sizeof(_[slot]));
#endif
    mfence();
    set_hash(slot, meta_hash, probe);
    return 0;
  }

  /*if delete success, then return 0, else return -1*/
  int Delete(T key, uint8_t meta_hash, bool probe) {
    /*do the simd and check the key, then do the delete operation*/
    int mask = 0;
    SSE_CMP8(finger_array, meta_hash);
    if (!probe) {
      mask = mask & GET_BITMAP(bitmap) & ((~(*(int *)membership)) & allocMask);
    } else {
      mask = mask & GET_BITMAP(bitmap) & ((*(int *)membership) & allocMask);
    }
    /*loop unrolling*/
    if constexpr (std::is_pointer_v<T>) {
      string_key *_key = reinterpret_cast<string_key *>(key);
      /*loop unrolling*/
      if (mask != 0) {
        for (int i = 0; i < 12; i += 4) {
          if (CHECK_BIT(mask, i) &&
              (var_compare((reinterpret_cast<string_key *>(_[i].key))->key,
                           _key->key,
                           (reinterpret_cast<string_key *>(_[i].key))->length,
                           _key->length))) {
            unset_hash(i, false);
            return 0;
          }

          if (CHECK_BIT(mask, i + 1) &&
              (var_compare(
                  reinterpret_cast<string_key *>(_[i + 1].key)->key, _key->key,
                  (reinterpret_cast<string_key *>(_[i + 1].key))->length,
                  _key->length))) {
            unset_hash(i + 1, false);
            return 0;
          }

          if (CHECK_BIT(mask, i + 2) &&
              (var_compare(
                  reinterpret_cast<string_key *>(_[i + 2].key)->key, _key->key,
                  (reinterpret_cast<string_key *>(_[i + 2].key))->length,
                  _key->length))) {
            unset_hash(i + 2, false);
            return 0;
          }

          if (CHECK_BIT(mask, i + 3) &&
              (var_compare(
                  reinterpret_cast<string_key *>(_[i + 3].key)->key, _key->key,
                  (reinterpret_cast<string_key *>(_[i + 3].key))->length,
                  _key->length))) {
            unset_hash(i + 3, false);
            return 0;
          }
        }

        if (CHECK_BIT(mask, 12) &&
            (var_compare(reinterpret_cast<string_key *>(_[12].key)->key,
                         _key->key,
                         (reinterpret_cast<string_key *>(_[12].key))->length,
                         _key->length))) {
          unset_hash(12, false);
          return 0;
        }

        if (CHECK_BIT(mask, 13) &&
            (var_compare(reinterpret_cast<string_key *>(_[13].key)->key,
                         _key->key,
                         (reinterpret_cast<string_key *>(_[13].key))->length,
                         _key->length))) {
          unset_hash(13, false);
          return 0;
        }
      }

    } else {
      if (mask != 0) {
        for (int i = 0; i < 12; i += 4) {
          if (CHECK_BIT(mask, i) && (_[i].key == key)) {
            unset_hash(i, false);
            return 0;
          }

          if (CHECK_BIT(mask, i + 1) && (_[i + 1].key == key)) {
            unset_hash(i + 1, false);
            return 0;
          }

          if (CHECK_BIT(mask, i + 2) && (_[i + 2].key == key)) {
            unset_hash(i + 2, false);
            return 0;
          }

          if (CHECK_BIT(mask, i + 3) && (_[i + 3].key == key)) {
            unset_hash(i + 3, false);
            return 0;
          }
        }

        if (CHECK_BIT(mask, 12) && (_[12].key == key)) {
          unset_hash(12, false);
          return 0;
        }

        if (CHECK_BIT(mask, 13) && (_[13].key == key)) {
          unset_hash(13, false);
          return 0;
        }
      }
    }
    return -1;
  }

  int Insert_with_noflush(T key, Value_t value, uint8_t meta_hash, bool probe) {
    auto slot = find_empty_slot();
    /* this branch can be optimized out*/
    assert(slot < kNumPairPerBucket);
    if (slot == -1) {
      printf("cannot find the empty slot, for key %llu\n", key);
      return -1;
    }
    _[slot].value = value;
    _[slot].key = key;
    set_hash(slot, meta_hash, probe);
    return 0;
  }

  void Insert_displace(T key, Value_t value, uint8_t meta_hash, int slot,
                       bool probe) {
    _[slot].value = value;
    _[slot].key = key;
#ifdef PMEM
    Allocator::Persist(&_[slot], sizeof(_Pair<T>));
#endif
    mfence();
    set_hash(slot, meta_hash, probe);
  }

  void Insert_displace_with_noflush(T key, Value_t value, uint8_t meta_hash,
                                    int slot, bool probe) {
    _[slot].value = value;
    _[slot].key = key;
    set_hash(slot, meta_hash, probe);
  }

  /* Find the displacment element in this bucket*/
  /*
  int Find_displacement(int x){
          for (int i = 0; i < kNumPairPerBucket; ++i)
          {
                  auto key_hash = h(&_[i], sizeof(Key_t));
                  auto y = BUCKET_INDEX(key_hash);
                  if (x == y)
                  {
                          return i;
                  }
          }
          return -1;
  }*/

  inline int Find_org_displacement() {
    int mask = (~(*((int *)membership))) & allocMask;
    if (mask == 0) {
      return -1;
    }
    return __builtin_ctz(mask);
  }

  /*find element that it is in the probe*/
  inline int Find_probe_displacement() {
    int mask = (*((int *)membership)) & allocMask;
    if (mask == 0) {
      return -1;
    }
    return __builtin_ctz(mask);
  }

  /*suite of function to set/unset the flush state of the node*/
  inline void setPreNonFlush() {
    overflowMember = overflowMember | preNeighborSet;
  }

  inline void unsetPreNonFlush() {
    overflowMember = overflowMember & (~preNeighborSet);
  }

  /* If the non-flush is set, return true*/
  inline bool testPreNonFlush() {
    return ((overflowMember & preNeighborSet) != 0);
  }

  inline void setNextNonFlush() {
    overflowMember = overflowMember | nextNeighborSet;
  }

  inline void unsetNextNonFlush() {
    overflowMember = overflowMember & (~nextNeighborSet);
  }

  inline bool testNextNonFlush() {
    return ((overflowMember & nextNeighborSet) != 0);
  }

  inline void resetLock() { version_lock = 0; }

  inline void resetOverflowFP() {
    finger_array[18] = 0;
    finger_array[19] = 0;
    overflowMember = 0;
    overflowCount = 0;
    clear_stash_check();
  }

  uint32_t version_lock;
  int bitmap;               // allocation bitmap + pointer bitmao + counter
  uint8_t finger_array[20]; /*only use the first 14 bytes, can be accelerated by
                               SSE instruction,0-13 for finger, 14-17 for
                               overflowed, 18 as the bitmap, 19 as the overflow
                               bucket index indicator*/
  uint8_t membership[2];    /*Used to test whether the key originally belongs to
                               this bucket*/
  uint8_t overflowMember; /*overflowmember indicates membership of the overflow
                             fingerprint*/
  uint8_t overflowCount;

  _Pair<T> _[kNumPairPerBucket];
};

template <class T>
struct Table;

template <class T>
struct Directory {
  typedef Table<T> *table_p;
  uint32_t global_depth;
  uint32_t version;
  uint32_t depth_count;
  table_p _[0];

  Directory(size_t capacity, size_t _version) {
    version = _version;
    global_depth = static_cast<size_t>(log2(capacity));
    depth_count = 0;
  }

  static void New(PMEMoid *dir, size_t capacity, size_t version) {
#ifdef PMEM
    auto callback = [](PMEMobjpool *pool, void *ptr, void *arg) {
      auto value_ptr = reinterpret_cast<std::tuple<size_t, size_t> *>(arg);
      auto dir_ptr = reinterpret_cast<Directory *>(ptr);
      dir_ptr->version = std::get<1>(*value_ptr);
      dir_ptr->global_depth =
          static_cast<size_t>(log2(std::get<0>(*value_ptr)));
      size_t cap = std::get<0>(*value_ptr);
      // memset(&dir_ptr->_, 0, sizeof(table_p) * cap);
      pmemobj_persist(pool, dir_ptr,
                      sizeof(Directory<T>) + sizeof(uint64_t) * cap);
      return 0;
    };
    std::tuple callback_args = {capacity, version};
    Allocator::Allocate(dir, kCacheLineSize,
                        sizeof(Directory<T>) + sizeof(table_p) * capacity,
                        callback, reinterpret_cast<void *>(&callback_args));
#else
    Allocator::Allocate((void **)dir, kCacheLineSize, sizeof(Directory<T>));
    new (*dir) Directory(capacity, version, tables);
#endif
  }
};

/*thread local table allcoation pool*/
template <class T>
struct TlsTablePool {
  static Table<T> *all_tables;
  static PMEMoid p_all_tables;
  static std::atomic<uint32_t> all_allocated;
  static const uint32_t kAllTables = 327680;

  static void AllocateMore() {
    auto callback = [](PMEMobjpool *pool, void *ptr, void *arg) { return 0; };
    std::pair callback_para(0, nullptr);
    /*
    Allocator::Allocate((void **)&all_tables, kCacheLineSize, sizeof(Table<T>) *
    kAllTables, callback, reinterpret_cast<void *>(&callback_para));*/
    Allocator::Allocate(&p_all_tables, kCacheLineSize,
                        sizeof(Table<T>) * kAllTables, callback,
                        reinterpret_cast<void *>(&callback_para));
    all_tables = reinterpret_cast<Table<T> *>(pmemobj_direct(p_all_tables));
    memset((void *)all_tables, 0, sizeof(Table<T>) * kAllTables);
    all_allocated = 0;
    printf("MORE ");
  }

  TlsTablePool() {}
  static void Initialize() { AllocateMore(); }

  Table<T> *tables = nullptr;
  static const uint32_t kTables = 128;
  uint32_t allocated = kTables;

  void TlsPrepare() {
  retry:
    uint32_t n = all_allocated.fetch_add(kTables);
    if (n == kAllTables) {
      AllocateMore();
      printf("NO MORE\n");
      abort();
      goto retry;
    }
    tables = all_tables + n;
    allocated = 0;
  }

  Table<T> *Get() {
    if (allocated == kTables) {
      TlsPrepare();
    }
    return &tables[allocated++];
  }
};

template <class T>
std::atomic<uint32_t> TlsTablePool<T>::all_allocated(0);
template <class T>
Table<T> *TlsTablePool<T>::all_tables = nullptr;
template <class T>
PMEMoid TlsTablePool<T>::p_all_tables = OID_NULL;

/* the meta hash-table referenced by the directory*/
template <class T>
struct Table {
  // static void New(Table<T> **tbl, size_t depth, Table<T> *pp) {
  static void New(PMEMoid *tbl, size_t depth, PMEMoid pp) {
#ifdef PMEM
#ifdef PREALLOC
    thread_local TlsTablePool<T> tls_pool;
    auto ptr = tls_pool.Get();
    ptr->local_depth = depth;
    ptr->next = pp;
    *tbl = pmemobj_oid(ptr);
#else
    auto callback = [](PMEMobjpool *pool, void *ptr, void *arg) {
      auto value_ptr = reinterpret_cast<std::pair<size_t, PMEMoid> *>(arg);
      auto table_ptr = reinterpret_cast<Table<T> *>(ptr);
      table_ptr->local_depth = value_ptr->first;
      table_ptr->next = value_ptr->second;

      int sumBucket = kNumBucket + stashBucket;
      for (int i = 0; i < sumBucket; ++i) {
        auto curr_bucket = table_ptr->bucket + i;
        memset(curr_bucket, 0, 64);
      }

      pmemobj_persist(pool, table_ptr, sizeof(Table<T>));
      return 0;
    };
    std::pair callback_para(depth, pp);
    Allocator::Allocate(tbl, kCacheLineSize, sizeof(Table<T>), callback,
                        reinterpret_cast<void *>(&callback_para));
#endif
#else
    Allocator::Allocate((void **)tbl, kCacheLineSize, sizeof(Table<T>));
    (*tbl)->local_depth = depth;
    (*tbl)->next = pp;
#endif
  };
  ~Table(void) {}

  bool Acquire_and_verify(size_t _pattern) {
    bucket->get_lock();
    if (pattern != _pattern) {
      bucket->release_lock();
      return false;
    } else {
      return true;
    }
  }

  void Acquire_remaining_locks() {
    for (int i = 1; i < kNumBucket; ++i) {
      auto curr_bucket = bucket + i;
      curr_bucket->get_lock();
    }
  }

  void Release_all_locks() {
    for (int i = 0; i < kNumBucket; ++i) {
      auto curr_bucket = bucket + i;
      curr_bucket->release_lock();
    }
  }

  int Insert(T key, Value_t value, size_t key_hash, uint8_t meta_hash,
             Directory<T> **);
  void Insert4split(T key, Value_t value, size_t key_hash, uint8_t meta_hash);
  void Insert4splitWithCheck(T key, Value_t value, size_t key_hash,
                             uint8_t meta_hash);
  void Insert4merge(T key, Value_t value, size_t key_hash, uint8_t meta_hash,
                    bool flag = false);
  Table<T> *Split(size_t);
  void HelpSplit(Table<T> *);
  void Merge(Table<T> *, bool flag = false);
  int Delete(T key, size_t key_hash, uint8_t meta_hash, Directory<T> **_dir);
  int Next_displace(Bucket<T> *target, Bucket<T> *neighbor,
                    Bucket<T> *next_neighbor, T key, Value_t value,
                    uint8_t meta_hash) {
    int displace_index = neighbor->Find_org_displacement();
    if ((GET_COUNT(next_neighbor->bitmap) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in next bucket, the displaced key is %lld,
      // the new key is %lld\n", neighbor->_[displace_index].key, key);
      next_neighbor->Insert(neighbor->_[displace_index].key,
                            neighbor->_[displace_index].value,
                            neighbor->finger_array[displace_index], true);
      // neighbor->setNextNonFlush();
      next_neighbor->release_lock();
#ifdef PMEM
      Allocator::Persist(&next_neighbor->bitmap, sizeof(next_neighbor->bitmap));
#endif
      // neighbor->unsetNextNonFlush();
      neighbor->unset_hash(displace_index);
      neighbor->Insert_displace(key, value, meta_hash, displace_index, true);
      // target->setNextNonFlush();
      neighbor->release_lock();
#ifdef PMEM
      Allocator::Persist(&neighbor->bitmap, sizeof(neighbor->bitmap));
#endif
      // target->unsetNextNonFlush();
      target->release_lock();
#ifdef COUNTING
      __sync_fetch_and_add(&number, 1);
#endif
      return 0;
    }
    return -1;
  }

  int Prev_displace(Bucket<T> *target, Bucket<T> *prev_neighbor,
                    Bucket<T> *neighbor, T key, Value_t value,
                    uint8_t meta_hash) {
    int displace_index = target->Find_probe_displacement();
    if ((GET_COUNT(prev_neighbor->bitmap) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in previous bucket,the displaced key is
      // %lld, the new key is %lld\n", target->_[displace_index].key, key);
      prev_neighbor->Insert(target->_[displace_index].key,
                            target->_[displace_index].value,
                            target->finger_array[displace_index], false);
      // target->setPreNonFlush();
      prev_neighbor->release_lock();
#ifdef PMEM
      Allocator::Persist(&prev_neighbor->bitmap, sizeof(prev_neighbor->bitmap));
#endif
      // target->unsetPreNonFlush();
      target->unset_hash(displace_index);
      target->Insert_displace(key, value, meta_hash, displace_index, false);
      // neighbor->setPreNonFlush();
      target->release_lock();
#ifdef PMEM
      Allocator::Persist(&target->bitmap, sizeof(target->bitmap));
#endif
      // neighbor->unsetPreNonFlush();
      neighbor->release_lock();
#ifdef COUNTING
      __sync_fetch_and_add(&number, 1);
#endif
      return 0;
    }
    return -1;
  }

  int Stash_insert(Bucket<T> *target, Bucket<T> *neighbor, T key, Value_t value,
                   uint8_t meta_hash, int stash_pos) {
    for (int i = 0; i < stashBucket; ++i) {
      Bucket<T> *curr_bucket =
          bucket + kNumBucket + ((stash_pos + i) & stashMask);
      if (GET_COUNT(curr_bucket->bitmap) < kNumPairPerBucket) {
        // printf("insertion in the stash for key %lld\n", key);
        curr_bucket->Insert(key, value, meta_hash, false);
#ifdef PMEM
        Allocator::Persist(&curr_bucket->bitmap, sizeof(curr_bucket->bitmap));
#endif
        target->set_indicator(meta_hash, neighbor, (stash_pos + i) & stashMask);
#ifdef COUNTING
        __sync_fetch_and_add(&number, 1);
#endif
        return 0;
      }
    }
    return -1;
  }

  void recoverMetadata() {
    Bucket<T> *curr_bucket, *neighbor_bucket;
    /*reset the lock and overflow meta-data*/
    uint64_t knumber = 0;
    for (int i = 0; i < kNumBucket; ++i) {
      // printf("start: recover bucket %d\n",i);
      curr_bucket = bucket + i;
      curr_bucket->resetLock();
      curr_bucket->resetOverflowFP();
      neighbor_bucket = bucket + ((i + 1) & bucketMask);
      for (int j = 0; j < kNumPairPerBucket; ++j) {
        int mask = curr_bucket->get_current_mask();
        if (CHECK_BIT(mask, j) && (neighbor_bucket->check_and_get(
                                       curr_bucket->finger_array[j],
                                       curr_bucket->_[j].key, true) != NONE)) {
          curr_bucket->unset_hash(j);
        }
      }

#ifdef COUNTING
      knumber += __builtin_popcount(GET_BITMAP(curr_bucket->bitmap));
#endif
    }

    /*scan the stash buckets and re-insert the overflow FP to initial buckets*/
    for (int i = 0; i < stashBucket; ++i) {
      curr_bucket = bucket + kNumBucket + i;
      curr_bucket->resetLock();
#ifdef COUNTING
      knumber += __builtin_popcount(GET_BITMAP(curr_bucket->bitmap));
#endif
      uint64_t key_hash;
      auto mask = GET_BITMAP(curr_bucket->bitmap);
      for (int j = 0; j < kNumPairPerBucket; ++j) {
        if (CHECK_BIT(mask, j)) {
          if constexpr (std::is_pointer_v<T>) {
            auto curr_key = curr_bucket->_[j].key;
            key_hash = h(curr_key->key, curr_key->length);
          } else {
            key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
          }
          /*compute the initial bucket*/
          auto bucket_ix = BUCKET_INDEX(key_hash);
          auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
          auto org_bucket = bucket + bucket_ix;
          auto neighbor_bucket = bucket + ((bucket_ix + 1) & bucketMask);
          org_bucket->set_indicator(meta_hash, neighbor_bucket, i);
        }
      }
    }
#ifdef COUNTING
    number = knumber;
#endif
    /* No need to flush these meta-data because persistent or not does not
     * influence the correctness*/
    /*fix the duplicates between the neighbor buckets*/
  }

  char dummy[48];
  Bucket<T> bucket[kNumBucket + stashBucket];
  size_t local_depth;
  size_t pattern;
  int number;
  PMEMoid next;
  int state; /*-1 means this bucket is merging, -2 means this bucket is
                splitting, so we cannot count the depth_count on it during
                scanning operation*/
  int dirty_bit;
  int lock_bit;
};

/* it needs to verify whether this bucket has been deleted...*/
template <class T>
int Table<T>::Insert(T key, Value_t value, size_t key_hash, uint8_t meta_hash,
                     Directory<T> **_dir) {
RETRY:
  /*we need to first do the locking and then do the verify*/
  auto y = BUCKET_INDEX(key_hash);
  Bucket<T> *target = bucket + y;
  Bucket<T> *neighbor = bucket + ((y + 1) & bucketMask);
  // printf("for key %lld, target bucket is %lld, meta_hash is %d\n", key,
  target->get_lock();
  if (!neighbor->try_get_lock()) {
    target->release_lock();
    return -2;
  }

  auto old_sa = *_dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  if (old_sa->_[x] != this) /* verify process*/
  {
    neighbor->release_lock();
    target->release_lock();
    return -2;
  }

  /*unique check, needs to check 2 hash table*/
  auto ret =
      target->unique_check(meta_hash, key, neighbor, bucket + kNumBucket);
  if (ret == -1) {
    neighbor->release_lock();
    target->release_lock();
    return 0;
  }

  if (((GET_COUNT(target->bitmap)) == kNumPairPerBucket) &&
      ((GET_COUNT(neighbor->bitmap)) == kNumPairPerBucket)) {
    Bucket<T> *next_neighbor = bucket + ((y + 2) & bucketMask);
    // Next displacement
    if (!next_neighbor->try_get_lock()) {
      neighbor->release_lock();
      target->release_lock();
      return -2;
    }
    auto ret =
        Next_displace(target, neighbor, next_neighbor, key, value, meta_hash);
    if (ret == 0) {
      return 0;
    }
    next_neighbor->release_lock();

    Bucket<T> *prev_neighbor;
    int prev_index;
    if (y == 0) {
      prev_neighbor = bucket + kNumBucket - 1;
      prev_index = kNumBucket - 1;
    } else {
      prev_neighbor = bucket + y - 1;
      prev_index = y - 1;
    }
    if (!prev_neighbor->try_get_lock()) {
      target->release_lock();
      neighbor->release_lock();
      return -2;
    }

    ret = Prev_displace(target, prev_neighbor, neighbor, key, value, meta_hash);
    if (ret == 0) {
      return 0;
    }

    Bucket<T> *stash = bucket + kNumBucket;
    if (!stash->try_get_lock()) {
      neighbor->release_lock();
      target->release_lock();
      prev_neighbor->release_lock();
      return -2;
    }
    ret = Stash_insert(target, neighbor, key, value, meta_hash, y & stashMask);

    stash->release_lock();
    neighbor->release_lock();
    target->release_lock();
    prev_neighbor->release_lock();
    return ret;
  }

  if (GET_COUNT(target->bitmap) <= GET_COUNT(neighbor->bitmap)) {
    target->Insert(key, value, meta_hash, false);
    // neighbor->setPreNonFlush();
    target->release_lock();
#ifdef PMEM
    Allocator::Persist(&target->bitmap, sizeof(target->bitmap));
#endif
    // neighbor->unsetPreNonFlush();
    neighbor->release_lock();
  } else {
    neighbor->Insert(key, value, meta_hash, true);
    // target->setNextNonFlush();
    neighbor->release_lock();
#ifdef PMEM
    Allocator::Persist(&neighbor->bitmap, sizeof(neighbor->bitmap));
#endif
    // target->unsetNextNonFlush();
    target->release_lock();
  }
#ifdef COUNTING
  __sync_fetch_and_add(&number, 1);
#endif
  return 0;
}

template <class T>
void Table<T>::Insert4splitWithCheck(T key, Value_t value, size_t key_hash,
                                     uint8_t meta_hash) {
  auto y = BUCKET_INDEX(key_hash);
  Bucket<T> *target = bucket + y;
  Bucket<T> *neighbor = bucket + ((y + 1) & bucketMask);
  auto ret =
      target->unique_check(meta_hash, key, neighbor, bucket + kNumBucket);
  if (ret == -1) return;
  Bucket<T> *insert_target;
  bool probe = false;
  if (GET_COUNT(target->bitmap) <= GET_COUNT(neighbor->bitmap)) {
    insert_target = target;
  } else {
    insert_target = neighbor;
    probe = true;
  }

  /*some bucket may be overflowed?*/
  if (GET_COUNT(insert_target->bitmap) < kNumPairPerBucket) {
    insert_target->_[GET_COUNT(insert_target->bitmap)].key = key;
    insert_target->_[GET_COUNT(insert_target->bitmap)].value = value;
    insert_target->set_hash(GET_COUNT(insert_target->bitmap), meta_hash, probe);
#ifdef COUNTING
    ++number;
#endif
  } else {
    /*do the displacement or insertion in the stash*/
    Bucket<T> *next_neighbor = bucket + ((y + 2) & bucketMask);
    int displace_index;
    displace_index = neighbor->Find_org_displacement();
    if (((GET_COUNT(next_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in next bucket, the displaced key is %lld,
      // the new key is %lld\n", neighbor->_[displace_index].key, key);
      next_neighbor->Insert_with_noflush(
          neighbor->_[displace_index].key, neighbor->_[displace_index].value,
          neighbor->finger_array[displace_index], true);
      neighbor->unset_hash(displace_index);
      neighbor->Insert_displace_with_noflush(key, value, meta_hash,
                                             displace_index, true);
#ifdef COUNTING
      ++number;
#endif
      return;
    }
    Bucket<T> *prev_neighbor;
    int prev_index;
    if (y == 0) {
      prev_neighbor = bucket + kNumBucket - 1;
      prev_index = kNumBucket - 1;
    } else {
      prev_neighbor = bucket + y - 1;
      prev_index = y - 1;
    }

    displace_index = target->Find_probe_displacement();
    if (((GET_COUNT(prev_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in previous bucket,the displaced key is
      // %lld, the new key is %lld\n", target->_[displace_index].key, key);
      prev_neighbor->Insert_with_noflush(
          target->_[displace_index].key, target->_[displace_index].value,
          target->finger_array[displace_index], false);
      target->unset_hash(displace_index);
      target->Insert_displace_with_noflush(key, value, meta_hash,
                                           displace_index, false);
#ifdef COUNTING
      ++number;
#endif
      return;
    }

    Stash_insert(target, neighbor, key, value, meta_hash, y & stashMask);
  }
}

/*the insert needs to be perfectly balanced, not destory the power of balance*/
template <class T>
void Table<T>::Insert4split(T key, Value_t value, size_t key_hash,
                            uint8_t meta_hash) {
  auto y = BUCKET_INDEX(key_hash);
  Bucket<T> *target = bucket + y;
  Bucket<T> *neighbor = bucket + ((y + 1) & bucketMask);
  // auto insert_target =
  // (target->count&lowCountMask)<=(neighbor->count&lowCountMask)?target:neighbor;
  Bucket<T> *insert_target;
  bool probe = false;
  if (GET_COUNT(target->bitmap) <= GET_COUNT(neighbor->bitmap)) {
    insert_target = target;
  } else {
    insert_target = neighbor;
    probe = true;
  }

  // assert(insert_target->count < kNumPairPerBucket);
  /*some bucket may be overflowed?*/
  if (GET_COUNT(insert_target->bitmap) < kNumPairPerBucket) {
    insert_target->_[GET_COUNT(insert_target->bitmap)].key = key;
    insert_target->_[GET_COUNT(insert_target->bitmap)].value = value;
    insert_target->set_hash(GET_COUNT(insert_target->bitmap), meta_hash, probe);
#ifdef COUNTING
    ++number;
#endif
  } else {
    /*do the displacement or insertion in the stash*/
    Bucket<T> *next_neighbor = bucket + ((y + 2) & bucketMask);
    int displace_index;
    displace_index = neighbor->Find_org_displacement();
    if (((GET_COUNT(next_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in next bucket, the displaced key is %lld,
      // the new key is %lld\n", neighbor->_[displace_index].key, key);
      next_neighbor->Insert_with_noflush(
          neighbor->_[displace_index].key, neighbor->_[displace_index].value,
          neighbor->finger_array[displace_index], true);
      neighbor->unset_hash(displace_index);
      neighbor->Insert_displace_with_noflush(key, value, meta_hash,
                                             displace_index, true);
#ifdef COUNTING
      ++number;
#endif
      return;
    }
    Bucket<T> *prev_neighbor;
    int prev_index;
    if (y == 0) {
      prev_neighbor = bucket + kNumBucket - 1;
      prev_index = kNumBucket - 1;
    } else {
      prev_neighbor = bucket + y - 1;
      prev_index = y - 1;
    }

    displace_index = target->Find_probe_displacement();
    if (((GET_COUNT(prev_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in previous bucket,the displaced key is
      // %lld, the new key is %lld\n", target->_[displace_index].key, key);
      prev_neighbor->Insert_with_noflush(
          target->_[displace_index].key, target->_[displace_index].value,
          target->finger_array[displace_index], false);
      target->unset_hash(displace_index);
      target->Insert_displace_with_noflush(key, value, meta_hash,
                                           displace_index, false);
#ifdef COUNTING
      ++number;
#endif
      return;
    }

    Stash_insert(target, neighbor, key, value, meta_hash, y & stashMask);
  }
}

template <class T>
void Table<T>::Insert4merge(T key, Value_t value, size_t key_hash,
                            uint8_t meta_hash, bool unique_check_flag) {
  auto y = BUCKET_INDEX(key_hash);
  Bucket<T> *target = bucket + y;
  Bucket<T> *neighbor = bucket + ((y + 1) & bucketMask);

  if (unique_check_flag) {
    auto ret =
        target->unique_check(meta_hash, key, neighbor, bucket + kNumBucket);
    if (ret == -1) return;
  }

  // auto insert_target =
  // (target->count&lowCountMask)<=(neighbor->count&lowCountMask)?target:neighbor;
  Bucket<T> *insert_target;
  bool probe = false;
  if (GET_COUNT(target->bitmap) <= GET_COUNT(neighbor->bitmap)) {
    insert_target = target;
  } else {
    insert_target = neighbor;
    probe = true;
  }

  // assert(insert_target->count < kNumPairPerBucket);
  /*some bucket may be overflowed?*/
  if (GET_COUNT(insert_target->bitmap) < kNumPairPerBucket) {
    insert_target->Insert(key, value, meta_hash, probe);
#ifdef COUNTING
    ++number;
#endif
  } else {
    /*do the displacement or insertion in the stash*/
    Bucket<T> *next_neighbor = bucket + ((y + 2) & bucketMask);
    int displace_index;
    displace_index = neighbor->Find_org_displacement();
    if (((GET_COUNT(next_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in next bucket, the displaced key is %lld,
      // the new key is %lld\n", neighbor->_[displace_index].key, key);
      next_neighbor->Insert_with_noflush(
          neighbor->_[displace_index].key, neighbor->_[displace_index].value,
          neighbor->finger_array[displace_index], true);
      neighbor->unset_hash(displace_index);
      neighbor->Insert_displace_with_noflush(key, value, meta_hash,
                                             displace_index, true);
#ifdef COUNTING
      ++number;
#endif
      return;
    }
    Bucket<T> *prev_neighbor;
    int prev_index;
    if (y == 0) {
      prev_neighbor = bucket + kNumBucket - 1;
      prev_index = kNumBucket - 1;
    } else {
      prev_neighbor = bucket + y - 1;
      prev_index = y - 1;
    }

    displace_index = target->Find_probe_displacement();
    if (((GET_COUNT(prev_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in previous bucket,the displaced key is
      // %lld, the new key is %lld\n", target->_[displace_index].key, key);
      prev_neighbor->Insert_with_noflush(
          target->_[displace_index].key, target->_[displace_index].value,
          target->finger_array[displace_index], false);
      target->unset_hash(displace_index);
      target->Insert_displace_with_noflush(key, value, meta_hash,
                                           displace_index, false);
#ifdef COUNTING
      ++number;
#endif
      return;
    }

    Stash_insert(target, neighbor, key, value, meta_hash, y & stashMask);
  }
}

template <class T>
void Table<T>::HelpSplit(Table<T> *next_table) {
  size_t new_pattern = (pattern << 1) + 1;
  size_t old_pattern = pattern << 1;

  size_t key_hash;
  int invalid_array[kNumBucket + stashBucket];
  for (int i = 0; i < kNumBucket; ++i) {
    auto *curr_bucket = bucket + i;
    auto mask = GET_BITMAP(curr_bucket->bitmap);
    int invalid_mask = 0;
    for (int j = 0; j < kNumPairPerBucket; ++j) {
      if (CHECK_BIT(mask, j)) {
        if constexpr (std::is_pointer_v<T>) {
          auto curr_key = curr_bucket->_[j].key;
          key_hash = h(curr_key->key, curr_key->length);
        } else {
          key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
        }

        if ((key_hash >> (64 - local_depth - 1)) == new_pattern) {
          invalid_mask = invalid_mask | (1 << (j + 4));
          next_table->Insert4splitWithCheck(
              curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
              curr_bucket->finger_array[j]); /*this shceme may destory the
                                                balanced segment*/
                                             // curr_bucket->unset_hash(j);
#ifdef COUNTING
          number--;
#endif
        }
      }
    }
    invalid_array[i] = invalid_mask;
  }

  for (int i = 0; i < stashBucket; ++i) {
    auto *curr_bucket = bucket + kNumBucket + i;
    auto mask = GET_BITMAP(curr_bucket->bitmap);
    int invalid_mask = 0;
    for (int j = 0; j < kNumPairPerBucket; ++j) {
      if (CHECK_BIT(mask, j)) {
        if constexpr (std::is_pointer_v<T>) {
          auto curr_key = curr_bucket->_[j].key;
          key_hash = h(curr_key->key, curr_key->length);
        } else {
          key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
        }
        if ((key_hash >> (64 - local_depth - 1)) == new_pattern) {
          invalid_mask = invalid_mask | (1 << (j + 4));
          next_table->Insert4splitWithCheck(
              curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
              curr_bucket->finger_array[j]); /*this shceme may destory the
                                                balanced segment*/
          auto bucket_ix = BUCKET_INDEX(key_hash);
          auto org_bucket = bucket + bucket_ix;
          auto neighbor_bucket = bucket + ((bucket_ix + 1) & bucketMask);
          org_bucket->unset_indicator(curr_bucket->finger_array[j],
                                      neighbor_bucket, curr_bucket->_[j].key,
                                      i);
          // curr_bucket->unset_hash(j);
#ifdef COUNTING
          number--;
#endif
        }
      }
    }
    invalid_array[kNumBucket + i] = invalid_mask;
  }
  next_table->pattern = new_pattern;
  Allocator::Persist(&next_table->pattern, sizeof(next_table->pattern));
  pattern = old_pattern;
  Allocator::Persist(&pattern, sizeof(pattern));

#ifdef PMEM
  Allocator::Persist(next_table, sizeof(Table));
  size_t sumBucket = kNumBucket + stashBucket;
  for (int i = 0; i < sumBucket; ++i) {
    auto curr_bucket = bucket + i;
    curr_bucket->bitmap = curr_bucket->bitmap & (~invalid_array[i]);
    auto count = __builtin_popcount(invalid_array[i]);
    curr_bucket->bitmap = curr_bucket->bitmap - count;

    *((int *)curr_bucket->membership) =
        (~(invalid_array[i] >> 4)) &
        (*((int *)
               curr_bucket->membership)); /*since they are in the same
                                cacheline, therefore no performance influence?*/
  }

  // if constexpr (std::is_pointer_v<T>) {
  Allocator::Persist(this, sizeof(Table));
  //}
#endif
}

template <class T>
Table<T> *Table<T>::Split(size_t _key_hash) {
  size_t new_pattern = (pattern << 1) + 1;
  size_t old_pattern = pattern << 1;

  for (int i = 1; i < kNumBucket; ++i) {
    (bucket + i)->get_lock();
  }
  // printf("my pattern is %lld, my load factor is %f\n", pattern,
  // ((double)number)/(kNumBucket*kNumPairPerBucket+kNumPairPerBucket));
  state = -2;
  Allocator::Persist(&state, sizeof(state));
  Table<T>::New(&next, local_depth + 1, next);
  Table<T> *next_table = reinterpret_cast<Table<T> *>(pmemobj_direct(next));

  next_table->state = -2;
  Allocator::Persist(&next_table->state, sizeof(next_table->state));
  next_table->bucket
      ->get_lock(); /* get the first lock of the new bucket to avoid it
                 is operated(split or merge) by other threads*/
  size_t key_hash;
  int invalid_array[kNumBucket + stashBucket];
  for (int i = 0; i < kNumBucket; ++i) {
    auto *curr_bucket = bucket + i;
    auto mask = GET_BITMAP(curr_bucket->bitmap);
    int invalid_mask = 0;
    for (int j = 0; j < kNumPairPerBucket; ++j) {
      if (CHECK_BIT(mask, j)) {
        if constexpr (std::is_pointer_v<T>) {
          auto curr_key = curr_bucket->_[j].key;
          key_hash = h(curr_key->key, curr_key->length);
        } else {
          key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
        }

        if ((key_hash >> (64 - local_depth - 1)) == new_pattern) {
          invalid_mask = invalid_mask | (1 << (j + 4));
          next_table->Insert4split(
              curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
              curr_bucket->finger_array[j]); /*this shceme may destory the
                                                balanced segment*/
                                             // curr_bucket->unset_hash(j);
#ifdef COUNTING
          number--;
#endif
        }
      }
    }
    invalid_array[i] = invalid_mask;
  }

  for (int i = 0; i < stashBucket; ++i) {
    auto *curr_bucket = bucket + kNumBucket + i;
    auto mask = GET_BITMAP(curr_bucket->bitmap);
    int invalid_mask = 0;
    for (int j = 0; j < kNumPairPerBucket; ++j) {
      if (CHECK_BIT(mask, j)) {
        if constexpr (std::is_pointer_v<T>) {
          auto curr_key = curr_bucket->_[j].key;
          key_hash = h(curr_key->key, curr_key->length);
        } else {
          key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
        }
        if ((key_hash >> (64 - local_depth - 1)) == new_pattern) {
          invalid_mask = invalid_mask | (1 << (j + 4));
          next_table->Insert4split(
              curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
              curr_bucket->finger_array[j]); /*this shceme may destory the
                                                balanced segment*/
          auto bucket_ix = BUCKET_INDEX(key_hash);
          auto org_bucket = bucket + bucket_ix;
          auto neighbor_bucket = bucket + ((bucket_ix + 1) & bucketMask);
          org_bucket->unset_indicator(curr_bucket->finger_array[j],
                                      neighbor_bucket, curr_bucket->_[j].key,
                                      i);
          // curr_bucket->unset_hash(j);
#ifdef COUNTING
          number--;
#endif
        }
      }
    }
    invalid_array[kNumBucket + i] = invalid_mask;
  }
  next_table->pattern = new_pattern;
  Allocator::Persist(&next_table->pattern, sizeof(next_table->pattern));
  pattern = old_pattern;
  Allocator::Persist(&pattern, sizeof(pattern));

#ifdef PMEM
  Allocator::Persist(next_table, sizeof(Table));
  size_t sumBucket = kNumBucket + stashBucket;
  for (int i = 0; i < sumBucket; ++i) {
    auto curr_bucket = bucket + i;
    curr_bucket->bitmap = curr_bucket->bitmap & (~invalid_array[i]);
    auto count = __builtin_popcount(invalid_array[i]);
    curr_bucket->bitmap = curr_bucket->bitmap - count;

    *((int *)curr_bucket->membership) =
        (~(invalid_array[i] >> 4)) &
        (*((int *)
               curr_bucket->membership)); 
  }

  // if constexpr (std::is_pointer_v<T>) {
  Allocator::Persist(this, sizeof(Table));
  //}
#endif
  return next_table;
}

template <class T>
void Table<T>::Merge(Table<T> *neighbor, bool unique_check_flag) {
  /*Restore the split/merge procedure*/
  if (unique_check_flag) {
    size_t key_hash;
    for (int i = 0; i < kNumBucket; ++i) {
      auto *curr_bucket = neighbor->bucket + i;
      auto mask = GET_BITMAP(curr_bucket->bitmap);
      for (int j = 0; j < kNumPairPerBucket; ++j) {
        if (CHECK_BIT(mask, j)) {
          if constexpr (std::is_pointer_v<T>) {
            auto curr_key = curr_bucket->_[j].key;
            key_hash = h(curr_key->key, curr_key->length);
          } else {
            key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
          }

          Insert4merge(curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
                       curr_bucket->finger_array[j],
                       true); /*this shceme may destory
                           the balanced segment*/
        }
      }
    }

    /*split the stash bucket, the stash must be full, right?*/
    for (int i = 0; i < stashBucket; ++i) {
      auto *curr_bucket = neighbor->bucket + kNumBucket + i;
      auto mask = GET_BITMAP(curr_bucket->bitmap);
      for (int j = 0; j < kNumPairPerBucket; ++j) {
        if (CHECK_BIT(mask, j)) {
          if constexpr (std::is_pointer_v<T>) {
            auto curr_key = curr_bucket->_[j].key;
            key_hash = h(curr_key->key, curr_key->length);
          } else {
            key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
          }
          Insert4merge(curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
                       curr_bucket->finger_array[j], true); /*this shceme may destory
                                                         the balanced segment*/
        }
      }
    }

  } else {
    size_t key_hash;
    for (int i = 0; i < kNumBucket; ++i) {
      auto *curr_bucket = neighbor->bucket + i;
      auto mask = GET_BITMAP(curr_bucket->bitmap);
      for (int j = 0; j < kNumPairPerBucket; ++j) {
        if (CHECK_BIT(mask, j)) {
          if constexpr (std::is_pointer_v<T>) {
            auto curr_key = curr_bucket->_[j].key;
            key_hash = h(curr_key->key, curr_key->length);
          } else {
            key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
          }

          Insert4merge(curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
                       curr_bucket->finger_array[j]); /*this shceme may destory
                                                         the balanced segment*/
        }
      }
    }

    /*split the stash bucket, the stash must be full, right?*/
    for (int i = 0; i < stashBucket; ++i) {
      auto *curr_bucket = neighbor->bucket + kNumBucket + i;
      auto mask = GET_BITMAP(curr_bucket->bitmap);
      for (int j = 0; j < kNumPairPerBucket; ++j) {
        if (CHECK_BIT(mask, j)) {
          if constexpr (std::is_pointer_v<T>) {
            auto curr_key = curr_bucket->_[j].key;
            key_hash = h(curr_key->key, curr_key->length);
          } else {
            key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
          }
          Insert4merge(curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
                       curr_bucket->finger_array[j]); /*this shceme may destory
                                                         the balanced segment*/
        }
      }
    }
  }
}

template <class T>
class Finger_EH : public Hash<T> {
 public:
  Finger_EH(void);
  Finger_EH(size_t, PMEMobjpool *_pool);
  ~Finger_EH(void);
  void Insert(T key, Value_t, int is_crash);
  inline void Insert(T key, Value_t value);
  void Insert(T key, Value_t value, bool);
  inline bool Delete(T);
  bool Delete(T, bool);
  inline Value_t Get(T);
  Value_t Get(T key, bool is_in_epoch);
  void TryMerge(uint64_t);
  void Directory_Doubling(int x, Table<T> *new_b);
  void Directory_Merge_Update(Directory<T> *_sa, uint64_t key_hash,
                              Table<T> *left_seg);
  void Directory_Update(Directory<T> *_sa, int x, Table<T> *new_b);
  void Halve_Directory();
  void Lock_Directory();
  void Unlock_Directory();
  int FindAnyway(T key);
  void CheckDepthCount();
  void getNumber() {
    printf("the size of the bucket is %lld\n", sizeof(struct Bucket<T>));
    // printf("the size of the Table is %lld\n", sizeof(struct Table));

    size_t _count = 0;
    size_t seg_count = 0;
    Directory<T> *seg = dir;
    Table<T> **dir_entry = seg->_;
    Table<T> *ss;
    auto global_depth = seg->global_depth;
    size_t depth_diff;
    int capacity = pow(2, global_depth);
    for (int i = 0; i < capacity;) {
      ss = dir_entry[i];
      depth_diff = global_depth - ss->local_depth;
      _count += ss->number;
      seg_count++;
      i += pow(2, depth_diff);
    }

    ss = dir_entry[0];
    uint64_t verify_seg_count = 1;
    while (!OID_IS_NULL(ss->next)) {
      verify_seg_count++;
      ss = reinterpret_cast<Table<T> *>(pmemobj_direct(ss->next));
    }
    std::cout << "seg_count = " << seg_count << std::endl;
    std::cout << "verify_seg_count = " << verify_seg_count << std::endl;

    printf("#items: %lld\n", _count);
    // printf("the segment number in this hash table is %lld\n", seg_count);
    printf("load_factor: %f\n",
           (double)_count / (seg_count * kNumPairPerBucket * (kNumBucket + 2)));
    printf("Raw_Space: %f\n",
           (double)(_count * 16) / (seg_count * sizeof(Table<T>)));
  }

  void recoverTable(Table<T> **target_table, size_t);
  void Recovery();

  inline bool Acquire(void) {
    int unlocked = 0;
    return CAS(&lock, &unlocked, 1);
  }

  inline bool Release(void) {
    int locked = 1;
    return CAS(&lock, &locked, 0);
  }

  inline int Test_Directory_Lock_Set(void) {
    return __atomic_load_n(&lock, __ATOMIC_ACQUIRE);
  }

  Directory<T> *dir;
  int lock;
#ifdef PMEM
  PMEMobjpool *pool_addr;
  /* directory allocation will write to here first,
   * in oder to perform safe directory allocation
   * */
  PMEMoid back_dir;
#endif
};

template <class T>
Finger_EH<T>::Finger_EH(size_t initCap, PMEMobjpool *_pool) {
  pool_addr = _pool;
  Directory<T>::New(&back_dir, initCap, 0);
  dir = reinterpret_cast<Directory<T> *>(pmemobj_direct(back_dir));
  back_dir = OID_NULL;
  lock = 0;
  PMEMoid ptr;
  Table<T>::New(&ptr, dir->global_depth, OID_NULL);
  dir->_[initCap - 1] = (Table<T> *)pmemobj_direct(ptr);

  dir->_[initCap - 1]->pattern = initCap - 1;
  /* Initilize the Directory*/
  for (int i = initCap - 2; i >= 0; --i) {
    Table<T>::New(&ptr, dir->global_depth, ptr);
    dir->_[i] = (Table<T> *)pmemobj_direct(ptr);
    dir->_[i]->pattern = i;
  }
  dir->depth_count = initCap;
}

template <class T>
Finger_EH<T>::Finger_EH() {
  std::cout << "Reinitialize up" << std::endl;
}

template <class T>
Finger_EH<T>::~Finger_EH(void) {
  // TO-DO
}

template <class T>
void Finger_EH<T>::Lock_Directory() {
  while (!Acquire()) {
    asm("nop");
  }
}

template <class T>
void Finger_EH<T>::Unlock_Directory() {
  while (!Release()) {
    asm("nop");
  }
}

template <class T>
void Finger_EH<T>::Halve_Directory() {
  printf("Begin::Directory_Halving towards %lld\n", dir->global_depth);
  auto d = dir->_;

  Directory<T> *new_dir;
#ifdef PMEM
  Directory<T>::New(&back_dir, pow(2, dir->global_depth - 1), dir->version + 1);
  new_dir = reinterpret_cast<Directory<T> *>(pmemobj_direct(back_dir));
#else
  Directory<T>::New(&new_dir, pow(2, dir->global_depth - 1), dir->version + 1);
#endif

  auto _dir = new_dir->_;
  new_dir->depth_count = 0;
  auto capacity = pow(2, new_dir->global_depth);
  bool skip = false;
  for (int i = 0; i < capacity; ++i) {
    _dir[i] = d[2 * i];
    assert(d[2 * i] == d[2 * i + 1]);
    if (!skip) {
      if ((_dir[i]->local_depth == (dir->global_depth - 1)) &&
          (_dir[i]->state != -2)) {
        if (_dir[i]->state != -1) {
          new_dir->depth_count += 1;
        } else {
          skip = true;
        }
      }
    } else {
      skip = false;
    }
  }

#ifdef PMEM
  Allocator::Persist(new_dir,
                     sizeof(Directory<T>) + sizeof(uint64_t) * capacity);

  // auto old_dir = dir;
  void **reserve_addr = Allocator::ReserveMemory();
  TX_BEGIN(pool_addr) {
    pmemobj_tx_add_range_direct(reserve_addr, sizeof(void *));
    pmemobj_tx_add_range_direct(&dir, sizeof(dir));
    pmemobj_tx_add_range_direct(&back_dir, sizeof(back_dir));
    *reserve_addr = (void *)dir;
    dir = new_dir;
    back_dir = OID_NULL;
  }
  TX_ONABORT {
    std::cout << "TXN fails during halvling directory" << std::endl;
  }
  TX_END
/*
#ifdef EPOCH
  Allocator::Free(old_dir);
#endif
*/
#else
  dir = new_dir;
#endif
  printf("End::Directory_Halving towards %lld\n", dir->global_depth);
}

template <class T>
void Finger_EH<T>::Directory_Doubling(int x, Table<T> *new_b) {
  Table<T> **d = dir->_;
  auto global_depth = dir->global_depth;
  printf("Directory_Doubling towards %lld\n", global_depth + 1);

  std::cout << "current position = "<< x << std::endl;
  std::cout << "new position = " << 2*x << std::endl; 
  auto capacity = pow(2, global_depth);
  Directory<T>::New(&back_dir, 2 * capacity, dir->version + 1);
  Directory<T> *new_sa =
      reinterpret_cast<Directory<T> *>(pmemobj_direct(back_dir));
  auto dd = new_sa->_;

  for (unsigned i = 0; i < capacity; ++i) {
    dd[2 * i] = d[i];
    dd[2 * i + 1] = d[i];
  }
  dd[2 * x + 1] = new_b;
  new_sa->depth_count = 2;

#ifdef PMEM
  Allocator::Persist(new_sa,
                     sizeof(Directory<T>) + sizeof(uint64_t) * 2 * capacity);
  void **reserve_addr = Allocator::ReserveMemory();
  //++merge_time;
  auto old_dir = dir;
  TX_BEGIN(pool_addr) {
    pmemobj_tx_add_range_direct(reserve_addr, sizeof(void *));
    pmemobj_tx_add_range_direct(&dir, sizeof(dir));
    pmemobj_tx_add_range_direct(&back_dir, sizeof(back_dir));
    *reserve_addr = (void *)dir;
    dir = new_sa;
    back_dir = OID_NULL;
  }
  TX_ONABORT {
    std::cout << "TXN fails during doubling directory" << std::endl;
  }
  TX_END
/*
#ifdef EPOCH
  Allocator::Free(old_dir);
#endif
*/
#else
  dir = new_sa;
#endif
  // printf("Done!!Directory_Doubling towards %lld\n", dir->global_depth);
}

template <class T>
void Finger_EH<T>::Directory_Update(Directory<T> *_sa, int x, Table<T> *new_b) {
  // printf("directory update for %d\n", x);
  /*
#ifdef CRASH
      split_num++;
      if(random()%5000 == 0){
        abort();
      }
#endif
*/
  Table<T> **dir_entry = _sa->_;
  auto global_depth = _sa->global_depth;
  unsigned depth_diff = global_depth - new_b->local_depth;
  if (depth_diff == 0) {
    if (x % 2 == 0) {
      dir_entry[x + 1] = new_b;
      Allocator::Persist(&dir_entry[x + 1], sizeof(uint64_t));
    } else {
      dir_entry[x] = new_b;
      Allocator::Persist(&dir_entry[x], sizeof(uint64_t));
    }
    __sync_fetch_and_add(&_sa->depth_count, 2);
  } else {
    int chunk_size = pow(2, global_depth - (new_b->local_depth - 1));
    x = x - (x % chunk_size);
    int base = chunk_size / 2;
    for (int i = base - 1; i >= 0; --i) {
      dir_entry[x + base + i] = new_b;
      Allocator::Persist(&dir_entry[x + base + i], sizeof(uint64_t));
    }
  }
  // printf("Done!directory update for %d\n", x);
}

template <class T>
void Finger_EH<T>::Directory_Merge_Update(Directory<T> *_sa, uint64_t key_hash,
                                          Table<T> *left_seg) {
  // printf("directory update for %d\n", x);
  Table<T> **dir_entry = _sa->_;
  auto global_depth = _sa->global_depth;
  auto x = (key_hash >> (8 * sizeof(key_hash) - global_depth));
  uint64_t chunk_size = pow(2, global_depth - (left_seg->local_depth));
  auto left = x - (x % chunk_size);
  auto right = left + chunk_size / 2;

  for (int i = right; i < right + chunk_size / 2; ++i) {
    dir_entry[i] = left_seg;
    Allocator::Persist(&dir_entry[i], sizeof(uint64_t));
  }

  if ((left_seg->local_depth + 1) == global_depth) {
    SUB(&_sa->depth_count, 2);
  }
}

template <class T>
void Finger_EH<T>::recoverTable(Table<T> **target_table, size_t key_hash) {
  /*Set the lockBit to ahieve the mutal exclusion of the recover process*/
  uint64_t snapshot = (uint64_t)*target_table;
  Table<T> *target = (Table<T> *)(snapshot & (~recoverBit));

  if (!target->dirty_bit) {
    /*reset the recovery bit of the pointer*/
    *target_table = target;
    return;
  }

  auto old_lock = target->lock_bit;
  if (old_lock) return;
  int new_lock = 1;

  if (!CAS(&target->lock_bit, &old_lock, new_lock)) {
    return; /*fail to set the lock*/
  }

  target->recoverMetadata();
  if (target->state != 0) {
    target->pattern = key_hash >> (8 * sizeof(key_hash) - target->local_depth);
    Allocator::Persist(&target->pattern, sizeof(target->pattern));
    Table<T> *next_table = (Table<T> *)pmemobj_direct(target->next);
    if (target->state == -2) {
      if ((next_table->pattern == 0) ||
          (next_table->pattern == ((target->pattern << 1) + 1))) {
        /*Help finish the split operation*/
        next_table->recoverMetadata();
        target->HelpSplit(next_table);
        std::cout <<"target's number = " << target->number<< std::endl;
        std::cout <<"next's number = " << next_table->number<< std::endl;
        /*
        std::cout << "target's pattern = " << std::hex<<target->pattern << std::endl;
        std::cout << "next_table's pattern = " << std::hex<<next_table->pattern << std::endl;
        std::cout << "target's local depth = " << std::dec<<target->local_depth << std::endl;
        std::cout << "neightbor's local depth = " << std::dec<<next_table->local_depth << std::endl;
        */
        Lock_Directory();
        auto x = (key_hash >> (8 * sizeof(key_hash) - dir->global_depth));
        if (target->local_depth < dir->global_depth) {
          Directory_Update(dir, x, next_table);
        } else {
          Directory_Doubling(x, next_table);
        }
        Unlock_Directory();
        Allocator::NTWrite64(&target->local_depth, target->local_depth + 1);
        /*release the lock for the target bucket and the new bucket*/
        next_table->state = 0;
        Allocator::Persist(&next_table->state, sizeof(int));
      }
    } else if (target->state == -1) {
      if (next_table->pattern == ((target->pattern << 1) + 1)) {
        target->Merge(next_table, true);
        Allocator::Persist(target, sizeof(Table<T>));
        target->next = next_table->next;
#ifdef EPOCH
        Allocator::Free(next_table);
#endif
      }
    }
    target->state = 0;
    Allocator::Persist(&target->state, sizeof(int));
  }
  target->dirty_bit = 0;
  *target_table = target;
  /*No need to reset the lock since the data has been recovered*/
}

template <class T>
void Finger_EH<T>::Recovery() {
  /*scan the directory, set the clear bit, and also set the dirty bit in the
   * segment to indicate that this segment is clean*/
  // Allocator::EpochRecovery();
  lock = 0;
  /*first check the back_dir log*/
  if (!OID_IS_NULL(back_dir)) {
    pmemobj_free(&back_dir);
  }

  auto dir_entry = dir->_;
  int length = pow(2, dir->global_depth);
  std::cout << "The length = " << length << std::endl;
  Table<T> *target;
  size_t i = 0, global_depth = dir->global_depth, depth_cur, stride, buddy;
  auto old_depth_count = dir->depth_count;
  dir->depth_count = 0;

  while (i < length) {
    dir_entry[i] = (Table<T> *)((uint64_t)dir_entry[i] | recoverBit);
    target = (Table<T> *)((uint64_t)dir_entry[i] & (~recoverBit));
    target->dirty_bit = 1;
    target->lock_bit = 0;
    depth_cur = target->local_depth;
    stride = pow(2, global_depth - depth_cur);
    if (depth_cur == global_depth) dir->depth_count++;
    //target->pattern = i >> (global_depth - depth_cur);
    buddy = i + stride;
    for (int j = buddy - 1; j > i; j--) {
      dir_entry[j] = (Table<T> *)((uint64_t)dir_entry[j] | recoverBit);
      target = (Table<T> *)((uint64_t)dir_entry[j] & (~recoverBit));
      target->dirty_bit = 1;
      target->lock_bit = 0;
      if (dir_entry[j] != dir_entry[i]) {
        std::cout << "Fix the link "<< j << std::endl;
        dir_entry[j] = dir_entry[i];
        target->pattern = i >> (global_depth - depth_cur);
        // target->state =
        //    -3; /*means that this bucket needs to fix its right link*/
      }
    }
    i = i + stride;
  }

#ifdef PMEM
  Allocator::Persist(dir_entry, sizeof(uint64_t) * length);
#endif
}

template<class T>
void Finger_EH<T>::Insert(T key, Value_t value){
  return Insert(key, value, 0);
}

template<class T>
void Finger_EH<T>::Insert(T key, Value_t value, bool is_in_epoch){
  if(!is_in_epoch){
    auto epoch_guard = Allocator::AquireEpochGuard();
    return Insert(key, value, 0);
  }

  return Insert(key, value, 0);
}

template <class T>
void Finger_EH<T>::Insert(T key, Value_t value, int is_crash) {
  uint64_t key_hash;
  if constexpr (std::is_pointer_v<T>) {
    key_hash = h(key->key, key->length);
  } else {
    key_hash = h(&key, sizeof(key));
  }

  auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
RETRY:
  auto old_sa = dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  auto dir_entry = old_sa->_;
  Table<T> *target = dir_entry[x];
  if (((uint64_t)target & recoverBit)) {
    recoverTable(&dir_entry[x], key_hash);
    goto RETRY;
  }

  if(is_crash == 1){
    //std::cout << "Crash Test" << std::endl;
    if(random()%1000000 == 0){
      abort();
    }
  }
  
  // printf("insert key %lld, x = %d, y = %d, meta_hash = %d\n", key, x,
  // BUCKET_INDEX(key_hash), meta_hash);
  auto ret = target->Insert(key, value, key_hash, meta_hash, &dir);
  if (ret == -1) {
    if (!target->bucket->try_get_lock()) {
      goto RETRY;
    }

    /*verify procedure*/
    auto old_sa = dir;
    auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
    if (old_sa->_[x] != target) /* verify process*/
    {
      target->bucket->release_lock();
      goto RETRY;
    }

    auto new_b =
        target->Split(key_hash); /* also needs the verify..., and we use try
                                    lock for this rather than the spin lock*/
    /* update directory*/
  REINSERT:
    // the following three statements may be unnecessary...
    old_sa = dir;
    dir_entry = old_sa->_;
    x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
    // assert(target == dir_entry[x].bucket_p);
    if (target->local_depth < old_sa->global_depth) {
      if (Test_Directory_Lock_Set()) {
        goto REINSERT;
      }
      Directory_Update(old_sa, x, new_b);
      /*after the update, I need to recheck*/
      if (Test_Directory_Lock_Set() || old_sa->version != dir->version) {
        goto REINSERT;
      }
    } else {
      Lock_Directory();
      if (old_sa->version != dir->version) {
        Unlock_Directory();
        goto REINSERT;
      }
      /*
      std::cout << "target count = " << target->number << std::endl;
      std::cout << "new table count = " << new_b->number << std::endl;
      std::cout << "target's pattern = " <<std::hex<< target->pattern << std::endl;
      std::cout << "target's local depth = "<< target->local_depth << std::endl;
      std::cout << "x = "<< x << std::endl;*/
      Directory_Doubling(x, new_b);
      Unlock_Directory();
    }
#ifdef PMEM
    Allocator::NTWrite64(&target->local_depth, target->local_depth + 1);
#else
    target->local_depth += 1;
#endif
    /*release the lock for the target bucket and the new bucket*/
    target->state = 0;
    new_b->state = 0;
    Allocator::Persist(&target->state, sizeof(int));
    Allocator::Persist(&new_b->state, sizeof(int));
    Bucket<T> *curr_bucket;
    for (int i = 0; i < kNumBucket; ++i) {
      curr_bucket = target->bucket + i;
      curr_bucket->release_lock();
    }
    curr_bucket = new_b->bucket;
    curr_bucket->release_lock();
    goto RETRY;
  } else if (ret == -2) {
    goto RETRY;
  }
}

template <class T>
Value_t Finger_EH<T>::Get(T key, bool is_in_epoch) {
  if (!is_in_epoch) {
#ifdef EPOCH
    auto epoch_guard = Allocator::AquireEpochGuard();
#endif
    return Get(key);
  }
  return Get(key);
}

template <class T>
Value_t Finger_EH<T>::Get(T key) {
  uint64_t key_hash;
  if constexpr (std::is_pointer_v<T>) {
    key_hash = h(key->key, key->length);
  } else {
    key_hash = h(&key, sizeof(key));
  }
  auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
RETRY:
  auto old_sa = dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  auto y = BUCKET_INDEX(key_hash);
  auto dir_entry = old_sa->_;
  Table<T> *target = dir_entry[x];

  if (unlikely((uint64_t)target & recoverBit)) {
    recoverTable(&dir_entry[x], key_hash);
    goto RETRY;
  }

  Bucket<T> *target_bucket = target->bucket + y;
  Bucket<T> *neighbor_bucket = target->bucket + ((y + 1) & bucketMask);
  // printf("Get key %lld, x = %d, y = %d, meta_hash = %d\n", key, x,
  // BUCKET_INDEX(key_hash), meta_hash);

  uint32_t old_version =
      __atomic_load_n(&target_bucket->version_lock, __ATOMIC_ACQUIRE);
  uint32_t old_neighbor_version =
      __atomic_load_n(&neighbor_bucket->version_lock, __ATOMIC_ACQUIRE);

  if ((old_version & lockSet) || (old_neighbor_version & lockSet)) {
    goto RETRY;
  }

  /*verification procedure*/
  auto new_sa = dir;
  x = (key_hash >> (8 * sizeof(key_hash) - new_sa->global_depth));
  if (new_sa->_[x] != target) {
    goto RETRY;
  }

  auto ret = target_bucket->check_and_get(meta_hash, key, false);
  if (target_bucket->test_lock_version_change(old_version)) {
    goto RETRY;
  }
  if (ret != NONE) {
    return ret;
  }

  /*no need for verification procedure, we use the version number of
   * target_bucket to test whether the bucket has ben spliteted*/
  ret = neighbor_bucket->check_and_get(meta_hash, key, true);
  if (neighbor_bucket->test_lock_version_change(old_neighbor_version)) {
    goto RETRY;
  }
  if (ret != NONE) {
    return ret;
  }

  if (target_bucket->test_stash_check()) {
    auto test_stash = false;
    if (target_bucket->test_overflow()) {
      /*this only occur when the bucket has more key-values than 10 that are
       * overfloed int he shared bucket area, therefore it needs to search in
       * the extra bucket*/
      test_stash = true;
    } else {
      /*search in the original bucket*/
      int mask = target_bucket->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (target_bucket->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & target_bucket->overflowMember) == 0)) {
            // test_stash = true;
            // goto TEST_STASH;
            /*directly check stash*/
            Bucket<T> *stash =
                target->bucket + kNumBucket +
                ((target_bucket->finger_array[19] >> (i * 2)) & stashMask);
            auto ret = stash->check_and_get(meta_hash, key, false);
            if (ret != NONE) {
              if (target_bucket->test_lock_version_change(old_version)) {
                goto RETRY;
              }
              return ret;
            }
          }
        }
      }

      mask = neighbor_bucket->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (neighbor_bucket->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & neighbor_bucket->overflowMember) != 0)) {
            // test_stash = true;
            // break;
            Bucket<T> *stash =
                target->bucket + kNumBucket +
                ((neighbor_bucket->finger_array[19] >> (i * 2)) & stashMask);
            auto ret = stash->check_and_get(meta_hash, key, false);
            if (ret != NONE) {
              if (target_bucket->test_lock_version_change(old_version)) {
                goto RETRY;
              }
              return ret;
            }
          }
        }
      }
      goto FINAL;
    }
  TEST_STASH:
    if (test_stash == true) {
      for (int i = 0; i < stashBucket; ++i) {
        Bucket<T> *stash =
            target->bucket + kNumBucket + ((i + (y & stashMask)) & stashMask);
        auto ret = stash->check_and_get(meta_hash, key, false);
        if (ret != NONE) {
          if (target_bucket->test_lock_version_change(old_version)) {
            goto RETRY;
          }
          return ret;
        }
      }
    }
  }
FINAL:
  // printf("the x = %lld, the y = %lld, the meta_hash is %d\n", x, y,
  // meta_hash);
  return NONE;
}

template <class T>
void Finger_EH<T>::TryMerge(size_t key_hash) {
  /*Compute the left segment and right segment*/
  do {
    auto old_dir = dir;
    auto x = (key_hash >> (8 * sizeof(key_hash) - old_dir->global_depth));
    auto target = old_dir->_[x];
    int chunk_size = pow(2, old_dir->global_depth - (target->local_depth - 1));
    assert(chunk_size >= 2);
    int left = x - (x % chunk_size);
    int right = left + chunk_size / 2;
    auto left_seg = old_dir->_[left];
    auto right_seg = old_dir->_[right];

    if (((uint64_t)left_seg & recoverBit)) {
      recoverTable(&old_dir->_[left], key_hash);
      continue;
    }

    if (((uint64_t)right_seg & recoverBit)) {
      recoverTable(&old_dir->_[right], key_hash);
      continue;
    }

    size_t _pattern0 =
        ((key_hash >> (8 * sizeof(key_hash) - target->local_depth + 1)) << 1);
    size_t _pattern1 =
        ((key_hash >> (8 * sizeof(key_hash) - target->local_depth + 1)) << 1) +
        1;

    /* Get the lock from left to right*/
    if (left_seg->Acquire_and_verify(_pattern0)) {
      if (right_seg->Acquire_and_verify(_pattern1)) {
        if (left_seg->local_depth != right_seg->local_depth) {
          left_seg->bucket->release_lock();
          right_seg->bucket->release_lock();
          return;
        }

        if ((left_seg->number != 0) && (right_seg->number != 0)) {
          left_seg->bucket->release_lock();
          right_seg->bucket->release_lock();
          return;
        }
        left_seg->Acquire_remaining_locks();
        right_seg->Acquire_remaining_locks();

        /*First improve the local depth, */
        left_seg->local_depth = left_seg->local_depth - 1;
        Allocator::Persist(&left_seg->local_depth, sizeof(uint64_t));
        left_seg->state = -1;
        Allocator::Persist(&left_seg->state, sizeof(int));
        right_seg->state = -1;
        Allocator::Persist(&right_seg->state, sizeof(int));
      REINSERT:
        old_dir = dir;
        /*Update the directory from left to right*/
        while (Test_Directory_Lock_Set()) {
          asm("nop");
        }
        /*start the merge operation*/
        Directory_Merge_Update(old_dir, key_hash, left_seg);

        if (Test_Directory_Lock_Set() || old_dir->version != dir->version) {
          goto REINSERT;
        }

        if (right_seg->number != 0) {
          left_seg->Merge(right_seg);
        }
        //std::cout << "reserve a memory addr "<<++merge_time<< std::endl;
        void **reserve_addr = Allocator::ReserveMemory();
        // std::cout << "successfully get a memory addr" << std::endl;
        TX_BEGIN(pool_addr) {
          pmemobj_tx_add_range_direct(reserve_addr, sizeof(void *));
          pmemobj_tx_add_range_direct(&left_seg->next, sizeof(left_seg->next));
          *reserve_addr = right_seg;
          left_seg->next = right_seg->next;
        }
        TX_ONABORT { std::cout << "Error for merge txn" << std::endl; }
        TX_END

        // left_seg->next = right_seg->next;
        left_seg->pattern = left_seg->pattern >> 1;
        Allocator::Persist(&left_seg->pattern, sizeof(uint64_t));
        left_seg->state = 0;
        Allocator::Persist(&left_seg->state, sizeof(int));
        right_seg->Release_all_locks();
        left_seg->Release_all_locks();
        /*
        #ifdef EPOCH
                Allocator::Free(right_seg);
        #endif
        */
        /*Try to halve directory?*/
        if ((dir->depth_count == 0) && (dir->global_depth > 2)) {
          Lock_Directory();
          if (dir->depth_count == 0) {
            Halve_Directory();
          }
          Unlock_Directory();
        }
      } else {
        left_seg->bucket->release_lock();
        if (old_dir == dir) {
          return;
        }
      }
    } else {
      if (old_dir == dir) {
        /* If the directory itself does not change, directory return*/
        return;
      }
    }

  } while (true);
}

template <class T>
bool Finger_EH<T>::Delete(T key, bool is_in_epoch) {
  if (!is_in_epoch) {
    auto epoch_guard = Allocator::AquireEpochGuard();
    return Delete(key);
  }
  return Delete(key);
}

/*the delete operation of the */
template <class T>
bool Finger_EH<T>::Delete(T key) {
  /*
  #ifdef EPOCH
    auto epoch_guard = Allocator::AquireEpochGuard();
  #endif
  */
  /*Basic delete operation and merge operation*/
  uint64_t key_hash;
  if constexpr (std::is_pointer_v<T>) {
    key_hash = h(key->key, key->length);
  } else {
    key_hash = h(&key, sizeof(key));
  }
  auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
RETRY:
  auto old_sa = dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  auto dir_entry = old_sa->_;
  Table<T> *target_table = dir_entry[x];

  if (((uint64_t)target_table & recoverBit)) {
    recoverTable(&dir_entry[x], key_hash);
    goto RETRY;
  }

  /*we need to first do the locking and then do the verify*/
  auto y = BUCKET_INDEX(key_hash);
  Bucket<T> *target = target_table->bucket + y;
  Bucket<T> *neighbor = target_table->bucket + ((y + 1) & bucketMask);
  target->get_lock();
  if (!neighbor->try_get_lock()) {
    target->release_lock();
    goto RETRY;
  }

  old_sa = dir;
  x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  if (old_sa->_[x] != target_table) {
    target->release_lock();
    neighbor->release_lock();
    goto RETRY;
  }

  auto ret = target->Delete(key, meta_hash, false);
  if (ret == 0) {
#ifdef COUNTING
    auto num = SUB(&target_table->number, 1);
#endif
    target->release_lock();
#ifdef PMEM
    Allocator::Persist(&target->bitmap, sizeof(target->bitmap));
#endif
    neighbor->release_lock();
#ifdef COUNTING
    if (num == 0) {
      TryMerge(key_hash);
    }
#endif
    return true;
  }

  ret = neighbor->Delete(key, meta_hash, true);
  if (ret == 0) {
#ifdef COUNTING
    auto num = SUB(&target_table->number, 1);
#endif
    neighbor->release_lock();
#ifdef PMEM
    Allocator::Persist(&neighbor->bitmap, sizeof(neighbor->bitmap));
#endif
    target->release_lock();
#ifdef COUNTING
    if (num == 0) {
      TryMerge(key_hash);
    }
#endif
    return true;
  }

  if (target->test_stash_check()) {
    auto test_stash = false;
    if (target->test_overflow()) {
      /*this only occur when the bucket has more key-values than 10 that are
       * overfloed int he shared bucket area, therefore it needs to search in
       * the extra bucket*/
      test_stash = true;
    } else {
      /*search in the original bucket*/
      int mask = target->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (target->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & target->overflowMember) == 0)) {
            test_stash = true;
            goto TEST_STASH;
          }
        }
      }

      mask = neighbor->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (neighbor->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & neighbor->overflowMember) != 0)) {
            test_stash = true;
            break;
          }
        }
      }
    }

  TEST_STASH:
    if (test_stash == true) {
      Bucket<T> *stash = target_table->bucket + kNumBucket;
      stash->get_lock();
      for (int i = 0; i < stashBucket; ++i) {
        int index = ((i + (y & stashMask)) & stashMask);
        Bucket<T> *curr_stash = target_table->bucket + kNumBucket + index;
        auto ret = curr_stash->Delete(key, meta_hash, false);
        if (ret == 0) {
          /*need to unset indicator in original bucket*/
          stash->release_lock();
#ifdef PMEM
          Allocator::Persist(&curr_stash->bitmap, sizeof(curr_stash->bitmap));
#endif
          auto bucket_ix = BUCKET_INDEX(key_hash);
          auto org_bucket = target_table->bucket + bucket_ix;
          assert(org_bucket == target);
          target->unset_indicator(meta_hash, neighbor, key, index);
#ifdef COUNTING
          auto num = SUB(&target_table->number, 1);
#endif
          neighbor->release_lock();
          target->release_lock();
#ifdef COUNTING
          if (num == 0) {
            TryMerge(key_hash);
          }
#endif
          return true;
        }
      }
      stash->release_lock();
    }
  }
  neighbor->release_lock();
  target->release_lock();
  // printf("Not found key %lld, the position is %d, the bucket is %lld, the
  // local depth = %lld, the global depth = %lld, the pattern is %lld\n", key,
  // x, y,target_table->local_depth, dir->global_depth,target_table->pattern);
  return false;
}

/*
template<class T>
void Finger_EH::CheckDepthCount() {
  auto capacity = pow(2, dir->global_depth);
  auto dir_entry = dir->_;
  Table<T> *current_table;
  int count = 0;
  for (int i = 0; i < capacity; ++i) {
    current_table = dir_entry[i];
    count += (current_table->local_depth == dir->global_depth) ? 1 : 0;
  }
  printf("calculate count = %d\n", count);
  printf("the recorded depth_count = %d\n", dir->depth_count);
}
*/

/*DEBUG FUNCTION: search the position of the key in this table and print
 * correspongdign informantion in this table, to test whether it is correct*/

template <class T>
int Finger_EH<T>::FindAnyway(T key) {
  uint64_t key_hash;
  if constexpr (std::is_pointer_v<T>) {
    // key_hash = h(key, (reinterpret_cast<string_key *>(key))->length);
    key_hash = h(key->key, key->length);
  } else {
    key_hash = h(&key, sizeof(key));
  }
  auto meta_hash = ((uint8_t)(key_hash & kMask));
  auto x = (key_hash >> (8 * sizeof(key_hash) - dir->global_depth));

  size_t _count = 0;
  size_t seg_count = 0;
  Directory<T> *seg = dir;
  Table<T> **dir_entry = seg->_;
  Table<T> *ss;
  auto global_depth = seg->global_depth;
  size_t depth_diff;
  int capacity = pow(2, global_depth);
  for (int i = 0; i < capacity;) {
    ss = dir_entry[i];
    Bucket<T> *curr_bucket;
    for (int j = 0; j < kNumBucket; ++j) {
      curr_bucket = ss->bucket + j;
      auto ret = curr_bucket->check_and_get(meta_hash, key, false);
      if (ret != NONE) {
        printf("successfully find in the normal bucket with false\n");
        printf(
            "the segment is %d, the bucket is %d, the local depth = %lld, the "
            "pattern is %lld\n",
            i, j, ss->local_depth, ss->pattern);
        return 0;
      }
      ret = curr_bucket->check_and_get(meta_hash, key, true);
      if (ret != NONE) {
        printf("successfully find in the normal bucket with true\n");
        printf(
            "the segment is %d, the bucket is %d, the local depth is %lld, the "
            "pattern is %lld\n",
            i, j, ss->local_depth, ss->pattern);
        return 0;
      }
    }

    for (int i = 0; i < stashBucket; ++i) {
      curr_bucket = ss->bucket + kNumBucket + i;
      auto ret = curr_bucket->check_and_get(meta_hash, key, false);
      if (ret != NONE) {
        printf("successfully find in the stash bucket\n");
        auto bucket_ix = BUCKET_INDEX(key_hash);
        auto org_bucket = ss->bucket + bucket_ix;
        auto neighbor_bucket = ss->bucket + ((bucket_ix + 1) & bucketMask);
        printf("the segment number is %d, the bucket_ix is %d\n", x, bucket_ix);

        printf("the image of org_bucket\n");
        // printf("the stash check is %d\n", org_bucket->test_stash_check());
        int mask = org_bucket->finger_array[14];
        for (int j = 0; j < 4; ++j) {
          printf(
              "the hash is %d, the pos bit is %d, the alloc bit is %d, the "
              "stash bucket info is %d, the real stash bucket info is %d\n",
              org_bucket->finger_array[15 + j],
              (org_bucket->overflowMember >> (j)) & 1,
              (org_bucket->finger_array[14] >> j) & 1,
              (org_bucket->finger_array[19] >> (j * 2)) & stashMask, i);
        }

        printf("the image of the neighbor bucket\n");
        printf("the stash check is %d\n", neighbor_bucket->test_stash_check());
        mask = neighbor_bucket->finger_array[14];
        for (int j = 0; j < 4; ++j) {
          printf(
              "the hash is %d, the pos bit is %d, the alloc bit is %d, the "
              "stash bucket info is %d, the real stash bucket info is %d\n",
              neighbor_bucket->finger_array[15 + j],
              (neighbor_bucket->overflowMember >> (j)) & 1,
              (neighbor_bucket->finger_array[14] >> j) & 1,
              (neighbor_bucket->finger_array[19] >> (j * 2)) & stashMask, i);
        }

        if (org_bucket->test_overflow()) {
          printf("the org bucket has overflowed\n");
        }
        return 0;
      }
    }

    depth_diff = global_depth - ss->local_depth;
    _count += ss->number;
    seg_count++;
    i += pow(2, depth_diff);
  }
  return -1;
}

}  // namespace extendible