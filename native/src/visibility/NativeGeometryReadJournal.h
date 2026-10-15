#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace rdr95 {
// Owned comparison bytes only. Neither representation grants native lifetime,
// graph immutability, or skip authority. Entry order is the native read order.
// Keep both implementations for whole-path OFF/ON validation and measurement.
template<bool Packed> class NativeGeometryReadJournal;

template<> class NativeGeometryReadJournal<false> {
public:
 struct Entry {
  std::uint64_t address{};
  std::vector<std::byte> saved;
  bool backingStorage{};
 };
 bool append(std::uint64_t address,const void* data,std::size_t size,bool storage) {
  if(!data||!size||size>192)return false;
  const auto* first=static_cast<const std::byte*>(data);
  entries_.push_back({address,{first,first+size},storage});return true;
 }
 void clear()noexcept{entries_.clear();}
 std::span<const std::byte> bytes(const Entry& entry)const noexcept{return entry.saved;}
 auto begin()const noexcept{return entries_.begin();}
 auto end()const noexcept{return entries_.end();}
 std::size_t size()const noexcept{return entries_.size();}
private:
 std::vector<Entry> entries_;
};

template<> class NativeGeometryReadJournal<true> {
public:
 NativeGeometryReadJournal()=default;
 NativeGeometryReadJournal(const NativeGeometryReadJournal&)=default;
 NativeGeometryReadJournal(NativeGeometryReadJournal&&)noexcept=default;
 NativeGeometryReadJournal& operator=(NativeGeometryReadJournal&&)noexcept=default;
 NativeGeometryReadJournal& operator=(const NativeGeometryReadJournal& other){
  if(this!=&other){
   // Offsets and arena form one value. A failed second allocation must never
   // publish new descriptors over the old arena in an existing Output.
   NativeGeometryReadJournal copy(other);
   entries_.swap(copy.entries_);saved_.swap(copy.saved_);
  }
  return *this;
 }
 struct Entry {
  std::uint64_t address{};
  std::uint32_t offset{};
  std::uint16_t length{};
  bool backingStorage{};
 };
 static_assert(sizeof(Entry)==16);
 bool append(std::uint64_t address,const void* data,std::size_t size,bool storage) {
  if(!data||!size||size>192||saved_.size()>std::numeric_limits<std::uint32_t>::max()||
     size>std::numeric_limits<std::uint32_t>::max()-saved_.size())return false;
  const auto offset=static_cast<std::uint32_t>(saved_.size());
  const auto* first=static_cast<const std::byte*>(data);
  saved_.insert(saved_.end(),first,first+size);
  entries_.push_back({address,offset,static_cast<std::uint16_t>(size),storage});return true;
 }
 // Reuse the owned arena's capacity across cold recaptures. No address into the
 // arena is retained in an Entry, so vector growth and Output moves are safe.
 void clear()noexcept{entries_.clear();saved_.clear();}
 std::span<const std::byte> bytes(const Entry& entry)const noexcept {
  return {saved_.data()+entry.offset,entry.length};
 }
 auto begin()const noexcept{return entries_.begin();}
 auto end()const noexcept{return entries_.end();}
 std::size_t size()const noexcept{return entries_.size();}
private:
 std::vector<Entry> entries_;
 std::vector<std::byte> saved_;
};
}
