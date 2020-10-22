/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef REQ_SKETCH_IMPL_HPP_
#define REQ_SKETCH_IMPL_HPP_

#include <stdexcept>
#include <cmath>
#include <sstream>

#include "count_zeros.hpp"

namespace datasketches {

template<typename T, bool H, typename C, typename A>
req_compactor<T, H, C, A>::req_compactor(uint8_t lg_weight, uint32_t section_size):
lg_weight_(lg_weight),
coin_(false),
sorted_(true),
section_size_raw_(section_size),
section_size_(section_size),
num_sections_(req_constants::INIT_NUM_SECTIONS),
num_compactions_(0),
state_(0)
{}

template<typename T, bool H, typename C, typename A>
bool req_compactor<T, H, C, A>::is_sorted() const {
  return sorted_;
}

template<typename T, bool H, typename C, typename A>
uint32_t req_compactor<T, H, C, A>::get_num_items() const {
  return items_.size();
}

template<typename T, bool H, typename C, typename A>
uint32_t req_compactor<T, H, C, A>::get_nom_capacity() const {
  return 2 * num_sections_ * section_size_;
}

template<typename T, bool H, typename C, typename A>
template<typename FwdT>
void req_compactor<T, H, C, A>::append(FwdT&& item) {
  items_.push_back(std::forward<FwdT>(item));
  sorted_ = false;
}

template<typename T, bool H, typename C, typename A>
void req_compactor<T, H, C, A>::sort() {
  std::sort(items_.begin(), items_.end(), C());
  sorted_ = true;
}

template<typename T, bool H, typename C, typename A>
void req_compactor<T, H, C, A>::merge_sort_in(std::vector<T, A>&& items) {
  if (!sorted_) throw std::logic_error("compactor must be sorted at this point");
  if (items_.capacity() < items_.size() + items.size()) items_.reserve(items_.size() + items.size());
  auto middle = items_.end();
  std::move(items.begin(), items.end(), std::back_inserter(items_));
  std::inplace_merge(items_.begin(), middle, items_.end());
}

template<typename T, bool H, typename C, typename A>
std::vector<T, A> req_compactor<T, H, C, A>::compact() {
  // choose a part of the buffer to compact
  const uint32_t secs_to_compact = std::min(static_cast<uint32_t>(count_trailing_zeros_in_u32(~state_) + 1), num_sections_);
  const size_t compaction_range = compute_compaction_range(secs_to_compact);
  const uint32_t compact_from = compaction_range & 0xFFFFFFFFLL; // low 32
  const uint32_t compact_to = compaction_range >> 32; // high 32
  if (compact_to - compact_from < 2) throw std::logic_error("compaction range error");

  if ((num_compactions_ & 1) == 1) { coin_ = !coin_; } // for odd flip coin;
  else { coin_ = req_random_bit(); } // random coin flip

  auto promote = get_evens_or_odds(items_.begin() + compact_from, items_.begin() + compact_to, coin_);
  items_.erase(items_.begin() + compact_from, items_.begin() + compact_to);

  ++num_compactions_;
  ++state_;
  ensure_enough_sections();
  return promote;
}

template<typename T, bool H, typename C, typename A>
bool req_compactor<T, H, C, A>::ensure_enough_sections() {
  const double ssr = section_size_raw_ / sqrt(2);
  const uint32_t ne = nearest_even(ssr);
  if (num_compactions_ >= 1 << (num_sections_ - 1) && ne >= req_constants::MIN_K) {
    section_size_raw_ = ssr;
    section_size_ = ne;
    num_sections_ <<= 1;
    //ensure_capacity(2 * get_nom_capacity());
    return true;
  }
  return false;
}

template<typename T, bool H, typename C, typename A>
size_t req_compactor<T, H, C, A>::compute_compaction_range(uint32_t secs_to_compact) const {
  const uint32_t num_items = items_.size();
  uint32_t non_compact = get_nom_capacity() / 2 + (num_sections_ - secs_to_compact) * section_size_;
  // make compacted region even
  if ((num_items - non_compact & 1) == 1) ++non_compact;
  const size_t low = H ? 0 : non_compact;
  const size_t high = H ? num_items - non_compact : num_items;
  return (high << 32) + low;
}

template<typename T, bool H, typename C, typename A>
uint32_t req_compactor<T, H, C, A>::nearest_even(double value) {
  return static_cast<uint32_t>(round(value / 2)) << 1;
}

template<typename T, bool H, typename C, typename A>
template<typename Iter>
std::vector<T, A> req_compactor<T, H, C, A>::get_evens_or_odds(Iter from, Iter to, bool odds) {
  std::vector<T, A> result;
  if (from == to) return result;
  Iter i = from;
  if (odds) ++i;
  while (i != to) {
    result.push_back(*i);
    ++i;
    if (i == to) break;
    ++i;
  }
  return result;
}

// sketch

template<typename T, bool H, typename C, typename S, typename A>
req_sketch<T, H, C, S, A>::req_sketch(uint32_t k, const A& allocator):
allocator_(allocator),
k_(k),
max_nom_size_(0),
num_retained_(0),
n_(0)
{
  grow();
}

template<typename T, bool H, typename C, typename S, typename A>
bool req_sketch<T, H, C, S, A>::is_empty() const {
  return n_ == 0;
}

template<typename T, bool H, typename C, typename S, typename A>
uint64_t req_sketch<T, H, C, S, A>::get_n() const {
  return n_;
}

template<typename T, bool H, typename C, typename S, typename A>
uint32_t req_sketch<T, H, C, S, A>::get_num_retained() const {
  return num_retained_;
}

template<typename T, bool H, typename C, typename S, typename A>
bool req_sketch<T, H, C, S, A>::is_estimation_mode() const {
  return compactors_.size() > 1;
}

template<typename T, bool H, typename C, typename S, typename A>
template<typename FwdT>
void req_sketch<T, H, C, S, A>::update(FwdT&& item) {
//  if (Float.isNaN(item)) { return; }
//  if (isEmpty()) {
//    minValue = item;
//    maxValue = item;
//  } else {
//    if (item < minValue) { minValue = item; }
//    if (item > maxValue) { maxValue = item; }
//  }
  compactors_[0].append(item);
  ++num_retained_;
  ++n_;
  if (num_retained_ == max_nom_size_) {
    compactors_[0].sort();
    compress();
  }
  //  aux = null;
}

template<typename T, bool H, typename C, typename S, typename A>
void req_sketch<T, H, C, S, A>::grow() {
  const uint8_t lg_weight = get_num_levels();
  compactors_.push_back(Compactor(lg_weight, k_));
  update_max_nom_size();
}

template<typename T, bool H, typename C, typename S, typename A>
uint8_t req_sketch<T, H, C, S, A>::get_num_levels() const {
  return compactors_.size();
}

template<typename T, bool H, typename C, typename S, typename A>
void req_sketch<T, H, C, S, A>::update_max_nom_size() {
  max_nom_size_ = 0;
  for (const auto& compactor: compactors_) max_nom_size_ += compactor.get_nom_capacity();
}

template<typename T, bool H, typename C, typename S, typename A>
void req_sketch<T, H, C, S, A>::update_num_retained() {
  num_retained_ = 0;
  for (const auto& compactor: compactors_) num_retained_ += compactor.get_num_items();
}

template<typename T, bool H, typename C, typename S, typename A>
void req_sketch<T, H, C, S, A>::compress() {
  for (size_t h = 0; h < compactors_.size(); ++h) {
    if (compactors_[h].get_num_items() >= compactors_[h].get_nom_capacity()) {
      if (h + 1 >= get_num_levels()) { // at the top?
        grow(); // add a level, increases max_nom_size
      }
      auto promoted = compactors_[h].compact();
      compactors_[h + 1].merge_sort_in(std::move(promoted));
      update_num_retained();
      if (num_retained_ < max_nom_size_) break;
    }
  }
  update_max_nom_size();
  // aux = null;
}

template<typename T, bool H, typename C, typename S, typename A>
string<A> req_sketch<T, H, C, S, A>::to_string(bool print_levels, bool print_items) const {
  std::basic_ostringstream<char, std::char_traits<char>, AllocChar<A>> os;
  os << "### REQ sketch summary:" << std::endl;
  os << "   K              : " << k_ << std::endl;
  os << "   N              : " << n_ << std::endl;
  os << "   Empty          : " << (is_empty() ? "true" : "false") << std::endl;
  os << "   Estimation mode: " << (is_estimation_mode() ? "true" : "false") << std::endl;
  os << "   Levels         : " << compactors_.size() << std::endl;
  os << "   Sorted         : " << (compactors_[0].is_sorted() ? "true" : "false") << std::endl;
  os << "   Capacity items : " << max_nom_size_ << std::endl;
  os << "   Retained items : " << num_retained_ << std::endl;
//  os << "   Storage bytes  : " << get_serialized_size_bytes() << std::endl;
//  if (!is_empty()) {
//    os << "   Min value      : " << *min_value_ << std::endl;
//    os << "   Max value      : " << *max_value_ << std::endl;
//  }
  os << "### End sketch summary" << std::endl;

  if (print_levels) {
    os << "### REQ sketch levels:" << std::endl;
    os << "   index: nominal capacity, actual size" << std::endl;
    for (uint8_t i = 0; i < compactors_.size(); i++) {
      os << "   " << (unsigned int) i << ": "
        << compactors_[i].get_nom_capacity() << ", "
        << compactors_[i].get_num_items() << std::endl;
    }
    os << "### End sketch levels" << std::endl;
  }

  if (print_items) {
    os << "### REQ sketch data:" << std::endl;
    os << "### End sketch data" << std::endl;
  }
  return os.str();
}

} /* namespace datasketches */

#endif
